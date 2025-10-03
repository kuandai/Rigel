from conan import ConanFile
from conan.tools.cmake import CMake, cmake_layout

class RigelConan(ConanFile):
    name = "rigel"
    version = "0.0.0"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    # Dependencies
    requires = (
        "spdlog/1.12.0",
        "glew/2.2.0",
        "glfw/3.3.8"
    )

    # Options for dependencies
    default_options = {
        "spdlog/*:shared": False,
        "glew/*:shared": False,
        "glfw/*:shared": False
    }

    def layout(self):
        cmake_layout(self)

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
