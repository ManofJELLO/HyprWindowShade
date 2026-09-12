#include "Globals.hpp"
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/supplementary/DragController.hpp>
#include <algorithm>
#include <cmath>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <string_view>

#include <cstdio>

// --- ACTIVE RENDER CONTEXT ---
// Plain globals, not thread_local — see the note in Globals.hpp. A thread_local
// with a non-trivial destructor pins this .so against dlclose and breaks
// live-reload; rendering is main-thread-only, so TLS bought nothing here.
PHLWINDOWREF        g_pCurrentRenderWindow;
PHLLSREF            g_pCurrentRenderLayer;
CompiledShader*     g_pCurrentCompiledShader = nullptr;
float               g_pCurrentAnimProgress   = -1.0f;
float               g_pCurrentAnimSeed       = -1.0f;
const MotionRecord* g_pCurrentMotion         = nullptr;
float               g_pCurrentSettle         = -1.0f;
uint8_t             g_pCurrentOneShotKind    = TRANSFORM_NONE;
Vector2D            g_pCurrentBoxSize;
Vector2D            g_pCurrentElemSize;
CBox                g_pCurrentWindowRect = {0.0, 0.0, 1.0, 1.0};
// The fadeout record being drawn this element, or null. Borrowed for the length
// of one draw only; nothing inserts into g_mFadeoutAnims during a draw.
static FadeoutAnim* g_pCurrentFadeoutAnim = nullptr;
float               g_pCurrentRound          = 0.0f;
float               g_pCurrentRoundPower     = 2.0f;

// --- SHADER STACKING STATE ---
// Allocated on first use and never freed: see the note in Globals.hpp. These
// destructors call into GL that Hyprland may already have torn down at unload.
std::unordered_map<uint64_t, StageFramebuffers>* g_pStageFBs = nullptr;
bool                                             g_bIntermediatePass = false;

// Plugin-relative reference time so the `time` uniform stays in float-precision
// range. steady_clock::now() seconds-since-epoch is in the billions and loses
// sub-second resolution after a few days of uptime.
static const auto g_pluginStartTime = std::chrono::steady_clock::now();

// --- LAYER NAMESPACE LOOKUP ---
// Layer entries are keyed by exact namespace, with `*` reserved as a catch-all
// consulted only when the exact name misses. That ordering is the whole point:
// a specific namespace always beats the catch-all, so unlike window tags — which
// Hyprland keeps in an alphabetically sorted set — there is nothing here whose
// precedence depends on how the shader paths happen to sort.
template <typename Map>
static const typename Map::mapped_type* lookupLayerEntry(const Map& map, const std::string& ns) {
    if (auto it = map.find(ns); it != map.end()) return &it->second;
    if (auto it = map.find(LAYER_CATCH_ALL); it != map.end()) return &it->second;
    return nullptr;
}

// --- PATH RESOLUTION ---
// Returns a pointer into one of the global maps, or nullptr. The pointer is
// valid until the next mutation of the underlying map; render hooks run on the
// main thread between dispatcher/listener invocations so this is safe within a
// single draw chain.
static const std::string* resolveShaderPath(const PHLWINDOW& pWindow, const PHLLS& pLS) {
    if (pWindow) {
        Desktop::View::CWindow* rawWin = pWindow.get();

        // Fullscreen drops everything unless the window opted in, checked before
        // anything else so `+shader_replace:1` cannot become a way around the
        // policy the stacking path enforces. The manual-toggle lookup below used
        // to come first and unconditionally, which let a toggled shader render
        // over a fullscreen window even when `+shader_fullscreen:` named a
        // different one.
        if (Fullscreen::controller()->isFullscreen(pWindow)) {
            auto it = g_mWindowRuleShaders.find(rawWin);
            if (it == g_mWindowRuleShaders.end())  return nullptr;
            if (!it->second.fullscreen.empty())    return &it->second.fullscreen;
            if (!it->second.fullscreenStack)       return nullptr;
            // `+shader_fullscreen_stack:1` — fall through and resolve normally.
        }

        if (auto it = g_mWindowManualShaders.find(rawWin); it != g_mWindowManualShaders.end())
            return &it->second;

        if (auto it = g_mWindowRuleShaders.find(rawWin); it != g_mWindowRuleShaders.end()) {
            const auto& state      = it->second;
            const bool  isActive   = Desktop::focusState()->isWindowActive(pWindow);
            const bool  isFloating = rawWin->m_isFloating;
            if      (isFloating  && !state.floating.empty())  return &state.floating;
            else if (!isFloating && !state.tiled.empty())     return &state.tiled;
            else if (isActive    && !state.active.empty())    return &state.active;
            else if (!isActive   && !state.inactive.empty())  return &state.inactive;
            else if (!state.fallback.empty())                 return &state.fallback;
        }

        const auto& initClass    = rawWin->m_initialClass;
        const auto& currentClass = rawWin->m_class;
        auto classIt = g_mWindowClassShaderMap.find(initClass);
        if (classIt == g_mWindowClassShaderMap.end()) classIt = g_mWindowClassShaderMap.find(currentClass);
        if (classIt != g_mWindowClassShaderMap.end()) return &classIt->second;

        return nullptr;
    }

    if (pLS) {
        if (const std::string* p = lookupLayerEntry(g_mLayerNamespaceShaderMap, pLS->m_namespace))
            return p;
    }
    return nullptr;
}

// --- ONE-SHOT ANIMATIONS ---

// Stable 0..1 value from a pointer, so two windows running the same open
// animation don't play it identically. finalizer from splitmix64 — the PR's
// single multiply left the low bits of consecutive heap addresses correlated,
// which shows up as neighbouring windows animating in lockstep.
float animSeedFor(const void* p) {
    uint64_t h = (uint64_t)(uintptr_t)p;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (float)(h >> 40) / (float)((1u << 24) - 1);
}

AnimSpec parseAnimSpec(const std::string& arg) {
    AnimSpec spec;
    spec.path = arg;

    if (const size_t at = arg.rfind('@'); at != std::string::npos) {
        try {
            const float d = std::stof(arg.substr(at + 1));
            // Clamped, not rejected: `@10` means "as long as you'll allow", and
            // silently falling back to 0.3s is the least useful reading of it.
            if (d > 0.0f) {
                spec.duration = std::min(d, MAX_ANIM_DURATION);
                spec.path     = arg.substr(0, at);
            }
        } catch (...) {}
    }
    return spec;
}

static float secondsSince(const std::chrono::steady_clock::time_point& t) {
    return std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - t).count();
}

// A shader that binds `progress` is unambiguously written as an animation, so
// reaching the default because it forgot to say how long it wants to run is an
// oversight worth surfacing rather than silently papering over. Shaders that
// don't bind `progress` stay quiet — the duration barely matters for those.
// Deduped by mtime so it's one toast per edit, not one per frame.
static void warnMissingDuration(const std::string& path, time_t mtime) {
    if (auto it = g_mDurationWarnedMtimes.find(path); it != g_mDurationWarnedMtimes.end() && it->second == mtime)
        return;
    g_mDurationWarnedMtimes[path] = mtime;

    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "%.3g", DEFAULT_ANIM_DURATION);

    HyprlandAPI::addNotification(PHANDLE,
                                 "[HyprWindowShade] " + path + "\nuses `progress` but declares no `// @duration <sec>` — defaulting to " +
                                     fallback + "s.",
                                 CHyprColor(1.0f, 0.7f, 0.2f, 1.0f), 8000.0f);
}

// Duration precedence: an explicit `@<sec>` on the window rule wins, then the
// shader's own `// @duration`, then the built-in default. The shader is the
// natural home for this — a dissolve and a CRT collapse want different lengths,
// and the binding shouldn't have to restate what the effect already knows.
//
// Only ever called from the draw path, so a compile here has a live GL context.
static float resolveAnimDuration(const std::string& path, float ruleOverride) {
    // An explicit override is unambiguous — nothing to warn about.
    if (ruleOverride > 0.0f) return std::min(ruleOverride, MAX_ANIM_DURATION);

    const CompiledShader* cs = getOrCompileShader(path);
    if (cs && cs->animDuration > 0.0f) return std::min(cs->animDuration, MAX_ANIM_DURATION);

    // Broken shaders (cs == nullptr) already toast their compile error; don't
    // pile a second notification on top of it.
    if (cs && cs->progressLoc >= 0) warnMissingDuration(path, cs->sourceMtime);

    return DEFAULT_ANIM_DURATION;
}

// Open animation for a live window. Returns the shader to use and sets the
// progress/seed globals, or nullptr once the animation has run its course (at
// which point the window reverts to its normal shader).
static const std::string* resolveOpenAnim(const PHLWINDOW& pWindow) {
    if (g_mWindowOpenTimes.empty()) return nullptr;

    Desktop::View::CWindow* rawWin = pWindow.get();
    auto                    tIt    = g_mWindowOpenTimes.find(rawWin);
    if (tIt == g_mWindowOpenTimes.end()) return nullptr;

    // Resolved here rather than when the window opened: window.open and the
    // rule-application event have no guaranteed order, but by the time we're
    // drawing the window its rules are definitely in place.
    auto rIt = g_mWindowRuleShaders.find(rawWin);
    if (rIt == g_mWindowRuleShaders.end() || rIt->second.openAnim.empty()) {
        g_mWindowOpenTimes.erase(tIt);
        return nullptr;
    }

    const std::string& path     = rIt->second.openAnim;
    const float        duration = resolveAnimDuration(path, rIt->second.openAnimDuration);
    const float        elapsed  = secondsSince(tIt->second);

    if (elapsed >= duration) {
        g_mWindowOpenTimes.erase(tIt);
        return nullptr;
    }

    g_pCurrentAnimProgress = elapsed / duration;
    g_pCurrentAnimSeed     = animSeedFor(rawWin);
    return &path;
}

// Same idea for layer surfaces, but keyed by namespace rather than a rule tag —
// layer rules carry no tags in v0.56, so this follows the existing layer API.
static const std::string* resolveLayerOpenAnim(const PHLLS& pLS) {
    if (g_mLayerOpenTimes.empty()) return nullptr;

    Desktop::View::CLayerSurface* rawLS = pLS.get();
    auto                          tIt   = g_mLayerOpenTimes.find(rawLS);
    if (tIt == g_mLayerOpenTimes.end()) return nullptr;

    const AnimSpec* spec = lookupLayerEntry(g_mLayerOpenAnims, pLS->m_namespace);
    if (!spec || spec->path.empty()) {
        g_mLayerOpenTimes.erase(tIt);
        return nullptr;
    }

    const std::string& path     = spec->path;
    const float        duration = resolveAnimDuration(path, spec->duration);
    const float        elapsed  = secondsSince(tIt->second);

    if (elapsed >= duration) {
        g_mLayerOpenTimes.erase(tIt);
        return nullptr;
    }

    g_pCurrentAnimProgress = elapsed / duration;
    g_pCurrentAnimSeed     = animSeedFor(rawLS);
    return &path;
}

// Drops records whose fadeout no longer exists. The keys are raw pointers, so a
// stale entry could otherwise be matched by a future fadeout allocated at the
// same address. Cheap: the list is the set of windows closing right now.
static void pruneFadeoutAnims() {
    if (g_mFadeoutAnims.empty()) return;

    auto& state = Desktop::fadingOutState();
    if (!state) {
        g_mFadeoutAnims.clear();
        return;
    }

    const auto& live = state->fadeouts();
    for (auto it = g_mFadeoutAnims.begin(); it != g_mFadeoutAnims.end();) {
        bool found = false;
        for (const auto& f : live) {
            if (f.get() == it->first) { found = true; break; }
        }
        it = found ? std::next(it) : g_mFadeoutAnims.erase(it);
    }
}

