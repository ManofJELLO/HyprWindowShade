#pragma once

// 1. ABSOLUTE FIRST: Include native GLES3.
#include <GLES3/gl32.h>
#include <functional>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include <unordered_map>
#include <chrono>
#include <sys/stat.h>

// --- V0.56 RENDER INCLUDES ---
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>

// --- V0.56 NAMESPACE FIX ---
using Render::GL::CHyprOpenGLImpl;

// --- V0.56 HOOK TARGETS ---
#include <hyprland/src/render/pass/TexPassElement.hpp>

// --- PLUGIN SYSTEM ---
#include <hyprland/src/plugins/HookSystem.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/View.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprutils/memory/UniquePtr.hpp>
#include <hyprland/src/Compositor.hpp>

// --- V0.56 STATE/MANAGER SPLIT ---
// 0.56 moved these off CCompositor into dedicated state objects and managers:
//   m_windows              -> Desktop::windowState()->windows()
//   m_monitors             -> State::monitorState()->monitors()
//   isWindowActive()       -> Desktop::focusState()->isWindowActive()
//   scheduleFrameForMonitor-> PHLMONITOR::scheduleFrame()
//   CWindow::isFullscreen()-> Fullscreen::controller()->isFullscreen()
//   g_pPointerManager      -> Pointer::mgr()
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>

// --- CLOSE-ANIMATION SUPPORT ---
// A closing window's surface is gone before we could ever shade it, so close
// animations ride Hyprland's own fadeout instead: it snapshots the window into
// a framebuffer and renders that texture for the duration of the fadeout. We
// hook CWindowFadeout::create to learn which window a fadeout belongs to, then
// match the snapshot texture at draw time.
#include <hyprland/src/desktop/state/FadingOutState.hpp>
#include <hyprland/src/desktop/state/WindowFadeout.hpp>
#include <hyprland/src/desktop/state/LayerFadeout.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>

// --- SHARED GLOBALS ---
extern HANDLE PHANDLE;
extern std::vector<CHyprSignalListener> g_Listeners;

// --- ISOLATED MEMORY MAPS ---
// THREADING INVARIANT: all global maps below are mutated and read exclusively
// on Hyprland's wayland main thread (event listeners, dispatchers, render
// hooks). Adding access from any other thread requires synchronization.
// Raw-pointer-keyed maps queried per textured surface on the render hot path;
// unordered_map gets O(1) average lookups, and pointer hashing is essentially
// identity. String-keyed maps below stay std::map — small-N tree lookups on
// short keys are comparable to hashing without the bucket-allocation overhead.
extern std::unordered_map<Desktop::View::CWindow*, std::string> g_mWindowManualShaders;

struct WindowShaderState {
    std::string active;
    std::string inactive;
    std::string floating;
    std::string tiled;
    std::string fullscreen;
    std::string fallback;
    // One-shot animations. Duration is normally declared by the shader itself
    // (`// @duration <sec>`); a negative value here means "ask the shader".
    // `shader_open:<path>@<sec>` / `shader_close:<path>@<sec>` overrides it.
    std::string openAnim;
    float       openAnimDuration  = -1.0f;
    std::string closeAnim;
    float       closeAnimDuration = -1.0f;
    // `shader_replace:1` opts this window out of stacking and back to the
    // first-match-wins ladder the plugin used before stacking existed.
    bool        replaceMode       = false;
    // `shader_fullscreen_stack:1` opts this window into stacking *while
    // fullscreen*. Off by default: a game or a video is the one place a
    // permanent effect is least likely to be wanted.
    bool        fullscreenStack   = false;
};
extern std::unordered_map<Desktop::View::CWindow*, WindowShaderState> g_mWindowRuleShaders;

// --- SHADER STACKING ---
// Every matching rule contributes a layer instead of the first match winning
// outright, so a window can carry a base look AND a state-specific effect AND
// an open/close animation at once.
//
// Each stage keeps its own GL program and renders offscreen into the next
// stage's input, rather than being spliced into one combined source. Splicing
// would be cheaper, but two shaders from different authors routinely both
// define `rand`/`noise`/the Ashima `mod289` boilerplate, and the combined
// program would fail to link — unusable for shaders users download rather than
// write. Separate programs compose without touching the source at all.
inline constexpr int MAX_SHADER_STAGES = 5; // base + geometry + focus + fullscreen + anim

