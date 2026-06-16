from conan import ConanFile

class MyProject(ConanFile):
    # 1. 设置关键属性
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    # 2. 定义依赖（核心逻辑）
    def requirements(self):
        # 始终需要的依赖
        self.requires("libwebsockets/4.5.8")
        self.requires("libarchive/3.8.7")

        # 条件依赖：仅在非 Termux 环境下添加 SDL3
        # 判断依据：如果 os 是 Linux 且 arch 是 armv8，则认为是 Termux（Android）环境
        is_termux = (self.settings.os == "Linux" and self.settings.arch == "armv8")

        if not is_termux:
            self.requires("sdl/3.4.8")  # 桌面环境用 Conan 管理 SDL3
        # Termux 下不添加 SDL3，由系统包管理器（pkg）提供

    # 3. 可选：为不同平台生成额外的配置信息
    def generate(self):
        pass
