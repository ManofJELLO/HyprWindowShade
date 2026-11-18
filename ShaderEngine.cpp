#include "Globals.hpp"
#include <algorithm>
#include <regex>

// Returns the file's mtime, or 0 if the file can't be stat()ed. The 0-sentinel
// also matches the default-initialized field on CompiledShader, so a missing
// file just behaves like "never seen before" rather than spuriously matching.
static time_t fileMtime(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;
    return st.st_mtime;
}

// Matches `void main()` / `void main ()` / `void main(void)` plus whitespace
// and newline variants. Compiled once (constructor runs at first call into
// getOrCompileShader, not per draw).
static const std::regex MAIN_RE(R"(\bvoid\s+main\s*\(\s*(?:void\s*)?\))");

// A shader used as an open/close animation declares how long it wants to run:
//
//     // @duration 0.35
//
// A comment rather than a real GLSL construct on purpose — GLSL ES forbids
// initializers on uniforms, so there's no in-language way to declare a value
// the CPU can read back before the shader ever runs. The plugin needs the
// number on the CPU side to know when the animation is over.
static const std::regex DURATION_RE(R"(//[ \t]*@duration[ \t]+([0-9]*\.?[0-9]+))");

// A move/resize shader can ask to keep running after the motion stops:
//
//     // @settle 0.4
//
// Same comment-directive reasoning as @duration. This is NOT a duration for the
// animation itself — the compositor's own move animation is the clock — it is
// only the tail on the end of it, which is why it is a separate directive
// rather than a reinterpretation of @duration.
static const std::regex SETTLE_RE(R"(//[ \t]*@settle[ \t]+([0-9]*\.?[0-9]+))");

// An open/close animation can ask to composite with Hyprland's own animation
// rather than replace it:
//
//     // @overlay
//
// A bare flag, so no capture group. Same comment-directive channel as @duration
// and @settle, and for the same reason: it has to reach layer animations too,
// and layers carry no rule tags.
static const std::regex OVERLAY_RE(R"(//[ \t]*@overlay\b)");

// The rounding mask appended below needs `v_texcoord` to locate the fragment
// within the box. Shaders that don't declare it keep the plain wrapper.
static const std::regex TEXCOORD_RE(R"(\bin\s+vec2\s+v_texcoord\s*;)");

// Reads the `// @duration` directive out of shader source. Returns <0 only when
// the directive is absent or unparseable, meaning "caller picks". An over-long
// duration is CLAMPED to MAX_ANIM_DURATION rather than discarded: discarding it
// looked identical to "no directive at all", so a shader declaring `@duration 8`
// got the 0.3s default *and* a toast telling it to declare a duration it had
// already declared.
static float parseDeclaredDuration(const std::string& src) {
    std::smatch m;
    if (!std::regex_search(src, m, DURATION_RE)) return -1.0f;
    try {
        const float d = std::stof(m[1].str());
        if (d > 0.0f) return std::min(d, MAX_ANIM_DURATION);
    } catch (...) {}
    return -1.0f;
}

// Reads `// @settle` out of shader source. 0 when absent — a shader that didn't
// ask for a tail doesn't get one, since a tail keeps a *live* window damaging
// every frame and has none of the natural backstops a close animation has.
static float parseDeclaredSettle(const std::string& src) {
    std::smatch m;
    if (!std::regex_search(src, m, SETTLE_RE)) return 0.0f;
    try {
        const float d = std::stof(m[1].str());
        if (d > 0.0f) return std::min(d, MAX_ANIM_DURATION);
    } catch (...) {}
    return 0.0f;
}

// Compile the fragment shader standalone purely to capture glGetShaderInfoLog
// text. Only called after CShader::createProgram has already failed, so the
// extra compile cost is paid once per broken edit, not per draw.
// True when the shader carries `// @overlay`. Read off the ORIGINAL source, like
// the other directives, before the auto-alpha wrapper appends anything.
static bool parseDeclaredOverlay(const std::string& src) {
    return std::regex_search(src, OVERLAY_RE);
}

