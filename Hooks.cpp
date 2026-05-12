#include "Globals.hpp"

// --- THREAD-LOCAL RENDER CONTEXT ---
thread_local PHLWINDOWREF                  g_pCurrentRenderWindow;
thread_local Desktop::View::CLayerSurface* g_pCurrentRenderLayer  = nullptr;

// --- V0.55 HOOK: CGLElementRenderer::draw(CTexPassElement, CRegion) ---
// This is the concrete, exported method that renders every textured surface.
// We resolve the window from the surface resource via g_pCompositor->getWindowFromSurface.
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
    g_pCurrentRenderLayer  = pLS ? pLS.get() : nullptr;

    // Schedule continuous redraw if a time-based shader is in play.
    const std::string* pathToUse = nullptr;
    if (pWindow) {
        Desktop::View::CWindow* rawWin = pWindow.get();
        auto manualIt = g_mWindowManualShaders.find(rawWin);
        if (manualIt != g_mWindowManualShaders.end()) {
            pathToUse = &manualIt->second;
        } else {
            const auto& initClass    = rawWin->m_initialClass;
            const auto& currentClass = rawWin->m_class;
            auto classIt = g_mWindowClassShaderMap.find(initClass);
            if (classIt == g_mWindowClassShaderMap.end()) classIt = g_mWindowClassShaderMap.find(currentClass);
            if (classIt != g_mWindowClassShaderMap.end()) pathToUse = &classIt->second;
        }
        if (pathToUse) {
            auto sit = g_mCompiledCShaders.find(*pathToUse);
            if (sit != g_mCompiledCShaders.end() && sit->second.usesTime)
                g_pHyprRenderer->damageWindow(pWindow);
        }
    } else if (pLS) {
        const auto& ns = pLS->m_namespace;
        auto nsIt = g_mLayerNamespaceShaderMap.find(ns);
        if (nsIt != g_mLayerNamespaceShaderMap.end()) {
            auto sit = g_mCompiledCShaders.find(nsIt->second);
            if (sit != g_mCompiledCShaders.end() && sit->second.usesTime) {
                // Schedule the layer's own monitor only, not every monitor.
                if (auto mon = pLS->m_monitor.lock())
                    g_pCompositor->scheduleFrameForMonitor(mon);
            }
        }
    }

    ((TGLDrawTex)g_pGLDrawTexHook->m_original)(thisptr, element, damage);

    g_pCurrentRenderWindow.reset();
    g_pCurrentRenderLayer  = nullptr;
}

// --- V0.55 HOOK: useShader ---
typedef Hyprutils::Memory::CWeakPointer<CShader> (*TUseShader)(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);

Hyprutils::Memory::CWeakPointer<CShader> hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog) {

    const std::string* pathToUse     = nullptr;
    PHLWINDOW          contextWindow = g_pCurrentRenderWindow.lock();

    if (contextWindow) {
        Desktop::View::CWindow* rawWin = contextWindow.get();

        auto manualIt = g_mWindowManualShaders.find(rawWin);
        if (manualIt != g_mWindowManualShaders.end()) {
            pathToUse = &manualIt->second;
        } else if (rawWin->isFullscreen()) {
            auto ruleIt = g_mWindowRuleShaders.find(rawWin);
            if (ruleIt != g_mWindowRuleShaders.end() && !ruleIt->second.fullscreen.empty())
                pathToUse = &ruleIt->second.fullscreen;
        } else {
            auto ruleIt = g_mWindowRuleShaders.find(rawWin);
            if (ruleIt != g_mWindowRuleShaders.end()) {
                const auto& state      = ruleIt->second;
                const bool  isActive   = g_pCompositor->isWindowActive(contextWindow);
                const bool  isFloating = rawWin->m_isFloating;
                if      (isFloating  && !state.floating.empty())  pathToUse = &state.floating;
                else if (!isFloating && !state.tiled.empty())     pathToUse = &state.tiled;
                else if (isActive    && !state.active.empty())    pathToUse = &state.active;
                else if (!isActive   && !state.inactive.empty())  pathToUse = &state.inactive;
                else if (!state.fallback.empty())                 pathToUse = &state.fallback;
            }
            if (!pathToUse) {
                const auto& initClass    = rawWin->m_initialClass;
                const auto& currentClass = rawWin->m_class;
                auto classIt = g_mWindowClassShaderMap.find(initClass);
                if (classIt == g_mWindowClassShaderMap.end()) classIt = g_mWindowClassShaderMap.find(currentClass);
                if (classIt != g_mWindowClassShaderMap.end()) pathToUse = &classIt->second;
            }
        }
    } else if (g_pCurrentRenderLayer) {
        const auto& ns = g_pCurrentRenderLayer->m_namespace;
        auto nsIt = g_mLayerNamespaceShaderMap.find(ns);
        if (nsIt != g_mLayerNamespaceShaderMap.end())
            pathToUse = &nsIt->second;
    }

    // Swap shader if we have a custom one
    CompiledShader* activeEntry = nullptr;
    if (pathToUse && !pathToUse->empty()) {
        activeEntry = getOrCompileShader(*pathToUse);
        if (activeEntry && activeEntry->shader && activeEntry->shader->program() != 0)
            prog = activeEntry->shader;
        else
            activeEntry = nullptr;
    }

    auto result = ((TUseShader)g_pUseShaderHook->m_original)(thisptr, prog);

    // Inject uniforms using cached locations (no per-frame glGetUniformLocation).
    if (activeEntry) {
        if (activeEntry->timeLoc >= 0) {
            auto  now = std::chrono::steady_clock::now();
            float t   = std::chrono::duration_cast<std::chrono::duration<float>>(now.time_since_epoch()).count();
            glUniform1f(activeEntry->timeLoc, t);
        }
        if (activeEntry->alphaLoc >= 0) {
            const float currentAlpha = contextWindow ? contextWindow->alphaTotal() : 1.0f;
            glUniform1f(activeEntry->alphaLoc, currentAlpha);
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
        std::string cleanTag = tag;
        while (!cleanTag.empty() && (cleanTag.back() == '*' || cleanTag.back() == ' ')) cleanTag.pop_back();

        if      (cleanTag.find("shader:")            == 0) { state.fallback   = cleanTag.substr(7);  hasRules = true; }
        else if (cleanTag.find("shader_active:")     == 0) { state.active     = cleanTag.substr(14); hasRules = true; }
        else if (cleanTag.find("shader_inactive:")   == 0) { state.inactive   = cleanTag.substr(16); hasRules = true; }
        else if (cleanTag.find("shader_floating:")   == 0) { state.floating   = cleanTag.substr(16); hasRules = true; }
        else if (cleanTag.find("shader_tiled:")      == 0) { state.tiled      = cleanTag.substr(13); hasRules = true; }
        else if (cleanTag.find("shader_fullscreen:") == 0) { state.fullscreen = cleanTag.substr(18); hasRules = true; }
    }

    if (hasRules) {
        g_mWindowRuleShaders[rawWin] = state;
        g_pHyprRenderer->damageWindow(pWindow);
    } else {
        g_mWindowRuleShaders.erase(rawWin);
    }
}
