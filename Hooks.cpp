#include "Globals.hpp"
#include <string_view>

// --- THREAD-LOCAL RENDER CONTEXT ---
thread_local PHLWINDOWREF        g_pCurrentRenderWindow;
thread_local PHLLSREF            g_pCurrentRenderLayer;
thread_local const std::string*  g_pCurrentShaderPath     = nullptr;
thread_local CompiledShader*     g_pCurrentCompiledShader = nullptr;
thread_local float               g_pCurrentAnimProgress   = -1.0f;

// Plugin-relative reference time so the `time` uniform stays in float-precision
// range. steady_clock::now() seconds-since-epoch is in the billions and loses
// sub-second resolution after a few days of uptime.
static const auto g_pluginStartTime = std::chrono::steady_clock::now();

// --- PATH RESOLUTION ---
// Returns a pointer into one of the global maps, or nullptr. The pointer is
// valid until the next mutation of the underlying map; render hooks run on the
// main thread between dispatcher/listener invocations so this is safe within a
// single draw chain.
static const std::string* resolveShaderPath(const PHLWINDOW& pWindow, const PHLLS& pLS) {
    if (pWindow) {
        Desktop::View::CWindow* rawWin = pWindow.get();

        if (auto it = g_mWindowManualShaders.find(rawWin); it != g_mWindowManualShaders.end())
            return &it->second;

        if (Fullscreen::controller()->isFullscreen(pWindow)) {
            auto it = g_mWindowRuleShaders.find(rawWin);
            if (it != g_mWindowRuleShaders.end() && !it->second.fullscreen.empty())
                return &it->second.fullscreen;
        } else {
            auto it = g_mWindowRuleShaders.find(rawWin);
            if (it != g_mWindowRuleShaders.end()) {
                const auto& state      = it->second;
                const bool  isActive   = Desktop::focusState()->isWindowActive(pWindow);
                const bool  isFloating = rawWin->m_isFloating;
                if      (isFloating  && !state.floating.empty())  return &state.floating;
                else if (!isFloating && !state.tiled.empty())     return &state.tiled;
                else if (isActive    && !state.active.empty())    return &state.active;
                else if (!isActive   && !state.inactive.empty())  return &state.inactive;
                else if (!state.fallback.empty())                 return &state.fallback;
            }
        }

        const auto& initClass    = rawWin->m_initialClass;
        const auto& currentClass = rawWin->m_class;
        auto classIt = g_mWindowClassShaderMap.find(initClass);
        if (classIt == g_mWindowClassShaderMap.end()) classIt = g_mWindowClassShaderMap.find(currentClass);
        if (classIt != g_mWindowClassShaderMap.end()) return &classIt->second;

        return nullptr;
    }

    if (pLS) {
        auto it = g_mLayerNamespaceShaderMap.find(pLS->m_namespace);
        if (it != g_mLayerNamespaceShaderMap.end()) return &it->second;
    }
    return nullptr;
}

// --- OPEN ANIMATION STATE ---
// One-shot reveal animations live in g_mWindowOpenAnims keyed by raw window
// pointer. They're created by the window.open listener and dropped either when
// progress reaches 1.0 (in hkGLDrawTex, after the draw uses them) or when the
// window is destroyed.
OpenAnimState* getActiveOpenAnim(Desktop::View::CWindow* rawWin) {
    auto it = g_mWindowOpenAnims.find(rawWin);
    return it == g_mWindowOpenAnims.end() ? nullptr : &it->second;
}