// `// @speed <n>` — Umbriel's preset-speed contract: the host premultiplies
// umbriel_time by the preset's speed (src/view/effects.cpp: seconds *
// preset->speed). Our port feeds raw seconds, so the directive does the same
// job here. Defaults to 1 (raw seconds).
static const std::regex SPEED_RE(R"(//[ \t]*@speed[ \t]+([0-9]*\.?[0-9]+))");
static float parseDeclaredSpeed(const std::string& src) {
    std::smatch m;
    if (!std::regex_search(src, m, SPEED_RE)) return 1.0f;
    try {
        const float s = std::stof(m[1].str());
        return s > 0.0f ? s : 1.0f;
    } catch (...) { return 1.0f; }
}

static std::string captureFragmentLog(const std::string& src) {
    GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
    const char* csrc = src.c_str();
    glShaderSource(sh, 1, &csrc, nullptr);
    glCompileShader(sh);

    GLint status = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &status);

    std::string log;
    GLint len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    if (len > 1) {
        log.resize(static_cast<size_t>(len));
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        // glGetShaderInfoLog writes a trailing NUL; trim it.
        while (!log.empty() && log.back() == '\0') log.pop_back();
    }
    glDeleteShader(sh);
    (void)status;
    return log;
}

// --- HELPER: SHADER COMPILATION (WITH AUTO-ALPHA INJECTION) ---
// V0.55: No longer needs thisptr — we use global g_pHyprOpenGL directly.
// How often a cached shader's mtime is actually re-checked. Hot-reload within a
// quarter second is indistinguishable from instant when you are saving a file in
// an editor, and it turns a per-frame syscall into a rare one.
static constexpr auto STAT_INTERVAL = std::chrono::milliseconds(250);

