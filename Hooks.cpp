#include "Globals.hpp"
#include <string_view>
#include <cstdio>

// --- ACTIVE RENDER CONTEXT ---
// Plain globals, not thread_local — see the note in Globals.hpp. A thread_local
// with a non-trivial destructor pins this .so against dlclose and breaks
// live-reload; rendering is main-thread-only, so TLS bought nothing here.
PHLWINDOWREF        g_pCurrentRenderWindow;
PHLLSREF            g_pCurrentRenderLayer;
CompiledShader*     g_pCurrentCompiledShader = nullptr;
float               g_pCurrentAnimProgress   = -1.0f;
float               g_pCurrentAnimSeed       = -1.0f;

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

// --- ONE-SHOT ANIMATIONS ---

// Stable 0..1 value from a pointer, so two windows running the same open
// animation don't play it identically. finalizer from splitmix64 — the PR's
// single multiply left the low bits of consecutive heap addresses correlated,
// which shows up as neighbouring windows animating in lockstep.
float animSeedFor(const void* p) {
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (float)(h >> 40) / (float)((1u << 24) - 1);
}

AnimSpec parseAnimSpec(const std::string& arg) {
    AnimSpec spec;
    spec.path = arg;

    if (const size_t at = arg.rfind('@'); at != std::string::npos) {
        try {
            const float d = std::stof(arg.substr(at + 1));
            // Clamped, not rejected: `@10` means "as long as you'll allow", and
            // silently falling back to 0.3s is the least useful reading of it.
            if (d > 0.0f) {
                spec.duration = std::min(d, MAX_ANIM_DURATION);
                spec.path     = arg.substr(0, at);
            }
        } catch (...) {}
    }
    return spec;
}

static float secondsSince(const std::chrono::steady_clock::time_point& t) {
    return std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - t).count();
}

// A shader that binds `progress` is unambiguously written as an animation, so
// reaching the default because it forgot to say how long it wants to run is an
// oversight worth surfacing rather than silently papering over. Shaders that
// don't bind `progress` stay quiet — the duration barely matters for those.
// Deduped by mtime so it's one toast per edit, not one per frame.
static void warnMissingDuration(const std::string& path, time_t mtime) {
    if (auto it = g_mDurationWarnedMtimes.find(path); it != g_mDurationWarnedMtimes.end() && it->second == mtime)
        return;
    g_mDurationWarnedMtimes[path] = mtime;

    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "%.3g", DEFAULT_ANIM_DURATION);

    HyprlandAPI::addNotification(PHANDLE,
                                 "[HyprWindowShade] " + path + "\nuses `progress` but declares no `// @duration <sec>` — defaulting to " +
                                     fallback + "s.",
                                 CHyprColor(1.0f, 0.7f, 0.2f, 1.0f), 8000.0f);
}

// Duration precedence: an explicit `@<sec>` on the window rule wins, then the
// shader's own `// @duration`, then the built-in default. The shader is the
// natural home for this — a dissolve and a CRT collapse want different lengths,
// and the binding shouldn't have to restate what the effect already knows.
//
// Only ever called from the draw path, so a compile here has a live GL context.
static float resolveAnimDuration(const std::string& path, float ruleOverride) {
    // An explicit override is unambiguous — nothing to warn about.
    if (ruleOverride > 0.0f) return std::min(ruleOverride, MAX_ANIM_DURATION);

    const CompiledShader* cs = getOrCompileShader(path);
    if (cs && cs->animDuration > 0.0f) return std::min(cs->animDuration, MAX_ANIM_DURATION);

    // Broken shaders (cs == nullptr) already toast their compile error; don't
    // pile a second notification on top of it.
    if (cs && cs->progressLoc >= 0) warnMissingDuration(path, cs->sourceMtime);

    return DEFAULT_ANIM_DURATION;
}

