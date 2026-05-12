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

// --- V0.55 RENDER INCLUDES ---
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>

// --- V0.55 NAMESPACE FIX ---
using Render::GL::CHyprOpenGLImpl;

// --- V0.55 HOOK TARGETS ---
#include <hyprland/src/render/pass/TexPassElement.hpp>

// --- PLUGIN SYSTEM ---
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/managers/PointerManager.hpp>

// --- SAFEGUARD: ABI VERSION ---
const std::string TARGET_HYPRLAND_VERSION = "v0.55.0";

// --- SHARED GLOBALS ---
extern HANDLE PHANDLE;
extern std::vector<CHyprSignalListener> g_Listeners;

// --- ISOLATED MEMORY MAPS ---
// THREADING INVARIANT: all global maps below are mutated and read exclusively
// on Hyprland's wayland main thread (event listeners, dispatchers, render
// hooks). Adding access from any other thread requires synchronization.
extern std::map<Desktop::View::CWindow*, std::string> g_mWindowManualShaders;

struct WindowShaderState {
    std::string active;
    std::string inactive;
    std::string floating;
    std::string tiled;
    std::string fullscreen;
    std::string fallback;
};
extern std::map<Desktop::View::CWindow*, WindowShaderState> g_mWindowRuleShaders;

struct CompiledShader {
    Hyprutils::Memory::CSharedPointer<CShader> shader;
    GLint timeLoc         = -1;
    GLint alphaLoc        = -1;
    GLint resolutionLoc   = -1; // vec2: current monitor pixel size
    GLint surfaceSizeLoc  = -1; // vec2: window size (logical px); 0,0 for layers
    GLint mouseLoc        = -1; // vec2: pointer position
    GLint isActiveLoc     = -1; // float 0/1
    GLint isFloatingLoc   = -1; // float 0/1
    GLint isFullscreenLoc = -1; // float 0/1
    bool  usesTime        = false;
};

extern std::map<std::string, std::string>          g_mLayerNamespaceShaderMap;
extern std::map<std::string, std::string>          g_mWindowClassShaderMap;
extern std::map<std::string, CompiledShader>       g_mCompiledCShaders;

// --- HOOK POINTERS ---
// V0.55: hook the concrete renderer that draws texture pass elements
extern CFunctionHook* g_pGLDrawTexHook;
extern CFunctionHook* g_pUseShaderHook;

// --- ACTIVE RENDER CONTEXT ---
// Set by hkGLDrawTex before delegating; consumed by hkUseShader during the call.
// Weak refs avoid lifetime hazards if Hyprland tears down a surface mid-call.
// g_pCurrentShaderPath points into one of the maps above; valid only until the
// hook returns (maps aren't mutated within a single draw chain on this thread).
extern thread_local PHLWINDOWREF      g_pCurrentRenderWindow;
extern thread_local PHLLSREF          g_pCurrentRenderLayer;
extern thread_local const std::string* g_pCurrentShaderPath;

// --- FUNCTION DECLARATIONS ---
CompiledShader*                             getOrCompileShader(const std::string& shaderPath);
void                                        hkGLDrawTex(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage);
Hyprutils::Memory::CWeakPointer<CShader>    hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);
void                                        applyShaderRulesSafe(PHLWINDOW pWindow);