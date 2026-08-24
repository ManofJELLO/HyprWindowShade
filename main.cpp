#include "Globals.hpp"

// Hyprland (v0.56) links liblua.so.5.5 and /usr/include/lua.h is 5.5; symbols
// resolve from the host process at dlopen, so we don't link -llua ourselves.
#include <lua.hpp>

// --- DEFINE EXTERN VARIABLES ---
HANDLE                                                PHANDLE = nullptr;
std::vector<CHyprSignalListener>                      g_Listeners;
std::unordered_map<Desktop::View::CWindow*, std::string>        g_mWindowManualShaders;
std::unordered_map<Desktop::View::CWindow*, WindowShaderState>  g_mWindowRuleShaders;
std::map<std::string, std::string>                    g_mLayerNamespaceShaderMap;
std::map<std::string, std::string>                    g_mWindowClassShaderMap;
std::map<std::string, CompiledShader>                 g_mCompiledCShaders;
std::map<std::string, time_t>                         g_mFailedShaderMtimes;
std::map<std::string, time_t>                         g_mDurationWarnedMtimes;
std::unordered_map<Desktop::View::CWindow*, std::chrono::steady_clock::time_point> g_mWindowOpenTimes;
std::unordered_map<Desktop::IFadeout*, FadeoutAnim>   g_mFadeoutAnims;
CFunctionHook*                                        g_pGLDrawTexHook     = nullptr;
CFunctionHook*                                        g_pUseShaderHook     = nullptr;
CFunctionHook*                                        g_pFadeoutCreateHook = nullptr;
CFunctionHook*                                        g_pFadeoutDoneHook   = nullptr;

// Tracks the most recently activated window so togglewindowshader doesn't have
// to linear-scan Desktop::windowState()->windows() asking isWindowActive on each.
static PHLWINDOWREF g_lastActiveWindow;

// --- HELPERS ---
static inline void trimInPlace(std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    if (start == std::string::npos) { s.clear(); return; }
    s.erase(0, start);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
}

// Splits "<key> <value...>" into trimmed (key, value). Returns false if no separator.
// Recognizes a leading double-quoted key so class names or layer namespaces that
// contain whitespace work via the .conf dispatchers, matching the per-arg shape
// Lua callers already get. Escapes inside the quoted string are not supported —
// keep it to plain whitespace handling, which is all the realistic case needs.
static bool splitTwo(const std::string& args, std::string& key, std::string& value) {
    size_t pos = 0;
    while (pos < args.size() && (args[pos] == ' ' || args[pos] == '\t')) pos++;
    if (pos >= args.size()) return false;

    size_t keyEnd;
    if (args[pos] == '"') {
        const size_t closing = args.find('"', pos + 1);
        if (closing == std::string::npos) return false;
        key    = args.substr(pos + 1, closing - pos - 1);
        keyEnd = closing + 1;
    } else {
        const size_t ws = args.find_first_of(" \t", pos);
        if (ws == std::string::npos) return false;
        key    = args.substr(pos, ws - pos);
        keyEnd = ws;
    }

    if (keyEnd >= args.size()) return false;
    value = args.substr(keyEnd);
    trimInPlace(value);
    return !value.empty();
}

// --- SHARED DISPATCH/LUA ACTIONS ---
// Each function below is the canonical body of one shader action. Both the
// .conf-style dispatcher lambdas (which still parse one string via splitTwo)
// and the Lua C wrappers (which read separate args from the Lua stack) call
// these — keeping behavior identical across both call sites.
namespace shadeActions {
    static void setLayerShader(const std::string& ns, const std::string& path) {
        if (path == "clear" || path == "none") g_mLayerNamespaceShaderMap.erase(ns);
        else                                   g_mLayerNamespaceShaderMap[ns] = path;
    }

    static void toggleLayerShader(const std::string& ns, const std::string& path) {
        if (g_mLayerNamespaceShaderMap.find(ns) != g_mLayerNamespaceShaderMap.end())
            g_mLayerNamespaceShaderMap.erase(ns);
        else
            g_mLayerNamespaceShaderMap[ns] = path;
    }