// One-shot revert check for close animations. After the close request was sent,
// a well-behaved client destroys the surface (-> window.destroy cleans up the
// entry). If it's still alive after the grace period, it likely showed a dialog
// (e.g. "unsaved changes") — restore the window so it's visible/usable again.
static void scheduleCloseRevert(const PHLWINDOW& pWindow) {
    Desktop::View::CWindow* rawWin = pWindow.get();
    PHLWINDOWREF            weak   = pWindow;

    auto timer = Hyprutils::Memory::makeShared<CEventLoopTimer>(
        std::chrono::milliseconds(600),
        [rawWin, weak](SP<CEventLoopTimer> self, void* data) {
            (void)self;
            (void)data;
            auto w = weak.lock();
            if (!w) return;
            auto it = g_mWindowOpenAnims.find(rawWin);
            if (it == g_mWindowOpenAnims.end() || !it->second.isClose) return;
            g_mWindowOpenAnims.erase(rawWin);
            g_pHyprRenderer->damageWindow(w);
        },
        nullptr);

    g_pEventLoopManager->addTimer(timer);
}

// --- V0.56 HOOK: CGLElementRenderer::draw(CTexPassElement, CRegion) ---
typedef void (*TGLDrawTex)(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage);

void hkGLDrawTex(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage) {
    // V0.56: CRenderPass owns its elements as UP<IPassElement>, so this WP is a
    // weak-over-unique. CWeakPointer::lock() hard-asserts on that case
    // (WeakPtr.hpp:181 -> std::terminate) and takes the whole compositor down on
    // the first textured surface we see. get() has no such assert, and the pass
    // that owns the element is synchronously calling us, so borrowing is safe.
    CTexPassElement* elem = element.get();

    PHLWINDOW pWindow;
    PHLLS     pLS;

    if (elem && elem->m_data.surface) {
        // V0.56: CCompositor::getWindowFromSurface is gone. Resolve the owning
        // view straight off the surface resource instead.
        if (auto wlSurface = Desktop::View::CWLSurface::fromResource(elem->m_data.surface)) {
            if (auto view = wlSurface->view()) {
                pWindow = Desktop::View::CWindow::fromView(view);

                // Subsurfaces and popups are views in their own right and don't
                // cast to a window; their owning window is private in 0.56, so
                // fall back to whichever window the renderer is currently
                // walking — that's the window they belong to.
                if (!pWindow) {
                    const auto type = view->type();
                    if (type == Desktop::View::VIEW_TYPE_SUBSURFACE || type == Desktop::View::VIEW_TYPE_POPUP)
                        pWindow = g_pHyprRenderer->m_renderData.currentWindow.lock();
                }
            }
        }
        pLS = elem->m_data.currentLS.lock();
    }

    g_pCurrentRenderWindow = pWindow;
    g_pCurrentRenderLayer  = pLS;

    // Resolve the path once and stash it. Then do a single lookup-or-compile
    // on g_mCompiledCShaders and stash the resulting pointer for hkUseShader
    // to consume — saves the second find that used to happen down there.
    const std::string* pathToUse = resolveShaderPath(pWindow, pLS);

    // A one-shot open animation overrides whatever the normal resolution picked,
    // and its progress (0..1) is fed to hkUseShader via the thread-local.
    g_pCurrentAnimProgress = -1.0f;
    if (pWindow) {
        if (OpenAnimState* anim = getActiveOpenAnim(pWindow.get())) {
            pathToUse = &anim->path;
            const float elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(
                                      std::chrono::steady_clock::now() - anim->start)
                                      .count();
            g_pCurrentAnimProgress = anim->duration > 0.0f ? std::min(elapsed / anim->duration, 1.0f) : 1.0f;
        }
    }

    g_pCurrentShaderPath         = pathToUse;
    g_pCurrentCompiledShader     = pathToUse && !pathToUse->empty()
                                       ? getOrCompileShader(*pathToUse)
                                       : nullptr;

    // Schedule continuous redraw if the resolved shader uses `time`, or while
    // a one-shot animation is still running (its shader may not bind `time`).
    const bool animActive = g_pCurrentAnimProgress >= 0.0f;
    bool       needsDamage = false;
    if (g_pCurrentCompiledShader && g_pCurrentCompiledShader->usesTime)
        needsDamage = true;
    else if (animActive && pWindow)
        needsDamage = true;

    // A close animation freezes at progress==1.0 with the window invisible once
    // the close request was sent — stop re-damaging it; it either gets destroyed
    // or is reverted by the timer below.
    if (needsDamage && pWindow) {
        if (OpenAnimState* a = getActiveOpenAnim(pWindow.get()); a && a->isClose && a->closeSent)
            needsDamage = false;
    }
    if (needsDamage) {
        if (pWindow)
            g_pHyprRenderer->damageWindow(pWindow);
        else if (pLS) {
            if (auto mon = pLS->m_monitor.lock())
                mon->scheduleFrame();
        }
    }

    ((TGLDrawTex)g_pGLDrawTexHook->m_original)(thisptr, element, damage);

    // Animation completion.
    // Open animations: drop the entry and damage once more so the frame after
    // completion renders with the normal (non-anim) shader — the final animated
    // frame at progress==1.0 already shows the fully-revealed window.
    // Close animations: send the close request and KEEP the entry so the window
    // stays invisible (shader frozen at progress==1.0) until it's destroyed or
    // a revert timer restores it (client showed a dialog instead of closing).
    if (animActive && g_pCurrentAnimProgress >= 1.0f && pWindow) {
        OpenAnimState* anim = getActiveOpenAnim(pWindow.get());
        if (anim && anim->isClose) {
            if (!anim->closeSent) {
                anim->closeSent = true;
                pWindow->sendClose();
                scheduleCloseRevert(pWindow);
            }
        } else if (g_mWindowOpenAnims.erase(pWindow.get()))
            g_pHyprRenderer->damageWindow(pWindow);
    }

    g_pCurrentRenderWindow.reset();
    g_pCurrentRenderLayer.reset();
    g_pCurrentShaderPath     = nullptr;
    g_pCurrentCompiledShader = nullptr;
    g_pCurrentAnimProgress   = -1.0f;
}

