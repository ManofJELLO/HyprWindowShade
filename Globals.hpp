#pragma once

// 1. ABSOLUTE FIRST: Include native GLES3. 
#include <GLES3/gl32.h>
#include <functional>
#include <any>
#include <string>
#include <vector>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <map>
#include <chrono>
#include <sys/stat.h>
#include <iostream> 

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

// --- SAFEGUARD 1: ABI VERSION SHIELD ---
const std::string TARGET_HYPRLAND_VERSION = "v0.54.2"; 

// --- SHARED GLOBALS ---
extern HANDLE PHANDLE;
extern std::vector<CHyprSignalListener> g_Listeners;

// --- THE FIX: ISOLATED MEMORY MAPS ---
// We split the maps so manual keybind toggles and dynamic window rules don't overwrite each other!
extern std::map<Desktop::View::CWindow*, std::string> g_mWindowManualShaders; 

struct WindowShaderState {
    std::string active;
    std::string inactive;
    std::string floating; 
    std::string tiled;    
    std::string fullscreen; // NEW: Fullscreen state tracker
    std::string fallback;
};
extern std::map<Desktop::View::CWindow*, WindowShaderState> g_mWindowRuleShaders;

// Map for associating Layer Surface namespaces to shader paths
extern std::map<std::string, std::string> g_mLayerNamespaceShaderMap;

// --- CRITICAL FIX 58: APP CLASS MAP ---
// Map for associating specific application classes (like "kitty") to shader paths
extern std::map<std::string, std::string> g_mWindowClassShaderMap;

// --- CRITICAL FIX 50: NATIVE CSHADER CACHE ---
extern std::map<std::string, Hyprutils::Memory::CSharedPointer<CShader>> g_mCompiledCShaders;

// --- CRITICAL FIX 62: TIME TRACKER MAP ---
// Intelligently tracks which shaders require a continuous render loop!
extern std::map<std::string, bool> g_mShaderUsesTime;

// --- HOOK POINTERS ---
extern CFunctionHook* g_pGetSurfaceShaderHook;
extern CFunctionHook* g_pUseShaderHook;

// --- FUNCTION DECLARATIONS ---
Hyprutils::Memory::CSharedPointer<CShader> getOrCompileShader(CHyprOpenGLImpl* thisptr, const std::string& shaderPath);
Hyprutils::Memory::CWeakPointer<CShader> hkGetSurfaceShader(CHyprOpenGLImpl* thisptr, uint8_t features);
Hyprutils::Memory::CWeakPointer<CShader> hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);
void applyShaderRulesSafe(PHLWINDOW pWindow);