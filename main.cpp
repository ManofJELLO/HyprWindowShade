#include "Globals.hpp"

// --- DEFINE EXTERN VARIABLES ---
HANDLE                                                PHANDLE = nullptr;
std::vector<CHyprSignalListener>                      g_Listeners;
std::map<Desktop::View::CWindow*, std::string>        g_mWindowManualShaders;
std::map<Desktop::View::CWindow*, WindowShaderState>  g_mWindowRuleShaders;
std::map<std::string, std::string>                    g_mLayerNamespaceShaderMap;
std::map<std::string, std::string>                    g_mWindowClassShaderMap;
std::map<std::string, CompiledShader>                 g_mCompiledCShaders;
CFunctionHook*                                        g_pGLDrawTexHook  = nullptr;
CFunctionHook*                                        g_pUseShaderHook  = nullptr;

// --- HELPERS ---
static inline void trimInPlace(std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) { s.clear(); return; }
    s.erase(0, start);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
}

// Splits "<key> <value...>" into trimmed (key, value). Returns false if no separator.
static bool splitTwo(const std::string& args, std::string& key, std::string& value) {
    size_t spacePos = args.find_first_of(" \t");
    if (spacePos == std::string::npos) return false;
    key   = args.substr(0, spacePos);
    value = args.substr(spacePos + 1);
    trimInPlace(value);
    return true;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // --- V0.55 HOOK 1: CGLElementRenderer::draw(CTexPassElement, CRegion) ---
    // The concrete, exported method that draws every textured surface element.
    auto methodsDraw = HyprlandAPI::findFunctionsByName(PHANDLE, "draw");
    void* drawAddr = nullptr;
    for (auto& m : methodsDraw) {
        if (m.demangled.find("CGLElementRenderer::draw")  != std::string::npos &&
            m.demangled.find("CTexPassElement")           != std::string::npos) {
            drawAddr = m.address;
            break;
        }
    }
    if (drawAddr) {
        g_pGLDrawTexHook = HyprlandAPI::createFunctionHook(PHANDLE, drawAddr, (void*)&hkGLDrawTex);
        g_pGLDrawTexHook->hook();
    } else {
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] FATAL: CGLElementRenderer::draw(CTexPassElement) not found!", CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 10000.0f);
    }

    // --- V0.55 HOOK 2: useShader ---
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
    } else {
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] FATAL: useShader not found!", CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 10000.0f);
    }

    // --- LISTENERS ---
    g_Listeners.push_back(Event::bus()->m_events.window.updateRules.listen([&](PHLWINDOW window) {
        try { applyShaderRulesSafe(window); } catch (...) {}
    }));

    static PHLWINDOWREF s_lastActive;
    g_Listeners.push_back(Event::bus()->m_events.window.active.listen([&](auto window, auto reason) {
        // Only the previously- and currently-active windows can change appearance.
        if (auto prev = s_lastActive.lock(); prev && g_mWindowRuleShaders.find(prev.get()) != g_mWindowRuleShaders.end())
            g_pHyprRenderer->damageWindow(prev);
        if (window && g_mWindowRuleShaders.find(window.get()) != g_mWindowRuleShaders.end())
            g_pHyprRenderer->damageWindow(window);
        s_lastActive = window;
    }));

    g_Listeners.push_back(Event::bus()->m_events.window.fullscreen.listen([&](auto window) {
        if (window) g_pHyprRenderer->damageWindow(window);
    }));

    // Drop entries keyed by raw CWindow* when the window is destroyed so they
    // can't accidentally match a future window at the same address.
    g_Listeners.push_back(Event::bus()->m_events.window.destroy.listen([&](PHLWINDOW window) {
        if (!window) return;
        Desktop::View::CWindow* rawWin = window.get();
        g_mWindowManualShaders.erase(rawWin);
        g_mWindowRuleShaders.erase(rawWin);
    }));

    // --- DISPATCHERS ---

    HyprlandAPI::addDispatcherV2(PHANDLE, "layershader", [&](std::string args) -> SDispatchResult {
        std::string ns, path;
        if (!splitTwo(args, ns, path)) return SDispatchResult{};
        if (path == "clear" || path == "none") g_mLayerNamespaceShaderMap.erase(ns);
        else g_mLayerNamespaceShaderMap[ns] = path;
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "togglelayershader", [&](std::string args) -> SDispatchResult {
        std::string ns, path;
        if (!splitTwo(args, ns, path)) return SDispatchResult{};
        if (g_mLayerNamespaceShaderMap.find(ns) != g_mLayerNamespaceShaderMap.end())
            g_mLayerNamespaceShaderMap.erase(ns);
        else
            g_mLayerNamespaceShaderMap[ns] = path;
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "togglewindowshader", [&](std::string path) -> SDispatchResult {
        trimInPlace(path);
        if (path.empty()) return SDispatchResult{};

        PHLWINDOW pWindow;
        for (auto& w : g_pCompositor->m_windows)
            if (g_pCompositor->isWindowActive(w)) { pWindow = w; break; }
        if (!pWindow) return SDispatchResult{};

        Desktop::View::CWindow* rawWin = pWindow.get();
        if (path == "clear" || path == "none" || g_mWindowManualShaders.find(rawWin) != g_mWindowManualShaders.end())
            g_mWindowManualShaders.erase(rawWin);
        else
            g_mWindowManualShaders[rawWin] = path;
        g_pHyprRenderer->damageWindow(pWindow);
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "classshader", [&](std::string args) -> SDispatchResult {
        std::string cls, path;
        if (!splitTwo(args, cls, path)) return SDispatchResult{};

        if (path == "clear" || path == "none") g_mWindowClassShaderMap.erase(cls);
        else g_mWindowClassShaderMap[cls] = path;

        for (auto& w : g_pCompositor->m_windows)
            if (w && (w->m_initialClass == cls || w->m_class == cls))
                g_pHyprRenderer->damageWindow(w);
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "toggleclassshader", [&](std::string args) -> SDispatchResult {
        std::string cls, path;
        if (!splitTwo(args, cls, path)) return SDispatchResult{};

        if (g_mWindowClassShaderMap.find(cls) != g_mWindowClassShaderMap.end()) g_mWindowClassShaderMap.erase(cls);
        else g_mWindowClassShaderMap[cls] = path;

        for (auto& w : g_pCompositor->m_windows)
            if (w && (w->m_initialClass == cls || w->m_class == cls))
                g_pHyprRenderer->damageWindow(w);
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "reloadshaders", [&](std::string args) -> SDispatchResult {
        g_mCompiledCShaders.clear();
        for (auto& w : g_pCompositor->m_windows) if (w) g_pHyprRenderer->damageWindow(w);
        for (auto& m : g_pCompositor->m_monitors) if (m) g_pCompositor->scheduleFrameForMonitor(m);
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] Shaders Reloaded from Disk!", CHyprColor(0.2f, 1.0f, 0.2f, 1.0f), 3000.0f);
        return SDispatchResult{};
    });

    return {"HyprWindowShade", "Native CShader Injection (v0.55)", "ManofJELLO", "1.3"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_Listeners.clear();

    g_mWindowManualShaders.clear();
    g_mWindowRuleShaders.clear();
    g_mLayerNamespaceShaderMap.clear();
    g_mWindowClassShaderMap.clear();

    // Intentionally leak compiled shaders: their CShader destructors call into
    // GL state owned by Hyprland, which may already be torn down at this point.
    // Moving the map into a heap allocation suppresses dtors at plugin unload.
    auto leak = new std::map<std::string, CompiledShader>(std::move(g_mCompiledCShaders));
    (void)leak;

    if (g_pGLDrawTexHook)  HyprlandAPI::removeFunctionHook(PHANDLE, g_pGLDrawTexHook);
    if (g_pUseShaderHook)  HyprlandAPI::removeFunctionHook(PHANDLE, g_pUseShaderHook);

    HyprlandAPI::removeDispatcher(PHANDLE, "layershader");
    HyprlandAPI::removeDispatcher(PHANDLE, "togglelayershader");
    HyprlandAPI::removeDispatcher(PHANDLE, "togglewindowshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "classshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "toggleclassshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "reloadshaders");
}