CompiledShader* getOrCompileShader(const std::string& shaderPath) {
    const auto now = std::chrono::steady_clock::now();

    // Success cache. If mtime matches we serve the cached entry; otherwise the
    // file was edited and we drop it so the recompile path below runs. mtime==0
    // (stat failed) is treated as "unchanged" — we keep serving cached shader
    // rather than evicting on a transient stat error.
    //
    // The mtime check is what makes editing a .glsl take effect on the next
    // frame, but it is a stat() and this function runs per stage per textured
    // surface per frame — a shaded window with a few subsurfaces was issuing
    // hundreds of them a second purely to notice an edit that had not happened.
    // Throttled to STAT_INTERVAL, which removes essentially all of them without
    // making a save feel any less immediate.
    if (auto it = g_mCompiledCShaders.find(shaderPath); it != g_mCompiledCShaders.end()) {
        if (now - it->second.lastStatAt < STAT_INTERVAL)
            return &it->second;
        it->second.lastStatAt = now;

        const time_t mtimeNow = fileMtime(shaderPath);
        if (mtimeNow == 0 || it->second.sourceMtime == mtimeNow)
            return &it->second;
        g_mCompiledCShaders.erase(it);
    }

    // Reached only on a cache miss or an actual edit, so the stat here is rare.
    const time_t currentMtime = fileMtime(shaderPath);

    // Failure cache. If the same path failed with the same mtime, the source
    // hasn't changed, so retrying would just fail identically — return nullptr
    // silently. Once the user saves a fix the mtime advances and we fall
    // through to recompile.
    //
    // mtime 0 (stat failed / file missing) is a value like any other here, so a
    // path that doesn't exist stays suppressed. Guarding this on `currentMtime
    // != 0` meant the missing-file entry cached below was erased on every
    // lookup, costing a stat() + open() per textured surface per frame for as
    // long as a rule pointed at a typo.
    if (auto fit = g_mFailedShaderMtimes.find(shaderPath); fit != g_mFailedShaderMtimes.end()) {
        if (fit->second == currentMtime)
            return nullptr;
        g_mFailedShaderMtimes.erase(fit);
    }

    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) {
        // Don't toast on missing file every frame either — cache the "failure"
        // under mtime 0; gets cleared the moment the file appears with any
        // nonzero mtime.
        g_mFailedShaderMtimes[shaderPath] = 0;
        return nullptr;
    }

    std::stringstream buffer;
    buffer << shaderFile.rdbuf();
    std::string shaderCode = buffer.str();

    // Read the animation duration off the ORIGINAL source, before the auto-alpha
    // wrapper appends anything.
    const float declaredDuration = parseDeclaredDuration(shaderCode);
    const float declaredSettle   = parseDeclaredSettle(shaderCode);
    const bool  declaredOverlay  = parseDeclaredOverlay(shaderCode);

    // --- SHADER WRAPPING (AUTO-ALPHA) ---
    // Tolerant `void main()` matcher — handles whitespace, newlines, and an
    // explicit `(void)` parameter list. If the shader has no main at all we
    // skip wrapping and let the GLSL compiler produce its own error.
    if (std::smatch m; std::regex_search(shaderCode, m, MAIN_RE)) {
        shaderCode.replace(m.position(0), m.length(0), "void user_main()");
        shaderCode += R"(
            uniform float plugin_alpha;
        )";

        // Corner rounding, alpha and dim are re-applied below ONLY for the
        // fallback draw path. Normally every stage renders offscreen and
        // Hyprland draws the composed result with its own program, which
        // applies all three natively — there these multiplies see 1.0 / 0 and
        // do nothing. They matter when runIntermediateStages declines the job
        // (a rotated source buffer, a projection that rotates), where our shader is
        // the on-screen draw and nothing else would put them back.
        //
        // Only when the shader declares `v_texcoord`, which the mask needs to
        // know where in the box it is. Every shader following the documented
        // interface does; one that works purely off gl_FragCoord would fail to
        // compile if we referenced it anyway, and losing rounding is a much
        // smaller problem than losing the shader.
        if (std::regex_search(shaderCode, TEXCOORD_RE)) {
            shaderCode += R"(
            uniform vec2  plugin_box_size;
            uniform float plugin_round;
            uniform float plugin_round_power;
            uniform float plugin_dim;
            void main() {
                user_main();
                // Hyprland's dim_inactive is a tint multiply inside the
                // fragment program we replaced, so re-apply it here or a shaded
                // window is undimmed for exactly as long as the shader is bound.
                fragColor.rgb *= plugin_dim;
                // Normalise to premultiplied alpha. Surface colours are
                // premultiplied, and the compositor blends with
                // `src.rgb + dst * (1 - src.a)`, so a fragment carrying more
                // colour than its alpha allows is *added* to whatever is behind
                // it instead of covering it — a shader that returns full-
                // intensity colour at a low alpha paints a pale slab over
                // popups and shadows. Clamping fixes the colour channel without
                // touching alpha, so shaders that drive an effect through alpha
                // (every open/close animation) are unaffected.
                fragColor.rgb = min(fragColor.rgb, vec3(fragColor.a));
                fragColor *= plugin_alpha;
                if (plugin_round > 0.0 && plugin_box_size.x > 0.0) {
                    // Distance past the corner's rounding square, measured as a
                    // superellipse so `roundingPower` behaves as it does in
                    // Hyprland's own shader.
                    vec2  p = v_texcoord * plugin_box_size;
                    vec2  c = min(p, plugin_box_size - p);
                    vec2  d = max(vec2(plugin_round) - c, vec2(0.0));
                    float e = pow(pow(d.x, plugin_round_power) + pow(d.y, plugin_round_power), 1.0 / plugin_round_power);
                    // discard, not just a zero alpha. Zeroing alpha alone was
                    // measured not to cut the corner at all on a shader that
                    // writes its own alpha; discard does, and is what Hyprland's
                    // own rounding uses. The alpha ramp after it softens the
                    // edge on top of that.
                    if (e > plugin_round) discard;
                    fragColor *= 1.0 - smoothstep(plugin_round - 1.0, plugin_round, e);
                }
            }
        )";
        } else {
            shaderCode += R"(
            uniform float plugin_dim;
            void main() {
                user_main();
                // Same dim re-application as the rounded variant above.
                fragColor.rgb *= plugin_dim;
                // Normalise to premultiplied alpha. Surface colours are
                // premultiplied, and the compositor blends with
                // `src.rgb + dst * (1 - src.a)`, so a fragment carrying more
                // colour than its alpha allows is *added* to whatever is behind
                // it instead of covering it — a shader that returns full-
                // intensity colour at a low alpha paints a pale slab over
                // popups and shadows. Clamping fixes the colour channel without
                // touching alpha, so shaders that drive an effect through alpha
                // (every open/close animation) are unaffected.
                fragColor.rgb = min(fragColor.rgb, vec3(fragColor.a));
                fragColor *= plugin_alpha;
            }
        )";
        }
    }

    CompiledShader entry;
    entry.shader = Hyprutils::Memory::makeShared<CShader>();

    // V0.55: Access g_pHyprOpenGL directly (lives in Render::GL namespace)
    entry.shader->createProgram(Render::GL::g_pHyprOpenGL->m_shaders->TEXVERTSRC320, shaderCode, true, true);

    const GLuint prog = entry.shader->program();
    // Window-pass shaders may declare the theme palette too (Umbriel's paired
    // window overlays read the same stops the border effect sees).
    entry.paletteCountLoc = glGetUniformLocation(prog, "umbriel_palette_count");
    entry.paletteLoc      = glGetUniformLocation(prog, "umbriel_palette");
    if (prog == 0) {
        std::string log = captureFragmentLog(shaderCode);
        std::string msg = "[HyprWindowShade] Shader Compile FAILED: " + shaderPath;
        if (!log.empty()) {
            // Notifications are short — keep just the first ~200 chars of the log.
            if (log.size() > 200) log.resize(200);
            msg += "\n";
            msg += log;
        }
        HyprlandAPI::addNotification(PHANDLE, msg, CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 15000.0f);
        // Remember this mtime so subsequent frames don't re-fire the toast
        // until the user saves a fix.
        g_mFailedShaderMtimes[shaderPath] = currentMtime;
        return nullptr;
    }

    // Cache uniform locations once; -1 means "not present in this shader".
    entry.timeLoc         = glGetUniformLocation(prog, "time");
    entry.alphaLoc        = glGetUniformLocation(prog, "plugin_alpha");
    entry.resolutionLoc   = glGetUniformLocation(prog, "resolution");
    entry.surfaceSizeLoc  = glGetUniformLocation(prog, "surface_size");
    entry.mouseLoc        = glGetUniformLocation(prog, "mouse");
    entry.isActiveLoc     = glGetUniformLocation(prog, "is_active");
    entry.isFloatingLoc   = glGetUniformLocation(prog, "is_floating");
    entry.isFullscreenLoc = glGetUniformLocation(prog, "is_fullscreen");
    entry.progressLoc     = glGetUniformLocation(prog, "progress");
    entry.seedLoc         = glGetUniformLocation(prog, "seed");
    entry.boxSizeLoc      = glGetUniformLocation(prog, "plugin_box_size");
    entry.roundLoc        = glGetUniformLocation(prog, "plugin_round");
    entry.roundPowerLoc   = glGetUniformLocation(prog, "plugin_round_power");
    entry.dimLoc          = glGetUniformLocation(prog, "plugin_dim");
    entry.moveDeltaLoc     = glGetUniformLocation(prog, "move_delta");
    entry.moveRemainingLoc = glGetUniformLocation(prog, "move_remaining");
    entry.velocityLoc      = glGetUniformLocation(prog, "velocity");
    entry.sizeDeltaLoc     = glGetUniformLocation(prog, "size_delta");
    entry.sizeVelocityLoc  = glGetUniformLocation(prog, "size_velocity");
    entry.windowBoxLoc     = glGetUniformLocation(prog, "window_box");
    entry.windowRectLoc    = glGetUniformLocation(prog, "window_rect");
    entry.isMovingLoc      = glGetUniformLocation(prog, "is_moving");
    entry.isResizingLoc    = glGetUniformLocation(prog, "is_resizing");
    entry.isDraggingLoc    = glGetUniformLocation(prog, "is_dragging");
    entry.animKindLoc      = glGetUniformLocation(prog, "anim_kind");
    entry.curveLoc         = glGetUniformLocation(prog, "curve");
    entry.durationLoc      = glGetUniformLocation(prog, "duration");
    entry.settleLoc        = glGetUniformLocation(prog, "settle");
    entry.releaseVelLoc    = glGetUniformLocation(prog, "release_velocity");
    entry.peakVelLoc       = glGetUniformLocation(prog, "peak_velocity");
    entry.peakSizeVelLoc   = glGetUniformLocation(prog, "peak_size_velocity");
    entry.animDuration    = declaredDuration;
    entry.settleDuration  = declaredSettle;
    entry.wantsOverlay    = declaredOverlay;
    // Continuous redraw is needed only when the shader actually binds `time`.
    // Using the location instead of substring matching avoids false positives
    // like "lifetime" or "uniform_time_offset".
    entry.usesTime    = (entry.timeLoc >= 0);
    entry.timeSpeed   = parseDeclaredSpeed(shaderCode);
    entry.sourceMtime = currentMtime;
    entry.lastStatAt  = now;

    auto [insertedIt, _] = g_mCompiledCShaders.emplace(shaderPath, std::move(entry));
    return &insertedIt->second;
}