// Ping-pong render targets for the intermediate stages, keyed by texture size.
// An intermediate result is consumed by the very next stage in the same draw
// call and never outlives it, so one pair per size serves every window on
// screen rather than needing per-window targets.
//
// Heap-allocated behind a pointer and deliberately leaked at unload, for the
// same reason g_mCompiledCShaders is: these destructors call into GL state
// owned by Hyprland, which may already be torn down by the time a global's
// destructor would run.
struct StageFramebuffers {
    Render::GL::CGLFramebuffer fb[2];
};
extern std::unordered_map<uint64_t, StageFramebuffers>* g_pStageFBs;

// True only while an offscreen stage is being drawn. Window opacity has to be
// applied exactly once, by the final on-screen stage — if every layer
// underneath multiplied it in too, a 50%-opacity window with three stages would
// come out at 12.5%.
extern bool g_bIntermediatePass;

// Fallback when neither the shader nor the rule declares a duration.
inline constexpr float DEFAULT_ANIM_DURATION = 0.3f;
// Upper bound on how long we'll hold a fadeout open, so a bad `// @duration`
// can never strand a snapshot on screen forever.
inline constexpr float MAX_ANIM_DURATION = 5.0f;

// When a window with a `shader_open:` rule maps we record the timestamp only —
// not the resolved shader. Window rules are applied by a separate event whose
// order relative to window.open isn't guaranteed, so resolving lazily at first
// draw (by which point rules are definitely applied) avoids the race entirely.
extern std::unordered_map<Desktop::View::CWindow*, std::chrono::steady_clock::time_point> g_mWindowOpenTimes;
extern std::unordered_map<Desktop::View::CLayerSurface*, std::chrono::steady_clock::time_point> g_mLayerOpenTimes;

// Live close animations, keyed by the IFadeout that owns the snapshot. Populated
// in hkFadeoutCreate (the only place a fadeout and its window are both visible)
// and drained when the fadeout is dropped.
struct FadeoutAnim {
    std::string                           path;
    std::chrono::steady_clock::time_point start;
    float                                 duration = -1.0f; // <0: ask the shader
    float                                 seed     = 0.5f;  // captured from the window
};
extern std::unordered_map<Desktop::IFadeout*, FadeoutAnim> g_mFadeoutAnims;

struct CompiledShader {
    Hyprutils::Memory::CSharedPointer<CShader> shader;
    GLint     timeLoc         = -1;
    GLint     alphaLoc        = -1;
    GLint     resolutionLoc   = -1; // vec2: current monitor pixel size
    GLint     surfaceSizeLoc  = -1; // vec2: window size (logical px); 0,0 for layers
    GLint     mouseLoc        = -1; // vec2: pointer position
    GLint     isActiveLoc     = -1; // float 0/1
    GLint     isFloatingLoc   = -1; // float 0/1
    GLint     isFullscreenLoc = -1; // float 0/1
    GLint     progressLoc     = -1; // float 0..1 across a one-shot open/close animation
    GLint     seedLoc         = -1; // float 0..1, stable per window, so instances differ
    // Corner rounding, re-applied by the wrapper. Hyprland rounds inside the
    // fragment program this shader replaces, so without these a shaded window
    // comes out with square corners.
    GLint     boxSizeLoc      = -1; // vec2: size in px of the box being drawn
    GLint     roundLoc        = -1; // float: corner radius in px, 0 = no rounding
    GLint     roundPowerLoc   = -1; // float: superellipse exponent
    bool      usesTime        = false;
    // Seconds the one-shot animation should run for, declared in the shader as
    // `// @duration 0.35`. <0 means the shader didn't say.
    float     animDuration    = -1.0f;
    time_t    sourceMtime     = 0; // mtime at compile time; lets us auto-evict on edit
};

// Reserved key: a layer entry stored under this namespace applies to any layer
// with no entry of its own. Layers have no rule/tag system to express a
// catch-all with, and no client would pick this as a real namespace.
inline constexpr const char* LAYER_CATCH_ALL = "*";

extern std::map<std::string, std::string>          g_mLayerNamespaceShaderMap;
extern std::map<std::string, std::string>          g_mWindowClassShaderMap;

// Layer surfaces (bars, notifications, rofi/wofi) have no rule tags in v0.56, so
// their animations are keyed by namespace like the rest of the layer API rather
// than by a windowrule tag. `duration` <0 means "ask the shader".
struct AnimSpec {
    std::string path;
    float       duration = -1.0f;
};
extern std::map<std::string, AnimSpec>             g_mLayerOpenAnims;
extern std::map<std::string, AnimSpec>             g_mLayerCloseAnims;