// Open animation for a live window. Returns the shader to use and sets the
// progress/seed globals, or nullptr once the animation has run its course (at
// which point the window reverts to its normal shader).
static const std::string* resolveOpenAnim(const PHLWINDOW& pWindow) {
    if (g_mWindowOpenTimes.empty()) return nullptr;

    Desktop::View::CWindow* rawWin = pWindow.get();
    auto                    tIt    = g_mWindowOpenTimes.find(rawWin);
    if (tIt == g_mWindowOpenTimes.end()) return nullptr;

    // Resolved here rather than when the window opened: window.open and the
    // rule-application event have no guaranteed order, but by the time we're
    // drawing the window its rules are definitely in place.
    auto rIt = g_mWindowRuleShaders.find(rawWin);
    if (rIt == g_mWindowRuleShaders.end() || rIt->second.openAnim.empty()) {
        g_mWindowOpenTimes.erase(tIt);
        return nullptr;
    }

    const std::string& path     = rIt->second.openAnim;
    const float        duration = resolveAnimDuration(path, rIt->second.openAnimDuration);
    const float        elapsed  = secondsSince(tIt->second);

    if (elapsed >= duration) {
        g_mWindowOpenTimes.erase(tIt);
        return nullptr;
    }

    g_pCurrentAnimProgress = elapsed / duration;
    g_pCurrentAnimSeed     = animSeedFor(rawWin);
    return &path;
}

// Same idea for layer surfaces, but keyed by namespace rather than a rule tag —
// layer rules carry no tags in v0.56, so this follows the existing layer API.
static const std::string* resolveLayerOpenAnim(const PHLLS& pLS) {
    if (g_mLayerOpenTimes.empty()) return nullptr;

    Desktop::View::CLayerSurface* rawLS = pLS.get();
    auto                          tIt   = g_mLayerOpenTimes.find(rawLS);
    if (tIt == g_mLayerOpenTimes.end()) return nullptr;

    auto sIt = g_mLayerOpenAnims.find(pLS->m_namespace);
    if (sIt == g_mLayerOpenAnims.end() || sIt->second.path.empty()) {
        g_mLayerOpenTimes.erase(tIt);
        return nullptr;
    }

    const std::string& path     = sIt->second.path;
    const float        duration = resolveAnimDuration(path, sIt->second.duration);
    const float        elapsed  = secondsSince(tIt->second);

    if (elapsed >= duration) {
        g_mLayerOpenTimes.erase(tIt);
        return nullptr;
    }

    g_pCurrentAnimProgress = elapsed / duration;
    g_pCurrentAnimSeed     = animSeedFor(rawLS);
    return &path;
}

// Drops records whose fadeout no longer exists. The keys are raw pointers, so a
// stale entry could otherwise be matched by a future fadeout allocated at the
// same address. Cheap: the list is the set of windows closing right now.
static void pruneFadeoutAnims() {
    if (g_mFadeoutAnims.empty()) return;

    auto& state = Desktop::fadingOutState();
    if (!state) {
        g_mFadeoutAnims.clear();
        return;
    }

    const auto& live = state->fadeouts();
    for (auto it = g_mFadeoutAnims.begin(); it != g_mFadeoutAnims.end();) {
        bool found = false;
        for (const auto& f : live) {
            if (f.get() == it->first) { found = true; break; }
        }
        it = found ? std::next(it) : g_mFadeoutAnims.erase(it);
    }
}