// --- GL-FREE DIRECTIVE READ ---
// getOrCompileShader resolves `@duration` and `@overlay` as a side effect of
// compiling, which is fine on the draw path but useless to a close: the layout
// reflow that has to agree with the close shader's clock is started inside
// CWindow::unmapWindow, a wayland event handler with no guarantee of a current
// GL context. Compiling there would fail and poison the failure cache — the same
// reason holdFadeoutOpen refuses to call getOrCompileShader.
//
// Both directives are plain comment scans over the source, so they are readable
// without a context. A compiled entry that still matches the file on disk answers
// for free; otherwise the source is read, which happens once per close rather
// than per draw. Returns false only when the source cannot be read at all, which
// the caller treats as "no close animation".
bool declaredAnimTiming(const std::string& shaderPath, float& duration, bool& overlay) {
    // A shader already known not to compile has no animation to time anything
    // against, so report it as unusable rather than reading a duration out of it.
    // Without this a close shader with a GLSL typo — which draws nothing at all,
    // since it never reaches the stack — would still stretch every survivor's
    // reflow to its declared duration, turning a broken effect into a workspace
    // that reflows in slow motion. Same mtime rule as the success cache: once the
    // file is saved again the entry stops matching and the shader gets another go.
    if (auto fit = g_mFailedShaderMtimes.find(shaderPath); fit != g_mFailedShaderMtimes.end()) {
        if (fit->second == fileMtime(shaderPath)) return false;
    }

    // The compiled entry is only trustworthy while it still matches the file.
    // Unlike the draw path there is no throttle to justify here: this runs once
    // per close, not per surface per frame, and serving a stale directive would
    // mean an edited `@duration` took effect for the animation but not for the
    // reflow timed against it.
    if (auto it = g_mCompiledCShaders.find(shaderPath); it != g_mCompiledCShaders.end()) {
        const time_t mtimeNow = fileMtime(shaderPath);
        if (mtimeNow == 0 || it->second.sourceMtime == mtimeNow) {
            duration = it->second.animDuration;
            overlay  = it->second.wantsOverlay;
            return true;
        }
    }

    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) return false;

    std::stringstream buffer;
    buffer << shaderFile.rdbuf();
    const std::string src = buffer.str();

    duration = parseDeclaredDuration(src);
    overlay  = parseDeclaredOverlay(src);
    return true;
}

