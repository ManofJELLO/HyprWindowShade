#include "Globals.hpp"

// --- HELPER: SHADER COMPILATION (WITH AUTO-ALPHA INJECTION) ---
// V0.55: No longer needs thisptr — we use global g_pHyprOpenGL directly.
Hyprutils::Memory::CSharedPointer<CShader> getOrCompileShader(const std::string& shaderPath) {
    if (g_mCompiledCShaders.find(shaderPath) == g_mCompiledCShaders.end()) {
        std::ifstream shaderFile(shaderPath);
        if (shaderFile.is_open()) {
            std::stringstream buffer;
            buffer << shaderFile.rdbuf();

            std::string shaderCode = buffer.str();

            // --- SMART DAMAGE DETECTION ---
            g_mShaderUsesTime[shaderPath] = (shaderCode.find("time") != std::string::npos);

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

            auto customShader = Hyprutils::Memory::makeShared<CShader>();

            // V0.55: Access g_pHyprOpenGL directly (lives in Render::GL namespace)
            customShader->createProgram(Render::GL::g_pHyprOpenGL->m_shaders->TEXVERTSRC320, shaderCode, true, true);
            g_mCompiledCShaders[shaderPath] = customShader;

            if (customShader->program() == 0) {
                HyprlandAPI::addNotification(PHANDLE, "[HyprWindowShade] Shader Compile FAILED!", CHyprColor(1.0f, 0.0f, 0.0f, 1.0f), 10000.0f);
            }
        } else {
            return nullptr; // File not found
        }
    }
    return g_mCompiledCShaders[shaderPath];
}