// Close animation. The window is already gone; what's being drawn is the
// snapshot framebuffer Hyprland captured for the fadeout, so we identify it by
// exact texture identity rather than guessing from geometry.
static const std::string* resolveCloseAnim(const SP<Render::ITexture>& tex, PHLMONITOR& outMonitor) {
    if (g_mFadeoutAnims.empty() || !tex) return nullptr;

    auto& state = Desktop::fadingOutState();
    if (!state) return nullptr;

    for (const auto& f : state->fadeouts()) {
        if (!f) continue;
        const auto fb = f->framebuffer();
        if (!fb || fb->getTexture().get() != tex.get()) continue;

        auto it = g_mFadeoutAnims.find(f.get());
        if (it == g_mFadeoutAnims.end()) return nullptr;

        FadeoutAnim& anim = it->second;
        if (anim.duration < 0.0f) anim.duration = resolveAnimDuration(anim.path, -1.0f);

        const float elapsed = secondsSince(anim.start);
        g_pCurrentAnimProgress = anim.duration > 0.0f ? std::min(elapsed / anim.duration, 1.0f) : 1.0f;
        g_pCurrentAnimSeed     = anim.seed;
        outMonitor             = f->monitor().lock();
        return &anim.path;
    }
    return nullptr;
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
    g_pCurrentAnimProgress = -1.0f;
    g_pCurrentAnimSeed     = -1.0f;

    // A one-shot open/close animation overrides the window's normal shader for
    // as long as it runs. Close animations have no window and no surface — they
    // draw a snapshot texture — so they're matched on a separate branch.
    PHLMONITOR         animMonitor;
    const std::string* pathToUse = nullptr;

    if (pWindow)
        pathToUse = resolveOpenAnim(pWindow);
    else if (pLS)
        pathToUse = resolveLayerOpenAnim(pLS);
    else if (elem && !elem->m_data.surface && elem->m_data.tex)
        pathToUse = resolveCloseAnim(elem->m_data.tex, animMonitor);

    const bool animActive = pathToUse != nullptr;

    // Resolve the path once, then do a single lookup-or-compile on
    // g_mCompiledCShaders and stash the resulting pointer for hkUseShader to
    // consume — saves the second find that used to happen down there.
    if (!animActive)
        pathToUse = resolveShaderPath(pWindow, pLS);

    g_pCurrentCompiledShader = pathToUse && !pathToUse->empty()
                                   ? getOrCompileShader(*pathToUse)
                                   : nullptr;

    // Schedule continuous redraw if the resolved shader uses `time`, or while a
    // one-shot animation is mid-flight — an animation shader drives itself off
    // `progress` and may never bind `time`, so it needs frames either way.
    if (animActive || (g_pCurrentCompiledShader && g_pCurrentCompiledShader->usesTime)) {
        if (pWindow)
            g_pHyprRenderer->damageWindow(pWindow);
        else if (pLS) {
            if (auto mon = pLS->m_monitor.lock())
                mon->scheduleFrame();
        } else if (animMonitor)
            animMonitor->scheduleFrame();
    }

    ((TGLDrawTex)g_pGLDrawTexHook->m_original)(thisptr, element, damage);

    g_pCurrentRenderWindow.reset();
    g_pCurrentRenderLayer.reset();
    g_pCurrentCompiledShader = nullptr;
    g_pCurrentAnimProgress   = -1.0f;
    g_pCurrentAnimSeed       = -1.0f;
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
            // 1.0 outside an animation, so a shader written against `progress`
            // still renders its finished state if bound as a normal shader.
            glUniform1f(activeEntry->progressLoc, g_pCurrentAnimProgress >= 0.0f ? g_pCurrentAnimProgress : 1.0f);
        }
        if (activeEntry->seedLoc >= 0) {
            float s = g_pCurrentAnimSeed;
            if (s < 0.0f) {
                // Not an animation — derive from whatever we're drawing so the
                // seed stays stable for the life of the window or layer.
                if (contextWindow)
                    s = animSeedFor(contextWindow.get());
                else if (auto l = g_pCurrentRenderLayer.lock())
                    s = animSeedFor(l.get());
                else
                    s = 0.5f;
            }
            glUniform1f(activeEntry->seedLoc, s);
        }
    }

    return result;
}

// Records the close shader for a freshly created fadeout. Everything after this
// point keys off the IFadeout, because the window or layer is already gone.
static void tagFadeout(Desktop::IFadeout* key, const std::string& path, float duration, const void* seedSource) {
    FadeoutAnim anim;
    anim.path     = path;
    anim.start    = std::chrono::steady_clock::now();
    anim.duration = duration; // <0 -> resolved from the shader on first draw
    anim.seed     = animSeedFor(seedSource);
    g_mFadeoutAnims[key] = std::move(anim);
}