// Close animation. The window is already gone; what's being drawn is the
// snapshot framebuffer Hyprland captured for the fadeout, so we identify it by
// exact texture identity rather than guessing from geometry.
static const std::string* resolveCloseAnim(const SP<Render::ITexture>& tex, PHLMONITOR& outMonitor) {
    if (g_mFadeoutAnims.empty() || !tex) return nullptr;

    auto& state = Desktop::fadingOutState();
    if (!state) return nullptr;

    for (const auto& f : state->fadeouts()) {
        if (!f) continue;
        const auto fb = f->framebuffer();
        if (!fb || fb->getTexture().get() != tex.get()) continue;

        auto it = g_mFadeoutAnims.find(f.get());
        if (it == g_mFadeoutAnims.end()) return nullptr;

        FadeoutAnim& anim = it->second;
        if (anim.duration < 0.0f) anim.duration = resolveAnimDuration(anim.path, -1.0f);

        const float elapsed = secondsSince(anim.start);
        g_pCurrentAnimProgress = anim.duration > 0.0f ? std::min(elapsed / anim.duration, 1.0f) : 1.0f;
        g_pCurrentAnimSeed     = anim.seed;
        outMonitor             = f->monitor().lock();
        g_pCurrentFadeoutAnim  = &anim;

        // `surface_size` for a snapshot. The element's box is renderBox(), which is
        // `m_transformedSize * (m_realSize / m_sourceSize)` — the monitor's size
        // times a factor windowsOut animates all the way down. Measured over one
        // close: 2560x1080 on the first drawn frame, 14x11 on the last. A shader
        // converting pixels to texels through `1.0 / surface_size` therefore had
        // its divisor shrink by two orders of magnitude mid-animation.
        //
        // The texture's own size is the honest answer, and it is constant for the
        // life of the fade. v_texcoord spans the TEXTURE, not the box it is drawn
        // into: Hyprland reconciles the two by composing MONITOR_INVERTED into
        // glMatrix, which transforms positions only, while the UVs come from the
        // untransformed unit quad. m_transformedSize was the tempting answer and
        // is wrong on exactly the monitors this matters on — at 90 degrees the
        // snapshot texture is the panel, 2560x1080, while m_transformedSize is
        // 1080x2560, so it had the axes the wrong way round.
        g_pCurrentElemSize = tex->m_size;

        // The captured rect is monitor-local device px in TRANSFORMED space, and
        // the texture is in panel space, so the monitor transform has to be
        // applied before normalising — the two differ by a transpose at 90/270.
        // Verified on hardware before this was written: a window at (69,1049)
        // 942x511 on a 90-degree monitor previously produced a window_rect whose
        // block rendered at (438,322) 184x1922, x and y swapped.
        //
        // Hyprutils' own box transform does the mapping, the same call
        // CMonitor::onConnect uses to go from transformed space to panel space.
        // invertTransform matches the direction Hyprland composes into glMatrix.
        // At transform 0 this is the identity, so the ordinary case is untouched.
        if (anim.srcSize.x > 0 && anim.srcSize.y > 0 && outMonitor) {
            const Vector2D xfmd = outMonitor->m_transformedSize;
            const Vector2D texSz = tex->m_size;
            if (xfmd.x > 0 && xfmd.y > 0 && texSz.x > 0 && texSz.y > 0) {
                CBox r{anim.srcPos.x, anim.srcPos.y, anim.srcSize.x, anim.srcSize.y};
                r.transform(Math::wlTransformToHyprutils(Math::invertTransform(outMonitor->m_transform)), xfmd.x, xfmd.y);
                g_pCurrentWindowRect = CBox{r.x / texSz.x, r.y / texSz.y, r.w / texSz.x, r.h / texSz.y};
            }
        }
        return &anim.path;
    }
    return nullptr;
}

// --- WINDOW TRANSFORMS (move / resize) ---

// Below this, two ticks landed close enough together that differentiating over
// the gap would amplify noise instead of measuring anything. The render stage
// this is driven from fires once per *monitor*, so a two-monitor frame ticks
// every window twice microseconds apart; without this the second tick would
// overwrite a good velocity with a meaningless one.
static constexpr float MIN_VELOCITY_DT = 0.002f; // 2ms — well under any real frame

// Time constant for smoothing drag velocity. Measured at ~100Hz, 64 of 240
// frames during a real drag carried no new pointer event, so the raw signal
// alternates between real values and hard zeros. 45ms rides over those gaps
// without visibly lagging the cursor.
static constexpr float DRAG_VELOCITY_TAU = 0.045f;

// How long a latched flavour survives without a motion starting. Long enough to
// bridge the gap between the toggle event and the first animated frame, short
// enough that a toggle which animates nothing does not leave a stale record.
static constexpr float FLAVOUR_GRACE = 0.25f;

// How long an unused offscreen bucket is kept before its framebuffers are
// released, and the hard ceiling on how many are held at once. The TTL is what
// actually bounds the pool; the ceiling only matters if something pathological
// outruns it.
static constexpr float  STAGE_FB_TTL = 2.0f;
static constexpr size_t STAGE_FB_MAX = 32;

// How long one run of an animated variable lasts, in seconds.
static float animVarDuration(const CAnimatedVariable<Vector2D>* av) {
    // A spring has no duration — it runs until it settles — so there is no
    // honest number to report. Shaders are handed -1 rather than a
    // plausible-looking lie they might divide by.
    if (!av || av->isSpringCurve()) return -1.0f;

    // The node itself carries only overrides; the resolved values live behind
    // pValues, which is how the config tree does inheritance. Both are weak refs
    // over shared-owned config objects, so lock() is safe here — unlike the
    // animated variables themselves, which are UP-owned and would abort.
    const auto cfg = av->getConfig().lock();
    if (!cfg) return -1.0f;
    const auto vals = cfg->pValues.lock();
    if (!vals) return -1.0f;

    // Hyprland's `animation = <name>, <enabled>, <speed>, ...` is in deciseconds.
    return vals->internalSpeed > 0.0f ? vals->internalSpeed / 10.0f : -1.0f;
}

// Samples every window's motion once per frame. Driven from RENDER_PRE_WINDOWS,
// before any window is drawn, so all surfaces in the frame read one consistent
// snapshot rather than each re-deriving its own.
//
// Everything here keys off the window's own animated variables, never off the
// config. That is deliberate: when `misc:animate_manual_resizes` or
// `misc:animate_mouse_windowdragging` is off, Hyprland warps the window instead
// of animating it, isBeingAnimated() stays false, and no transform shader runs.
// The user's per-operation preference is inherited for free, with no config
// reads and no branching.
void latchOneShot(Desktop::View::CWindow* raw, uint8_t kind) {
    if (!raw) return;
    // Cheap gate: only windows that actually carry a one-shot rule get an
    // entry, so focus changes on ordinary windows cost one map lookup and
    // nothing else. Focus events fire constantly.
    auto rIt = g_mWindowRuleShaders.find(raw);
    if (rIt == g_mWindowRuleShaders.end()) return;
    const auto& st = rIt->second;
    const bool  wanted = (kind == ONESHOT_URGENT  && !st.urgentAnim.empty())
                      || (kind == ONESHOT_FOCUS   && !st.focusAnim.empty())
                      || (kind == ONESHOT_UNFOCUS && !st.unfocusAnim.empty());
    if (!wanted) return;

    g_mWindowOneShots[raw] = OneShotAnim{kind, std::chrono::steady_clock::now()};
}

// One-shot animation for a live window: an attention pulse or a focus
// transition. Mirrors resolveOpenAnim exactly — there is no compositor
// animation to ride, so the shader declares its own duration and this runs it
// down and clears itself.
static const std::string* resolveOneShotAnim(const PHLWINDOW& pWindow, bool urgentOnly) {
    if (g_mWindowOneShots.empty()) return nullptr;

    Desktop::View::CWindow* raw = pWindow.get();
    auto oIt = g_mWindowOneShots.find(raw);
    if (oIt == g_mWindowOneShots.end()) return nullptr;

    // Urgency is an alert and outranks motion; a focus flicker does not, and is
    // asked for separately once the transform paths have declined.
    if (urgentOnly && oIt->second.kind != ONESHOT_URGENT) return nullptr;

    auto rIt = g_mWindowRuleShaders.find(raw);
    if (rIt == g_mWindowRuleShaders.end()) { g_mWindowOneShots.erase(oIt); return nullptr; }
    const auto& state = rIt->second;

    const std::string* path = nullptr;
    float              dur  = -1.0f;
    uint8_t            kind = TRANSFORM_NONE;
    switch (oIt->second.kind) {
        case ONESHOT_URGENT:  path = &state.urgentAnim;  dur = state.urgentDuration;  kind = TRANSFORM_URGENT;  break;
        case ONESHOT_FOCUS:   path = &state.focusAnim;   dur = state.focusDuration;   kind = TRANSFORM_FOCUS;   break;
        case ONESHOT_UNFOCUS: path = &state.unfocusAnim; dur = state.unfocusDuration; kind = TRANSFORM_UNFOCUS; break;
        default: break;
    }
    if (!path || path->empty()) { g_mWindowOneShots.erase(oIt); return nullptr; }

    const float duration = resolveAnimDuration(*path, dur);
    const float elapsed  = secondsSince(oIt->second.start);
    if (elapsed >= duration) { g_mWindowOneShots.erase(oIt); return nullptr; }

    g_pCurrentAnimProgress = elapsed / duration;
    g_pCurrentSettle       = -1.0f;
    g_pCurrentAnimSeed     = animSeedFor(raw);
    g_pCurrentOneShotKind  = kind;
    return path;
}

// Records what kind of change is about to cause a motion. Called from the
// fullscreen/floating listeners, which fire once at the toggle; the motion they
// cause starts on this frame or the next.
void latchTransformFlavour(Desktop::View::CWindow* raw, uint8_t flavour) {
    if (!raw) return;
    // Only windows carrying rules can act on a flavour — resolveTransformAnim
    // returns immediately without a rule entry — so do not mint a motion record
    // for every fullscreen or float toggle on the desktop just to prune it again
    // moments later.
    if (g_mWindowRuleShaders.find(raw) == g_mWindowRuleShaders.end()) return;

    auto& rec     = g_mWindowMotion[raw];
    rec.flavour   = flavour;
    rec.flavourAt = std::chrono::steady_clock::now();
}

// The window the user is physically dragging right now, or nullptr. Asked once
// per frame rather than per window: the drag controller is global state, and a
// drag involves exactly one target.
static Desktop::View::CWindow* draggedWindow(eMouseBindMode& outMode) {
    outMode = MBIND_INVALID;
    if (!g_layoutManager) return nullptr;
    const auto& dc = g_layoutManager->dragController();
    if (!dc) return nullptr;
    outMode = dc->mode();
    if (outMode == MBIND_INVALID) return nullptr;
    const auto t = dc->target();
    if (!t) return nullptr;
    const PHLWINDOW w = t->window();
    return w ? w.get() : nullptr;
}

