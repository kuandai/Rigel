#include "Rigel/Application.h"
#include <spdlog/spdlog.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include "Rigel/input/keypress.h"
#include "Rigel/version.h"

namespace Rigel {

struct Application::Impl {
    GLFWwindow* window = nullptr;
};

Application::Application() : m_impl(std::make_unique<Impl>()) {
    #ifdef DEBUG
    spdlog::info("Rigel v{} Developer Preview", RIGEL_VERSION);
    #else
    spdlog::info("Rigel v{}", RIGEL_VERSION);
    #endif

    // Initialize GLFW
    if (!glfwInit()) {
        spdlog::error("GLFW initialization failed");
        throw std::runtime_error("GLFW initialization failed");
    }
    spdlog::info("GLFW initialized successfully");

    // Create a simple GLFW window
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    m_impl->window = glfwCreateWindow(800, 600, "Rigel", nullptr, nullptr);
    if (!m_impl->window) {
        spdlog::error("Failed to create GLFW window");
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    glfwMakeContextCurrent(m_impl->window);

    // Initialize GLEW
    if (glewInit() != GLEW_OK) {
        spdlog::error("GLEW initialization failed");
        glfwDestroyWindow(m_impl->window);
        glfwTerminate();
        throw std::runtime_error("GLEW initialization failed");
    }
    spdlog::info("GLEW initialized successfully");

    // Print OpenGL version
    spdlog::info("OpenGL Version: {}", (char*)glGetString(GL_VERSION));

    // Set initial viewport
    glViewport(0, 0, 800, 600);

    // Set Callbacks
    glfwSetFramebufferSizeCallback(m_impl->window, [](GLFWwindow* window, int width, int height)-> void {
        glViewport(0, 0, width, height);
    });
    glfwSetKeyCallback(m_impl->window, Rigel::keyCallback);
}

Application::~Application() {
    if (m_impl->window) {
        glfwDestroyWindow(m_impl->window);
    }
    glfwTerminate();
    spdlog::info("Application terminated successfully");
}

void Application::run() {
    // Render loop
    while (!glfwWindowShouldClose(m_impl->window)) {
        // Frame setup
        glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Flush event queue
        glfwPollEvents();
        Rigel::keyupdate();

        glfwSwapBuffers(m_impl->window);

        // Exit on ESC
        if (isKeyPressed(GLFW_KEY_ESCAPE)) {
            glfwSetWindowShouldClose(m_impl->window, true);
        }
    }
}

} // namespace Rigel
