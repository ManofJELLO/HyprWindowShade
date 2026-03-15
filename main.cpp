// 1. ABSOLUTE FIRST: Include native GLES3. 
#include <GLES3/gl32.h>
#include <functional>
#include <any>
#include <string>
#include <vector>
#include <stdexcept>

// --- DARKWINDOW FIX 1 (AMENDED): CLEAN INCLUDES ---
// We completely removed the `#define private public` hack. It breaks modern 
// GCC C++15 STL headers. We will track the rendering window natively instead!
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>

// --- CRITICAL FIX 5: PHYSICAL MEMORY HOOKING ---
// We are bypassing the event system entirely using physical memory hooks.
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp> 

// --- LAYER SURFACE SUPPORT (FIXED) ---
// In v0.54.2, LayerSurface was moved into the view/ subdirectory alongside Window.hpp
#include <hyprland/src/desktop/view/LayerSurface.hpp>

#include <hyprland/src/event/EventBus.hpp>
#include <hyprutils/memory/UniquePtr.hpp>

// --- COMPOSITOR SUPPORT ---
// Required to grab the currently focused window for our new toggle dispatcher!
#include <hyprland/src/Compositor.hpp>

#include <fstream>
#include <sstream>
#include <map>
#include <chrono>
#include <sys/stat.h>
#include <iostream> 

// --- SAFEGUARD 1: ABI VERSION SHIELD ---
const std::string TARGET_HYPRLAND_VERSION = "v0.54.2"; 

inline HANDLE PHANDLE = nullptr;

// Native event listeners must be stored so they aren't destroyed
std::vector<CHyprSignalListener> g_Listeners;

// Use raw CWindow* to completely decouple from PHLWINDOW smart pointer issues
std::map<Desktop::View::CWindow*, std::string> g_mWindowShaderMap;

// Map for associating Layer Surface namespaces to shader paths
std::map<std::string, std::string> g_mLayerNamespaceShaderMap;

// --- CRITICAL FIX 58: APP CLASS MAP ---
// Map for associating specific application classes (like "kitty") to shader paths
std::map<std::string, std::string> g_mWindowClassShaderMap;

// --- CRITICAL FIX 50: NATIVE CSHADER CACHE ---
std::map<std::string, Hyprutils::Memory::CSharedPointer<CShader>> g_mCompiledCShaders;

// --- CRITICAL FIX 62: TIME TRACKER MAP ---
// Intelligently tracks which shaders require a continuous render loop!
std::map<std::string, bool> g_mShaderUsesTime;


// --- HELPER: SHADER COMPILATION ---
// Extracts the compilation logic so both Windows and Layers can use it cleanly.
Hyprutils::Memory::CSharedPointer<CShader> getOrCompileShader(CHyprOpenGLImpl* thisptr, const std::string& shaderPath) {
    if (g_mCompiledCShaders.find(shaderPath) == g_mCompiledCShaders.end()) {
        std::ifstream shaderFile(shaderPath);
        if (shaderFile.is_open()) {
            std::stringstream buffer;
            buffer << shaderFile.rdbuf();
            
            auto customShader = Hyprutils::Memory::makeShared<CShader>();
            
            // --- SMART DAMAGE DETECTION ---
            // If the shader file contains the word "time", we flag it for continuous animation!
            std::string shaderCode = buffer.str();
            g_mShaderUsesTime[shaderPath] = (shaderCode.find("time") != std::string::npos);

            // --- CRITICAL FIX 53: MATCH GLES VERSIONS ---
            // Because pixelate.glsl uses #version 320 es, we MUST link it
            // against Hyprland's TEXVERTSRC320 vertex shader!
            customShader->createProgram(thisptr->m_shaders->TEXVERTSRC320, shaderCode, true, true);
            g_mCompiledCShaders[shaderPath] = customShader;
            
            if (customShader->program() == 0) {
                HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] Shader Compile FAILED!", CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 10000.0f);
            }
        } else {
            return nullptr; // File not found
        }
    }
    return g_mCompiledCShaders[shaderPath];
}

// --- DARKWINDOW FIX 3 (AMENDED): Hooking getSurfaceShader ---
inline CFunctionHook* g_pGetSurfaceShaderHook = nullptr;
typedef Hyprutils::Memory::CWeakPointer<CShader> (*TGetSurfaceShader)(CHyprOpenGLImpl* thisptr, uint8_t features);

