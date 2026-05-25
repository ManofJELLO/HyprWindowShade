#include "Globals.hpp"
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
    if (auto fit = g_mFailedShaderMtimes.find(shaderPath); fit != g_mFailedShaderMtimes.end()) {
        if (currentMtime != 0 && fit->second == currentMtime)
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

    // --- SHADER WRAPPING (AUTO-ALPHA) ---
    // Tolerant `void main()` matcher — handles whitespace, newlines, and an
    // explicit `(void)` parameter list. If the shader has no main at all we
    // skip wrapping and let the GLSL compiler produce its own error.
    if (std::smatch m; std::regex_search(shaderCode, m, MAIN_RE)) {
        shaderCode.replace(m.position(0), m.length(0), "void user_main()");
        shaderCode += R"(
            uniform float plugin_alpha;
            void main() {
                user_main();
                fragColor *= plugin_alpha;
            }
        )";
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
    // Continuous redraw is needed only when the shader actually binds `time`.
    // Using the location instead of substring matching avoids false positives
    // like "lifetime" or "uniform_time_offset".
    entry.usesTime    = (entry.timeLoc >= 0);
    entry.sourceMtime = currentMtime;

    auto [insertedIt, _] = g_mCompiledCShaders.emplace(shaderPath, std::move(entry));
    return &insertedIt->second;
}
