// 1. ABSOLUTE FIRST: Include native GLES3. 
// We dropped GLEW to rely on native GLES3, solving all redeclarations.
#include <GLES3/gl32.h>
#include <functional>
#include <any>
#include <string>
#include <vector>
#include <stdexcept>

// --- CRITICAL FIX 5: PHYSICAL MEMORY HOOKING ---
// The EventManager in this version is locked, and the string callbacks are bricked.
// We are bypassing the event system entirely by physically intercepting the 
// C++ memory address of CHyprRenderer::renderWindow.
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp> 
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp> 

#include <fstream>
#include <sstream>
#include <map>
#include <chrono>
#include <sys/stat.h>

// We removed the dynamic path found by the Makefile for Log.hpp 
// and will use standard C++ logging to bypass volatile Hyprland namespaces.
#include <iostream> 

// --- SAFEGUARD 1: ABI VERSION SHIELD ---
// Update this string ONLY when you have manually verified the TRenderWindow 
// memory signature against a new Hyprland release. Run `hyprctl version` in 
// your terminal to find your exact current version or commit hash.
const std::string TARGET_HYPRLAND_VERSION = "v0.54.2"; 

inline HANDLE PHANDLE = nullptr;
auto startTime = std::chrono::high_resolution_clock::now();

// --- SAFEGUARD 5: SOFT KILLSWITCH ---
// If a critical C++ exception occurs, this flag is thrown. It instantly disables 
// all custom rendering logic to protect the compositor from a total crash.
inline bool g_bFailsafeTriggered = false;

void triggerKillswitch(const std::string& errorMsg) {
    if (g_bFailsafeTriggered) return; // Prevent spamming the screen
    g_bFailsafeTriggered = true;
    
    std::string fullMsg = "[HyprWindowShade] CRITICAL CRASH PREVENTED!\nPlugin disabled. Run ./build.sh or unload manually.\nError: " + errorMsg;
    std::cerr << fullMsg << "\n";
    // 15 seconds of a solid red banner so the user cannot miss it
    HyprlandAPI::addNotification(PHANDLE, fullMsg, CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 15000.0f);
}

struct SWindowShader {
    GLuint programID = 0;
    GLint  locPos    = -1;
    GLint  locSize   = -1;
    GLint  locTime   = -1;
    GLint  locTex    = -1;
    time_t lastModified = 0;
};

// Maps using the modern Shared Pointer (SP) type
std::map<std::string, SWindowShader> g_mCompiledShaders;
std::map<PHLWINDOW, std::string> g_mWindowShaderMap;

// Helper to get file modification time
time_t getFileModTime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        return st.st_mtime;
    return 0;
}

