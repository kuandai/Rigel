#pragma once

#include "Rigel/Render/OpenGLRuntime.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <string>
#include <utility>

namespace Rigel::Test {

class HiddenOpenGLContext {
public:
    HiddenOpenGLContext() {
        if (!glfwInit()) {
            setGlfwError("GLFW initialization failed");
            return;
        }
        m_glfwInitialized = true;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, Render::kOpenGLContextMajorVersion);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, Render::kOpenGLContextMinorVersion);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

        m_window = glfwCreateWindow(64, 64, "Rigel test", nullptr, nullptr);
        if (!m_window) {
            setGlfwError("OpenGL context creation failed");
            return;
        }

        glfwMakeContextCurrent(m_window);
        glewExperimental = GL_TRUE;
        const GLenum glewStatus = glewInit();
        if (glewStatus != GLEW_OK) {
            m_error = "GLEW initialization failed: ";
            m_error += reinterpret_cast<const char*>(glewGetErrorString(glewStatus));
            return;
        }

        // GLEW can leave GL_INVALID_ENUM set after probing a core context.
        glGetError();
        m_available = true;
    }

    ~HiddenOpenGLContext() {
        if (m_window) {
            glfwMakeContextCurrent(nullptr);
            glfwDestroyWindow(m_window);
        }
        if (m_glfwInitialized) {
            glfwTerminate();
        }
    }

    bool available() const { return m_available; }
    const std::string& error() const { return m_error; }

private:
    void setGlfwError(std::string prefix) {
        const char* description = nullptr;
        const int code = glfwGetError(&description);
        m_error = std::move(prefix) + " (GLFW error " + std::to_string(code);
        if (description) {
            m_error += ": ";
            m_error += description;
        }
        m_error += ")";
    }

    GLFWwindow* m_window = nullptr;
    bool m_glfwInitialized = false;
    bool m_available = false;
    std::string m_error;
};

} // namespace Rigel::Test
