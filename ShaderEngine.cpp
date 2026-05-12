#include "Globals.hpp"

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
    auto it = g_mCompiledCShaders.find(shaderPath);
    if (it != g_mCompiledCShaders.end()) return &it->second;

    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) return nullptr;

    std::stringstream buffer;
    buffer << shaderFile.rdbuf();
    std::string shaderCode = buffer.str();

    // --- SHADER WRAPPING (AUTO-ALPHA) ---
    size_t mainPos = shaderCode.find("void main()");
    if (mainPos != std::string::npos) {
        shaderCode.replace(mainPos, 11, "void user_main()");
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
        // Do not cache failed compiles: the user can fix the file and the next
        // draw call will retry without needing a manual reloadshaders.
        std::string log = captureFragmentLog(shaderCode);
        std::string msg = "[HyprWindowShade] Shader Compile FAILED: " + shaderPath;
        if (!log.empty()) {
            // Notifications are short — keep just the first ~200 chars of the log.
            if (log.size() > 200) log.resize(200);
            msg += "\n";
            msg += log;
        }
        HyprlandAPI::addNotification(PHANDLE, msg, CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 15000.0f);
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
    entry.usesTime = (entry.timeLoc >= 0);

    auto [insertedIt, _] = g_mCompiledCShaders.emplace(shaderPath, std::move(entry));
    return &insertedIt->second;
}