SWindowShader compileShader(const std::string& path) {
    SWindowShader shaderData;
    shaderData.lastModified = getFileModTime(path);

    // Minimal Pass-through Vertex Shader
    // This provides the 'v_texcoord' that pixelate.glsl expects
    const char* vertSource = R"(
        #version 320 es
        precision highp float;
        layout (location = 0) in vec2 a_pos;
        layout (location = 1) in vec2 a_tex;
        out vec2 v_texcoord;
        void main() {
            gl_Position = vec4(a_pos, 0.0, 1.0);
            v_texcoord = a_tex;
        }
    )";

    std::ifstream shaderFile(path);
    if (!shaderFile.is_open()) {
        std::cerr << "[HyprWindowShade] Failed to open shader at: " << path << "\n";
        return shaderData;
    }

    std::stringstream buffer;
    buffer << shaderFile.rdbuf();
    std::string fragSource = buffer.str();
    const char* fragSrcPtr = fragSource.c_str();

    // --- SAFEGUARD 4: GLSL ON-SCREEN ERROR REPORTING ---
    // This lambda intercepts raw OpenGL compilation errors and forwards 
    // them to Hyprland's UI notification system for seamless hot-reloading.
    auto checkCompileErrors = [&](GLuint shaderObj, std::string type) -> bool {
        GLint success;
        GLchar infoLog[1024];
        if (type != "PROGRAM") {
            glGetShaderiv(shaderObj, GL_COMPILE_STATUS, &success);
            if (!success) {
                glGetShaderInfoLog(shaderObj, 1024, NULL, infoLog);
                std::string err = "[HyprWindowShade] " + type + " Shader Error:\n" + std::string(infoLog);
                std::cerr << err << "\n";
                // Output to screen: Red banner, visible for 10 seconds
                HyprlandAPI::addNotification(PHANDLE, err, CHyprColor(1.0f, 0.2f, 0.2f, 1.0f), 10000.0f);
                return false;
            }
        } else {
            glGetProgramiv(shaderObj, GL_LINK_STATUS, &success);
            if (!success) {
                glGetProgramInfoLog(shaderObj, 1024, NULL, infoLog);
                std::string err = "[HyprWindowShade] GLSL Linking Error:\n" + std::string(infoLog);
                std::cerr << err << "\n";
                HyprlandAPI::addNotification(PHANDLE, err, CHyprColor(1.0f, 0.2f, 0.2f, 1.0f), 10000.0f);
                return false;
            }
        }
        return true;
    };

    GLuint vertShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertShader, 1, &vertSource, nullptr);
    glCompileShader(vertShader);
    if (!checkCompileErrors(vertShader, "VERTEX")) return shaderData;

    GLuint fragShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragShader, 1, &fragSrcPtr, nullptr);
    glCompileShader(fragShader);
    if (!checkCompileErrors(fragShader, "FRAGMENT")) {
        glDeleteShader(vertShader);
        glDeleteShader(fragShader);
        return shaderData; // Abort compilation to keep the old valid shader active
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertShader);
    glAttachShader(program, fragShader);
    glLinkProgram(program);
    if (!checkCompileErrors(program, "PROGRAM")) {
        glDeleteShader(vertShader);
        glDeleteShader(fragShader);
        glDeleteProgram(program);
        return shaderData;
    }
    
    // Cache locations - Matching pixelate.glsl 'tex' sampler
    shaderData.programID = program;
    shaderData.locTex    = glGetUniformLocation(program, "tex"); 
    shaderData.locPos    = glGetUniformLocation(program, "pos");
    shaderData.locSize   = glGetUniformLocation(program, "size");
    shaderData.locTime   = glGetUniformLocation(program, "time");

    glDeleteShader(vertShader);
    glDeleteShader(fragShader); 
    
    std::cout << "[HyprWindowShade] Successfully compiled: " << path << "\n";
    
    return shaderData;
}

// --- CRITICAL FIX 2: SFINAE RULE RESOLUTION ---
// By passing cm by reference (&), we avoid the CUniquePointer deleted copy error.
template<typename ConfigManager, typename WindowPtr>
void applyShaderRulesSafe(ConfigManager& cm, WindowPtr pWindow) {
    if (!pWindow) return;

    // We use a completely generic auto lambda. This prevents the compiler from 
    // throwing "-Wtemplate-body" errors by forcing it to delay evaluation 
    // until it actually tries to fetch the rules.
    auto getRules = [](auto& config, auto& win) {
        if constexpr (requires { config->getMatchingRules(win); }) {
            return config->getMatchingRules(win);
        } else if constexpr (requires { config->getWindowRules(win); }) {
            return config->getWindowRules(win);
        } else {
            // Failsafe dummy return if neither API exists in your version
            struct DummyRule { std::string szRule; };
            return std::vector<DummyRule>{};
        }
    };

    auto rules = getRules(cm, pWindow);

    for (auto& rule : rules) {
        std::string ruleStr;
        // Safely extract the string regardless of if the rule is a pointer or value
        if constexpr (requires { rule->szRule; }) { ruleStr = rule->szRule; }
        else if constexpr (requires { rule.szRule; }) { ruleStr = rule.szRule; }
        else if constexpr (requires { rule->rule; }) { ruleStr = rule->rule; }
        else if constexpr (requires { rule.rule; }) { ruleStr = rule.rule; }
        else continue;

        // --- CRITICAL FIX 7: HYPRLAND STRICT PARSER BYPASS ---
        // Hyprland's internal parser strictly rejects custom/plugin window rules,
        // throwing an "invalid field type" error. To bypass this entirely, we 
        // piggyback on the native "tag" rule, which natively allows arbitrary strings!
        if (ruleStr.starts_with("tag +shader:")) {
            std::string shaderPath = ruleStr.substr(12);
            if (g_mCompiledShaders.find(shaderPath) == g_mCompiledShaders.end()) {
                g_mCompiledShaders[shaderPath] = compileShader(shaderPath);
            }
            g_mWindowShaderMap[pWindow] = shaderPath;
        }
    }
}