// --- V0.56 HOOK: useShader ---
typedef Hyprutils::Memory::CWeakPointer<CShader> (*TUseShader)(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);

Hyprutils::Memory::CWeakPointer<CShader> hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog) {
    // The compiled-shader pointer was already resolved in hkGLDrawTex; just
    // pick it up here. If null there's no shader to apply for this surface —
    // skip the weak_ptr lock entirely (common case for most surfaces).
    CompiledShader* activeEntry = g_pCurrentCompiledShader;
    if (activeEntry && activeEntry->shader)
        prog = activeEntry->shader;
    else
        activeEntry = nullptr;

    auto result = ((TUseShader)g_pUseShaderHook->m_original)(thisptr, prog);

    // Inject uniforms using cached locations (no per-frame glGetUniformLocation).
    if (activeEntry) {
        PHLWINDOW contextWindow = g_pCurrentRenderWindow.lock();
        if (activeEntry->timeLoc >= 0) {
            const float t = std::chrono::duration_cast<std::chrono::duration<float>>(
                                std::chrono::steady_clock::now() - g_pluginStartTime)
                                .count();
            glUniform1f(activeEntry->timeLoc, t);
        }
        if (activeEntry->alphaLoc >= 0) {
            const float currentAlpha = contextWindow ? contextWindow->alphaTotal() : 1.0f;
            glUniform1f(activeEntry->alphaLoc, currentAlpha);
        }
        if (activeEntry->resolutionLoc >= 0) {
            Vector2D res(0, 0);
            if (auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock())
                res = mon->m_pixelSize;
            glUniform2f(activeEntry->resolutionLoc, (float)res.x, (float)res.y);
        }
        if (activeEntry->surfaceSizeLoc >= 0) {
            // V0.56: CWindow::m_size is gone; IView::logicalBox() is the
            // supported way to read a window's logical geometry.
            Vector2D sz(0, 0);
            if (contextWindow) {
                if (const auto box = contextWindow->logicalBox())
                    sz = box->size();
            }
            glUniform2f(activeEntry->surfaceSizeLoc, (float)sz.x, (float)sz.y);
        }
        if (activeEntry->mouseLoc >= 0 && Pointer::mgr()) {
            const Vector2D p = Pointer::mgr()->position();
            glUniform2f(activeEntry->mouseLoc, (float)p.x, (float)p.y);
        }
        if (activeEntry->isActiveLoc >= 0) {
            const float v = (contextWindow && Desktop::focusState()->isWindowActive(contextWindow)) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isActiveLoc, v);
        }
        if (activeEntry->isFloatingLoc >= 0) {
            const float v = (contextWindow && contextWindow->m_isFloating) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isFloatingLoc, v);
        }
        if (activeEntry->isFullscreenLoc >= 0) {
            const float v = (contextWindow && Fullscreen::controller()->isFullscreen(contextWindow)) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isFullscreenLoc, v);
        }
        if (activeEntry->progressLoc >= 0) {
            const float p = g_pCurrentAnimProgress >= 0.0f ? g_pCurrentAnimProgress : 1.0f;
            glUniform1f(activeEntry->progressLoc, p);
        }
        if (activeEntry->seedLoc >= 0) {
            // Stable per-window random seed (0..1) so each open animation has its
            // own pattern, like Niri's niri_random_seed. Derived from the window
            // pointer; stable for the whole life of the window.
            float s = 0.5f;
            if (contextWindow) {
                const uint64_t h = (uint64_t)(uintptr_t)contextWindow.get() * 2654435761u;
                s = (float)(h >> 32) / 4294967295.0f;
            } else if (auto l = g_pCurrentRenderLayer.lock()) {
                s = (float)(std::hash<std::string>{}(l->m_namespace) % 100000) / 100000.0f;
            }
            glUniform1f(activeEntry->seedLoc, s);
        }
    }

    return result;
}