// Splits "<path>[@<seconds>]". The suffix is only consumed if it parses as a
// number in range, so a path that happens to contain '@' still works.
AnimSpec parseAnimSpec(const std::string& arg);
extern std::map<std::string, CompiledShader>       g_mCompiledCShaders;
// Paths whose last compile attempt failed, keyed to the mtime at failure. Lets
// us suppress notification spam (one toast per broken edit, not per frame) and
// automatically retry once the user saves a fix.
extern std::map<std::string, time_t>               g_mFailedShaderMtimes;
// Animation shaders we've already warned about for missing `// @duration`,
// keyed to the mtime at warn time. Same one-toast-per-edit contract as the
// failure cache above: warn once, then stay quiet until the file is saved again.
extern std::map<std::string, time_t>               g_mDurationWarnedMtimes;

// --- HOOK POINTERS ---
// V0.55: hook the concrete renderer that draws texture pass elements
extern CFunctionHook* g_pGLDrawTexHook;
extern CFunctionHook* g_pUseShaderHook;
// Optional — close animations degrade to "off" if either symbol is missing,
// rather than taking the whole plugin down.
extern CFunctionHook* g_pFadeoutCreateHook;
extern CFunctionHook* g_pFadeoutDoneHook;
extern CFunctionHook* g_pLayerFadeoutCreateHook;
extern CFunctionHook* g_pLayerFadeoutDoneHook;

// --- ACTIVE RENDER CONTEXT ---
// Set by hkGLDrawTex before delegating; consumed by hkUseShader during the call.
// Weak refs avoid lifetime hazards if Hyprland tears down a surface mid-call.
//
// Deliberately NOT thread_local: rendering is main-thread-only (the shader maps
// above are plain globals accessed from these same hooks with no locking), and a
// thread_local whose type has a non-trivial destructor emits a
// __cxa_thread_atexit registration that takes a reference on this .so. glibc
// then refuses to unmap it on dlclose, so `hyprctl plugin load` on the same path
// hands back the STALE still-mapped build and silently keeps running the old
// code. Keep these plain globals or live-reload breaks. See build.sh.
extern PHLWINDOWREF        g_pCurrentRenderWindow;
extern PHLLSREF            g_pCurrentRenderLayer;
// Cached compiled-shader pointer for the current draw. Set by hkGLDrawTex (one
// resolve+compile lookup per surface), consumed by hkUseShader so it can apply
// the shader and push uniforms without doing a second find on g_mCompiledCShaders.
extern CompiledShader*     g_pCurrentCompiledShader;
// Progress (0..1) of the one-shot animation covering the surface being drawn,
// or <0 when none applies. Resolved in hkGLDrawTex, pushed by hkUseShader.
extern float               g_pCurrentAnimProgress;
// Per-instance random seed for the current draw, or <0 to derive one from the
// render context. Carried explicitly because a closing window's fadeout has no
// window pointer left to hash.
extern float               g_pCurrentAnimSeed;
// Rounding parameters for the element being drawn, taken from the pass element
// so they match exactly what Hyprland would have applied. Zeroed for offscreen
// stages: the mask belongs to the final on-screen draw, applying it per layer
// would cut the corners repeatedly and leave them ragged.
extern Vector2D            g_pCurrentBoxSize;
extern float               g_pCurrentRound;
extern float               g_pCurrentRoundPower;

// --- FUNCTION DECLARATIONS ---
CompiledShader*                             getOrCompileShader(const std::string& shaderPath);
void                                        hkGLDrawTex(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage);
Hyprutils::Memory::CWeakPointer<CShader>    hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);
void                                        applyShaderRulesSafe(PHLWINDOW pWindow);

// Close-animation hooks. hkFadeoutCreate tags a new fadeout with the closing
// window's shader; hkFadeoutDone holds that fadeout open until the shader has
// had its full duration.
Hyprutils::Memory::CSharedPointer<Desktop::CWindowFadeout>
     hkFadeoutCreate(PHLWINDOW window, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer> snapshot, float sourceAlpha);
bool hkFadeoutDone(void* thisptr);

// Layer-surface equivalents. CLayerFadeout mirrors CWindowFadeout exactly, and
// both derive from IFadeout, so everything downstream of creation — the anim
// map, the snapshot matching, the hold-open logic — is shared.
Hyprutils::Memory::CSharedPointer<Desktop::CLayerFadeout>
     hkLayerFadeoutCreate(PHLLS layer, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer> snapshot, float sourceAlpha);
bool hkLayerFadeoutDone(void* thisptr);

// Maps a pointer to a stable 0..1 value, so each window's animation differs.
float animSeedFor(const void* p);