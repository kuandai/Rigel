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
        "glfw/3.3.8",
        "rapidyaml/0.10.0",
        "stb/cci.20240531",
        "glm/cci.20230113"
    )

    # Options for dependencies
    default_options = {
        "spdlog/*:shared": False,
        "glew/*:shared": False,
        "glfw/*:shared": False
    }