void updateMotionRecords() {
    const auto& state = Desktop::windowState();
    if (!state) return;

    const auto     now        = std::chrono::steady_clock::now();
    eMouseBindMode dragMode   = MBIND_INVALID;
    const auto     draggedRaw = draggedWindow(dragMode);

    for (const auto& w : state->windows()) {
        if (!w) continue;
        Desktop::View::CWindow* raw = w.get();

        auto&      posAnim  = w->positionAnimation();
        auto&      sizeAnim = w->sizeAnimation();
        // Reference, not a copy: PHLWORKSPACE is a shared pointer, and copying
        // it for every window on every frame is pure atomic refcount traffic.
        const auto& ws      = w->m_workspace;
        const bool moving   = posAnim  && posAnim->isBeingAnimated();
        const bool resizing = sizeAnim && sizeAnim->isBeingAnimated();
        const bool wsMoving = ws && ws->m_renderOffset && ws->m_renderOffset->isBeingAnimated();

        // Hyprland warps a drag, so none of the three flags above are set while
        // one is in progress — the drag has to be part of the liveness test in
        // its own right. Without it a window with no existing record was
        // skipped here and its `dragging` flag never assigned, so a cold drag
        // on a window that had not moved recently did nothing at all. Every
        // drag that appeared to work did so only because a float toggle or an
        // earlier move had left a record alive.
        const bool isDragged = (raw == draggedRaw);

        auto it = g_mWindowMotion.find(raw);

        // Fast path: on any given frame nearly every window is idle. A few bool
        // reads and a miss, then out.
        if (!moving && !resizing && !wsMoving && !isDragged && it == g_mWindowMotion.end()) continue;

        if (it == g_mWindowMotion.end())
            it = g_mWindowMotion.emplace(raw, MotionRecord{}).first;

        MotionRecord&  rec  = it->second;
        rec.dragging        = isDragged;

        // A window's on-screen position is its own position PLUS its
        // workspace's render offset — during a switch the window does not move
        // at all, the whole workspace slides. Folding the offset in here means
        // velocity is always the true on-screen velocity, with one formula
        // covering moves, drags and workspace transitions alike. Outside a
        // switch the offset is (0,0), so nothing changes for the other paths.
        //
        // m_workspace is frequently null — measured, 56 of 60 sampled windows
        // (unmapped Steam surfaces) had none — so this is not a defensive
        // maybe, it is the common case.
        const Vector2D wsOff  = (ws && ws->m_renderOffset) ? ws->m_renderOffset->value() : Vector2D(0, 0);
        const Vector2D ownPos = posAnim ? posAnim->value() : Vector2D(0, 0);
        const Vector2D pos    = ownPos + wsOff;
        const Vector2D size   = sizeAnim ? sizeAnim->value() : Vector2D(0, 0);

        if (!rec.haveSample) {
            rec.pos        = pos;
            rec.size       = size;
            rec.sampledAt  = now;
            rec.haveSample = true;
        } else if (const float dt = std::chrono::duration_cast<std::chrono::duration<float>>(now - rec.sampledAt).count();
                   dt >= MIN_VELOCITY_DT) {
            const Vector2D instantV  = (pos  - rec.pos)  / dt;
            const Vector2D instantSV = (size - rec.size) / dt;

            // A compositor-driven animation is already smooth, so it is fed
            // straight through — smoothing there would only add lag. A drag is
            // not: it is sampled from discrete pointer events, and measured at
            // ~100Hz roughly a quarter of frames carry no new event at all, so
            // the raw signal alternates between real values and hard zeros. A
            // shader cannot fix that itself (no memory between frames), so the
            // exponential average lives here. Time-constant based, so it
            // behaves the same at any refresh rate.
            if (rec.dragging) {
                const float a    = 1.0f - std::exp(-dt / DRAG_VELOCITY_TAU);
                rec.velocity     = rec.velocity     + (instantV  - rec.velocity)     * a;
                rec.sizeVelocity = rec.sizeVelocity + (instantSV - rec.sizeVelocity) * a;
            } else {
                rec.velocity     = instantV;
                rec.sizeVelocity = instantSV;
            }
            rec.pos          = pos;
            rec.size         = size;
            rec.sampledAt    = now;
        }

        const bool wasInMotion = rec.moving || rec.resizing || rec.wasDragging;
        rec.moving             = moving;
        rec.resizing           = resizing;
        if (moving || resizing || wsMoving) rec.lastAnimated = now;

        // Hyprland WARPS an interactive drag — measured: goal == value on every
        // frame of a 240-frame capture, and isBeingAnimated() never true, even
        // with misc:animate_mouse_windowdragging on. So a drag has to be driven
        // off the drag controller instead. There is no progress, curve or
        // duration to report for it; velocity is the whole signal.
        const bool dragMoving   = rec.dragging && dragMode == MBIND_MOVE;
        const bool dragResizing = rec.dragging && dragMode != MBIND_MOVE && dragMode != MBIND_INVALID;
        const bool inMotion     = moving || resizing || wsMoving || rec.dragging;

        if (inMotion) {
            // A new gesture starts its own peak; carrying the previous one over
            // would let a fast move leave a loud settle on the slow one after it.
            if (!wasInMotion) {
                rec.peakVelocity     = Vector2D(0, 0);
                rec.peakSizeVelocity = Vector2D(0, 0);
                rec.settleTail       = -1.0f;

                // A flavour belongs to the motion its toggle caused, and was
                // only ever cleared by the record being dropped. A record
                // survives its settle, so a plain move started during that
                // settle inherited the previous toggle's flavour — replaying a
                // fullscreen-exit shader for an ordinary move, or worse, being
                // silently suppressed by a stale fullscreen-ENTER. If nothing
                // latched a flavour for THIS gesture, it is an ordinary one.
                const float sinceLatch =
                    std::chrono::duration_cast<std::chrono::duration<float>>(now - rec.flavourAt).count();
                if (sinceLatch > FLAVOUR_GRACE) rec.flavour = FLAVOUR_NONE;
            }
            if (rec.velocity.distanceSq(Vector2D(0, 0)) > rec.peakVelocity.distanceSq(Vector2D(0, 0)))
                rec.peakVelocity = rec.velocity;
            if (rec.sizeVelocity.distanceSq(Vector2D(0, 0)) > rec.peakSizeVelocity.distanceSq(Vector2D(0, 0)))
                rec.peakSizeVelocity = rec.sizeVelocity;

            rec.settling = false;

            // A tiling reflow moves AND resizes at once, and only one of them
            // can own the single animation slot in the stack. Position wins:
            // travel across the screen is the more visible of the two. Both
            // `is_moving` and `is_resizing` are still reported truthfully, so a
            // shader that cares about the other one can still see it.
            // A workspace switch outranks the window's own animation: the whole
            // surface is travelling, which is the more visible motion, and
            // measured the window's own animvars stay idle through one anyway.
            const CAnimatedVariable<Vector2D>* driver =
                wsMoving ? ws->m_renderOffset.get()
                         : (moving ? posAnim.get() : (resizing ? sizeAnim.get() : nullptr));

            if (driver) {
                rec.kind     = wsMoving ? TRANSFORM_WORKSPACE : (moving ? TRANSFORM_MOVE : TRANSFORM_RESIZE);
                rec.progress = driver->getPercent();
                rec.curve    = driver->getCurveValue();
                rec.duration = animVarDuration(driver);
            } else {
                // Drag-only: nothing is animating, so there is no progress to
                // report. Say so honestly rather than leaving the last
                // animation's values in place, which a shader could not tell
                // apart from a live transform. `is_dragging` is how a shader
                // knows to ignore them and use velocity instead.
                rec.kind     = dragMoving ? TRANSFORM_MOVE : TRANSFORM_RESIZE;
                rec.progress = 1.0f;
                rec.curve    = 1.0f;
                rec.duration = -1.0f;
            }

            // Reported truthfully during a drag as well: the window really is
            // moving or resizing, even though no animation is driving it.
            rec.moving   = moving   || dragMoving || wsMoving;
            rec.resizing = resizing || dragResizing;


            // Refresh only the axis that is actually animating. Updating both
            // unconditionally meant a resize-only transform reported the
            // position animvar's LAST COMPLETED trip as though it were current,
            // so `move_delta` handed the shader a stale vector from some
            // earlier move — plausible-looking, and undetectable from inside
            // GLSL. Collapsing the idle axis onto its current value makes its
            // delta and remaining both read as zero, which is the truth.
            // A drag has no trip at all: measured, begun/goal are one mouse
            // event apart, so reporting them as move_delta would hand the
            // shader a few pixels of jitter dressed up as a whole gesture.
            // Expressed in the same on-screen space as rec.pos, which already
            // has the offset folded in — otherwise move_remaining (goal - pos)
            // would subtract a window position from a workspace offset and
            // produce a number in no coordinate space at all.
            if (wsMoving)                           { rec.begun = ownPos + ws->m_renderOffset->begun();
                                                      rec.goal  = ownPos + ws->m_renderOffset->goal(); }
            else if (moving && posAnim && !rec.dragging) { rec.begun = posAnim->begun(); rec.goal = posAnim->goal(); }
            else                                    { rec.begun = rec.goal = rec.pos; }

            if (resizing && sizeAnim && !rec.dragging) { rec.sizeBegun = sizeAnim->begun(); rec.sizeGoal = sizeAnim->goal(); }
            else                                       { rec.sizeBegun = rec.sizeGoal = rec.size; }
            // Deliberately NOT touched once motion ends: the settle tail needs
            // the trip it is settling from, so these stay frozen through it.
        } else if (wasInMotion) {
            // Motion just ended. Freeze the velocity it ended with so a shader
            // can drive its own settle off it, and zero the live one — the
            // window really has stopped, and reporting otherwise would be a lie
            // a shader can't detect.
            rec.settling        = true;
            rec.motionEnd       = now;
            // On drag release a floating window simply stops — the compositor
            // runs no settle animation of its own — so the tail is the only
            // thing that carries the gesture's energy out. Seed it from the
            // peak rather than the final velocity, which is near zero after an
            // eased move and merely noisy at the end of a drag.
            rec.releaseVelocity = rec.velocity;
            rec.moving = rec.resizing = false;
            rec.velocity        = Vector2D(0, 0);
            rec.sizeVelocity    = Vector2D(0, 0);
            rec.progress        = 1.0f;
            rec.curve           = 1.0f;
        }

        // Records are keyed by raw CWindow*, so a stale one could be matched by
        // a future window allocated at the same address — the same hazard
        // pruneFadeoutAnims exists for. Drop anything that stopped moving too
        // long ago for any shader to still be settling on it.
        rec.wasDragging = rec.dragging;

        if (!inMotion) {
            const float since = std::chrono::duration_cast<std::chrono::duration<float>>(now - rec.motionEnd).count();
            // A flavour latched moments ago has not had its motion start yet;
            // dropping the record here would lose it before it is ever used.
            const float sinceFlavour = std::chrono::duration_cast<std::chrono::duration<float>>(now - rec.flavourAt).count();
            const bool  freshFlavour = rec.flavour != FLAVOUR_NONE && sinceFlavour < FLAVOUR_GRACE;
            if (!freshFlavour && (!rec.settling || since > MAX_ANIM_DURATION))
                g_mWindowMotion.erase(it);
        }
    }
}

// True while a window is fullscreen and has not opted into being shaded there.
// Fullscreen is opt-in for animations for the same reason it is for the steady
// state: the window is usually a game or a video, where an effect costs frames
// and misrepresents what is on screen. `+shader_fullscreen_stack:1` is the
// blanket opt-in; `+shader_fullscreen_enter:` / `+shader_fullscreen_exit:` opt
// the two transitions in individually and are resolved before this applies.
static bool fullscreenSuppresses(const PHLWINDOW& pWindow) {
    if (!pWindow || !Fullscreen::controller()->isFullscreen(pWindow)) return false;
    auto it = g_mWindowRuleShaders.find(pWindow.get());
    return it == g_mWindowRuleShaders.end() || !it->second.fullscreenStack;
}

// The stricter form: fullscreen, and nothing has opted this window into being
// shaded *or* animated there, so every path below would resolve to "no shaders"
// anyway. Lets hkGLDrawTex bail before it touches the class, manual and rule
// maps or builds a stack, which is what keeps a fullscreen game costing what it
// would cost with the plugin unloaded. Windows that are not fullscreen pay one
// predictable branch for this; only fullscreen ones reach the lookup.
static bool fullscreenRendersNothing(const PHLWINDOW& pWindow) {
    if (!pWindow || !Fullscreen::controller()->isFullscreen(pWindow)) return false;
    auto it = g_mWindowRuleShaders.find(pWindow.get());
    if (it == g_mWindowRuleShaders.end()) return true;
    const auto& st = it->second;
    return !st.fullscreenStack && st.fullscreen.empty()
        && st.fsEnterAnim.empty() && st.fsExitAnim.empty();
}

// Move/resize animation for a live window. Mirrors resolveOpenAnim, but there is
// no timer to run down: the compositor's own move animation is the clock, so
// this is a pure read of the record sampled above. `progress` follows whatever
// bezier or spring the user configured, and begins and ends exactly with the
// motion — which is why these rules declare no duration.
static const std::string* resolveTransformAnim(const PHLWINDOW& pWindow) {
    if (g_mWindowMotion.empty()) return nullptr;

    auto mIt = g_mWindowMotion.find(pWindow.get());
    if (mIt == g_mWindowMotion.end()) return nullptr;
    MotionRecord& rec = mIt->second;

    auto rIt = g_mWindowRuleShaders.find(pWindow.get());
    if (rIt == g_mWindowRuleShaders.end()) return nullptr;
    const auto& state = rIt->second;

    // While settling there is no live animation to ask, so `kind` carries which
    // one it was — the tail has to play the same shader the motion did.
    const bool idle      = !rec.moving && !rec.resizing;
    const bool isWs      = rec.kind == TRANSFORM_WORKSPACE;
    const bool wantWs    = isWs && (rec.moving || idle);
    const bool wantMove  = !isWs && (rec.moving   || (idle && rec.kind == TRANSFORM_MOVE));
    const bool wantResize = !isWs && (rec.resizing || (idle && rec.kind == TRANSFORM_RESIZE));

    // A workspace slide is checked first: it is reported through is_moving too,
    // so without this a window carrying both rules would play its move shader
    // for a workspace switch.
    const std::string* path       = nullptr;
    float              ruleSettle = -1.0f;

    // Flavour-specific tags outrank the generic move/resize ones: a fullscreen
    // toggle IS a move and a resize, so without this it is indistinguishable
    // from any other motion.
    bool suppressGeneric = false;
    switch (rec.flavour) {
        case FLAVOUR_FULLSCREEN_ENTER:
            // Silent unless explicitly asked for. A window going fullscreen is
            // usually a game or a video, which is the least welcome place for
            // an effect — the same reason `shader_fullscreen:` is opt-in for
            // the steady state. Note this SUPPRESSES the generic move/resize
            // shader rather than falling through to it; entering fullscreen is
            // mechanically a move and a resize, so without this it would
            // animate by default.
            if (!state.fsEnterAnim.empty()) { path = &state.fsEnterAnim; ruleSettle = state.fsEnterSettle; }
            else                            suppressGeneric = true;
            break;
        case FLAVOUR_FULLSCREEN_EXIT:
            // Mirrors ENTER: silent unless explicitly asked for, and it has to
            // suppress the generic move/resize shader rather than relying on a
            // fullscreen check. The window.fullscreen event fires AFTER the flip
            // (see main.cpp), so by the time this flavour is latched the window
            // already reports itself as NOT fullscreen — an isFullscreen() test
            // here would be false for the whole exit and suppress nothing.
            if (!state.fsExitAnim.empty()) { path = &state.fsExitAnim; ruleSettle = state.fsExitSettle; }
            else                           suppressGeneric = true;
            break;
        case FLAVOUR_FLOAT:
            if (!state.floatAnim.empty())  { path = &state.floatAnim;  ruleSettle = state.floatSettle; }
            break;
        case FLAVOUR_TILE:
            if (!state.tileAnim.empty())   { path = &state.tileAnim;   ruleSettle = state.tileSettle; }
            break;
        default: break;
    }
    if (suppressGeneric) return nullptr;

    // Past this point only the generic move/resize/workspace shaders remain, and
    // those are exactly what a fullscreen window should not be playing. The
    // fullscreen transition tags above already returned if they were set, so an
    // explicit `+shader_fullscreen_enter:` / `+shader_fullscreen_exit:` still
    // plays — this only suppresses the generic shader standing in for one.
    //
    // This covers a window that is fullscreen and moving for some other reason —
    // a workspace switch, say. The two fullscreen transitions do not rely on it:
    // ENTER and EXIT both set suppressGeneric above, because the fullscreen flag
    // has already flipped by the time either flavour is latched.
    if (!path && fullscreenSuppresses(pWindow)) return nullptr;

    if (!path && wantWs && !state.workspaceAnim.empty()) {
        path = &state.workspaceAnim; ruleSettle = state.workspaceSettle;
    } else if (!path && wantMove && !state.moveAnim.empty()) {
        path = &state.moveAnim;   ruleSettle = state.moveSettle;
    } else if (!path && wantResize && !state.resizeAnim.empty()) {
        path = &state.resizeAnim; ruleSettle = state.resizeSettle;
    }
    if (!path) return nullptr;

    g_pCurrentAnimSeed = animSeedFor(pWindow.get());

    if (rec.moving || rec.resizing) {
        // getPercent() is a linear time fraction; the eased value rides
        // alongside it as `curve`, which on a spring can legitimately exceed 1
        // while overshooting. Only the time fraction is clamped.
        //
        // A drag reaches here too. Hyprland warps those, so rec.progress was
        // set to 1.0 upstream rather than left holding a stale animation's
        // value — a dragged window is not partway through anything.
        g_pCurrentAnimProgress = std::clamp(rec.progress, 0.0f, 1.0f);
        g_pCurrentSettle       = -1.0f;
        return path;
    }

    if (!rec.settling) return nullptr;

    // Settle length precedence matches the rest of the plugin: an explicit
    // `@<sec>` on the rule wins, then the shader's own `// @settle`, then none.
    //
    // Resolved once per settle and cached. getOrCompileShader() stat()s the
    // file to check for edits, and this runs per textured surface per frame —
    // so without the cache a one-second settle on a window with a few
    // subsurfaces was hundreds of syscalls for a value that cannot change
    // mid-settle.
    if (rec.settleTail < 0.0f) {
        float tail = ruleSettle;
        if (tail < 0.0f) {
            const CompiledShader* cs = getOrCompileShader(*path);
            tail                     = cs ? cs->settleDuration : 0.0f;
        }
        rec.settleTail = std::max(tail, 0.0f);
    }
    const float tail = rec.settleTail;
    if (tail <= 0.0f) return nullptr;

    const float since = secondsSince(rec.motionEnd);
    if (since >= tail) return nullptr;

    g_pCurrentAnimProgress = 1.0f;
    g_pCurrentSettle       = since / tail;
    return path;
}

