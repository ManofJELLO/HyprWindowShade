#include "Globals.hpp"

// --- DARKWINDOW FIX 3 (AMENDED): Hooking getSurfaceShader ---
typedef Hyprutils::Memory::CWeakPointer<CShader> (*TGetSurfaceShader)(CHyprOpenGLImpl* thisptr, uint8_t features);

Hyprutils::Memory::CWeakPointer<CShader> hkGetSurfaceShader(CHyprOpenGLImpl* thisptr, uint8_t features) {
    
    // --- CRITICAL FIX 52: NATIVE RENDER DATA TRACKING ---
    auto window = thisptr->m_renderData.currentWindow.lock();
    auto layer  = thisptr->m_renderData.currentLS.lock();
    
    if (window) {
        Desktop::View::CWindow* rawWin = window.get();
        std::string pathToUse = "";
        
        // 1. Manual Keybind Overrides take absolute priority
        if (g_mWindowManualShaders.find(rawWin) != g_mWindowManualShaders.end()) {
            pathToUse = g_mWindowManualShaders[rawWin];
        } 
        // 2. Fullscreen Block (Overrides all other rules by default!)
        // --- THE FIX: METHOD CALL (FULLSCREEN) ---
        // In v0.54.2, isFullscreen is a function, not a simple boolean variable!
        else if (rawWin->isFullscreen()) {
            if (g_mWindowRuleShaders.find(rawWin) != g_mWindowRuleShaders.end() && 
                !g_mWindowRuleShaders[rawWin].fullscreen.empty()) {
                pathToUse = g_mWindowRuleShaders[rawWin].fullscreen;
            }
            // If it's fullscreen and no specific fullscreen rule exists, pathToUse remains empty.
        }
        // 3. Dynamic Rules (Floating vs Active vs Inactive) evaluated dynamically!
        else {
            if (g_mWindowRuleShaders.find(rawWin) != g_mWindowRuleShaders.end()) {
                auto& state = g_mWindowRuleShaders[rawWin];
                bool isActive = g_pCompositor->isWindowActive(window);
                bool isFloating = rawWin->m_isFloating; 
                
                // Priority tree: Floating/Tiled overrides standard Active/Inactive Focus
                if (isFloating && !state.floating.empty()) pathToUse = state.floating;
                else if (!isFloating && !state.tiled.empty()) pathToUse = state.tiled;
                else if (isActive && !state.active.empty()) pathToUse = state.active;
                else if (!isActive && !state.inactive.empty()) pathToUse = state.inactive;
                else if (!state.fallback.empty()) pathToUse = state.fallback;
            }
            
            // 4. App Class map fallback
            if (pathToUse.empty()) {
                // --- CRITICAL FIX 59: RENAME FIXES ---
                std::string initClass = rawWin->m_initialClass;
                std::string currentClass = rawWin->m_class;
                if (g_mWindowClassShaderMap.find(initClass) != g_mWindowClassShaderMap.end()) {
                    pathToUse = g_mWindowClassShaderMap[initClass];
                } else if (g_mWindowClassShaderMap.find(currentClass) != g_mWindowClassShaderMap.end()) {
                    pathToUse = g_mWindowClassShaderMap[currentClass];
                }
            }
        }

        if (!pathToUse.empty()) {
            auto customShader = getOrCompileShader(thisptr, pathToUse);
            if (customShader && customShader->program() != 0) {
                if (g_mShaderUsesTime[pathToUse]) g_pHyprRenderer->damageWindow(window);
                return customShader;
            }
        }
    } 
    else if (layer) {
        std::string ns = layer->m_namespace;
        if (g_mLayerNamespaceShaderMap.find(ns) != g_mLayerNamespaceShaderMap.end()) {
            std::string path = g_mLayerNamespaceShaderMap[ns];
            auto customShader = getOrCompileShader(thisptr, path);
            if (customShader && customShader->program() != 0) {
                if (g_mShaderUsesTime[path]) {
                    auto pMon = thisptr->m_renderData.pMonitor.lock();
                    if (pMon) g_pCompositor->scheduleFrameForMonitor(pMon);
                }
                return customShader;
            }
        }
    }
    
    // Pass through to the original Hyprland shader builder for non-tagged elements
    return ((TGetSurfaceShader)g_pGetSurfaceShaderHook->m_original)(thisptr, features);
}

// --- CRITICAL FIX 63: TIME & AUTO-ALPHA UNIFORM INJECTION ---
typedef Hyprutils::Memory::CWeakPointer<CShader> (*TUseShader)(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);

Hyprutils::Memory::CWeakPointer<CShader> hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog) {
    auto result = ((TUseShader)g_pUseShaderHook->m_original)(thisptr, prog);

    auto shnd = prog.lock();
    if (shnd && shnd->program() != 0) {
        GLint timeLoc = glGetUniformLocation(shnd->program(), "time");
        if (timeLoc >= 0) {
            auto now = std::chrono::steady_clock::now();
            float t = std::chrono::duration_cast<std::chrono::duration<float>>(now.time_since_epoch()).count();
            glUniform1f(timeLoc, t);
        }

        GLint alphaLoc = glGetUniformLocation(shnd->program(), "plugin_alpha");
        if (alphaLoc >= 0) {
            float currentAlpha = 1.0f;
            auto window = thisptr->m_renderData.currentWindow.lock();
            auto layer = thisptr->m_renderData.currentLS.lock();
            
            if (window) {
                currentAlpha = window->m_alpha->value() * window->m_activeInactiveAlpha->value();
            } else if (layer) {
                currentAlpha = layer->m_alpha->value();
            }
            glUniform1f(alphaLoc, currentAlpha);
        }
    }
    return result;
}

void applyShaderRulesSafe(PHLWINDOW pWindow) {
    if (!pWindow || !pWindow->m_ruleApplicator) return;
    Desktop::View::CWindow* rawWin = pWindow.get();

    WindowShaderState state;
    bool hasRules = false;

    const auto& tagsSet = pWindow->m_ruleApplicator->m_tagKeeper.getTags();
    for (const auto& tag : tagsSet) {
        std::string cleanTag = tag;
        while (!cleanTag.empty() && (cleanTag.back() == '*' || cleanTag.back() == ' ')) cleanTag.pop_back();

        if (cleanTag.find("shader:") == 0) {
            state.fallback = cleanTag.substr(7);
            hasRules = true;
        } else if (cleanTag.find("shader_active:") == 0) {
            state.active = cleanTag.substr(14);
            hasRules = true;
        } else if (cleanTag.find("shader_inactive:") == 0) {
            state.inactive = cleanTag.substr(16);
            hasRules = true;
        } else if (cleanTag.find("shader_floating:") == 0) {
            state.floating = cleanTag.substr(16);
            hasRules = true;
        } else if (cleanTag.find("shader_tiled:") == 0) {
            state.tiled = cleanTag.substr(13);
            hasRules = true;
        } else if (cleanTag.find("shader_fullscreen:") == 0) {
            state.fullscreen = cleanTag.substr(18); // NEW: Fullscreen rule parser
            hasRules = true;
        }
    }

    if (hasRules) {
        g_mWindowRuleShaders[rawWin] = state;
        // --- DARKWINDOW FIX 2: Force Window Redraw ---
        g_pHyprRenderer->damageWindow(pWindow); 
    } else {
        g_mWindowRuleShaders.erase(rawWin);
    }
}