// Shared by both fadeout kinds: hold the snapshot on screen until the shader has
// had its declared duration, then let Hyprland drop it as usual.
static bool holdFadeoutOpen(Desktop::IFadeout* key, bool origDone) {
    if (g_mFadeoutAnims.empty()) return origDone;

    auto it = g_mFadeoutAnims.find(key);
    if (it == g_mFadeoutAnims.end()) return origDone;

    // Deliberately no getOrCompileShader here: `done` is called from fadeout
    // bookkeeping, which isn't guaranteed to run with a GL context current, and
    // compiling without one would fail (and poison the failure cache). The draw
    // path resolves the real duration; until it has, assume the default.
    const FadeoutAnim& anim = it->second;
    const float        dur  = anim.duration > 0.0f ? anim.duration : DEFAULT_ANIM_DURATION;

    if (secondsSince(anim.start) < std::min(dur, MAX_ANIM_DURATION)) {
        // Keep frames coming ourselves: once Hyprland's fade animation is over it
        // has no reason left to tick this monitor, and without a frame the
        // animation would freeze mid-way instead of playing out.
        if (auto mon = key->monitor().lock())
            mon->scheduleFrame();
        return false;
    }

    if (origDone) g_mFadeoutAnims.erase(it);
    return origDone;
}

// --- V0.56 HOOK: Desktop::CWindowFadeout::create ---
// The one place where a fadeout and the window it came from are both in scope.
// We tag the fadeout with that window's close shader; everything afterwards keys
// off the fadeout, because the window is already gone.
typedef Hyprutils::Memory::CSharedPointer<Desktop::CWindowFadeout> (*TFadeoutCreate)(PHLWINDOW, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer>, float);

Hyprutils::Memory::CSharedPointer<Desktop::CWindowFadeout>
hkFadeoutCreate(PHLWINDOW window, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer> snapshot, float sourceAlpha) {
    auto result = ((TFadeoutCreate)g_pFadeoutCreateHook->m_original)(window, snapshot, sourceAlpha);

    pruneFadeoutAnims();

    if (!result || !window) return result;

    auto it = g_mWindowRuleShaders.find(window.get());
    if (it == g_mWindowRuleShaders.end() || it->second.closeAnim.empty()) return result;

    // Derived-to-base conversion, not a reinterpret: IFadeout has a virtual base,
    // so the compiler has to apply the right offset for the key to match what
    // fadeouts() hands back.
    tagFadeout(result.get(), it->second.closeAnim, it->second.closeAnimDuration, window.get());

    return result;
}

// --- V0.56 HOOK: Desktop::CLayerFadeout::create ---
// Layer surfaces close the same way windows do — snapshot, then fade — so the
// only thing that differs is where the shader comes from: a namespace lookup
// rather than a window rule.
typedef Hyprutils::Memory::CSharedPointer<Desktop::CLayerFadeout> (*TLayerFadeoutCreate)(PHLLS, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer>, float);

Hyprutils::Memory::CSharedPointer<Desktop::CLayerFadeout>
hkLayerFadeoutCreate(PHLLS layer, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer> snapshot, float sourceAlpha) {
    auto result = ((TLayerFadeoutCreate)g_pLayerFadeoutCreateHook->m_original)(layer, snapshot, sourceAlpha);

    pruneFadeoutAnims();

    if (!result || !layer) return result;

    auto it = g_mLayerCloseAnims.find(layer->m_namespace);
    if (it == g_mLayerCloseAnims.end() || it->second.path.empty()) return result;

    tagFadeout(result.get(), it->second.path, it->second.duration, layer.get());

    return result;
}

// --- V0.56 HOOK: Desktop::CWindowFadeout::done ---
// Hyprland drops a fadeout as soon as its own fade-out animation finishes, which
// can be well before the close shader is done. Holding `done` false keeps the
// snapshot alive for exactly as long as the shader asked for.
typedef bool (*TFadeoutDone)(void* thisptr);