// A border shader can declare the ring width it needs, in px:
//
//     // @border 14
//
// Applied on activation (the ring is Hyprland's own general:border_size and
// has no per-shader equivalent), restored when the last border assignment is
// cleared. Same comment-directive channel as @duration: the CPU has to know
// the number and GLSL ES cannot carry it in a uniform initializer.
static const std::regex BORDER_RE(R"(//[ \t]*@border[ \t]+([0-9]*\.?[0-9]+))");

static int parseDeclaredBorderWidth(const std::string& shaderPath) {
    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) return 0;
    std::stringstream buffer;
    buffer << shaderFile.rdbuf();
    const std::string src = buffer.str();
    std::smatch m;
    if (!std::regex_search(src, m, BORDER_RE)) return 0;
    try {
        const int w = std::round(std::stof(m[1].str()));
        return w > 0 ? w : 0;
    } catch (...) {}
    return 0;
}

int declaredBorderWidth(const std::string& shaderPath) {
    return parseDeclaredBorderWidth(shaderPath);
}

// An effect can ask for paint space BEYOND the ring, in px:
//
//     // @padding 10
//
// Umbriel's model (border_rect.h): the effect declares transparent padding and
// the canvas grows to `thickness + margin + padding` while the LAYOUT keeps
// budgeting only the border width — the padding paints over the existing gap
// space. Same here: the hook expands the canvas by this amount and damages it,
// without touching layout or gaps.
static const std::regex PADDING_RE(R"(//[ \t]*@padding[ \t]+([0-9]*\.?[0-9]+))");

