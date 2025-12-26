#pragma once

/**
 * @file ShaderCompiler.h
 * @brief GLSL shader compilation and linking utilities.
 *
 * This file provides the ShaderCompiler class for compiling GLSL shader source
 * code into OpenGL shader programs. It handles:
 * - Preprocessing (injecting #define directives)
 * - Compilation of individual shader stages
 * - Linking stages into a complete program
 * - Detailed error reporting
 *
 * @section supported_shaders Supported Shader Types
 *
 * The compiler supports two shader configurations:
 *
 * **Graphics Shaders:**
 * - Vertex shader (required)
 * - Fragment shader (required)
 * - Geometry shader (optional)
 *
 * **Compute Shaders:**
 * - Compute shader (standalone, mutually exclusive with graphics shaders)
 *
 * @section preprocessing Preprocessing
 *
 * The compiler performs simple preprocessing on shader source:
 * 1. Locates the `#version` directive (or uses default `#version 410 core`)
 * 2. Injects `#define` statements immediately after the version line
 * 3. Converts boolean string values ("true"/"false") to "1"/"0"
 *
 * This allows manifest-driven configuration of shader behavior:
 *
 * @code{.yaml}
 * shaders:
 *   lit_shader:
 *     vertex: shaders/standard.vert
 *     fragment: shaders/standard.frag
 *     defines:
 *       USE_LIGHTING: true
 *       MAX_LIGHTS: 8
 *       SHADOW_QUALITY: 2
 * @endcode
 *
 * Results in the following being injected into shader source:
 * @code{.glsl}
 * #version 410 core
 * #define USE_LIGHTING 1
 * #define MAX_LIGHTS 8
 * #define SHADOW_QUALITY 2
 * // ... rest of shader ...
 * @endcode
 *
 * @section error_handling Error Handling
 *
 * Compilation and linking errors throw typed exceptions:
 * - ShaderCompileError: Includes the failing stage and compiler log
 * - ShaderLinkError: Includes the linker log
 *
 * Error messages include the shader ID for easy identification:
 * @code
 * try {
 *     GLuint program = ShaderCompiler::compile(source, "shaders/standard");
 * } catch (const ShaderCompileError& e) {
 *     spdlog::error("Shader {} failed at {} stage:\n{}",
 *                   e.assetId(), stageToString(e.stage()), e.log());
 * }
 * @endcode
 *
 * @see ShaderLoader for manifest-based shader loading
 * @see ShaderAsset for the compiled shader program wrapper
 */

#include <string>
#include <unordered_map>
#include <GL/glew.h>

namespace Rigel::Asset {

/**
 * @brief Container for shader source code and compile-time configuration.
 *
 * ShaderSource holds the raw GLSL source code for each shader stage along
 * with preprocessor definitions. This struct is typically populated by
 * ShaderLoader after resolving inheritance and loading source files.
 *
 * @section shader_configurations Shader Configurations
 *
 * **Graphics Pipeline:**
 * @code
 * ShaderSource source;
 * source.vertex = "... vertex shader source ...";
 * source.fragment = "... fragment shader source ...";
 * source.geometry = "... optional geometry shader ...";
 * source.defines["USE_NORMAL_MAP"] = "1";
 * @endcode
 *
 * **Compute Pipeline:**
 * @code
 * ShaderSource source;
 * source.compute = "... compute shader source ...";
 * source.defines["WORK_GROUP_SIZE"] = "256";
 * @endcode
 *
 * @warning Compute shaders are mutually exclusive with graphics shaders.
 *          If `compute` is non-empty, `vertex`, `fragment`, and `geometry`
 *          are ignored.
 */
struct ShaderSource {
    /**
     * @brief Vertex shader GLSL source code.
     *
     * Required for graphics shaders. Ignored if `compute` is set.
     * Typically loaded from a .vert file.
     */
    std::string vertex;

