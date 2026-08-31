#include "Globals.hpp"
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
Vector2D            g_pCurrentBoxSize;
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

// --- PATH RESOLUTION ---
// Returns a pointer into one of the global maps, or nullptr. The pointer is
// valid until the next mutation of the underlying map; render hooks run on the
// main thread between dispatcher/listener invocations so this is safe within a
// single draw chain.
static const std::string* resolveShaderPath(const PHLWINDOW& pWindow, const PHLLS& pLS) {
    if (pWindow) {
        Desktop::View::CWindow* rawWin = pWindow.get();

        if (auto it = g_mWindowManualShaders.find(rawWin); it != g_mWindowManualShaders.end())
            return &it->second;

        if (Fullscreen::controller()->isFullscreen(pWindow)) {
            auto it = g_mWindowRuleShaders.find(rawWin);
            if (it != g_mWindowRuleShaders.end() && !it->second.fullscreen.empty())
                return &it->second.fullscreen;
        } else {
            auto it = g_mWindowRuleShaders.find(rawWin);
            if (it != g_mWindowRuleShaders.end()) {
                const auto& state      = it->second;
                const bool  isActive   = Desktop::focusState()->isWindowActive(pWindow);
                const bool  isFloating = rawWin->m_isFloating;
                if      (isFloating  && !state.floating.empty())  return &state.floating;
                else if (!isFloating && !state.tiled.empty())     return &state.tiled;
                else if (isActive    && !state.active.empty())    return &state.active;
                else if (!isActive   && !state.inactive.empty())  return &state.inactive;
                else if (!state.fallback.empty())                 return &state.fallback;
            }
        }

        const auto& initClass    = rawWin->m_initialClass;
        const auto& currentClass = rawWin->m_class;
        auto classIt = g_mWindowClassShaderMap.find(initClass);
        if (classIt == g_mWindowClassShaderMap.end()) classIt = g_mWindowClassShaderMap.find(currentClass);
        if (classIt != g_mWindowClassShaderMap.end()) return &classIt->second;

        return nullptr;
    }

    if (pLS) {
        auto it = g_mLayerNamespaceShaderMap.find(pLS->m_namespace);
        if (it != g_mLayerNamespaceShaderMap.end()) return &it->second;
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

    auto sIt = g_mLayerOpenAnims.find(pLS->m_namespace);
    if (sIt == g_mLayerOpenAnims.end() || sIt->second.path.empty()) {
        g_mLayerOpenTimes.erase(tIt);
        return nullptr;
    }

    const std::string& path     = sIt->second.path;
    const float        duration = resolveAnimDuration(path, sIt->second.duration);
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
        return &anim.path;
    }
    return nullptr;
}

// --- SHADER STACKING ---

// True when this window asked to opt out of stacking and back to the old
// first-match-wins ladder.
static bool windowReplaceMode(const PHLWINDOW& pWindow) {
    auto it = g_mWindowRuleShaders.find(pWindow.get());
    return it != g_mWindowRuleShaders.end() && it->second.replaceMode;
}