// --- SHADER STACKING ---

// True when this window asked to opt out of stacking and back to the old
// first-match-wins ladder.
static bool windowReplaceMode(const PHLWINDOW& pWindow) {
    auto it = g_mWindowRuleShaders.find(pWindow.get());
    return it != g_mWindowRuleShaders.end() && it->second.replaceMode;
}

// Collects the shader layers for a window or layer surface, bottom first.
//
// The plugin's general rule is that shading sources STACK — they never stand in
// for one another. Whatever a window picks up from its class, from an always-on
// rule, from an imperative toggle and from its focus or geometry state all
// composite together, and layer surfaces follow the same rule for their
// catch-all and namespace entries. If two sources disagree about how a surface
// should look, the answer is both, in a defined order, not the more specific one
// winning. `+shader_replace:1` is the single opt-out, handled by the caller.
//
// The order is the one users can reason about, coarsest first: the class-wide
// look, then the always-on rule, then an imperative toggle, then the
// geometry-conditional layer, then the focus-conditional layer, then fullscreen
// on top. A one-shot animation is added above all of these by the caller, since
// it can also apply to a fadeout that has no window left.
//
// Returns how many entries were written. Never exceeds MAX_SHADER_STAGES - 1,
// leaving room for the animation.
static int collectBaseLayers(const PHLWINDOW& pWindow, const PHLLS& pLS, const std::string* out[MAX_SHADER_STAGES]) {
    int n = 0;

    if (pWindow) {
        Desktop::View::CWindow* rawWin = pWindow.get();

        // Every source of shading contributes its own layer. Nothing here
        // replaces anything else: a shader set by class, by an always-on rule,
        // by an imperative toggle and by focus state all composite, in that
        // order, and the caller adds the one-shot animation on top. The only
        // opt-out is `+shader_replace:1`, handled by the caller.
        const std::string* classShader = nullptr;
        {
            const auto& initClass    = rawWin->m_initialClass;
            const auto& currentClass = rawWin->m_class;
            auto        classIt      = g_mWindowClassShaderMap.find(initClass);
            if (classIt == g_mWindowClassShaderMap.end()) classIt = g_mWindowClassShaderMap.find(currentClass);
            if (classIt != g_mWindowClassShaderMap.end()) classShader = &classIt->second;
        }

        const std::string* manualShader = nullptr;
        if (auto it = g_mWindowManualShaders.find(rawWin); it != g_mWindowManualShaders.end())
            manualShader = &it->second;

        const bool isFullscreen = Fullscreen::controller()->isFullscreen(pWindow);

        if (auto it = g_mWindowRuleShaders.find(rawWin); it != g_mWindowRuleShaders.end()) {
            const auto& state      = it->second;
            const bool  isActive   = Desktop::focusState()->isWindowActive(pWindow);
            const bool  isFloating = rawWin->m_isFloating;

            // Fullscreen drops EVERYTHING by default — rule layers, class
            // shaders and imperative toggles alike. A fullscreen window is
            // usually a game or a video: an effect there costs frames, and it
            // misrepresents what the window is actually trying to show. So
            // fullscreen is opt-in rather than opt-out, and the opt-in has to be
            // deliberate: `+shader_fullscreen:` names one shader for the
            // fullscreen state, and `+shader_fullscreen_stack:1` restores the
            // whole normal stack for a window that genuinely wants it.
            if (isFullscreen && !state.fullscreenStack) {
                if (!state.fullscreen.empty())                  out[n++] = &state.fullscreen;
            } else {
                if (classShader)                                out[n++] = classShader;
                if (!state.fallback.empty())                    out[n++] = &state.fallback;
                if (manualShader)                               out[n++] = manualShader;
                if (isFloating)  { if (!state.floating.empty())  out[n++] = &state.floating; }
                else             { if (!state.tiled.empty())     out[n++] = &state.tiled; }
                if (isActive)    { if (!state.active.empty())    out[n++] = &state.active; }
                else             { if (!state.inactive.empty())  out[n++] = &state.inactive; }
                if (isFullscreen && !state.fullscreen.empty())   out[n++] = &state.fullscreen;
            }
        } else if (!isFullscreen) {
            // No rules at all, so there is nothing to opt this window into
            // being shaded while fullscreen — and opting in is the only way.
            if (classShader)                                    out[n++] = classShader;
            if (manualShader)                                   out[n++] = manualShader;
        }

        return n;
    }

    if (pLS) {
        // Layers follow the same invariant as windows: shading sources stack
        // rather than replace. A catch-all shader is the coarser of the two, so
        // it goes down first and a namespace-specific one composites over it,
        // instead of the specific entry standing in for the catch-all.
        if (auto it = g_mLayerNamespaceShaderMap.find(LAYER_CATCH_ALL); it != g_mLayerNamespaceShaderMap.end())
            out[n++] = &it->second;

        if (pLS->m_namespace != LAYER_CATCH_ALL) {
            if (auto it = g_mLayerNamespaceShaderMap.find(pLS->m_namespace); it != g_mLayerNamespaceShaderMap.end())
                out[n++] = &it->second;
        }
    }

    return n;
}

// Runs `count` stages offscreen, each one sampling the previous stage's output
// through its own `tex`, and returns the texture holding the result. The caller
// then hands that texture to Hyprland's own draw with the top stage bound, so
// geometry, rounding, damage and blending are still done by the compositor
// exactly as they were.
//
// Returns nullptr when the chain can't run, in which case the caller falls back
// to drawing the top stage alone — degraded, but never broken.
static SP<Render::ITexture> runIntermediateStages(CompiledShader* const* stages, int count, const SP<Render::ITexture>& src,
                                                  int animIndex = -1) {
    if (count <= 0 || !src || !Render::GL::g_pHyprOpenGL || !g_pHyprRenderer) return nullptr;

    // A rotated or flipped source buffer would need the targets' dimensions
    // swapped and the transform reapplied on the way back out. Declining beats
    // getting it subtly wrong — and measured on a rotated monitor, the fallback
    // that this drops into renders correctly: right geometry, right orientation,
    // rounding and dim intact. It loses only the effects Hyprland's own program
    // would have added. See the note on the monitor transform below.
    if (src->m_transform != HYPRUTILS_TRANSFORM_NORMAL) return nullptr;

    // Snapped to whole pixels up front and used everywhere below. A framebuffer
    // can only be allocated at integer dimensions, so comparing its size against
    // a fractional texture size would never match and would reallocate the pair
    // on every single frame.
    const int w = (int)src->m_size.x;
    const int h = (int)src->m_size.y;
    if (w < 1 || h < 1 || w > 16384 || h > 16384) return nullptr;
    const Vector2D size((double)w, (double)h);

    if (!g_pStageFBs) g_pStageFBs = new std::unordered_map<uint64_t, StageFramebuffers>();

    // One entry per distinct source size. Evicted by age, not by dropping the
    // whole pool.
    //
    // The pool used to be cleared outright once it passed a handful of entries,
    // which was harmless while this path ran only for multi-stage stacks —
    // rare, so the pool was nearly always tiny. Now that every shaded surface
    // renders offscreen, a pool of live buckets is the normal case, and
    // clearing it destroys framebuffers that are in use on every frame. The bad
    // case is resizing a shaded window: a new size arrives each frame, the cap
    // is reached continuously, and the pool is torn down and rebuilt while the
    // buckets it is dropping are the ones being drawn into.
    //
    // Age-based eviction keeps the transient sizes from a resize from
    // accumulating, without ever touching a bucket that is still being used.
    const auto poolNow = std::chrono::steady_clock::now();
    for (auto it = g_pStageFBs->begin(); it != g_pStageFBs->end();) {
        const float idle = std::chrono::duration_cast<std::chrono::duration<float>>(poolNow - it->second.lastUsed).count();
        it = (idle > STAGE_FB_TTL) ? g_pStageFBs->erase(it) : std::next(it);
    }
    // Backstop for a pathological frame that somehow outruns the TTL. Drops the
    // single oldest bucket rather than all of them.
    while (g_pStageFBs->size() > STAGE_FB_MAX) {
        auto oldest = g_pStageFBs->begin();
        for (auto it = std::next(oldest); it != g_pStageFBs->end(); ++it)
            if (it->second.lastUsed < oldest->second.lastUsed) oldest = it;
        g_pStageFBs->erase(oldest);
    }

    const uint64_t key = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    auto&          fbs = (*g_pStageFBs)[key];
    fbs.lastUsed       = poolNow;

    for (auto& fb : fbs.fb) {
        if (!fb.isAllocated() || fb.m_size != size) {
            fb.release();
            if (!fb.alloc(w, h)) return nullptr;
        }
        // Carry the source's color description down the chain, so a stage reads
        // the previous stage's output in the same color space the window's own
        // texture was in. Without this a wide-gamut or HDR surface picks up a
        // conversion at every hop.
        fb.setImageDescription(src->m_imageDescription);
    }

    auto& R = g_pHyprRenderer->m_renderData;

    // Coordinate space of the projection currently in force. A rotated monitor
    // needs the box transformed to match, and this does not do it — the blit
    // below assumes the projection does not rotate, which holds only at
    // transform 0.
    //
    // This is not a gap in Hyprland: it separates m_pixelSize (the physical
    // panel, e.g. 2560x1080) from m_transformedSize (the logical space windows
    // live in, 1080x2560 at 90 degrees) and carries the rotation in the monitor
    // projection, which is exactly right and hands us everything needed. The
    // work simply is not done here. Verified by rotating a real monitor: the
    // fallback path renders correctly, so a rotated setup loses Hyprland's
    // blur/CM/discard/motion-blur on shaded surfaces but nothing else.
    // Monitors carry a wl_output_transform, textures a Hyprutils eTransform.
    // The two NORMAL constants are both 0, so mixing them up still behaves —
    // it just isn't the same enum, and -Wenum-compare is right to say so.
    const auto mon = R.pMonitor.lock();
    if (!mon || mon->m_transform != WL_OUTPUT_TRANSFORM_NORMAL) return nullptr;
    const Vector2D projSpace = mon->m_pixelSize;
    if (projSpace.x < 1 || projSpace.y < 1) return nullptr;

    // Everything here describes where and how the *window* is being drawn on the
    // monitor. An intermediate stage is a 1:1 blit into an offscreen target, so
    // all of it has to be neutralised and put back afterwards.
    const auto      savedFB       = R.currentFB;
    const CRegion   savedDamage   = R.damage;
    const auto      savedModif    = R.renderModif;
    const CBox      savedClipBox  = R.clipBox;
    const Vector2D  savedUVTL     = R.primarySurfaceUVTopLeft;
    const Vector2D  savedUVBR     = R.primarySurfaceUVBottomRight;
    GLint           savedViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, savedViewport);
    // Read the real GL state rather than trusting a cache, but put it back
    // through Hyprland's setters — it caches enable/disable per capability and
    // skips redundant calls, so a raw glEnable behind its back leaves it
    // convinced the state is something it isn't for the rest of the frame.
    const bool      savedScissor = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
    const bool      savedBlend   = glIsEnabled(GL_BLEND) == GL_TRUE;
    // The framebuffer is restored by raw id: m_renderData.currentFB is the
    // tracker, not necessarily what is actually bound, and getting this wrong
    // means drawing the rest of the frame into our scratch target.
    GLint           savedFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFBO);

    R.renderModif = {};
    R.clipBox     = {};
    R.damage      = CRegion(0.0, 0.0, projSpace.x, projSpace.y);
    // A surface drawn from a sub-rect of its buffer must not have that crop
    // applied twice — the intermediate pass copies the whole texture, and the
    // final on-screen draw re-applies the real UVs to the result.
    R.primarySurfaceUVTopLeft     = Vector2D(-1, -1);
    R.primarySurfaceUVBottomRight = Vector2D(-1, -1);

    // The projection in force belongs to the monitor and was built back in
    // begin(); it cannot be swapped out this late (projectionType is only read
    // there, and overwriting targetProjection renders nothing at all). So
    // rather than fighting it, the blit is expressed in its coordinate space: a
    // box covering the whole monitor maps to the whole target, and since the
    // viewport is the framebuffer, that lands the source texture on the
    // framebuffer 1:1 whatever its own size is.
    const CBox box(0.0, 0.0, projSpace.x, projSpace.y);

    // Intermediate stages render their finished state: `progress` belongs to the
    // animation, not to the layers it is animating. `animIndex` names the one
    // stage that is the animation, if it is in this batch — when the final draw
    // is handed back to Hyprland every stage comes through here, the animation
    // included, and forcing its progress to -1 would freeze it on its last
    // frame instead of playing it.
    const float   savedProgress = g_pCurrentAnimProgress;
    const float   savedSettle   = g_pCurrentSettle;
    const uint8_t savedOneShot  = g_pCurrentOneShotKind;
    g_bIntermediatePass         = true;

    SP<Render::ITexture> cur = src;
    for (int i = 0; i < count; ++i) {
        auto& fb = fbs.fb[i % 2];

        fb.bind();
        Render::GL::g_pHyprOpenGL->setViewport(0, 0, (GLsizei)w, (GLsizei)h);
        Render::GL::g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
        // Overwrite rather than blend: the stage output *is* the new texture,
        // not something composited over what the target happened to hold.
        Render::GL::g_pHyprOpenGL->blend(false);

        g_pCurrentCompiledShader = stages[i];

        const bool isAnimStage   = (i == animIndex);
        g_pCurrentAnimProgress   = isAnimStage ? savedProgress : -1.0f;
        g_pCurrentSettle         = isAnimStage ? savedSettle   : -1.0f;
        g_pCurrentOneShotKind    = isAnimStage ? savedOneShot   : TRANSFORM_NONE;

        CHyprOpenGLImpl::STextureRenderData data;
        data.a        = 1.0f;
        data.damage   = &R.damage;
        data.allowDim = false;
        Render::GL::g_pHyprOpenGL->renderTexture(cur, box, data);

        cur = fb.getTexture();
    }

    g_bIntermediatePass    = false;
    g_pCurrentAnimProgress = savedProgress;
    g_pCurrentSettle       = savedSettle;
    g_pCurrentOneShotKind  = savedOneShot;

    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFBO);

    R.currentFB                   = savedFB;
    R.damage                      = savedDamage;
    R.renderModif                 = savedModif;
    R.clipBox                     = savedClipBox;
    R.primarySurfaceUVTopLeft     = savedUVTL;
    R.primarySurfaceUVBottomRight = savedUVBR;

    Render::GL::g_pHyprOpenGL->setViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
    Render::GL::g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, savedScissor);
    Render::GL::g_pHyprOpenGL->blend(savedBlend);

    return cur;
}