static int parseDeclaredBorderPadding(const std::string& shaderPath) {
    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) return 0;
    std::stringstream buffer;
    buffer << shaderFile.rdbuf();
    const std::string src = buffer.str();
    std::smatch m;
    if (!std::regex_search(src, m, PADDING_RE)) return 0;
    try {
        const int p = std::round(std::stof(m[1].str()));
        return p > 0 ? p : 0;
    } catch (...) {}
    return 0;
}

int declaredBorderPadding(const std::string& shaderPath) {
    return parseDeclaredBorderPadding(shaderPath);
}

// --- BORDER SHADER COMPILATION (Umbriel-style ring pass) ---
// Border shaders draw in Hyprland's own border pass: a ring quad around the
// window (window box + border_size on every side), with the window interior
// scissored away. The contract mirrors Umbriel's border effects, so community
// shaders port with minimal edits:
//
//   vec4 border(vec2 uv)   // premultiplied colour+coverage, uv 0..1 over the ring
//
// The wrapper provides everything Umbriel's compositor provides: umbriel_size,
// umbriel_scale, umbriel_time, umbriel_border_hole, umbriel_border_radius, the
// umbriel_border_distance() rounded-box SDF, and the hole cut with one buffer
// pixel of AA — Umbriel's kBorderSuffix, ported verbatim. umbriel_palette_count
// reads 0 so `theme_color` guards pass; umbriel_expand is fixed at 0 because
// this canvas is already the ring itself.
//
// Compiled and cached alongside window shaders under the "border:" key prefix:
// the cache, hot-reload and failure handling are identical, and a border shader
// and a window shader can share a path without colliding.
static const char kBorderPreamble[] = R"(
precision highp float;
in vec2 v_texcoord;
out vec4 fragColor;

// Provided by HyprWindowShade — Umbriel border API.
uniform vec2  umbriel_size;          // ring canvas in logical px
uniform float umbriel_scale;         // monitor scale
uniform vec4  umbriel_border_hole;   // window rect inside the canvas, normalised
uniform vec4  umbriel_border_radius; // per-corner radii, logical px (tl, tr, br, bl)
uniform vec2  umbriel_expand;        // declared effect padding, normalised to the canvas
uniform int   umbriel_palette_count; // 0 — theme palettes not supported here
uniform float plugin_alpha;          // window opacity
uniform float time;                  // seconds since plugin load; drives redraw
uniform float seed;                  // per-window random seed

