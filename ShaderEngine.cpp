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

// Compile the fragment shader standalone purely to capture glGetShaderInfoLog
// text. Only called after CShader::createProgram has already failed, so the
// extra compile cost is paid once per broken edit, not per draw.
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
CompiledShader* getOrCompileShader(const std::string& shaderPath) {
    const time_t currentMtime = fileMtime(shaderPath);

    // Success cache. If mtime matches we serve the cached entry; otherwise the
    // file was edited and we drop it so the recompile path below runs. mtime==0
    // (stat failed) is treated as "unchanged" — we keep serving cached shader
    // rather than evicting on a transient stat error.
    if (auto it = g_mCompiledCShaders.find(shaderPath); it != g_mCompiledCShaders.end()) {
        if (currentMtime == 0 || it->second.sourceMtime == currentMtime)
            return &it->second;
        g_mCompiledCShaders.erase(it);
    }

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

    // --- SHADER WRAPPING (AUTO-ALPHA) ---
    // Tolerant `void main()` matcher — handles whitespace, newlines, and an
    // explicit `(void)` parameter list. If the shader has no main at all we
    // skip wrapping and let the GLSL compiler produce its own error.
    if (std::smatch m; std::regex_search(shaderCode, m, MAIN_RE)) {
        shaderCode.replace(m.position(0), m.length(0), "void user_main()");
        shaderCode += R"(
            uniform float plugin_alpha;
        )";

        // Corner rounding lives in the fragment program Hyprland would have
        // bound here, so replacing that program squares off every shaded
        // window. Redo the mask in the wrapper instead.
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
            void main() {
                user_main();
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
            void main() {
                user_main();
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
    entry.animDuration    = declaredDuration;
    // Continuous redraw is needed only when the shader actually binds `time`.
    // Using the location instead of substring matching avoids false positives
    // like "lifetime" or "uniform_time_offset".
    entry.usesTime    = (entry.timeLoc >= 0);
    entry.sourceMtime = currentMtime;

    auto [insertedIt, _] = g_mCompiledCShaders.emplace(shaderPath, std::move(entry));
    return &insertedIt->second;
}