void OnBeforeRenderWindow(PHLWINDOW pWindow) {
    if (g_mWindowShaderMap.find(pWindow) == g_mWindowShaderMap.end()) {
        applyShaderRulesSafe(g_pConfigManager, pWindow);
    }

    if (g_mWindowShaderMap.find(pWindow) == g_mWindowShaderMap.end())
        return;

    std::string& path = g_mWindowShaderMap[pWindow];
    SWindowShader& shader = g_mCompiledShaders[path];

    // Watcher logic
    time_t currentModTime = getFileModTime(path);
    if (currentModTime > shader.lastModified && currentModTime != 0) {
        std::cout << "[HyprWindowShade] Reloading shader: " << path << "\n";
        GLuint oldProg = shader.programID;
        
        SWindowShader newShader = compileShader(path);
        // Only swap and delete the old program if the new one successfully compiled
        if (newShader.programID != 0) {
            shader = newShader;
            if (oldProg != 0) glDeleteProgram(oldProg);
        } else {
            // If compilation failed, just update the modified time so it doesn't spam re-checks
            shader.lastModified = currentModTime; 
        }
    }

    if (shader.programID == 0) return; // Failsafe if the initial compile broke

    // Modern API: Animated variables are smart pointers, accessed via ->value()
    const auto POS  = pWindow->m_realPosition->value();
    const auto SIZE = pWindow->m_realSize->value();
    float time = std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - startTime).count();

    glUseProgram(shader.programID);
    if (shader.locPos != -1)  glUniform2f(shader.locPos, POS.x, POS.y);
    if (shader.locSize != -1) glUniform2f(shader.locSize, SIZE.x, SIZE.y);
    if (shader.locTime != -1) glUniform1f(shader.locTime, time);
    if (shader.locTex != -1)  glUniform1i(shader.locTex, 0); 
}

void OnAfterRenderWindow(PHLWINDOW pWindow) {
    if (g_mWindowShaderMap.count(pWindow)) {
        glUseProgram(0);
    }
}

// --- CFunctionHook Setup ---
inline CFunctionHook* g_pRenderHook = nullptr;
// This typedef identically matches the memory layout of CHyprRenderer::renderWindow
typedef void (*TRenderWindow)(void* thisptr, PHLWINDOW pWindow, void* pMonitor, timespec* time, bool opaque, int passMode, bool ignorePosition, bool ignoreColor);

// Our trampoline function that intercepts the pipeline
void hkRenderWindow(void* thisptr, PHLWINDOW pWindow, void* pMonitor, timespec* time, bool opaque, int passMode, bool ignorePosition, bool ignoreColor) {
    // If a critical error already happened, instantly pass to original renderer without touching custom code
    if (g_bFailsafeTriggered) {
        ((TRenderWindow)g_pRenderHook->m_original)(thisptr, pWindow, pMonitor, time, opaque, passMode, ignorePosition, ignoreColor);
        return;
    }

    bool preRenderSuccess = true;

    // Safely attempt the BEFORE render logic
    try {
        if (pWindow) OnBeforeRenderWindow(pWindow);
    } catch (const std::exception& e) {
        triggerKillswitch(std::string("Pre-Render Error: ") + e.what());
        preRenderSuccess = false;
    } catch (...) {
        triggerKillswitch("Unknown Pre-Render Exception!");
        preRenderSuccess = false;
    }
    
    // Call the original, un-hooked renderWindow function so it ALWAYS draws the window
    ((TRenderWindow)g_pRenderHook->m_original)(thisptr, pWindow, pMonitor, time, opaque, passMode, ignorePosition, ignoreColor);
    
    // Safely attempt the AFTER render logic, but only if the BEFORE logic succeeded
    if (preRenderSuccess) {
        try {
            if (pWindow) OnAfterRenderWindow(pWindow);
        } catch (const std::exception& e) {
            triggerKillswitch(std::string("Post-Render Error: ") + e.what());
        } catch (...) {
            triggerKillswitch("Unknown Post-Render Exception!");
        }
    }
}