// Collects the shader layers for a window or layer surface, bottom first. The
// order is the one users can reason about: the base look goes down first, then
// the geometry-conditional layer, then the focus-conditional layer, then
// fullscreen on top. A one-shot animation is added above all of these by the
// caller, since it can also apply to a fadeout that has no window left.
//
// Returns how many entries were written. Never exceeds MAX_SHADER_STAGES - 1,
// leaving room for the animation.
static int collectBaseLayers(const PHLWINDOW& pWindow, const PHLLS& pLS, const std::string* out[MAX_SHADER_STAGES]) {
    int n = 0;

    if (pWindow) {
        Desktop::View::CWindow* rawWin = pWindow.get();

        // A manual `togglewindowshader` is an explicit, imperative override —
        // it replaces the rule layers rather than joining them, which is the
        // whole point of toggling one on. Animations still stack above it.
        if (auto it = g_mWindowManualShaders.find(rawWin); it != g_mWindowManualShaders.end()) {
            out[n++] = &it->second;
            return n;
        }

        if (auto it = g_mWindowRuleShaders.find(rawWin); it != g_mWindowRuleShaders.end()) {
            const auto& state        = it->second;
            const bool  isActive     = Desktop::focusState()->isWindowActive(pWindow);
            const bool  isFloating   = rawWin->m_isFloating;
            const bool  isFullscreen = Fullscreen::controller()->isFullscreen(pWindow);

            // Fullscreen drops the window's shaders by default. Someone who
            // put a permanent effect on a browser almost certainly does not
            // want it over a fullscreen video or game, so fullscreen is opt-in
            // rather than opt-out: `+shader_fullscreen:` is the deliberate
            // exception, and `+shader_fullscreen_stack:1` restores the normal
            // stack for a window that genuinely wants it.
            if (isFullscreen && !state.fullscreenStack) {
                if (!state.fullscreen.empty())                  out[n++] = &state.fullscreen;
            } else {
                if (!state.fallback.empty())                    out[n++] = &state.fallback;
                if (isFloating)  { if (!state.floating.empty())  out[n++] = &state.floating; }
                else             { if (!state.tiled.empty())     out[n++] = &state.tiled; }
                if (isActive)    { if (!state.active.empty())    out[n++] = &state.active; }
                else             { if (!state.inactive.empty())  out[n++] = &state.inactive; }
                if (isFullscreen && !state.fullscreen.empty())   out[n++] = &state.fullscreen;
            }
        }

        // Class shaders are a coarser fallback than any rule, so they only speak
        // up when no rule layer did — same as before stacking existed.
        if (n == 0) {
            const auto& initClass    = rawWin->m_initialClass;
            const auto& currentClass = rawWin->m_class;
            auto        classIt      = g_mWindowClassShaderMap.find(initClass);
            if (classIt == g_mWindowClassShaderMap.end()) classIt = g_mWindowClassShaderMap.find(currentClass);
            if (classIt != g_mWindowClassShaderMap.end()) out[n++] = &classIt->second;
        }

        return n;
    }

    if (pLS) {
        auto it = g_mLayerNamespaceShaderMap.find(pLS->m_namespace);
        if (it != g_mLayerNamespaceShaderMap.end()) out[n++] = &it->second;
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
static SP<Render::ITexture> runIntermediateStages(CompiledShader* const* stages, int count, const SP<Render::ITexture>& src) {
    if (count <= 0 || !src || !Render::GL::g_pHyprOpenGL || !g_pHyprRenderer) return nullptr;

    // A rotated or flipped source buffer would need the targets' dimensions
    // swapped and the transform reapplied on the way back out. Rare enough that
    // dropping to single-stage shading beats getting it subtly wrong.
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

    // One entry per distinct on-screen window size, so this stays small on its
    // own. The clear is a backstop for pathological cases (a window being
    // resized continuously allocates a bucket per intermediate size); dropping
    // the pool costs one reallocation on the next frame and nothing else.
    if (g_pStageFBs->size() > 8) g_pStageFBs->clear();

    const uint64_t key = ((uint64_t)(uint32_t)w << 32) | (uint32_t)h;
    auto&          fbs = (*g_pStageFBs)[key];

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
    // would need the box transformed to match; bail to single-stage instead of
    // guessing, the same way a rotated source buffer does above.
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
    // animation on top of the stack, not to the layers it's animating.
    const float savedProgress = g_pCurrentAnimProgress;
    g_pCurrentAnimProgress = -1.0f;
    g_bIntermediatePass    = true;

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

        CHyprOpenGLImpl::STextureRenderData data;
        data.a        = 1.0f;
        data.damage   = &R.damage;
        data.allowDim = false;
        Render::GL::g_pHyprOpenGL->renderTexture(cur, box, data);

        cur = fb.getTexture();
    }

    g_bIntermediatePass    = false;
    g_pCurrentAnimProgress = savedProgress;

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

    g_pCurrentRenderWindow = pWindow;
    g_pCurrentRenderLayer  = pLS ? pLS : pOwnerLS;
    g_pCurrentAnimProgress = -1.0f;
    g_pCurrentAnimSeed     = -1.0f;

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

    // --- BUILD THE STACK ---
    // A one-shot animation always sits on top of whatever the window normally
    // looks like. Close animations have no window and no surface — they draw a
    // snapshot texture, which already has the window's own shaders baked in —
    // so they're matched on a separate branch and end up as the only stage.
    PHLMONITOR         animMonitor;
    const std::string* stack[MAX_SHADER_STAGES];
    int                nPaths = 0;

    const std::string* animPath = nullptr;
    if (pWindow)
        animPath = resolveOpenAnim(pWindow);
    else if (pLS)
        animPath = resolveLayerOpenAnim(pLS);
    else if (elem && !elem->m_data.surface && elem->m_data.tex)
        animPath = resolveCloseAnim(elem->m_data.tex, animMonitor);

    // The resolvers above set the progress/seed globals as a side effect. Hold
    // them aside: only the animation stage should see a real `progress`.
    const float animProgress = g_pCurrentAnimProgress;
    const float animSeed     = g_pCurrentAnimSeed;

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
    SP<Render::ITexture> originalTex;
    if (nStages >= 2 && elem && elem->m_data.tex) {
        if (auto composed = runIntermediateStages(stages, nStages - 1, elem->m_data.tex)) {
            originalTex       = elem->m_data.tex;
            elem->m_data.tex  = composed;
        }
        // If the chain couldn't run we fall through with the top stage only,
        // which is the pre-stacking behaviour rather than a broken frame.
    }

    g_pCurrentCompiledShader = nStages > 0 ? stages[nStages - 1] : nullptr;
    // Only the animation stage gets the live progress. If the animation shader
    // failed to compile, the top stage is an ordinary layer and must render its
    // finished state, not be dragged through an animation it never asked for.
    g_pCurrentAnimProgress   = topIsAnim ? animProgress : -1.0f;
    g_pCurrentAnimSeed       = animSeed;

    // Schedule continuous redraw if any stage uses `time`, or while a one-shot
    // animation is mid-flight — an animation shader drives itself off `progress`
    // and may never bind `time`, so it needs frames either way. Keyed on the
    // animation being *resolved* rather than compiled, so a broken animation
    // shader still ticks down and clears itself instead of stranding the window.
    if (animPath || stackUsesTime) {
        if (pWindow)
            g_pHyprRenderer->damageWindow(pWindow);
        else if (auto ls = pLS ? pLS : pOwnerLS) {
            if (auto mon = ls->m_monitor.lock())
                mon->scheduleFrame();
        } else if (animMonitor)
            animMonitor->scheduleFrame();
    }

    ((TGLDrawTex)g_pGLDrawTexHook->m_original)(thisptr, element, damage);

    // The element belongs to the pass, not to us — put its texture back before
    // anything else in the frame looks at it.
    if (originalTex) elem->m_data.tex = originalTex;

    g_pCurrentRenderWindow.reset();
    g_pCurrentRenderLayer.reset();
    g_pCurrentCompiledShader = nullptr;
    g_pCurrentAnimProgress   = -1.0f;
    g_pCurrentAnimSeed       = -1.0f;
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
static void tagFadeout(Desktop::IFadeout* key, const std::string& path, float duration, const void* seedSource) {
    FadeoutAnim anim;
    anim.path     = path;
    anim.start    = std::chrono::steady_clock::now();
    anim.duration = duration; // <0 -> resolved from the shader on first draw
    anim.seed     = animSeedFor(seedSource);
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
        if (auto mon = key->monitor().lock())
            mon->scheduleFrame();
        return false;
    }

    if (origDone) g_mFadeoutAnims.erase(it);
    return origDone;
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
    tagFadeout(result.get(), it->second.closeAnim, it->second.closeAnimDuration, window.get());

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

    auto it = g_mLayerCloseAnims.find(layer->m_namespace);
    if (it == g_mLayerCloseAnims.end() || it->second.path.empty()) return result;

    tagFadeout(result.get(), it->second.path, it->second.duration, layer.get());

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

        // Animation tags additionally accept an optional `@<seconds>` suffix to
        // override the duration the shader declares. rfind, so a path that
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
        // Not a shader, and deliberately has no `_default` form: "unset" and
        // "explicitly false" are the same value for a bool, so a default could
        // never be overridden back off by a more specific rule. Also does not
        // set hasRules — on its own it has nothing to apply, and a window
        // carrying only this tag should fall out of the map entirely rather
        // than being kept alive with an empty state.
        else if (!isDefault && key == "shader_replace")
            state.replaceMode = (val != "0" && val != "false" && val != "no" && val != "off");
        // Same shape, and the same reason for having no `_default` form.
        else if (!isDefault && key == "shader_fullscreen_stack")
            state.fullscreenStack = (val != "0" && val != "false" && val != "no" && val != "off");
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

    // The animation tags carry a duration alongside the path, so they promote as
    // a pair rather than through `fill`.
    if (state.openAnim.empty() && !defaults.openAnim.empty()) {
        state.openAnim         = std::move(defaults.openAnim);
        state.openAnimDuration = defaults.openAnimDuration;
        hasRules               = true;
    }
    if (state.closeAnim.empty() && !defaults.closeAnim.empty()) {
        state.closeAnim         = std::move(defaults.closeAnim);
        state.closeAnimDuration = defaults.closeAnimDuration;
        hasRules                = true;
    }

    if (hasRules) {
        g_mWindowRuleShaders[rawWin] = std::move(state);
        g_pHyprRenderer->damageWindow(pWindow);
    } else {
        g_mWindowRuleShaders.erase(rawWin);
    }
}