Hyprutils::Memory::CWeakPointer<CShader> hkGetSurfaceShader(CHyprOpenGLImpl* thisptr, uint8_t features) {
    
    // --- CRITICAL FIX 52: NATIVE RENDER DATA TRACKING ---
    // Hyprland v0.54.2 uses an async Render Pass system. We extract the active
    // window OR layer directly from the OpenGL implementation's native state machine!
    auto window = thisptr->m_renderData.currentWindow.lock();
    auto layer  = thisptr->m_renderData.currentLS.lock();

    // --- THE FIX: GETSURFACESHADER ---
    // getShaderVariant is gone! Vaxry replaced it with getSurfaceShader, which 
    // ONLY targets the textured window surface. We drop the 'frag' enum entirely!
    
    if (window) {
        Desktop::View::CWindow* rawWin = window.get();
        
        // 1. Check for specific window instance overrides first
        if (g_mWindowShaderMap.find(rawWin) != g_mWindowShaderMap.end()) {
            std::string path = g_mWindowShaderMap[rawWin];
            auto customShader = getOrCompileShader(thisptr, path);
            if (customShader && customShader->program() != 0) {
                if (g_mShaderUsesTime[path]) g_pHyprRenderer->damageWindow(window);
                return customShader;
            }
        } 
        
        // 2. Fall back to checking if the entire app class is targeted
        // --- CRITICAL FIX 59: RENAME FIXES ---
        // Updated variable names to match v0.54.2 (m_initialClass and m_class)
        std::string initClass = rawWin->m_initialClass;
        std::string currentClass = rawWin->m_class;
        
        if (g_mWindowClassShaderMap.find(initClass) != g_mWindowClassShaderMap.end()) {
            std::string path = g_mWindowClassShaderMap[initClass];
            auto customShader = getOrCompileShader(thisptr, path);
            if (customShader && customShader->program() != 0) {
                if (g_mShaderUsesTime[path]) g_pHyprRenderer->damageWindow(window);
                return customShader;
            }
        } else if (g_mWindowClassShaderMap.find(currentClass) != g_mWindowClassShaderMap.end()) {
            std::string path = g_mWindowClassShaderMap[currentClass];
            auto customShader = getOrCompileShader(thisptr, path);
            if (customShader && customShader->program() != 0) {
                if (g_mShaderUsesTime[path]) g_pHyprRenderer->damageWindow(window);
                return customShader;
            }
        }
    } 
    else if (layer) {
        // --- LAYER NAMESPACE FIX ---
        // Updated to use 'm_namespace' as per the v0.54.2 LayerSurface.hpp
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

// --- CRITICAL FIX 63: TIME UNIFORM INJECTION ---
// By hooking useShader, we wait until the custom shader is actively bound 
// to the graphics card, and then we instantly push the steady_clock time into it!
inline CFunctionHook* g_pUseShaderHook = nullptr;
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
    }
    return result;
}