void applyShaderRulesSafe(PHLWINDOW pWindow) {
    if (!pWindow || !pWindow->m_ruleApplicator) return;
    Desktop::View::CWindow* rawWin = pWindow.get();

    WindowShaderState state;
    bool              hasRules = false;

    const auto& tagsSet = pWindow->m_ruleApplicator->m_tagKeeper.getTags();
    for (const auto& tag : tagsSet) {
        // Trim trailing '*' and spaces without repeated pop_back allocations.
        std::string_view sv(tag);
        size_t end = sv.find_last_not_of("* \t");
        if (end == std::string_view::npos) continue;
        sv = sv.substr(0, end + 1);

        // Fast reject: every recognized prefix starts with "shader".
        if (sv.size() < 7 || sv.substr(0, 6) != "shader") continue;

        const auto assign = [&](std::string& dst, size_t prefixLen) {
            dst.assign(sv.substr(prefixLen));
            hasRules = true;
        };

        if      (sv.substr(0, 7)  == "shader:")            assign(state.fallback,   7);
        else if (sv.substr(0, 14) == "shader_active:")     assign(state.active,     14);
        else if (sv.substr(0, 16) == "shader_inactive:")   assign(state.inactive,   16);
        else if (sv.substr(0, 16) == "shader_floating:")   assign(state.floating,   16);
        else if (sv.substr(0, 13) == "shader_tiled:")      assign(state.tiled,      13);
        else if (sv.substr(0, 18) == "shader_fullscreen:") assign(state.fullscreen, 18);
        else if (sv.substr(0, 17) == "shader_open_anim:") {
            // Optional @<seconds> suffix sets the animation duration.
            const std::string_view rest = sv.substr(17);
            const size_t          at   = rest.find('@');
            if (at != std::string_view::npos) {
                state.openAnim.assign(rest.substr(0, at));
                try {
                    const float d = std::stof(std::string(rest.substr(at + 1)));
                    if (d > 0.0f) state.openAnimDuration = d;
                } catch (...) {}
            } else {
                state.openAnim.assign(rest);
            }
            hasRules = true;
        }
    }

    if (hasRules) {
        g_mWindowRuleShaders[rawWin] = std::move(state);
        g_pHyprRenderer->damageWindow(pWindow);
    } else {
        g_mWindowRuleShaders.erase(rawWin);
    }
}