// --- REQUIRED HYPRLAND API EXPORTS ---
// We MUST use the exact macros provided by HyprlandAPI.hpp.
// They automatically handle the C-linkage and exact string names that dlsym expects.

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    // --- SAFEGUARD 2: RUNTIME ABI CHECK ---
    SVersionInfo hyprVer = HyprlandAPI::getHyprlandVersion(PHANDLE);
    std::string currentVerString = hyprVer.tag;

    if (currentVerString.find(TARGET_HYPRLAND_VERSION) == std::string::npos) {
        std::cerr << "\n[HyprWindowShade] =========================================\n";
        std::cerr << "[HyprWindowShade] FATAL ABI SHIELD: HYPRLAND VERSION MISMATCH!\n";
        std::cerr << "[HyprWindowShade] Expected: " << TARGET_HYPRLAND_VERSION << "\n";
        std::cerr << "[HyprWindowShade] Found:    " << currentVerString << "\n";
        std::cerr << "[HyprWindowShade] Aborting memory hook to protect the compositor from crashing.\n";
        std::cerr << "[HyprWindowShade] Please verify the TRenderWindow signature and update TARGET_HYPRLAND_VERSION.\n";
        std::cerr << "[HyprWindowShade] =========================================\n\n";
        
        return {"HyprWindowShade (DISABLED: ABI LOCKOUT)", "Version Safety Lockout", "YourName", "1.1"};
    }

    // Dynamically search the compositor's memory for the renderWindow function
    auto methods = HyprlandAPI::findFunctionsByName(PHANDLE, "renderWindow");
    void* renderWindowAddr = nullptr;
    
    for (auto& m : methods) {
        if (m.demangled.find("CHyprRenderer::renderWindow") != std::string::npos) {
            renderWindowAddr = m.address;
            break;
        }
    }

    // If we find the memory address, apply our trampoline hook to intercept it
    if (renderWindowAddr) {
        g_pRenderHook = HyprlandAPI::createFunctionHook(PHANDLE, renderWindowAddr, (void*)&hkRenderWindow);
        g_pRenderHook->hook();
        std::cout << "[HyprWindowShade] Version matches! Successfully hooked CHyprRenderer::renderWindow!\n";
    } else {
        std::cerr << "[HyprWindowShade] FATAL: Could not locate renderWindow address!\n";
    }

    return {"HyprWindowShade", "Per-window shaders via direct memory hooking", "YourName", "1.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    // --- SAFEGUARD 3: EXPLICIT UNHOOKING ---
    // We MUST restore the original renderWindow memory bytes before this 
    // plugin is unmapped from RAM to prevent a catastrophic dangling pointer segfault.
    if (g_pRenderHook) {
        g_pRenderHook->unhook();
        HyprlandAPI::removeFunctionHook(PHANDLE, g_pRenderHook);
        g_pRenderHook = nullptr;
        std::cout << "[HyprWindowShade] Successfully unhooked CHyprRenderer::renderWindow.\n";
    }

    for (auto const& [path, shader] : g_mCompiledShaders) {
        if (shader.programID != 0)
            glDeleteProgram(shader.programID);
    }
    g_mWindowShaderMap.clear();
    g_mCompiledShaders.clear();
}