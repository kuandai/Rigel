from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class RigelConan(ConanFile):
    name = "rigel"
    version = "0.0.0"
    settings = "os", "compiler", "build_type", "arch"
    options = {"with_graphics": [True, False]}
    default_options = {
        "with_graphics": True,
        "spdlog/*:shared": False,
        "glew/*:shared": False,
        "glfw/*:shared": False,
    }

    def requirements(self):
        self.requires("spdlog/1.12.0")
        self.requires("rapidyaml/0.10.0")
        self.requires("glm/cci.20230113")
        if self.options.with_graphics:
            self.requires("glew/2.2.0")
            self.requires("glfw/3.3.8")
            self.requires("stb/cci.20240531")
            self.requires("imgui/1.90.7")

    def generate(self):
        dependencies = CMakeDeps(self)
        dependencies.generate()

        toolchain = CMakeToolchain(self)
        toolchain.variables["RIGEL_BUILD_GRAPHICS"] = bool(
            self.options.with_graphics)
        if self.options.with_graphics:
            imgui = self.dependencies["imgui"]
            toolchain.variables["RIGEL_IMGUI_BINDINGS_DIR"] = (
                imgui.cpp_info.srcdirs[0]
            )
        toolchain.generate()