    /**
     * @brief Fragment shader GLSL source code.
     *
     * Required for graphics shaders. Ignored if `compute` is set.
     * Typically loaded from a .frag file.
     */
    std::string fragment;

    /**
     * @brief Geometry shader GLSL source code (optional).
     *
     * If non-empty, a geometry shader stage is added to the pipeline.
     * Ignored if `compute` is set. Typically loaded from a .geom file.
     */
    std::string geometry;

    /**
     * @brief Compute shader GLSL source code (optional).
     *
     * If non-empty, compiles as a compute shader. When set, all other
     * shader sources are ignored. Typically loaded from a .comp file.
     *
     * Compute shaders run outside the graphics pipeline and are used
     * for general-purpose GPU computation.
     */
    std::string compute;

    /**
     * @brief Preprocessor definitions to inject into shader source.
     *
     * Each key-value pair becomes a `#define KEY VALUE` directive injected
     * after the `#version` line in each shader source.
     *
     * Special value handling:
     * - "true" is converted to "1"
     * - "false" is converted to "0"
     *
     * Example:
     * @code
     * source.defines["MAX_BONES"] = "64";
     * source.defines["USE_SKINNING"] = "true";  // Becomes #define USE_SKINNING 1
     * @endcode
     */
    std::unordered_map<std::string, std::string> defines;
};

/**
 * @brief Static utility class for compiling GLSL shaders into OpenGL programs.
 *
 * ShaderCompiler provides the low-level shader compilation functionality used
 * by ShaderLoader. It handles:
 * - Source preprocessing (define injection)
 * - Individual stage compilation
 * - Program linking
 * - Error detection and reporting
 *
 * This class is stateless and all methods are static. It does not manage any
 * OpenGL resources; the caller is responsible for the returned program handle.
 *
 * @section usage Basic Usage
 *
 * @code
 * ShaderSource source;
 * source.vertex = loadFile("shaders/basic.vert");
 * source.fragment = loadFile("shaders/basic.frag");
 * source.defines["USE_COLOR"] = "1";
 *
 * try {
 *     GLuint program = ShaderCompiler::compile(source, "basic_shader");
 *     // Use program...
 *     glDeleteProgram(program);  // Cleanup when done
 * } catch (const ShaderCompileError& e) {
 *     handleCompileError(e);
 * } catch (const ShaderLinkError& e) {
 *     handleLinkError(e);
 * }
 * @endcode
 *
 * @section error_details Error Information
 *
 * On failure, exceptions contain detailed information:
 * - **ShaderCompileError**: Which stage failed (GL_VERTEX_SHADER, etc.) and
 *   the OpenGL compiler log
 * - **ShaderLinkError**: The OpenGL linker log
 *
 * @note This class requires a valid OpenGL context to be current on the
 *       calling thread. Shader compilation will fail if no context exists.
 *
 * @see ShaderSource for input data structure
 * @see ShaderLoader for high-level manifest-based loading
 */
class ShaderCompiler {
public:
    /**
     * @brief Compile and link shader sources into an OpenGL program.
     *
     * This is the main entry point for shader compilation. The method:
     * 1. Preprocesses each source (injects defines after #version)
     * 2. Compiles each shader stage
     * 3. Creates and links the program
     * 4. Cleans up intermediate shader objects
     *
     * For graphics shaders, both `source.vertex` and `source.fragment` are
     * required. For compute shaders, only `source.compute` is required.
     *
     * @param source The shader source code and defines to compile
     * @param shaderId Identifier used in error messages (typically asset ID)
     *
     * @return OpenGL program handle (non-zero on success)
     *
     * @throws ShaderCompileError if any shader stage fails to compile.
     *         The exception contains the failing stage and compiler log.
     * @throws ShaderLinkError if program linking fails.
     *         The exception contains the linker log.
     *
     * @pre A valid OpenGL context must be current on the calling thread
     * @post On success, caller owns the returned program and must call
     *       glDeleteProgram() when finished
     * @post On exception, no OpenGL resources are leaked
     *
     * @section compile_example Example
     *
     * @code
     * ShaderSource source;
     * source.vertex = R"(
     *     #version 410 core
     *     layout(location = 0) in vec3 a_position;
     *     uniform mat4 u_mvp;
     *     void main() {
     *         gl_Position = u_mvp * vec4(a_position, 1.0);
     *     }
     * )";
     * source.fragment = R"(
     *     #version 410 core
     *     out vec4 fragColor;
     *     void main() {
     *         fragColor = vec4(1.0);
     *     }
     * )";
     *
     * GLuint program = ShaderCompiler::compile(source, "simple_shader");
     * @endcode
     */
    static GLuint compile(const ShaderSource& source, const std::string& shaderId);