    static void toggleWindowShader(const std::string& path) {
        if (path.empty()) return;

        PHLWINDOW pWindow = g_lastActiveWindow.lock();
        if (!pWindow) return;

        Desktop::View::CWindow* rawWin = pWindow.get();
        if (path == "clear" || path == "none" || g_mWindowManualShaders.find(rawWin) != g_mWindowManualShaders.end())
            g_mWindowManualShaders.erase(rawWin);
        else
            g_mWindowManualShaders[rawWin] = path;
        g_pHyprRenderer->damageWindow(pWindow);
    }

    static void setClassShader(const std::string& cls, const std::string& path) {
        if (path == "clear" || path == "none") g_mWindowClassShaderMap.erase(cls);
        else                                   g_mWindowClassShaderMap[cls] = path;

        for (auto& w : Desktop::windowState()->windows())
            if (w && (w->m_initialClass == cls || w->m_class == cls))
                g_pHyprRenderer->damageWindow(w);
    }

    static void toggleClassShader(const std::string& cls, const std::string& path) {
        if (g_mWindowClassShaderMap.find(cls) != g_mWindowClassShaderMap.end())
            g_mWindowClassShaderMap.erase(cls);
        else
            g_mWindowClassShaderMap[cls] = path;

        for (auto& w : Desktop::windowState()->windows())
            if (w && (w->m_initialClass == cls || w->m_class == cls))
                g_pHyprRenderer->damageWindow(w);
    }

    static void reloadShaders() {
        g_mCompiledCShaders.clear();
        g_mFailedShaderMtimes.clear();
        // Same intent as the failure cache: an explicit reload means "report
        // everything again", including animation shaders missing a duration.
        g_mDurationWarnedMtimes.clear();
        for (auto& w : Desktop::windowState()->windows()) if (w) g_pHyprRenderer->damageWindow(w);
        for (auto& m : State::monitorState()->monitors()) if (m) m->scheduleFrame();
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] Shaders Reloaded from Disk!", CHyprColor(0.2f, 1.0f, 0.2f, 1.0f), 3000.0f);
    }
}