#define umbriel_time time
#define surface_size umbriel_size
// Theme palettes are not available in this pass: count reads 0 and
// umbriel_palette_at returns a neutral ice-white so effects that sample the
// palette without a guard (e.g. dual-orbit) stay visible. Guarding effects
// check umbriel_palette_count first and never reach this.
uniform vec4 umbriel_palette[4];
vec4 umbriel_palette_at(float t) {
    if (umbriel_palette_count <= 0) return vec4(0.07, 0.12, 0.19, 1.0);
    float span = float(umbriel_palette_count);
    float pos  = clamp(t, 0.0, 0.999) * span;
    float index = floor(pos);
    float next = index + 1.0;
    vec4 from = umbriel_palette[0];
    vec4 to = umbriel_palette[0];
    for (int i = 0; i < 4; i++) {
        if (float(i) >= span) break;
        if (float(i) == index) from = umbriel_palette[i];
        if (float(i) == next) to = umbriel_palette[i];
    }
    return mix(from, to, pos - index);
}

// Rounded-box SDF of the window hole, signed LOGICAL px, negative inside the
// window. Ported from Umbriel's kBorderSection (effect_shader.c).
float umbriel_border_distance(vec2 uv) {
  vec2 half_size = umbriel_border_hole.zw * umbriel_size * 0.5;
  vec2 p = (uv - umbriel_border_hole.xy) * umbriel_size - half_size;
  float r = p.y < 0.0 ? (p.x < 0.0 ? umbriel_border_radius.x : umbriel_border_radius.y)
                      : (p.x < 0.0 ? umbriel_border_radius.w : umbriel_border_radius.z);
  r = min(r, min(half_size.x, half_size.y));
  vec2 q = abs(p) - half_size + r;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// Window-content sampling is not available in the border pass (the ring has no
// window texture bound). Effects that call umbriel_sample get a SYNTHESIZED
// native instead: the window's coverage footprint — opaque inside the hole,
// then Umbriel's thin native rim (border_width defaults to 2) — with no
// colour. The rest of the reserved band stays transparent so the effect's own
// glow paints it: with a full-width opaque ring, (1 - native.a) would be zero
// across the band and halo-style effects (dual-orbit) would lose their heads.
vec4 umbriel_sample(vec2 uv) {
    float d = umbriel_border_distance(uv);
    float a = d < 0.0 ? 1.0 : (1.0 - smoothstep(1.5, 2.5, d));
    return vec4(0.0, 0.0, 0.0, a);
}
)";

static const char kBorderSuffix[] = R"(
void main() {
    vec4 c = border(v_texcoord);
    // Umbriel's compositor suffix: the client hole is cut with one buffer px of AA.
    c *= smoothstep(-0.5, 0.5, umbriel_border_distance(v_texcoord) * umbriel_scale);
    // Premultiplied output (blend is GL_ONE, GL_ONE_MINUS_SRC_ALPHA): clamp
    // colour to what alpha allows, then fold in the window's opacity.
    fragColor = vec4(min(c.rgb, vec3(c.a)), c.a) * plugin_alpha;
}
)";

