from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class RigelConan(ConanFile):
    name = "rigel"
    version = "0.0.0"
    settings = "os", "compiler", "build_type", "arch"
    # Dependencies
    requires = (
        "spdlog/1.12.0",
        "glew/2.2.0",
        "glfw/3.3.8",
        "rapidyaml/0.10.0",
        "stb/cci.20240531",
        "glm/cci.20230113",
        "imgui/1.90.7"
    )

    # Options for dependencies
    default_options = {
        "spdlog/*:shared": False,
        "glew/*:shared": False,
        "glfw/*:shared": False
    }

    def generate(self):
        dependencies = CMakeDeps(self)
        dependencies.generate()

        imgui = self.dependencies["imgui"]
        toolchain = CMakeToolchain(self)
        toolchain.variables["RIGEL_IMGUI_BINDINGS_DIR"] = imgui.cpp_info.srcdirs[0]
        toolchain.generate()