// --- V0.56 HOOK: CGLElementRenderer::draw(CTexPassElement, CRegion) ---
typedef void (*TGLDrawTex)(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage);

void hkGLDrawTex(void* thisptr, Hyprutils::Memory::CWeakPointer<CTexPassElement> element, const CRegion& damage) {
    // V0.56: CRenderPass owns its elements as UP<IPassElement>, so this WP is a
    // weak-over-unique. CWeakPointer::lock() hard-asserts on that case
    // (WeakPtr.hpp:181 -> std::terminate) and takes the whole compositor down on
    // the first textured surface we see. get() has no such assert, and the pass
    // that owns the element is synchronously calling us, so borrowing is safe.
    CTexPassElement* elem = element.get();

    PHLWINDOW pWindow;
    PHLLS     pLS;
    // A popup whose owner is a layer surface rather than a window — a bar's
    // tooltip or menu. Kept apart from pLS, which means "this element *is* a
    // layer surface": the owner supplies a shader to inherit, but the popup must
    // not also inherit the layer's open animation or its corner rounding, both
    // of which belong to the layer's own box.
    PHLLS     pOwnerLS;
    // Only a window's own surface is rounded. A subsurface or popup carries its
    // own box, so masking its corners would cut notches out of the middle of
    // the window instead of following its outline.
    bool      isMainSurface = false;

    if (elem && elem->m_data.surface) {
        // V0.56: CCompositor::getWindowFromSurface is gone. Resolve the owning
        // view straight off the surface resource instead.
        if (auto wlSurface = Desktop::View::CWLSurface::fromResource(elem->m_data.surface)) {
            if (auto view = wlSurface->view()) {
                pWindow = Desktop::View::CWindow::fromView(view);

                // Subsurfaces and popups are views in their own right and don't
                // cast to a window; their owning window is private in 0.56, so
                // fall back to whichever window the renderer is currently
                // walking — that's the window they belong to.
                if (!pWindow) {
                    const auto type = view->type();
                    if (type == Desktop::View::VIEW_TYPE_SUBSURFACE || type == Desktop::View::VIEW_TYPE_POPUP)
                        pWindow = g_pHyprRenderer->m_renderData.currentWindow.lock();

                    // The renderer only publishes currentWindow while it is
                    // walking a window's own tree. A popup snapshotted for its
                    // close fade is drawn outside that walk, so the fallback
                    // above comes back empty and the snapshot gets captured
                    // unshaded — the shader then visibly drops off the popup for
                    // the whole closing animation. Ask the popup who owns it
                    // instead, which holds no matter who is driving the draw.
                    if (!pWindow && type == Desktop::View::VIEW_TYPE_POPUP) {
                        if (auto popup = Desktop::View::CPopup::fromView(view)) {
                            if (auto owner = popup->getT1Owner()) {
                                if (auto ownerView = owner->view())
                                    pWindow = Desktop::View::CWindow::fromView(ownerView);
                            }

                            // Not every popup hangs off a window. A bar's
                            // tooltip or menu is owned by a layer surface, which
                            // has no window rules to match, so it inherits that
                            // layer's namespace shader instead.
                            if (!pWindow)
                                pOwnerLS = popup->layerOwner();
                        }
                    }
                } else
                    isMainSurface = true;
            }
        }
        pLS = elem->m_data.currentLS.lock();
    }

    // --- FULLSCREEN FAST PATH ---
    // A fullscreen window that opted into nothing must cost nothing. Everything
    // below — the class/manual/rule lookups, the stack build, the offscreen
    // chain, the damage scheduling — would resolve to "no shaders" for it, so
    // skip straight to Hyprland's own draw. This is the path a fullscreen game
    // takes on every surface of every frame, and it is the reason the plugin can
    // be left loaded while gaming.
    if (fullscreenRendersNothing(pWindow)) {
        g_pCurrentRenderWindow.reset();
        g_pCurrentRenderLayer.reset();
        g_pCurrentCompiledShader = nullptr;
        g_pCurrentMotion         = nullptr;
        g_pCurrentElemSize       = Vector2D(0, 0);
        g_pCurrentWindowRect     = CBox{0.0, 0.0, 1.0, 1.0};
        ((TGLDrawTex)g_pGLDrawTexHook->m_original)(thisptr, element, damage);
        return;
    }

    g_pCurrentRenderWindow = pWindow;
    g_pCurrentRenderLayer  = pLS ? pLS : pOwnerLS;
    g_pCurrentAnimProgress = -1.0f;
    g_pCurrentAnimSeed     = -1.0f;
    g_pCurrentSettle       = -1.0f;
    g_pCurrentOneShotKind  = TRANSFORM_NONE;

    // Motion is published to every stage, not just a `shader_move:` one — a
    // permanent shader is entitled to react to its window being thrown around.
    // Borrowing a pointer into the map is safe for the same reason
    // resolveShaderPath's returned pointer is: it is only mutated between
    // frames, never during a draw chain.
    g_pCurrentMotion = nullptr;
    if (pWindow) {
        if (auto mIt = g_mWindowMotion.find(pWindow.get()); mIt != g_mWindowMotion.end())
            g_pCurrentMotion = &mIt->second;
    }

    // Taken straight off the pass element rather than recomputed from the
    // window, so a shaded surface rounds exactly the way Hyprland was about to
    // round it — including the cases where it had already decided not to.
    if (elem && (isMainSurface || pLS)) {
        g_pCurrentBoxSize    = elem->m_data.box.size();
        g_pCurrentRound      = (float)elem->m_data.round;
        g_pCurrentRoundPower = elem->m_data.roundingPower;
    } else {
        g_pCurrentBoxSize    = Vector2D(0, 0);
        g_pCurrentRound      = 0.0f;
        g_pCurrentRoundPower = 2.0f;
    }

    // Kept apart from g_pCurrentBoxSize, which is deliberately gated on the
    // surface being one that gets rounded. `surface_size` has to answer for a
    // layer surface and for a close animation's snapshot as well, and neither
    // has a window to ask.
    g_pCurrentElemSize = elem ? elem->m_data.box.size() : Vector2D(0, 0);
    // Identity unless a close animation overrides it below: for every window,
    // layer and subsurface the texture IS the view, so the view fills it.
    g_pCurrentWindowRect = CBox{0.0, 0.0, 1.0, 1.0};
    g_pCurrentFadeoutAnim = nullptr;

    // --- BUILD THE STACK ---
    // A one-shot animation always sits on top of whatever the window normally
    // looks like. Close animations have no window and no surface — they draw a
    // snapshot texture, which already has the window's own shaders baked in —
    // so they're matched on a separate branch and end up as the only stage.
    PHLMONITOR         animMonitor;
    const std::string* stack[MAX_SHADER_STAGES];
    int                nPaths = 0;

    const std::string* animPath = nullptr;
    if (pWindow) {
        // Open, urgent and focus cues are suppressed outright for a fullscreen
        // window that has not opted in; only the fullscreen transition tags can
        // still speak, and resolveTransformAnim is what serves those.
        const bool fsQuiet = fullscreenSuppresses(pWindow);

        animPath = fsQuiet ? nullptr : resolveOpenAnim(pWindow);
        // Priority, and the ordering is deliberate:
        //   open/close  — a window appearing or leaving outranks everything.
        //   urgent      — an alert. Rare, and the user needs to see it even
        //                 while the window happens to be moving.
        //   transform   — move/resize/workspace.
        //   focus       — LAST, because window.active fires on every workspace
        //                 switch. Ranked above transforms it would fight the
        //                 workspace shader on every single switch.
        if (!animPath && !fsQuiet) animPath = resolveOneShotAnim(pWindow, /* urgentOnly */ true);
        if (!animPath)             animPath = resolveTransformAnim(pWindow);
        if (!animPath && !fsQuiet) animPath = resolveOneShotAnim(pWindow, /* urgentOnly */ false);
    } else if (pLS)
        animPath = resolveLayerOpenAnim(pLS);
    else if (elem && !elem->m_data.surface && elem->m_data.tex)
        animPath = resolveCloseAnim(elem->m_data.tex, animMonitor);

    // The resolvers above set the progress/seed globals as a side effect. Hold
    // them aside: only the animation stage should see a real `progress`.
    const float   animProgress = g_pCurrentAnimProgress;
    const float   animSeed     = g_pCurrentAnimSeed;
    const float   animSettle   = g_pCurrentSettle;
    const uint8_t animOneShot  = g_pCurrentOneShotKind;

    if (pWindow && windowReplaceMode(pWindow)) {
        // Opt-out: first match wins, exactly as it did before stacking existed.
        if (const std::string* p = animPath ? animPath : resolveShaderPath(pWindow, pLS ? pLS : pOwnerLS))
            stack[nPaths++] = p;
    } else {
        nPaths = collectBaseLayers(pWindow, pLS ? pLS : pOwnerLS, stack);
        if (animPath) stack[nPaths++] = animPath;
    }

    // --- COMPILE THE STACK ---
    // A stage that won't compile drops out and the rest still render. Losing one
    // layer of a stack is a much better failure than losing the window's shading
    // entirely, and getOrCompileShader has already toasted the reason.
    CompiledShader* stages[MAX_SHADER_STAGES];
    int             nStages      = 0;
    bool            topIsAnim    = false;
    bool            stackUsesTime = false;

    for (int i = 0; i < nPaths; ++i) {
        if (stack[i]->empty()) continue;
        CompiledShader* cs = getOrCompileShader(*stack[i]);
        if (!cs) continue;
        stages[nStages++] = cs;
        stackUsesTime     = stackUsesTime || cs->usesTime;
        topIsAnim         = (animPath && stack[i] == animPath);
    }

    // --- RUN THE OFFSCREEN STAGES ---
    // Everything below the top stage renders into an offscreen target; the top
    // stage is left for Hyprland's own draw, with the composed texture swapped
    // in underneath it, so geometry/rounding/damage stay entirely theirs.
    // Everything Hyprland's own fragment program does — blur, colour management,
    // discard, motion blur, on top of rounding and dim — is done in the program
    // this plugin substitutes, so binding our shader for the final draw throws
    // all of it away. Rounding and dim are re-applied by the wrapper; the rest
    // cannot reasonably be, and reimplementing each new one as Hyprland gains it
    // is a losing game on a plugin that has to work against everyone's config,
    // not one machine's.
    //
    // So when the element actually needs any of it, run EVERY user stage
    // offscreen and let Hyprland draw the composed result with its own program.
    // Each effect then applies natively and exactly once: intermediate stages
    // already neutralise alpha, dim and rounding precisely so the final draw can
    // own them.
    //
    // Taken unconditionally rather than only when one of Hyprland's effects is
    // detectably in play. Gating on a list of known flags means every effect
    // Hyprland gains in future is silently dropped until someone notices and
    // adds it to the list — the exact failure this is meant to end. The cost is
    // one offscreen pass per shaded element per frame.
    //
    // This applies to layer surfaces as much as windows: the decision is made
    // from the pass element, which does not care which it is drawing.
    const bool wantsNativeFinal = true;

    SP<Render::ITexture> originalTex;
    bool                 nativeFinal = false;
    if (nStages >= 1 && elem && elem->m_data.tex) {
        const int offscreen = wantsNativeFinal ? nStages : nStages - 1;
        if (offscreen >= 1) {
            // The animation is the top stage when there is one; tell the chain
            // so it keeps its progress instead of rendering its finished state.
            const int animIdx = topIsAnim ? nStages - 1 : -1;
            if (auto composed = runIntermediateStages(stages, offscreen, elem->m_data.tex, animIdx)) {
                originalTex      = elem->m_data.tex;
                elem->m_data.tex = composed;
                nativeFinal      = wantsNativeFinal;
            }
            // If the chain could not run — a rotated monitor or a rotated
            // source buffer, which runIntermediateStages declines rather than
            // getting subtly wrong — we fall back to binding the top stage as
            // the on-screen draw. That path is why the wrapper still re-applies
            // alpha, rounding and dim: they are dead code on the native path,
            // and the only thing standing between a vertical-monitor user and
            // square corners on the fallback one.
        }
    }

    // nullptr leaves Hyprland's own program bound, which is the whole point of
    // the native path.
    g_pCurrentCompiledShader = (nativeFinal || nStages == 0) ? nullptr : stages[nStages - 1];
    // Only the animation stage gets the live progress. If the animation shader
    // failed to compile, the top stage is an ordinary layer and must render its
    // finished state, not be dragged through an animation it never asked for.
    // In the native path the animation already ran as an offscreen stage, so the
    // on-screen draw is Hyprland's and wants none of this.
    g_pCurrentAnimProgress   = (topIsAnim && !nativeFinal) ? animProgress : -1.0f;
    g_pCurrentAnimSeed       = animSeed;
    // `settle` is scoped to the animation stage for the same reason: the tail's
    // length is a property of the transform shader, and a base layer has none.
    g_pCurrentSettle         = (topIsAnim && !nativeFinal) ? animSettle : -1.0f;
    g_pCurrentOneShotKind    = (topIsAnim && !nativeFinal) ? animOneShot : TRANSFORM_NONE;

    // Schedule continuous redraw if any stage uses `time`, or while a one-shot
    // animation is mid-flight — an animation shader drives itself off `progress`
    // and may never bind `time`, so it needs frames either way. Keyed on the
    // animation being *resolved* rather than compiled, so a broken animation
    // shader still ticks down and clears itself instead of stranding the window.
    //
    // Every branch has to DAMAGE, not merely schedule a frame. scheduleFrame()
    // sets needsFrame, which wakes the output and gets as far as
    // renderMonitor(); the actual draw sits behind a separate `if
    // (!finalDamage.empty())`, so a woken monitor with a clean damage ring
    // renders nothing at all and commits an empty frame. A window-less surface
    // therefore has to dirty the monitor itself or its shader silently stops
    // advancing the moment nothing else on that monitor happens to be moving —
    // which is why this only ever showed up on a second monitor that was
    // otherwise idle.
    if (animPath || stackUsesTime) {
        if (pWindow)
            g_pHyprRenderer->damageWindow(pWindow);
        else if (auto ls = pLS ? pLS : pOwnerLS) {
            // Whole monitor, matching what Hyprland does for layers: their
            // position/size/alpha animations are all created AVARDAMAGE_ENTIRE
            // and CLayerSurface damages the monitor outright. A layer's own box
            // is monitor-local while damageBox() wants global coordinates, so
            // this also avoids getting that conversion subtly wrong.
            if (auto mon = ls->m_monitor.lock())
                g_pHyprRenderer->damageMonitor(mon);
        } else if (animMonitor)
            g_pHyprRenderer->damageMonitor(animMonitor);
    }

    // --- LET THE CLOSE SHADER OWN THE FADE ---
    // renderFadeouts() hands the snapshot over with `a = fadeout->alpha()`, the
    // value of Hyprland's own `fadeOut` animation. Holding the fadeout alive past
    // that animation (holdFadeoutOpen) is pointless if the snapshot is pinned at
    // alpha 0 for the extra time, so the declared duration is the plugin's clock
    // for the fade as well: the close shader reaches full transparency at
    // progress 1.0 by contract, and it cannot honour that contract while
    // something else is driving alpha to zero on a different schedule.
    //
    // This is what made the bug monitor-transform-specific. On the native path
    // Hyprland's own program applies that alpha, so a close animation truncated
    // by a frozen frame was already invisible and nobody saw it. On a rotated
    // monitor runIntermediateStages declines, our shader is the on-screen draw,
    // and plugin_alpha resolves to 1.0 because a fadeout has no window to ask for
    // alphaTotal() — so the same frozen frame stayed fully opaque and read as
    // "the animation stopped two thirds through and left the window behind".
    //
    // Gated on topIsAnim so a close shader that failed to compile still fades on
    // Hyprland's schedule instead of sitting opaque until the hold expires. Only
    // the pass element's copy is touched, never the animated variable, so
    // CWindowFadeout::done() still reports true on Hyprland's own timing and the
    // hold stays the only thing keeping the snapshot alive.
    const bool  ownFade   = animMonitor && topIsAnim;
    // Proof the close shader exists and reached the stack. renderBox() reads this
    // to decide whether holding the snapshot still is safe.
    if (ownFade && g_pCurrentFadeoutAnim) g_pCurrentFadeoutAnim->shaderLive = true;
    const float origElemA = elem ? elem->m_data.a : 1.0f;
    if (ownFade) elem->m_data.a = 1.0f;

    ((TGLDrawTex)g_pGLDrawTexHook->m_original)(thisptr, element, damage);

    // The element belongs to the pass, not to us — put its texture back before
    // anything else in the frame looks at it.
    if (originalTex) elem->m_data.tex = originalTex;
    if (ownFade)     elem->m_data.a   = origElemA;

    g_pCurrentRenderWindow.reset();
    g_pCurrentRenderLayer.reset();
    g_pCurrentCompiledShader = nullptr;
    g_pCurrentAnimProgress   = -1.0f;
    g_pCurrentAnimSeed       = -1.0f;
    g_pCurrentSettle         = -1.0f;
    g_pCurrentOneShotKind    = TRANSFORM_NONE;
    g_pCurrentMotion         = nullptr;
    g_pCurrentElemSize       = Vector2D(0, 0);
    g_pCurrentWindowRect     = CBox{0.0, 0.0, 1.0, 1.0};
    g_pCurrentRound          = 0.0f;
}

