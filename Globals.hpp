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

// --- SAFEGUARD: ABI VERSION ---
const std::string TARGET_HYPRLAND_VERSION = "v0.55.0";

// --- SHARED GLOBALS ---
extern HANDLE PHANDLE;
extern std::vector<CHyprSignalListener> g_Listeners;

// --- ISOLATED MEMORY MAPS ---
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
    GLint timeLoc      = -1;
    GLint alphaLoc     = -1;
    bool  usesTime     = false;
};

extern std::map<std::string, std::string>          g_mLayerNamespaceShaderMap;
extern std::map<std::string, std::string>          g_mWindowClassShaderMap;
extern std::map<std::string, CompiledShader>       g_mCompiledCShaders;

// --- HOOK POINTERS ---
// V0.55: hook the concrete renderer that draws texture pass elements
extern CFunctionHook* g_pGLDrawTexHook;
extern CFunctionHook* g_pUseShaderHook;

// --- ACTIVE RENDER CONTEXT ---
// Set by hkGLDrawTex before delegating; read by hkUseShader during the call.
// PHLWINDOWREF avoids the per-draw linear scan that was previously needed to
// recover a shared pointer from a raw CWindow*.
extern thread_local PHLWINDOWREF                  g_pCurrentRenderWindow;
extern thread_local Desktop::View::CLayerSurface* g_pCurrentRenderLayer;

// --- FUNCTION DECLARATIONS ---
CompiledShader*                             getOrCompileShader(const std::string& shaderPath);
void                                        hkGLDrawTex(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage);
Hyprutils::Memory::CWeakPointer<CShader>    hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);
void                                        applyShaderRulesSafe(PHLWINDOW pWindow);