// --- LUA C WRAPPERS ---
// Registered via HyprlandAPI::addLuaFunction so users on Lua-based Hyprland
// configs can bind keys like: function() hl.plugin.HyprWindowShade.<name>(...) end
// (Native Hyprland dispatchers from plugins aren't surfaced to the Lua config
// layer, per vaxry — Lua functions are the supported workaround.)
static int luaLayerShader(lua_State* L) {
    shadeActions::setLayerShader(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int luaToggleLayerShader(lua_State* L) {
    shadeActions::toggleLayerShader(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int luaToggleWindowShader(lua_State* L) {
    shadeActions::toggleWindowShader(luaL_checkstring(L, 1));
    return 0;
}
static int luaClassShader(lua_State* L) {
    shadeActions::setClassShader(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int luaToggleClassShader(lua_State* L) {
    shadeActions::toggleClassShader(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    return 0;
}
static int luaReloadShaders(lua_State* L) {
    (void)L;
    shadeActions::reloadShaders();
    return 0;
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // --- V0.56 HOOK 1: CGLElementRenderer::draw(CTexPassElement, CRegion) ---
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

    // --- V0.56 HOOK 2: useShader ---
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

    // --- V0.56 HOOK 3+4: close animations (optional) ---
    // Both live on Desktop::CWindowFadeout. If either is missing we simply don't
    // do close animations — everything else keeps working.
    auto methodsCreate = HyprlandAPI::findFunctionsByName(PHANDLE, "create");
    void* fadeoutCreateAddr = nullptr;
    for (auto& m : methodsCreate) {
        if (m.demangled.find("CWindowFadeout::create") != std::string::npos) {
            fadeoutCreateAddr = m.address;
            break;
        }
    }

    auto methodsDone = HyprlandAPI::findFunctionsByName(PHANDLE, "done");
    void* fadeoutDoneAddr = nullptr;
    for (auto& m : methodsDone) {
        if (m.demangled.find("CWindowFadeout::done") != std::string::npos) {
            fadeoutDoneAddr = m.address;
            break;
        }
    }

    if (fadeoutCreateAddr && fadeoutDoneAddr) {
        g_pFadeoutCreateHook = HyprlandAPI::createFunctionHook(PHANDLE, fadeoutCreateAddr, (void*)&hkFadeoutCreate);
        g_pFadeoutDoneHook   = HyprlandAPI::createFunctionHook(PHANDLE, fadeoutDoneAddr,   (void*)&hkFadeoutDone);
        g_pFadeoutCreateHook->hook();
        g_pFadeoutDoneHook->hook();
    } else {
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] CWindowFadeout hooks not found — shader_close: disabled.",
                                     CHyprColor(1.0f, 0.7f, 0.2f, 1.0f), 8000.0f);
    }

    // --- LISTENERS ---
    g_Listeners.push_back(Event::bus()->m_events.window.updateRules.listen([](PHLWINDOW window) {
        try { applyShaderRulesSafe(window); } catch (...) {}
    }));

    // Record only WHEN the window opened. The shader itself is resolved at first
    // draw, because window.open and updateRules have no guaranteed ordering and
    // reading the rule here would silently miss whenever open fires first.
    g_Listeners.push_back(Event::bus()->m_events.window.open.listen([](PHLWINDOW window) {
        if (!window) return;
        g_mWindowOpenTimes[window.get()] = std::chrono::steady_clock::now();
        g_pHyprRenderer->damageWindow(window);
    }));

    g_Listeners.push_back(Event::bus()->m_events.window.active.listen([](auto window, auto reason) {
        // Only the previously- and currently-active windows can change appearance.
        if (auto prev = g_lastActiveWindow.lock(); prev && g_mWindowRuleShaders.find(prev.get()) != g_mWindowRuleShaders.end())
            g_pHyprRenderer->damageWindow(prev);
        if (window && g_mWindowRuleShaders.find(window.get()) != g_mWindowRuleShaders.end())
            g_pHyprRenderer->damageWindow(window);
        g_lastActiveWindow = window;
    }));

    g_Listeners.push_back(Event::bus()->m_events.window.fullscreen.listen([](auto window) {
        if (window) g_pHyprRenderer->damageWindow(window);
    }));

    // Drop entries keyed by raw CWindow* when the window is destroyed so they
    // can't accidentally match a future window at the same address.
    // V0.56: window.destroy emits PHLWINDOWREF (the shared ref is already gone
    // by the time it fires), so take the weak ref and read the raw pointer off
    // it — we only ever use it as a map key, never dereference it here.
    g_Listeners.push_back(Event::bus()->m_events.window.destroy.listen([](PHLWINDOWREF window) {
        if (!window) return;
        Desktop::View::CWindow* rawWin = window.get();
        g_mWindowManualShaders.erase(rawWin);
        g_mWindowRuleShaders.erase(rawWin);
        g_mWindowOpenTimes.erase(rawWin);
    }));

    // --- DISPATCHERS (.conf-style — Hyprland's native bind path) ---

    HyprlandAPI::addDispatcherV2(PHANDLE, "layershader", [](std::string args) -> SDispatchResult {
        std::string ns, path;
        if (!splitTwo(args, ns, path)) return SDispatchResult{};
        shadeActions::setLayerShader(ns, path);
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "togglelayershader", [](std::string args) -> SDispatchResult {
        std::string ns, path;
        if (!splitTwo(args, ns, path)) return SDispatchResult{};
        shadeActions::toggleLayerShader(ns, path);
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "togglewindowshader", [](std::string path) -> SDispatchResult {
        trimInPlace(path);
        shadeActions::toggleWindowShader(path);
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "classshader", [](std::string args) -> SDispatchResult {
        std::string cls, path;
        if (!splitTwo(args, cls, path)) return SDispatchResult{};
        shadeActions::setClassShader(cls, path);
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "toggleclassshader", [](std::string args) -> SDispatchResult {
        std::string cls, path;
        if (!splitTwo(args, cls, path)) return SDispatchResult{};
        shadeActions::toggleClassShader(cls, path);
        return SDispatchResult{};
    });

    HyprlandAPI::addDispatcherV2(PHANDLE, "reloadshaders", [](std::string) -> SDispatchResult {
        shadeActions::reloadShaders();
        return SDispatchResult{};
    });

    // --- LUA FUNCTIONS (hl.plugin.HyprWindowShade.* — for Lua-based configs) ---
    // Removed automatically on plugin unload (per PluginAPI.hpp), but PLUGIN_EXIT
    // calls removeLuaFunction defensively to keep teardown symmetric.
    HyprlandAPI::addLuaFunction(PHANDLE, "HyprWindowShade", "layershader",        &luaLayerShader);
    HyprlandAPI::addLuaFunction(PHANDLE, "HyprWindowShade", "togglelayershader",  &luaToggleLayerShader);
    HyprlandAPI::addLuaFunction(PHANDLE, "HyprWindowShade", "togglewindowshader", &luaToggleWindowShader);
    HyprlandAPI::addLuaFunction(PHANDLE, "HyprWindowShade", "classshader",        &luaClassShader);
    HyprlandAPI::addLuaFunction(PHANDLE, "HyprWindowShade", "toggleclassshader",  &luaToggleClassShader);
    HyprlandAPI::addLuaFunction(PHANDLE, "HyprWindowShade", "reloadshaders",      &luaReloadShaders);

    return {"HyprWindowShade", "Native CShader Injection (v0.56)", "ManofJELLO", "1.5"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_Listeners.clear();

    g_mWindowManualShaders.clear();
    g_mWindowRuleShaders.clear();
    g_mLayerNamespaceShaderMap.clear();
    g_mWindowClassShaderMap.clear();
    g_mFailedShaderMtimes.clear();
    g_mDurationWarnedMtimes.clear();
    g_mWindowOpenTimes.clear();
    // Any fadeout we were holding open falls back to Hyprland's own `done` as
    // soon as the hooks come off below.
    g_mFadeoutAnims.clear();

    // Intentionally leak compiled shaders: their CShader destructors call into
    // GL state owned by Hyprland, which may already be torn down at this point.
    // Moving the map into a heap allocation suppresses dtors at plugin unload.
    auto leak = new std::map<std::string, CompiledShader>(std::move(g_mCompiledCShaders));
    (void)leak;

    if (g_pGLDrawTexHook)      HyprlandAPI::removeFunctionHook(PHANDLE, g_pGLDrawTexHook);
    if (g_pUseShaderHook)      HyprlandAPI::removeFunctionHook(PHANDLE, g_pUseShaderHook);
    if (g_pFadeoutCreateHook)  HyprlandAPI::removeFunctionHook(PHANDLE, g_pFadeoutCreateHook);
    if (g_pFadeoutDoneHook)    HyprlandAPI::removeFunctionHook(PHANDLE, g_pFadeoutDoneHook);

    HyprlandAPI::removeDispatcher(PHANDLE, "layershader");
    HyprlandAPI::removeDispatcher(PHANDLE, "togglelayershader");
    HyprlandAPI::removeDispatcher(PHANDLE, "togglewindowshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "classshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "toggleclassshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "reloadshaders");

    HyprlandAPI::removeLuaFunction(PHANDLE, "HyprWindowShade", "layershader");
    HyprlandAPI::removeLuaFunction(PHANDLE, "HyprWindowShade", "togglelayershader");
    HyprlandAPI::removeLuaFunction(PHANDLE, "HyprWindowShade", "togglewindowshader");
    HyprlandAPI::removeLuaFunction(PHANDLE, "HyprWindowShade", "classshader");
    HyprlandAPI::removeLuaFunction(PHANDLE, "HyprWindowShade", "toggleclassshader");
    HyprlandAPI::removeLuaFunction(PHANDLE, "HyprWindowShade", "reloadshaders");
}