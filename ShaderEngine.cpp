#include "Globals.hpp"

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

    // --- SMART DAMAGE DETECTION ---
    const bool usesTime = (shaderCode.find("time") != std::string::npos);

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
    entry.shader   = Hyprutils::Memory::makeShared<CShader>();
    entry.usesTime = usesTime;

    // V0.55: Access g_pHyprOpenGL directly (lives in Render::GL namespace)
    entry.shader->createProgram(Render::GL::g_pHyprOpenGL->m_shaders->TEXVERTSRC320, shaderCode, true, true);

    const GLuint prog = entry.shader->program();
    if (prog == 0) {
        HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] Shader Compile FAILED!", CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 10000.0f);
    } else {
        // Cache uniform locations once; -1 means "not present in this shader".
        entry.timeLoc  = glGetUniformLocation(prog, "time");
        entry.alphaLoc = glGetUniformLocation(prog, "plugin_alpha");
    }

    auto [insertedIt, _] = g_mCompiledCShaders.emplace(shaderPath, std::move(entry));
    return &insertedIt->second;
}
