#include "Globals.hpp"

// --- THREAD-LOCAL RENDER CONTEXT ---
thread_local Desktop::View::CWindow*       g_pCurrentRenderWindow = nullptr;
thread_local Desktop::View::CLayerSurface* g_pCurrentRenderLayer  = nullptr;

// --- V0.55 HOOK: CGLElementRenderer::draw(CTexPassElement, CRegion) ---
// This is the concrete, exported method that renders every textured surface.
// We resolve the window from the surface resource via g_pCompositor->getWindowFromSurface.
typedef void (*TGLDrawTex)(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage);

void hkGLDrawTex(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage) {
    auto elem = element.lock();

    PHLWINDOW pWindow = nullptr;
    PHLLS     pLS     = nullptr;

    if (elem && elem->m_data.surface) {
        // Try to resolve to a window first
        pWindow = g_pCompositor->getWindowFromSurface(elem->m_data.surface);
        // Layer surface is directly available
        pLS = elem->m_data.currentLS.lock();
    }

    g_pCurrentRenderWindow = pWindow ? pWindow.get() : nullptr;
    g_pCurrentRenderLayer  = pLS     ? pLS.get()     : nullptr;

    // Schedule continuous redraw if a time-based shader is in play
    std::string pathToUse = "";
    if (pWindow) {
        Desktop::View::CWindow* rawWin = pWindow.get();
        if (g_mWindowManualShaders.count(rawWin))
            pathToUse = g_mWindowManualShaders[rawWin];
        else {
            std::string initClass    = rawWin->m_initialClass;
            std::string currentClass = rawWin->m_class;
            if      (g_mWindowClassShaderMap.count(initClass))    pathToUse = g_mWindowClassShaderMap[initClass];
            else if (g_mWindowClassShaderMap.count(currentClass)) pathToUse = g_mWindowClassShaderMap[currentClass];
        }
        if (!pathToUse.empty() && g_mShaderUsesTime[pathToUse])
            g_pHyprRenderer->damageWindow(pWindow);
    } else if (pLS) {
        std::string ns = pLS->m_namespace;
        if (g_mLayerNamespaceShaderMap.count(ns)) {
            pathToUse = g_mLayerNamespaceShaderMap[ns];
            if (!pathToUse.empty() && g_mShaderUsesTime[pathToUse]) {
                // Schedule monitor frame — we don't have direct access here
                for (auto& m : g_pCompositor->m_monitors)
                    if (m) g_pCompositor->scheduleFrameForMonitor(m);
            }
        }
    }

    ((TGLDrawTex)g_pGLDrawTexHook->m_original)(thisptr, element, damage);

    g_pCurrentRenderWindow = nullptr;
    g_pCurrentRenderLayer  = nullptr;
}

// --- V0.55 HOOK: useShader ---
typedef Hyprutils::Memory::CWeakPointer<CShader> (*TUseShader)(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);

Hyprutils::Memory::CWeakPointer<CShader> hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog) {

    std::string pathToUse     = "";
    PHLWINDOW   contextWindow = nullptr;

    if (g_pCurrentRenderWindow) {
        Desktop::View::CWindow* rawWin = g_pCurrentRenderWindow;

        for (auto& w : g_pCompositor->m_windows)
            if (w && w.get() == rawWin) { contextWindow = w; break; }

        if (contextWindow) {
            if (g_mWindowManualShaders.count(rawWin))
                pathToUse = g_mWindowManualShaders[rawWin];
            else if (rawWin->isFullscreen()) {
                if (g_mWindowRuleShaders.count(rawWin) && !g_mWindowRuleShaders[rawWin].fullscreen.empty())
                    pathToUse = g_mWindowRuleShaders[rawWin].fullscreen;
            } else {
                if (g_mWindowRuleShaders.count(rawWin)) {
                    auto& state      = g_mWindowRuleShaders[rawWin];
                    bool  isActive   = g_pCompositor->isWindowActive(contextWindow);
                    bool  isFloating = rawWin->m_isFloating;
                    if      (isFloating  && !state.floating.empty())  pathToUse = state.floating;
                    else if (!isFloating && !state.tiled.empty())     pathToUse = state.tiled;
                    else if (isActive    && !state.active.empty())    pathToUse = state.active;
                    else if (!isActive   && !state.inactive.empty())  pathToUse = state.inactive;
                    else if (!state.fallback.empty())                 pathToUse = state.fallback;
                }
                if (pathToUse.empty()) {
                    std::string initClass    = rawWin->m_initialClass;
                    std::string currentClass = rawWin->m_class;
                    if      (g_mWindowClassShaderMap.count(initClass))    pathToUse = g_mWindowClassShaderMap[initClass];
                    else if (g_mWindowClassShaderMap.count(currentClass)) pathToUse = g_mWindowClassShaderMap[currentClass];
                }
            }
        }
    } else if (g_pCurrentRenderLayer) {
        std::string ns = g_pCurrentRenderLayer->m_namespace;
        if (g_mLayerNamespaceShaderMap.count(ns))
            pathToUse = g_mLayerNamespaceShaderMap[ns];
    }

    // Swap shader if we have a custom one
    if (!pathToUse.empty()) {
        auto customShader = getOrCompileShader(pathToUse);
        if (customShader && customShader->program() != 0)
            prog = customShader;
    }

    auto result = ((TUseShader)g_pUseShaderHook->m_original)(thisptr, prog);

    // Inject uniforms
    auto shnd = prog.lock();
    if (shnd && shnd->program() != 0) {
        GLint timeLoc = glGetUniformLocation(shnd->program(), "time");
        if (timeLoc >= 0) {
            auto  now = std::chrono::steady_clock::now();
            float t   = std::chrono::duration_cast<std::chrono::duration<float>>(now.time_since_epoch()).count();
            glUniform1f(timeLoc, t);
        }

        GLint alphaLoc = glGetUniformLocation(shnd->program(), "plugin_alpha");
        if (alphaLoc >= 0) {
            float currentAlpha = 1.0f;
            if (contextWindow)
                currentAlpha = contextWindow->alphaTotal();
            glUniform1f(alphaLoc, currentAlpha);
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