void applyShaderRulesSafe(PHLWINDOW pWindow) {
    if (!pWindow || !pWindow->m_ruleApplicator) return;
    Desktop::View::CWindow* rawWin = pWindow.get();

    if (g_mWindowShaderMap.find(rawWin) != g_mWindowShaderMap.end()) return;

    const auto& tagsSet = pWindow->m_ruleApplicator->m_tagKeeper.getTags();
    for (const auto& tag : tagsSet) {
        if (tag.find("shader:") != std::string::npos) {
            std::string shaderPath = tag.substr(tag.find("shader:") + 7);
            while (!shaderPath.empty() && (shaderPath.back() == '*' || shaderPath.back() == ' ')) shaderPath.pop_back();

            g_mWindowShaderMap[rawWin] = shaderPath;
            
            // --- DARKWINDOW FIX 2: Force Window Redraw ---
            g_pHyprRenderer->damageWindow(pWindow);
            return;
        }
    }
}

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

    // 3. Hook useShader to push our time uniforms
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

    // 5. Custom Dispatcher for Layer Surfaces
    HyprlandAPI::addDispatcherV2(PHANDLE, "layershader", [&](std::string args) -> SDispatchResult {
        size_t spacePos = args.find_first_of(" \t");
        if (spacePos != std::string::npos) {
            std::string ns = args.substr(0, spacePos);
            std::string path = args.substr(spacePos + 1);
            
            size_t start = path.find_first_not_of(" \t");
            if (start != std::string::npos) path = path.substr(start);
            while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();

            if (path == "clear" || path == "none") {
                g_mLayerNamespaceShaderMap.erase(ns);
            } else {
                g_mLayerNamespaceShaderMap[ns] = path;
            }
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

            if (g_mLayerNamespaceShaderMap.find(ns) != g_mLayerNamespaceShaderMap.end()) {
                g_mLayerNamespaceShaderMap.erase(ns);
            } else {
                g_mLayerNamespaceShaderMap[ns] = path;
            }
        }
        return SDispatchResult{};
    });

    // --- CRITICAL FIX 57: ACTIVE WINDOW TOGGLE ---
    HyprlandAPI::addDispatcherV2(PHANDLE, "togglewindowshader", [&](std::string path) -> SDispatchResult {
        size_t start = path.find_first_not_of(" \t");
        if (start != std::string::npos) path = path.substr(start);
        while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();

        if (path.empty()) return SDispatchResult{};

        // --- THE FIX: ACTIVE WINDOW ---
        // m_pLastWindow was removed in v0.54.2. We instead iterate through
        // the active window list and ask the Compositor which one currently has focus.
        PHLWINDOW pWindow = nullptr;
        for (auto& w : g_pCompositor->m_windows) {
            if (g_pCompositor->isWindowActive(w)) {
                pWindow = w;
                break;
            }
        }

        if (pWindow) {
            Desktop::View::CWindow* rawWin = pWindow.get();
            
            if (path == "clear" || path == "none") {
                g_mWindowShaderMap.erase(rawWin);
            } else if (g_mWindowShaderMap.find(rawWin) != g_mWindowShaderMap.end()) {
                g_mWindowShaderMap.erase(rawWin);
            } else {
                g_mWindowShaderMap[rawWin] = path;
            }
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

            if (path == "clear" || path == "none") {
                g_mWindowClassShaderMap.erase(cls);
            } else {
                g_mWindowClassShaderMap[cls] = path;
            }

            // Instantly redraw all open windows matching this class
            for (auto& w : g_pCompositor->m_windows) {
                if (w && (w->m_initialClass == cls || w->m_class == cls)) {
                    g_pHyprRenderer->damageWindow(w);
                }
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

            if (g_mWindowClassShaderMap.find(cls) != g_mWindowClassShaderMap.end()) {
                g_mWindowClassShaderMap.erase(cls);
            } else {
                g_mWindowClassShaderMap[cls] = path;
            }

            // Instantly redraw all open windows matching this class
            for (auto& w : g_pCompositor->m_windows) {
                if (w && (w->m_initialClass == cls || w->m_class == cls)) {
                    g_pHyprRenderer->damageWindow(w);
                }
            }
        }
        return SDispatchResult{};
    });

    // --- CRITICAL FIX 64: RELOAD SHADERS DISPATCHER ---
    // Clears the compiled shader cache and forces a global screen redraw to live-reload changes!
    HyprlandAPI::addDispatcherV2(PHANDLE, "reloadshaders", [&](std::string args) -> SDispatchResult {
        
        // 1. Wipe the current compiled binaries from RAM
        g_mCompiledCShaders.clear();
        
        // 2. Force every open window to redraw
        for (auto& w : g_pCompositor->m_windows) {
            if (w) g_pHyprRenderer->damageWindow(w);
        }
        
        // 3. Force every monitor to redraw (this catches all Layer Surfaces like mpvpaper)
        for (auto& m : g_pCompositor->m_monitors) {
            if (m) g_pCompositor->scheduleFrameForMonitor(m);
        }
        
        // Pop a quick notification to let you know the files were re-read
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] Shaders Reloaded from Disk!", CHyprColor(0.2f, 1.0f, 0.2f, 1.0f), 3000.0f);
        
        return SDispatchResult{};
    });

    return {"HyprWindowShade", "Native CShader Injection", "ManofJELLO", "1.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_Listeners.clear();
    
    // --- CRITICAL FIX 54: STRICT MEMORY UNLOADING ---
    g_mWindowShaderMap.clear();
    g_mLayerNamespaceShaderMap.clear();
    g_mWindowClassShaderMap.clear(); 
    g_mShaderUsesTime.clear();

    // --- CRITICAL FIX 60: THE UNLOAD CRASH (GL CONTEXT) ---
    // If we clear the CShader map normally, the CShader destructor calls glDeleteProgram().
    // If the OpenGL context is not active during the exact millisecond of unload, 
    // Hyprland silently catches a segfault and aborts the unload, keeping the old plugin locked!
    // We intentionally leak the tiny shader pointers to the heap to bypass the GL crash.
    auto leak = new std::map<std::string, Hyprutils::Memory::CSharedPointer<CShader>>(std::move(g_mCompiledCShaders));
    (void)leak; // Suppress unused variable warning

    if (g_pGetSurfaceShaderHook) {
        // --- CRITICAL FIX 61: DOUBLE UNHOOK CRASH ---
        // removeFunctionHook calls unhook() internally. Calling it twice can cause a crash!
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pGetSurfaceShaderHook);
    }
    
    if (g_pUseShaderHook) {
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pUseShaderHook);
    }

    // --- CRITICAL FIX 55: DISPATCHER CLEANUP ---
    HyprlandAPI::removeDispatcher(PHANDLE, "layershader");
    HyprlandAPI::removeDispatcher(PHANDLE, "togglelayershader");
    HyprlandAPI::removeDispatcher(PHANDLE, "togglewindowshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "classshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "toggleclassshader");
    HyprlandAPI::removeDispatcher(PHANDLE, "reloadshaders"); // Add new dispatcher cleanup
}