// --- V0.56 HOOK: useShader ---
typedef Hyprutils::Memory::CWeakPointer<CShader> (*TUseShader)(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog);

Hyprutils::Memory::CWeakPointer<CShader> hkUseShader(CHyprOpenGLImpl* thisptr, Hyprutils::Memory::CWeakPointer<CShader> prog) {
    // The compiled-shader pointer was already resolved in hkGLDrawTex; just
    // pick it up here. If null there's no shader to apply for this surface —
    // skip the weak_ptr lock entirely (common case for most surfaces).
    CompiledShader* activeEntry = g_pCurrentCompiledShader;
    if (activeEntry && activeEntry->shader)
        prog = activeEntry->shader;
    else
        activeEntry = nullptr;

    auto result = ((TUseShader)g_pUseShaderHook->m_original)(thisptr, prog);

    // Inject uniforms using cached locations (no per-frame glGetUniformLocation).
    if (activeEntry) {
        PHLWINDOW contextWindow = g_pCurrentRenderWindow.lock();
        if (activeEntry->timeLoc >= 0) {
            const float t = std::chrono::duration_cast<std::chrono::duration<float>>(
                                std::chrono::steady_clock::now() - g_pluginStartTime)
                                .count();
            glUniform1f(activeEntry->timeLoc, t);
        }
        if (activeEntry->alphaLoc >= 0) {
            // Offscreen stages always render fully opaque. Window opacity is a
            // property of putting the finished result on screen, so it is
            // applied once by the top stage — folding it into every layer would
            // compound it (0.5 opacity over three stages would land at 0.125).
            const float currentAlpha = g_bIntermediatePass ? 1.0f : (contextWindow ? contextWindow->alphaTotal() : 1.0f);
            glUniform1f(activeEntry->alphaLoc, currentAlpha);
        }
        if (activeEntry->resolutionLoc >= 0) {
            Vector2D res(0, 0);
            if (auto mon = g_pHyprRenderer->m_renderData.pMonitor.lock())
                res = mon->m_pixelSize;
            glUniform2f(activeEntry->resolutionLoc, (float)res.x, (float)res.y);
        }
        if (activeEntry->surfaceSizeLoc >= 0) {
            // V0.56: CWindow::m_size is gone; IView::logicalBox() is the
            // supported way to read a window's logical geometry.
            Vector2D sz(0, 0);
            if (contextWindow) {
                if (const auto box = contextWindow->logicalBox())
                    sz = box->size();
            }
            // A layer surface and a close animation's snapshot both arrive with
            // no window, and used to leave this at (0,0). Any shader converting
            // pixels to texels through it then divided by one instead, so a
            // displacement meant to be a few pixels moved whole texture widths
            // and every sample fell outside the surface.
            if (sz.x <= 0.0 || sz.y <= 0.0)
                sz = g_pCurrentElemSize;
            // No scale conversion here, deliberately. 68c1f46 multiplied the
            // window branch by m_scale to put everything in device pixels, on the
            // premise that surface_size is "the size of what v_texcoord spans".
            // That premise is false twice over. The axis order was wrong for a
            // rotated snapshot (see resolveCloseAnim), and v_texcoord spans the
            // client's BUFFER, whose scale need not match the monitor's: an
            // XWayland or non-HiDPI app on a scale-2 monitor submits an 800x600
            // buffer for an 800x600 logical window, so the conversion reported
            // 1600x1200 for 800x600 texels and halved every displacement written
            // against the documented contract.
            //
            // So this stays the view's own size — the window's logical box, or the
            // element box for a layer or snapshot. Subsurfaces keep preferring
            // their parent window's box above; that is what lets a shader build
            // one coherent field across a window instead of per-surface.
            glUniform2f(activeEntry->surfaceSizeLoc, (float)sz.x, (float)sz.y);
        }
        if (activeEntry->mouseLoc >= 0 && Pointer::mgr()) {
            const Vector2D p = Pointer::mgr()->position();
            glUniform2f(activeEntry->mouseLoc, (float)p.x, (float)p.y);
        }
        if (activeEntry->isActiveLoc >= 0) {
            const float v = (contextWindow && Desktop::focusState()->isWindowActive(contextWindow)) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isActiveLoc, v);
        }
        if (activeEntry->isFloatingLoc >= 0) {
            const float v = (contextWindow && contextWindow->m_isFloating) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isFloatingLoc, v);
        }
        if (activeEntry->isFullscreenLoc >= 0) {
            const float v = (contextWindow && Fullscreen::controller()->isFullscreen(contextWindow)) ? 1.0f : 0.0f;
            glUniform1f(activeEntry->isFullscreenLoc, v);
        }
        if (activeEntry->progressLoc >= 0) {
            // 1.0 outside an animation, so a shader written against `progress`
            // still renders its finished state if bound as a normal shader.
            glUniform1f(activeEntry->progressLoc, g_pCurrentAnimProgress >= 0.0f ? g_pCurrentAnimProgress : 1.0f);
        }
        if (activeEntry->boxSizeLoc >= 0)
            glUniform2f(activeEntry->boxSizeLoc, (float)g_pCurrentBoxSize.x, (float)g_pCurrentBoxSize.y);
        if (activeEntry->roundLoc >= 0) {
            // Offscreen stages never round: the mask is the last thing applied,
            // by the stage that actually reaches the screen.
            glUniform1f(activeEntry->roundLoc, g_bIntermediatePass ? 0.0f : g_pCurrentRound);
        }
        if (activeEntry->roundPowerLoc >= 0)
            glUniform1f(activeEntry->roundPowerLoc, g_pCurrentRoundPower);
        if (activeEntry->dimLoc >= 0) {
            // Offscreen stages stay undimmed for the same reason they stay
            // fully opaque: the dim belongs to the finished result reaching the
            // screen, and folding it into every layer would compound it.
            float tint = 1.0f;
            if (!g_bIntermediatePass && contextWindow && contextWindow->m_dimPercent)
                tint = 1.0f - contextWindow->m_dimPercent->value();
            glUniform1f(activeEntry->dimLoc, std::clamp(tint, 0.0f, 1.0f));
        }
        // --- TRANSFORM UNIFORMS ---
        // Zeroed rather than skipped when the window has no motion record, so a
        // shader reading `velocity` on a stationary window sees a still window
        // instead of whatever the last moving one left in the program.
        if (activeEntry->velocityLoc >= 0) {
            const Vector2D v = g_pCurrentMotion ? g_pCurrentMotion->velocity : Vector2D(0, 0);
            glUniform2f(activeEntry->velocityLoc, (float)v.x, (float)v.y);
        }
        if (activeEntry->sizeVelocityLoc >= 0) {
            const Vector2D v = g_pCurrentMotion ? g_pCurrentMotion->sizeVelocity : Vector2D(0, 0);
            glUniform2f(activeEntry->sizeVelocityLoc, (float)v.x, (float)v.y);
        }
        if (activeEntry->moveDeltaLoc >= 0) {
            const Vector2D d = g_pCurrentMotion ? g_pCurrentMotion->goal - g_pCurrentMotion->begun : Vector2D(0, 0);
            glUniform2f(activeEntry->moveDeltaLoc, (float)d.x, (float)d.y);
        }
        if (activeEntry->moveRemainingLoc >= 0) {
            const Vector2D d = g_pCurrentMotion ? g_pCurrentMotion->goal - g_pCurrentMotion->pos : Vector2D(0, 0);
            glUniform2f(activeEntry->moveRemainingLoc, (float)d.x, (float)d.y);
        }
        if (activeEntry->sizeDeltaLoc >= 0) {
            const Vector2D d = g_pCurrentMotion ? g_pCurrentMotion->sizeGoal - g_pCurrentMotion->sizeBegun : Vector2D(0, 0);
            glUniform2f(activeEntry->sizeDeltaLoc, (float)d.x, (float)d.y);
        }
        if (activeEntry->releaseVelLoc >= 0) {
            const Vector2D v = g_pCurrentMotion ? g_pCurrentMotion->releaseVelocity : Vector2D(0, 0);
            glUniform2f(activeEntry->releaseVelLoc, (float)v.x, (float)v.y);
        }
        if (activeEntry->peakVelLoc >= 0) {
            const Vector2D v = g_pCurrentMotion ? g_pCurrentMotion->peakVelocity : Vector2D(0, 0);
            glUniform2f(activeEntry->peakVelLoc, (float)v.x, (float)v.y);
        }
        if (activeEntry->peakSizeVelLoc >= 0) {
            const Vector2D v = g_pCurrentMotion ? g_pCurrentMotion->peakSizeVelocity : Vector2D(0, 0);
            glUniform2f(activeEntry->peakSizeVelLoc, (float)v.x, (float)v.y);
        }
        if (activeEntry->isMovingLoc >= 0)
            glUniform1f(activeEntry->isMovingLoc, (g_pCurrentMotion && g_pCurrentMotion->moving) ? 1.0f : 0.0f);
        if (activeEntry->isResizingLoc >= 0)
            glUniform1f(activeEntry->isResizingLoc, (g_pCurrentMotion && g_pCurrentMotion->resizing) ? 1.0f : 0.0f);
        if (activeEntry->isDraggingLoc >= 0)
            glUniform1f(activeEntry->isDraggingLoc, (g_pCurrentMotion && g_pCurrentMotion->dragging) ? 1.0f : 0.0f);
        if (activeEntry->animKindLoc >= 0) {
            // A one-shot has no motion record, so it reports its kind through
            // its own global; motion supplies it otherwise.
            const float k = g_pCurrentOneShotKind != TRANSFORM_NONE ? (float)g_pCurrentOneShotKind
                          : (g_pCurrentMotion ? (float)g_pCurrentMotion->kind : 0.0f);
            glUniform1f(activeEntry->animKindLoc, k);
        }
        if (activeEntry->curveLoc >= 0) {
            // Matches `progress`: a shader bound as an ordinary layer, outside
            // any transform, should render its finished state rather than its
            // start. Not clamped — a spring overshooting past 1.0 is real
            // information a wobble shader wants.
            glUniform1f(activeEntry->curveLoc,
                        (g_pCurrentMotion && (g_pCurrentMotion->moving || g_pCurrentMotion->resizing)) ? g_pCurrentMotion->curve : 1.0f);
        }
        if (activeEntry->durationLoc >= 0)
            glUniform1f(activeEntry->durationLoc, g_pCurrentMotion ? g_pCurrentMotion->duration : -1.0f);
        if (activeEntry->settleLoc >= 0) {
            // 0 outside a tail, so `settle` reads as "not settling" rather than
            // as a completed one.
            glUniform1f(activeEntry->settleLoc, g_pCurrentSettle >= 0.0f ? g_pCurrentSettle : 0.0f);
        }
        if (activeEntry->windowBoxLoc >= 0) {
            // The window's own box, which a subsurface can use to place itself
            // inside the parent. hkGLDrawTex runs per surface, so a displacement
            // driven off v_texcoord alone deforms each subsurface about its own
            // centre and tears the window apart; this is what lets a shader
            // build one field across the whole thing.
            CBox b;
            if (contextWindow) {
                if (const auto lb = contextWindow->logicalBox())
                    b = *lb;
            }
            glUniform4f(activeEntry->windowBoxLoc, (float)b.x, (float)b.y, (float)b.w, (float)b.h);
        }
        if (activeEntry->windowRectLoc >= 0) {
            // Unlike window_box this is normalised and in texcoord space, which is
            // what lets one expression work on every path:
            //
            //     vec2 local = (v_texcoord - window_rect.xy) / window_rect.zw;
            //
            // (0,0,1,1) for a window or layer, so `local` is just v_texcoord there,
            // and the view's former sub-rect for a close animation, whose snapshot
            // covers the whole monitor. Being unitless it also sidesteps the
            // logical-versus-device-pixel question entirely.
            const CBox& r = g_pCurrentWindowRect;
            glUniform4f(activeEntry->windowRectLoc, (float)r.x, (float)r.y, (float)r.w, (float)r.h);
        }
        if (activeEntry->seedLoc >= 0) {
            float s = g_pCurrentAnimSeed;
            if (s < 0.0f) {
                // Not an animation — derive from whatever we're drawing so the
                // seed stays stable for the life of the window or layer.
                if (contextWindow)
                    s = animSeedFor(contextWindow.get());
                else if (auto l = g_pCurrentRenderLayer.lock())
                    s = animSeedFor(l.get());
                else
                    s = 0.5f;
            }
            glUniform1f(activeEntry->seedLoc, s);
        }
    }

    return result;
}

