#include "Globals.hpp"
#include <string_view>

// --- THREAD-LOCAL RENDER CONTEXT ---
thread_local PHLWINDOWREF        g_pCurrentRenderWindow;
thread_local PHLLSREF            g_pCurrentRenderLayer;
thread_local const std::string*  g_pCurrentShaderPath = nullptr;

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

        if (rawWin->isFullscreen()) {
            auto it = g_mWindowRuleShaders.find(rawWin);
            if (it != g_mWindowRuleShaders.end() && !it->second.fullscreen.empty())
                return &it->second.fullscreen;
        } else {
            auto it = g_mWindowRuleShaders.find(rawWin);
            if (it != g_mWindowRuleShaders.end()) {
                const auto& state      = it->second;
                const bool  isActive   = g_pCompositor->isWindowActive(pWindow);
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

// --- V0.55 HOOK: CGLElementRenderer::draw(CTexPassElement, CRegion) ---
typedef void (*TGLDrawTex)(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage);

void hkGLDrawTex(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage) {
    auto elem = element.lock();

    PHLWINDOW pWindow;
    PHLLS     pLS;

    if (elem && elem->m_data.surface) {
        pWindow = g_pCompositor->getWindowFromSurface(elem->m_data.surface);
        pLS     = elem->m_data.currentLS.lock();
    }

    g_pCurrentRenderWindow = pWindow;
    g_pCurrentRenderLayer  = pLS;

    // Resolve the path once and stash it for hkUseShader.
    const std::string* pathToUse = resolveShaderPath(pWindow, pLS);
    g_pCurrentShaderPath = pathToUse;

    // Schedule continuous redraw if the resolved shader uses `time`. usesTime
    // is set only after a successful compile, so the entry must already exist.
    if (pathToUse) {
        auto sit = g_mCompiledCShaders.find(*pathToUse);
        if (sit != g_mCompiledCShaders.end() && sit->second.usesTime) {
            if (pWindow)
                g_pHyprRenderer->damageWindow(pWindow);
            else if (pLS) {
                if (auto mon = pLS->m_monitor.lock())
                    g_pCompositor->scheduleFrameForMonitor(mon);
            }
        }
    }

    ((TGLDrawTex)g_pGLDrawTexHook->m_original)(thisptr, element, damage);

    g_pCurrentRenderWindow.reset();
    g_pCurrentRenderLayer.reset();
    g_pCurrentShaderPath = nullptr;
}

// --- V0.55 HOOK: useShader ---
typedef Hyprutils::Memory::CWeakPointer<CShader> (*TUseShader)(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);

Hyprutils::Memory::CWeakPointer<CShader> hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog) {
    PHLWINDOW contextWindow = g_pCurrentRenderWindow.lock();

    CompiledShader* activeEntry = nullptr;
    if (g_pCurrentShaderPath && !g_pCurrentShaderPath->empty()) {
        activeEntry = getOrCompileShader(*g_pCurrentShaderPath);
        if (activeEntry && activeEntry->shader)
            prog = activeEntry->shader;
        else
            activeEntry = nullptr;
    }

    auto result = ((TUseShader)g_pUseShaderHook->m_original)(thisptr, prog);

    // Inject uniforms using cached locations (no per-frame glGetUniformLocation).
    if (activeEntry) {
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
            Vector2D sz(0, 0);
            if (contextWindow) sz = contextWindow->m_size;
            glUniform2f(activeEntry->surfaceSizeLoc, (float)sz.x, (float)sz.y);
        }
        if (activeEntry->mouseLoc >= 0 && g_pPointerManager) {
            const Vector2D p = g_pPointerManager->position();
            glUniform2f(activeEntry->mouseLoc, (float)p.x, (float)p.y);
        }
        if (activeEntry->isActiveLoc >= 0) {
            const float v = (contextWindow && g_pCompositor->isWindowActive(contextWindow)) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isActiveLoc, v);
        }
        if (activeEntry->isFloatingLoc >= 0) {
            const float v = (contextWindow && contextWindow->m_isFloating) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isFloatingLoc, v);
        }
        if (activeEntry->isFullscreenLoc >= 0) {
            const float v = (contextWindow && contextWindow->isFullscreen()) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isFullscreenLoc, v);
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
    }

    if (hasRules) {
        g_mWindowRuleShaders[rawWin] = std::move(state);
        g_pHyprRenderer->damageWindow(pWindow);
    } else {
        g_mWindowRuleShaders.erase(rawWin);
    }
}