    /**
     * @brief Preprocess shader source by injecting defines after #version.
     *
     * Locates the `#version` directive in the source and injects `#define`
     * statements immediately after it. If no `#version` is found, prepends
     * `#version 410 core` before the defines.
     *
     * Boolean values are converted:
     * - "true" → "1"
     * - "false" → "0"
     *
     * @param source The raw GLSL source code
     * @param defines Map of define names to values
     * @return Preprocessed source with defines injected
     *
     * @section preprocess_example Example
     *
     * Input source:
     * @code{.glsl}
     * #version 410 core
     * uniform float brightness;
     * @endcode
     *
     * With defines: {"USE_HDR": "true", "GAMMA": "2.2"}
     *
     * Output:
     * @code{.glsl}
     * #version 410 core
     * #define USE_HDR 1
     * #define GAMMA 2.2
     * uniform float brightness;
     * @endcode
     */
    static std::string preprocess(const std::string& source,
                                  const std::unordered_map<std::string, std::string>& defines);

private:
    /**
     * @brief Compile a single shader stage.
     *
     * Creates an OpenGL shader object, sets its source, and compiles it.
     * Throws on compilation failure.
     *
     * @param type The shader type (GL_VERTEX_SHADER, GL_FRAGMENT_SHADER,
     *             GL_GEOMETRY_SHADER, or GL_COMPUTE_SHADER)
     * @param source The preprocessed GLSL source code
     * @param shaderId Identifier for error messages
     *
     * @return OpenGL shader handle (non-zero on success)
     *
     * @throws ShaderCompileError if compilation fails
     *
     * @post On success, caller must attach to a program and eventually
     *       call glDeleteShader()
     * @post On exception, no OpenGL resources are leaked
     */
    static GLuint compileStage(GLenum type, const std::string& source, const std::string& shaderId);

    /**
     * @brief Check for compilation errors and throw if found.
     *
     * Queries OpenGL for the shader's compile status. If compilation failed,
     * retrieves the info log and throws ShaderCompileError.
     *
     * @param shader The shader object to check
     * @param stage The shader type (for error message)
     * @param shaderId Identifier for error message
     *
     * @throws ShaderCompileError if GL_COMPILE_STATUS is GL_FALSE
     */
    static void checkCompileErrors(GLuint shader, GLenum stage, const std::string& shaderId);

    /**
     * @brief Check for link errors and throw if found.
     *
     * Queries OpenGL for the program's link status. If linking failed,
     * retrieves the info log and throws ShaderLinkError.
     *
     * @param program The program object to check
     * @param shaderId Identifier for error message
     *
     * @throws ShaderLinkError if GL_LINK_STATUS is GL_FALSE
     */
    static void checkLinkErrors(GLuint program, const std::string& shaderId);

    /**
     * @brief Convert shader stage enum to a human-readable string.
     *
     * @param stage The OpenGL shader type constant
     * @return String representation ("vertex", "fragment", "geometry", "compute", or "unknown")
     */
    static const char* stageToString(GLenum stage);
};

} // namespace Rigel::Asset