// Records the close shader for a freshly created fadeout. Everything after this
// point keys off the IFadeout, because the window or layer is already gone.
static void tagFadeout(Desktop::IFadeout* key, const std::string& path, float duration, const void* seedSource,
                       const Vector2D& srcPos, const Vector2D& srcSize) {
    FadeoutAnim anim;
    anim.path     = path;
    anim.start    = std::chrono::steady_clock::now();
    anim.duration = duration; // <0 -> resolved from the shader on first draw
    anim.seed     = animSeedFor(seedSource);
    anim.srcPos   = srcPos;
    anim.srcSize  = srcSize;
    g_mFadeoutAnims[key] = std::move(anim);
}

// Shared by both fadeout kinds: hold the snapshot on screen until the shader has
// had its declared duration, then let Hyprland drop it as usual.
static bool holdFadeoutOpen(Desktop::IFadeout* key, bool origDone) {
    if (g_mFadeoutAnims.empty()) return origDone;

    auto it = g_mFadeoutAnims.find(key);
    if (it == g_mFadeoutAnims.end()) return origDone;

    // Deliberately no getOrCompileShader here: `done` is called from fadeout
    // bookkeeping, which isn't guaranteed to run with a GL context current, and
    // compiling without one would fail (and poison the failure cache). The draw
    // path resolves the real duration; until it has, assume the default.
    const FadeoutAnim& anim = it->second;
    const float        dur  = anim.duration > 0.0f ? anim.duration : DEFAULT_ANIM_DURATION;

    if (secondsSince(anim.start) < std::min(dur, MAX_ANIM_DURATION)) {
        // Keep frames coming ourselves: once Hyprland's fade animation is over it
        // has no reason left to tick this monitor, and without a frame the
        // animation would freeze mid-way instead of playing out.
        //
        // DAMAGE, not just scheduleFrame(). A scheduled frame only sets
        // needsFrame, which gets the monitor as far as renderMonitor() and no
        // further: with an empty damage region the whole `else if
        // (!finalDamage.empty())` branch is skipped, so renderWorkspace() never
        // runs, no fadeout is drawn, and the frame commits with nothing in it.
        // The close shader then stops advancing at whatever progress it had
        // reached when Hyprland's own fade animation ended — while the plugin
        // spun the output at full refresh producing empty frames for the rest of
        // the declared duration.
        //
        // Hyprland's fadeouts damage through exactly this call: all three of a
        // fadeout's animated variables are created AVARDAMAGE_NONE with an
        // update callback that calls damageMonitor, so the whole monitor is what
        // the compositor itself dirties on every tick of a fade. Matching that
        // is both correct and no coarser than what we are replacing — a
        // fadeout's renderBox is monitor-sized anyway, because the snapshot
        // framebuffer covers the monitor and is merely offset so the window's
        // part of it lands where the window was.
        if (auto mon = key->monitor().lock())
            g_pHyprRenderer->damageMonitor(mon);
        return false;
    }

    if (origDone) g_mFadeoutAnims.erase(it);
    return origDone;
}

// --- V0.56 HOOKS: CWindowFadeout::renderBox / CLayerFadeout::renderBox ---
// Hyprland builds this box as `m_transformedSize * (m_realSize / m_sourceSize)`,
// and windowsOut drives that second factor to nothing on its own schedule.
// Measured across one close: 2560x1080 on the first drawn frame, 14x11 on the
// last. Holding the fadeout open past that bought the shader time it could not
// actually draw in, so a shader declaring a duration longer than windowsOut spent
// its climax — the moment it is contracted to reach full transparency — inside a
// fourteen-pixel box.
//
// At SCALE == 1 the expression reduces exactly to {0, 0, m_transformedSize}: the
// position terms cancel, because at t=0 the window's animated position IS its
// source position. So this is not a new layout, it is the fadeout's own first
// frame held for the declared duration.
//
// This is hooked rather than patched in hkGLDrawTex because the box is read
// independently by pass-element construction, by boundingBox() for damage and
// culling, by the offscreen composition, and by the final draw. Patching it
// midway leaves those disagreeing — measured, a snapshot pinned in the draw hook
// filled the monitor on the fallback path and still collapsed on the native one.
//
// Note this cannot be done by disabling Hyprland's close animation instead: the
// snapshot exists BECAUSE Hyprland is animating the close, so turning that off
// deletes the feature rather than replacing it. Neutralise, never disable.
static std::optional<CBox> pinnedFadeoutBox(Desktop::IFadeout* key) {
    if (g_mFadeoutAnims.empty() || !key) return std::nullopt;

    auto it = g_mFadeoutAnims.find(key);
    // shaderLive, not merely "tagged": a close shader that failed to compile must
    // still collapse the way Hyprland intended rather than sit full-size and pop.
    if (it == g_mFadeoutAnims.end() || !it->second.shaderLive) return std::nullopt;

    const auto mon = key->monitor().lock();
    if (!mon) return std::nullopt;

    const Vector2D xf = mon->m_transformedSize;
    if (xf.x < 1.0 || xf.y < 1.0) return std::nullopt;

    return CBox{0.0, 0.0, xf.x, xf.y};
}

typedef CBox (*TFadeoutRenderBox)(void* thisptr);

CBox hkFadeoutRenderBox(void* thisptr) {
    // Derived-to-base first, so the compiler applies the right offset for the key
    // to match what fadeouts() hands back — IFadeout has a virtual base.
    if (auto b = pinnedFadeoutBox(static_cast<Desktop::CWindowFadeout*>(thisptr)))
        return *b;
    return ((TFadeoutRenderBox)g_pFadeoutRenderBoxHook->m_original)(thisptr);
}

CBox hkLayerFadeoutRenderBox(void* thisptr) {
    if (auto b = pinnedFadeoutBox(static_cast<Desktop::CLayerFadeout*>(thisptr)))
        return *b;
    return ((TFadeoutRenderBox)g_pLayerFadeoutRenderBoxHook->m_original)(thisptr);
}