bool hkFadeoutDone(void* thisptr) {
    const bool origDone = ((TFadeoutDone)g_pFadeoutDoneHook->m_original)(thisptr);
    // Cast to the concrete type first so the compiler applies the correct
    // derived-to-base offset — IFadeout has a virtual base.
    return holdFadeoutOpen(static_cast<Desktop::CWindowFadeout*>(thisptr), origDone);
}

// --- V0.56 HOOK: Desktop::CLayerFadeout::done ---
bool hkLayerFadeoutDone(void* thisptr) {
    const bool origDone = ((TFadeoutDone)g_pLayerFadeoutDoneHook->m_original)(thisptr);
    return holdFadeoutOpen(static_cast<Desktop::CLayerFadeout*>(thisptr), origDone);
}

void applyShaderRulesSafe(PHLWINDOW pWindow) {
    if (!pWindow || !pWindow->m_ruleApplicator) return;
    Desktop::View::CWindow* rawWin = pWindow.get();

    WindowShaderState state;
    bool              hasRules = false;
    std::string       defaultOpenAnim;
    float             defaultOpenAnimDuration = -1.0f;
    std::string       defaultCloseAnim;
    float             defaultCloseAnimDuration = -1.0f;

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

        // Animation rules additionally accept an optional `@<seconds>` suffix to
        // override the duration the shader declares. rfind, so a path that
        // happens to contain '@' still works; and the suffix is only stripped if
        // it actually parsed as a positive number (clamped to MAX_ANIM_DURATION).
        const auto assignAnim = [&](std::string& dst, float& dur, size_t prefixLen) {
            std::string_view rest = sv.substr(prefixLen);
            dur                   = -1.0f;

            if (const size_t at = rest.rfind('@'); at != std::string_view::npos) {
                try {
                    const float d = std::stof(std::string(rest.substr(at + 1)));
                    // Clamped rather than rejected — see parseAnimSpec.
                    if (d > 0.0f) {
                        dur  = std::min(d, MAX_ANIM_DURATION);
                        rest = rest.substr(0, at);
                    }
                } catch (...) {}
            }

            dst.assign(rest);
            hasRules = true;
        };

        if      (sv.substr(0, 7)  == "shader:")            assign(state.fallback,   7);
        else if (sv.substr(0, 14) == "shader_active:")     assign(state.active,     14);
        else if (sv.substr(0, 16) == "shader_inactive:")   assign(state.inactive,   16);
        else if (sv.substr(0, 16) == "shader_floating:")   assign(state.floating,   16);
        else if (sv.substr(0, 13) == "shader_tiled:")      assign(state.tiled,      13);
        else if (sv.substr(0, 18) == "shader_fullscreen:") assign(state.fullscreen, 18);
        else if (sv.substr(0, 20) == "shader_open_default:")  assignAnim(defaultOpenAnim,  defaultOpenAnimDuration,  20);
        else if (sv.substr(0, 21) == "shader_close_default:") assignAnim(defaultCloseAnim, defaultCloseAnimDuration, 21);
        else if (sv.substr(0, 12) == "shader_open:")       assignAnim(state.openAnim,  state.openAnimDuration,  12);
        else if (sv.substr(0, 13) == "shader_close:")      assignAnim(state.closeAnim, state.closeAnimDuration, 13);
    }

    if (state.openAnim.empty() && !defaultOpenAnim.empty()) {
        state.openAnim = std::move(defaultOpenAnim);
        state.openAnimDuration = defaultOpenAnimDuration;
        hasRules = true;
    }
    if (state.closeAnim.empty() && !defaultCloseAnim.empty()) {
        state.closeAnim = std::move(defaultCloseAnim);
        state.closeAnimDuration = defaultCloseAnimDuration;
        hasRules = true;
    }

    if (hasRules) {
        g_mWindowRuleShaders[rawWin] = std::move(state);
        g_pHyprRenderer->damageWindow(pWindow);
    } else {
        g_mWindowRuleShaders.erase(rawWin);
    }
}
