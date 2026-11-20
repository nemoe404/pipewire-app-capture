from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain


class PwcapConan(ConanFile):
    name = "pwcap"
    settings = "os", "compiler", "build_type", "arch"

    def build_requirements(self):
        # Test-only dependency. The library has no third-party runtime dependency.
        self.test_requires("gtest/1.15.0")

    def generate(self):
        tc = CMakeToolchain(self)
        # Keep Conan from writing CMakeUserPresets.json into the repository root.
        tc.user_presets_path = False
        tc.generate()
        CMakeDeps(self).generate()