CompiledShader* getOrCompileBorderShader(const std::string& shaderPath) {
    const auto now = std::chrono::steady_clock::now();
    // Same cache contract as getOrCompileShader, namespaced so a border shader
    // and a window shader can share a path without colliding.
    const std::string cacheKey = "border:" + shaderPath;

    if (auto it = g_mCompiledCShaders.find(cacheKey); it != g_mCompiledCShaders.end()) {
        if (now - it->second.lastStatAt < STAT_INTERVAL)
            return &it->second;
        it->second.lastStatAt = now;
        const time_t mtimeNow = fileMtime(shaderPath);
        if (mtimeNow == 0 || it->second.sourceMtime == mtimeNow)
            return &it->second;
        g_mCompiledCShaders.erase(it);
    }

    const time_t currentMtime = fileMtime(shaderPath);

    if (auto fit = g_mFailedShaderMtimes.find(cacheKey); fit != g_mFailedShaderMtimes.end()) {
        if (fit->second == currentMtime)
            return nullptr;
        g_mFailedShaderMtimes.erase(fit);
    }

    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) {
        g_mFailedShaderMtimes[cacheKey] = 0;
        return nullptr;
    }

    std::stringstream buffer;
    buffer << shaderFile.rdbuf();
    std::string shaderCode = buffer.str();

    // The preamble must sit AFTER the file's `#version` line (GLSL requires
    // #version first), so split on the first newline.
    const size_t eol = shaderCode.find('\n');
    if (eol == std::string::npos || shaderCode.compare(0, 8, "#version") != 0) {
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] Border shader must start with `#version 320 es`: " + shaderPath,
                                     CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 10000.0f);
        g_mFailedShaderMtimes[cacheKey] = currentMtime;
        return nullptr;
    }
    // A `void main(` in the border file would fight the wrapper's suffix main.
    // Rename it like the window path does; the function stays but never runs.
    if (std::smatch m; std::regex_search(shaderCode, m, MAIN_RE))
        shaderCode.replace(m.position(0), m.length(0), "void border_user_main()");

    shaderCode.insert(eol + 1, kBorderPreamble);
    shaderCode += kBorderSuffix;

    CompiledShader entry;
    entry.shader = Hyprutils::Memory::makeShared<CShader>();
    entry.shader->createProgram(Render::GL::g_pHyprOpenGL->m_shaders->TEXVERTSRC320, shaderCode, true, true);

    const GLuint prog = entry.shader->program();
    if (prog == 0) {
        std::string log = captureFragmentLog(shaderCode);
        std::string msg = "[HyprWindowShade] Border shader compile FAILED: " + shaderPath;
        if (!log.empty()) {
            if (log.size() > 200) log.resize(200);
            msg += "\n";
            msg += log;
        }
        HyprlandAPI::addNotification(PHANDLE, msg, CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 15000.0f);
        g_mFailedShaderMtimes[cacheKey] = currentMtime;
        return nullptr;
    }

    // umbriel_palette: a fixed 4-stop ice-white ramp, so palette-sampling
    // effects get something visible; effects that guard on the count ignore it.
    entry.paletteCountLoc = glGetUniformLocation(prog, "umbriel_palette_count");
    entry.paletteLoc      = glGetUniformLocation(prog, "umbriel_palette");
    entry.expandLoc       = glGetUniformLocation(prog, "umbriel_expand");
    entry.paddingPx       = parseDeclaredBorderPadding(shaderPath);
    entry.timeLoc         = glGetUniformLocation(prog, "time");
    entry.seedLoc         = glGetUniformLocation(prog, "seed");
    entry.alphaLoc        = glGetUniformLocation(prog, "plugin_alpha");
    entry.surfaceSizeLoc  = glGetUniformLocation(prog, "umbriel_size");
    entry.holeLoc         = glGetUniformLocation(prog, "umbriel_border_hole");
    entry.radiusLoc       = glGetUniformLocation(prog, "umbriel_border_radius");
    entry.scaleLoc        = glGetUniformLocation(prog, "umbriel_scale");
    entry.usesTime        = (entry.timeLoc >= 0);
    entry.timeSpeed       = parseDeclaredSpeed(shaderCode);
    entry.sourceMtime     = currentMtime;
    entry.lastStatAt      = now;

    auto [insertedIt, _] = g_mCompiledCShaders.emplace(cacheKey, std::move(entry));
    return &insertedIt->second;
}
