#include "Globals.hpp"

// --- DEFINE EXTERN VARIABLES ---
HANDLE PHANDLE = nullptr;
std::vector<CHyprSignalListener> g_Listeners;
std::map<Desktop::View::CWindow*, std::string> g_mWindowManualShaders; 
std::map<Desktop::View::CWindow*, WindowShaderState> g_mWindowRuleShaders;
std::map<std::string, std::string> g_mLayerNamespaceShaderMap;
std::map<std::string, std::string> g_mWindowClassShaderMap;
std::map<std::string, Hyprutils::Memory::CSharedPointer<CShader>> g_mCompiledCShaders;
std::map<std::string, bool> g_mShaderUsesTime;
CFunctionHook* g_pGetSurfaceShaderHook = nullptr;
CFunctionHook* g_pUseShaderHook = nullptr;


APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // 2. Hook getSurfaceShader to perform the injection
    auto methodsVariant = HyprlandAPI::findFunctionsByName(PHANDLE, "getSurfaceShader");
    void* getVariantAddr = nullptr;
    for (auto& m : methodsVariant) {
        if (m.demangled.find("CHyprOpenGLImpl::getSurfaceShader") != std::string::npos) {
            getVariantAddr = m.address;
            break;
        }
    }
    if (getVariantAddr) {
        g_pGetSurfaceShaderHook = HyprlandAPI::createFunctionHook(PHANDLE, getVariantAddr, (void*)&hkGetSurfaceShader);
        g_pGetSurfaceShaderHook->hook();
    } else {
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] FATAL: getSurfaceShader not found!", CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 10000.0f);
    }

    // 3. Hook useShader to push our time & alpha uniforms
    auto methodsUse = HyprlandAPI::findFunctionsByName(PHANDLE, "useShader");
    void* useAddr = nullptr;
    for (auto& m : methodsUse) {
        if (m.demangled.find("CHyprOpenGLImpl::useShader") != std::string::npos) {
            useAddr = m.address;
            break;
        }
    }
    if (useAddr) {
        g_pUseShaderHook = HyprlandAPI::createFunctionHook(PHANDLE, useAddr, (void*)&hkUseShader);
        g_pUseShaderHook->hook();
    }

    // 4. Listen to native rules
    g_Listeners.push_back(Event::bus()->m_events.window.updateRules.listen([&](PHLWINDOW window) {
        try { applyShaderRulesSafe(window); } catch (...) {}
    }));

    // --- CRITICAL FIX 65 (AMENDED): FOCUS SHIFT DAMAGE ---
    // In v0.54.0+, the event bus was strongly typed. "activeWindow" is now "window.active"
    // and strict parameters must be passed to the listener lambda!
    g_Listeners.push_back(Event::bus()->m_events.window.active.listen([&](auto window, auto reason) {
        for (auto& w : g_pCompositor->m_windows) {
            if (w) {
                Desktop::View::CWindow* rawWin = w.get();
                if (g_mWindowRuleShaders.find(rawWin) != g_mWindowRuleShaders.end()) {
                    g_pHyprRenderer->damageWindow(w);
                }
            }
        }
    }));

    // --- CRITICAL FIX 66: FULLSCREEN SHIFT DAMAGE ---
    // Forces an immediate redraw when a window enters or exits fullscreen mode.
    g_Listeners.push_back(Event::bus()->m_events.window.fullscreen.listen([&](auto window) {
        if (window) {
            Desktop::View::CWindow* rawWin = window.get();
            // We redraw to either apply the fullscreen shader OR instantly strip the active shader
            g_pHyprRenderer->damageWindow(window);
        }
    }));

    // 5. Custom Dispatcher for Layer Surfaces
    HyprlandAPI::addDispatcherV2(PHANDLE, "layershader", [&](std::string args) -> SDispatchResult {
        size_t spacePos = args.find_first_of(" \t");
        if (spacePos != std::string::npos) {
            std::string ns = args.substr(0, spacePos);
            std::string path = args.substr(spacePos + 1);
            
            size_t start = path.find_first_not_of(" \t");
            if (start != std::string::npos) path = path.substr(start);
            while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();

            if (path == "clear" || path == "none") g_mLayerNamespaceShaderMap.erase(ns);
            else g_mLayerNamespaceShaderMap[ns] = path;
        }
        return SDispatchResult{};
    });

    // --- CRITICAL FIX 56: TOGGLE DISPATCHER (LAYERS) ---
    HyprlandAPI::addDispatcherV2(PHANDLE, "togglelayershader", [&](std::string args) -> SDispatchResult {
        size_t spacePos = args.find_first_of(" \t");
        if (spacePos != std::string::npos) {
            std::string ns = args.substr(0, spacePos);
            std::string path = args.substr(spacePos + 1);
            
            size_t start = path.find_first_not_of(" \t");
            if (start != std::string::npos) path = path.substr(start);
            while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();

            if (g_mLayerNamespaceShaderMap.find(ns) != g_mLayerNamespaceShaderMap.end()) g_mLayerNamespaceShaderMap.erase(ns);
            else g_mLayerNamespaceShaderMap[ns] = path;
        }
        return SDispatchResult{};
    });

    // --- CRITICAL FIX 57: ACTIVE WINDOW TOGGLE ---
    HyprlandAPI::addDispatcherV2(PHANDLE, "togglewindowshader", [&](std::string path) -> SDispatchResult {
        size_t start = path.find_first_not_of(" \t");
        if (start != std::string::npos) path = path.substr(start);
        while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();

        if (path.empty()) return SDispatchResult{};

        PHLWINDOW pWindow = nullptr;
        for (auto& w : g_pCompositor->m_windows) {
            if (g_pCompositor->isWindowActive(w)) {
                pWindow = w;
                break;
            }
        }

        if (pWindow) {
            Desktop::View::CWindow* rawWin = pWindow.get();
            if (path == "clear" || path == "none") g_mWindowManualShaders.erase(rawWin);
            else if (g_mWindowManualShaders.find(rawWin) != g_mWindowManualShaders.end()) g_mWindowManualShaders.erase(rawWin);
            else g_mWindowManualShaders[rawWin] = path;
            
            g_pHyprRenderer->damageWindow(pWindow);
        }
        return SDispatchResult{};
    });

    // --- CRITICAL FIX 58: APP CLASS DISPATCHERS ---
    HyprlandAPI::addDispatcherV2(PHANDLE, "classshader", [&](std::string args) -> SDispatchResult {
        size_t spacePos = args.find_first_of(" \t");
        if (spacePos != std::string::npos) {
            std::string cls = args.substr(0, spacePos);
            std::string path = args.substr(spacePos + 1);
            
            size_t start = path.find_first_not_of(" \t");
            if (start != std::string::npos) path = path.substr(start);
            while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();

            if (path == "clear" || path == "none") g_mWindowClassShaderMap.erase(cls);
            else g_mWindowClassShaderMap[cls] = path;

            for (auto& w : g_pCompositor->m_windows) {
                if (w && (w->m_initialClass == cls || w->m_class == cls)) g_pHyprRenderer->damageWindow(w);
            }
        }
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "toggleclassshader", [&](std::string args) -> SDispatchResult {
        size_t spacePos = args.find_first_of(" \t");
        if (spacePos != std::string::npos) {
            std::string cls = args.substr(0, spacePos);
            std::string path = args.substr(spacePos + 1);
            
            size_t start = path.find_first_not_of(" \t");
            if (start != std::string::npos) path = path.substr(start);
            while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();

            if (g_mWindowClassShaderMap.find(cls) != g_mWindowClassShaderMap.end()) g_mWindowClassShaderMap.erase(cls);
            else g_mWindowClassShaderMap[cls] = path;

            for (auto& w : g_pCompositor->m_windows) {
                if (w && (w->m_initialClass == cls || w->m_class == cls)) g_pHyprRenderer->damageWindow(w);
            }
        }
        return SDispatchResult{};
    });

    // --- CRITICAL FIX 64: RELOAD SHADERS DISPATCHER ---
    HyprlandAPI::addDispatcherV2(PHANDLE, "reloadshaders", [&](std::string args) -> SDispatchResult {
        g_mCompiledCShaders.clear();
        for (auto& w : g_pCompositor->m_windows) if (w) g_pHyprRenderer->damageWindow(w);
        for (auto& m : g_pCompositor->m_monitors) if (m) g_pCompositor->scheduleFrameForMonitor(m);
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] Shaders Reloaded from Disk!", CHyprColor(0.2f, 1.0f, 0.2f, 1.0f), 3000.0f);
        return SDispatchResult{};
    });

    return {"HyprWindowShade", "Native CShader Injection", "ManofJELLO", "1.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_Listeners.clear();
    
    // --- CRITICAL FIX 54: STRICT MEMORY UNLOADING ---
    g_mWindowManualShaders.clear();
    g_mWindowRuleShaders.clear();
    g_mLayerNamespaceShaderMap.clear();
    g_mWindowClassShaderMap.clear(); 
    g_mShaderUsesTime.clear();

    // --- CRITICAL FIX 60: THE UNLOAD CRASH (GL CONTEXT) ---
    auto leak = new std::map<std::string, Hyprutils::Memory::CSharedPointer<CShader>>(std::move(g_mCompiledCShaders));
    (void)leak; // Suppress unused variable warning

    if (g_pGetSurfaceShaderHook) {
        // --- CRITICAL FIX 61: DOUBLE UNHOOK CRASH ---
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pGetSurfaceShaderHook);
    }
    if (g_pUseShaderHook) HyprlandAPI::removeFunctionHook(PHANDLE, g_pUseShaderHook);

    // --- CRITICAL FIX 55: DISPATCHER CLEANUP ---
    HyprlandAPI::removeDispatcher(PHANDLE, "layershader");
    HyprlandAPI::removeDispatcher(PHANDLE, "togglelayershader");
    HyprlandAPI::removeDispatcher(PHANDLE, "togglewindowshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "classshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "toggleclassshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "reloadshaders"); 
}