// --- V0.56 HOOK: Desktop::CWindowFadeout::create ---
// The one place where a fadeout and the window it came from are both in scope.
// We tag the fadeout with that window's close shader; everything afterwards keys
// off the fadeout, because the window is already gone.
typedef Hyprutils::Memory::CSharedPointer<Desktop::CWindowFadeout> (*TFadeoutCreate)(PHLWINDOW, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer>, float);

Hyprutils::Memory::CSharedPointer<Desktop::CWindowFadeout>
hkFadeoutCreate(PHLWINDOW window, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer> snapshot, float sourceAlpha) {
    auto result = ((TFadeoutCreate)g_pFadeoutCreateHook->m_original)(window, snapshot, sourceAlpha);

    pruneFadeoutAnims();

    if (!result || !window) return result;

    auto it = g_mWindowRuleShaders.find(window.get());
    if (it == g_mWindowRuleShaders.end() || it->second.closeAnim.empty()) return result;

    // Derived-to-base conversion, not a reinterpret: IFadeout has a virtual base,
    // so the compiler has to apply the right offset for the key to match what
    // fadeouts() hands back.
    // The window is still alive here, which is the only moment its geometry can be
    // read — everything after this keys off the IFadeout, and the window is gone.
    // GEOMETRIC_CURRENT, not GOAL: the snapshot captured where the window actually
    // was, not where it was heading. Scaled into the monitor's transformed space,
    // which is what m_transformedSize measures and what the snapshot box spans;
    // no rotation term is needed because that space is already rotated.
    Vector2D srcPos, srcSize;
    if (const auto mon = window->m_monitor.lock()) {
        srcPos  = (window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - mon->m_position) * mon->m_scale;
        srcSize = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) * mon->m_scale;
    }

    tagFadeout(result.get(), it->second.closeAnim, it->second.closeAnimDuration, window.get(), srcPos, srcSize);

    return result;
}

// --- V0.56 HOOK: Desktop::CLayerFadeout::create ---
// Layer surfaces close the same way windows do — snapshot, then fade — so the
// only thing that differs is where the shader comes from: a namespace lookup
// rather than a window rule.
typedef Hyprutils::Memory::CSharedPointer<Desktop::CLayerFadeout> (*TLayerFadeoutCreate)(PHLLS, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer>, float);

Hyprutils::Memory::CSharedPointer<Desktop::CLayerFadeout>
hkLayerFadeoutCreate(PHLLS layer, Hyprutils::Memory::CSharedPointer<Render::IFramebuffer> snapshot, float sourceAlpha) {
    auto result = ((TLayerFadeoutCreate)g_pLayerFadeoutCreateHook->m_original)(layer, snapshot, sourceAlpha);

    pruneFadeoutAnims();

    if (!result || !layer) return result;

    const AnimSpec* spec = lookupLayerEntry(g_mLayerCloseAnims, layer->m_namespace);
    if (!spec || spec->path.empty()) return result;

    // m_geometry is monitor-local already — CLayerSurface adds the monitor position
    // to it everywhere it wants a global box — so it only needs scaling, and the
    // monitor origin must not come off a second time.
    Vector2D srcPos, srcSize;
    if (const auto mon = layer->m_monitor.lock()) {
        const CBox& g = layer->m_geometry;
        srcPos  = Vector2D(g.x, g.y) * mon->m_scale;
        srcSize = Vector2D(g.w, g.h) * mon->m_scale;
    }

    tagFadeout(result.get(), spec->path, spec->duration, layer.get(), srcPos, srcSize);

    return result;
}

// --- V0.56 HOOK: Desktop::CWindowFadeout::done ---
// Hyprland drops a fadeout as soon as its own fade-out animation finishes, which
// can be well before the close shader is done. Holding `done` false keeps the
// snapshot alive for exactly as long as the shader asked for.
typedef bool (*TFadeoutDone)(void* thisptr);

bool hkFadeoutDone(void* thisptr) {
    const bool origDone = ((TFadeoutDone)g_pFadeoutDoneHook->m_original)(thisptr);
    // Cast to the concrete type first so the compiler applies the correct
    // derived-to-base offset — IFadeout has a virtual base.
    return holdFadeoutOpen(static_cast<Desktop::CWindowFadeout*>(thisptr), origDone);
}

// --- V0.56 HOOK: Desktop::CLayerFadeout::done ---
bool hkLayerFadeoutDone(void* thisptr) {
    const bool origDone = ((TFadeoutDone)g_pLayerFadeoutDoneHook->m_original)(thisptr);
    return holdFadeoutOpen(static_cast<Desktop::CLayerFadeout*>(thisptr), origDone);
}

void applyShaderRulesSafe(PHLWINDOW pWindow) {
    if (!pWindow || !pWindow->m_ruleApplicator) return;
    Desktop::View::CWindow* rawWin = pWindow.get();

    WindowShaderState state;
    // Values from `<tag>_default:` tags. Kept apart from `state` for the whole
    // parse and merged in afterwards, so a fallback can never overwrite a
    // specific rule no matter which order the tags are visited in.
    WindowShaderState defaults;
    bool              hasRules = false;

    const auto& tagsSet = pWindow->m_ruleApplicator->m_tagKeeper.getTags();
    for (const auto& tag : tagsSet) {
        // Trim trailing '*' and spaces without repeated pop_back allocations.
        std::string_view sv(tag);
        size_t end = sv.find_last_not_of("* \t");
        if (end == std::string_view::npos) continue;
        sv = sv.substr(0, end + 1);

        // Fast reject: every recognized tag starts with "shader".
        if (sv.size() < 7 || sv.substr(0, 6) != "shader") continue;

        // Split once into key and value rather than testing a hardcoded offset
        // per prefix. The offsets were easy to get wrong when adding a tag, and
        // the suffix handling below needs the key on its own anyway.
        const size_t colon = sv.find(':');
        if (colon == std::string_view::npos) continue;

        std::string_view key = sv.substr(0, colon);
        std::string_view val = sv.substr(colon + 1);

        // `<tag>_default:` marks a fallback — it applies only when no tag of the
        // same kind without the suffix is on this window.
        //
        // This exists because Hyprland stores tags in an alphabetically sorted
        // std::set. A catch-all rule and a per-app rule that both set, say,
        // `shader_close:` land two tags on the same window, and the loop below
        // would resolve them by whichever shader *path* sorts later — so
        // `smoke_close.glsl` would beat `matrix_close.glsl` for no reason the
        // user can see, and renaming a file would flip the result. Reported as
        // GitHub PR #4, which fixed it for the two animation tags; the suffix is
        // handled here for every tag instead, since the same collision hits
        // `shader:` and the state-conditional tags identically.
        const bool isDefault = key.ends_with("_default");
        if (isDefault) key.remove_suffix(8);

        WindowShaderState& dst = isDefault ? defaults : state;

        // A default tag on its own doesn't mark the window as having rules —
        // that's decided in the merge below, once it's known whether the
        // fallback was actually needed.
        const auto assign = [&](std::string& field) {
            field.assign(val);
            if (!isDefault) hasRules = true;
        };

        // Animation tags additionally accept an optional `@<seconds>` suffix.
        // For open/close it overrides the duration the shader declares; for the
        // transform tags there is no duration to override — the compositor's own
        // move animation is the clock — so there it sets the settle tail instead.
        // Same parse either way, different destination. rfind, so a path that
        // happens to contain '@' still works; and the suffix is only stripped if
        // it actually parsed as a positive number (clamped to MAX_ANIM_DURATION).
        const auto assignAnim = [&](std::string& field, float& dur) {
            std::string_view rest = val;
            dur                   = -1.0f;

            if (const size_t at = rest.rfind('@'); at != std::string_view::npos) {
                try {
                    const float d = std::stof(std::string(rest.substr(at + 1)));
                    // Clamped rather than rejected — see parseAnimSpec.
                    if (d > 0.0f) {
                        dur  = std::min(d, MAX_ANIM_DURATION);
                        rest = rest.substr(0, at);
                    }
                } catch (...) {}
            }

            field.assign(rest);
            if (!isDefault) hasRules = true;
        };

        if      (key == "shader")            assign(dst.fallback);
        else if (key == "shader_active")     assign(dst.active);
        else if (key == "shader_inactive")   assign(dst.inactive);
        else if (key == "shader_floating")   assign(dst.floating);
        else if (key == "shader_tiled")      assign(dst.tiled);
        else if (key == "shader_fullscreen") assign(dst.fullscreen);
        else if (key == "shader_open")       assignAnim(dst.openAnim,  dst.openAnimDuration);
        else if (key == "shader_close")      assignAnim(dst.closeAnim, dst.closeAnimDuration);
        else if (key == "shader_move")       assignAnim(dst.moveAnim,   dst.moveSettle);
        else if (key == "shader_resize")     assignAnim(dst.resizeAnim,    dst.resizeSettle);
        else if (key == "shader_workspace")  assignAnim(dst.workspaceAnim, dst.workspaceSettle);
        else if (key == "shader_fullscreen_enter") assignAnim(dst.fsEnterAnim, dst.fsEnterSettle);
        else if (key == "shader_fullscreen_exit")  assignAnim(dst.fsExitAnim,  dst.fsExitSettle);
        else if (key == "shader_float")      assignAnim(dst.floatAnim, dst.floatSettle);
        else if (key == "shader_tile")       assignAnim(dst.tileAnim,  dst.tileSettle);
        else if (key == "shader_urgent")     assignAnim(dst.urgentAnim,  dst.urgentDuration);
        else if (key == "shader_focus")      assignAnim(dst.focusAnim,   dst.focusDuration);
        else if (key == "shader_unfocus")    assignAnim(dst.unfocusAnim, dst.unfocusDuration);
        // Not a shader, and deliberately has no `_default` form: "unset" and
        // "explicitly false" are the same value for a bool, so a default could
        // never be overridden back off by a more specific rule. Also does not
        // set hasRules — on its own it has nothing to apply, and a window
        // carrying only this tag should fall out of the map entirely rather
        // than being kept alive with an empty state.
        else if (!isDefault && key == "shader_replace")
            state.replaceMode = (val != "0" && val != "false" && val != "no" && val != "off");
        // Same reason for having no `_default` form, but unlike shader_replace
        // this one DOES mark the window as ruled. It is the only way to opt a
        // class shader or an imperative toggle into rendering while fullscreen,
        // and both of those are set by dispatcher rather than by tag — so a
        // window whose only shading comes from one of them would otherwise be
        // erased from the map, and the opt-in would have nothing left to read.
        else if (!isDefault && key == "shader_fullscreen_stack") {
            state.fullscreenStack = (val != "0" && val != "false" && val != "no" && val != "off");
            if (state.fullscreenStack) hasRules = true;
        }
    }

    // Promote each fallback the window didn't override. A promoted default is a
    // rule like any other, so it marks the window as shaded.
    const auto fill = [&](std::string& specific, std::string& fallback) {
        if (!specific.empty() || fallback.empty()) return;
        specific = std::move(fallback);
        hasRules = true;
    };

    fill(state.fallback,   defaults.fallback);
    fill(state.active,     defaults.active);
    fill(state.inactive,   defaults.inactive);
    fill(state.floating,   defaults.floating);
    fill(state.tiled,      defaults.tiled);
    fill(state.fullscreen, defaults.fullscreen);

    // The animation tags carry a number alongside the path, so they promote as a
    // pair rather than through `fill`.
    const auto fillAnim = [&](std::string& specific, float& specificNum, std::string& fallback, float fallbackNum) {
        if (!specific.empty() || fallback.empty()) return;
        specific    = std::move(fallback);
        specificNum = fallbackNum;
        hasRules    = true;
    };

    fillAnim(state.openAnim,   state.openAnimDuration,  defaults.openAnim,   defaults.openAnimDuration);
    fillAnim(state.closeAnim,  state.closeAnimDuration, defaults.closeAnim,  defaults.closeAnimDuration);
    fillAnim(state.moveAnim,   state.moveSettle,        defaults.moveAnim,   defaults.moveSettle);
    fillAnim(state.resizeAnim, state.resizeSettle,      defaults.resizeAnim, defaults.resizeSettle);
    fillAnim(state.workspaceAnim, state.workspaceSettle, defaults.workspaceAnim, defaults.workspaceSettle);
    fillAnim(state.fsEnterAnim, state.fsEnterSettle, defaults.fsEnterAnim, defaults.fsEnterSettle);
    fillAnim(state.fsExitAnim,  state.fsExitSettle,  defaults.fsExitAnim,  defaults.fsExitSettle);
    fillAnim(state.floatAnim,   state.floatSettle,   defaults.floatAnim,   defaults.floatSettle);
    fillAnim(state.tileAnim,    state.tileSettle,    defaults.tileAnim,    defaults.tileSettle);
    fillAnim(state.urgentAnim,  state.urgentDuration,  defaults.urgentAnim,  defaults.urgentDuration);
    fillAnim(state.focusAnim,   state.focusDuration,   defaults.focusAnim,   defaults.focusDuration);
    fillAnim(state.unfocusAnim, state.unfocusDuration, defaults.unfocusAnim, defaults.unfocusDuration);

    if (hasRules) {
        g_mWindowRuleShaders[rawWin] = std::move(state);
        g_pHyprRenderer->damageWindow(pWindow);
    } else {
        g_mWindowRuleShaders.erase(rawWin);
    }
}
