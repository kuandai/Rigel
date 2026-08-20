#pragma once

/**
 * @file ShaderCompiler.h
 * @brief GLSL vertex/fragment shader compilation and linking utilities.
 */

#include <GL/glew.h>

#include <string>

namespace Rigel::Asset {

/**
 * @brief Source code for the two stages in a graphics shader program.
 */
struct ShaderSource {
    std::string vertex;
    std::string fragment;
};

/**
 * @brief Compile and link GLSL source into an OpenGL graphics program.
 *
 * A valid OpenGL context must be current on the calling thread. The caller
 * owns the returned program handle.
 */
class ShaderCompiler {
public:
    /**
     * @throws AssetLoadError if either required stage source is empty
     * @throws ShaderCompileError if a stage fails to compile
     * @throws ShaderLinkError if the program fails to link
     */
    static GLuint compile(const ShaderSource& source, const std::string& shaderId);

private:
    static GLuint compileStage(GLenum type, const std::string& source, const std::string& shaderId);
    static void checkCompileErrors(GLuint shader, GLenum stage, const std::string& shaderId);
    static void checkLinkErrors(GLuint program, const std::string& shaderId);
    static const char* stageToString(GLenum stage);
};

} // namespace Rigel::Asset
