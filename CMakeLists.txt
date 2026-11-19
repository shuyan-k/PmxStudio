cmake_minimum_required(VERSION 3.20)

# CMake 4 默认拒绝 cmake_minimum_required < 3.5 的第三方工程，GLFW 3.4 仍使用旧式声明。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(PmxStudio VERSION 0.1.0 LANGUAGES C CXX)

# ctest 入口：各组测试都用绝对路径引用 tests/ 下的样例模型，无需固定工作目录。
enable_testing()

# Windows 资源脚本（exe / 窗口图标）由 RC 编译器处理。
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    add_compile_options(/utf-8 /EHsc /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

option(PMX_BUILD_APP "Build the GLFW + ImGui viewer" ON)
option(PMX_BUILD_CLI "Build the headless pmxstudio-cli batch tool" ON)
option(PMX_BUILD_TESTS "Build parity/unit tests" ON)
option(PMX_ENABLE_PHYSICS "Build Bullet physics backend" ON)
option(PMX_USE_SYSTEM_BULLET "Use an installed Bullet instead of the vendored source" OFF)
option(PMX_FETCH_DEPS "Fetch dependencies from the mirror when third_party/ is missing" OFF)
option(PMX_STATIC_RUNTIME "Statically link the MSVC runtime for a single-file exe" ON)
# 2.0 功能模块：默认全开，关闭后对应源文件不参与编译、UI 标签不出现。
option(PMX_MODULE_TIMELINE "Timeline keyframe editor" ON)
option(PMX_MODULE_BAKE "Deterministic physics bake" ON)
option(PMX_MODULE_RENDER_OUT "Offscreen render + PNG/video export" ON)
option(PMX_MODULE_AUDIO "Audio track (miniaudio)" ON)
option(PMX_MODULE_SCRIPT "JSON batch script support" ON)
set(PMX_DEPS_MIRROR "https://gitee.com/mirrors" CACHE STRING
    "Git mirror used when PMX_FETCH_DEPS is enabled (use https://github.com for upstream)")
set(PMX_BULLET_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bullet/src"
    CACHE PATH "Vendored Bullet source tree")

# 静态 CRT：Windows 产物是单个 exe，不依赖 VCRUNTIME/MSVCP DLL。
# 必须在 add_subdirectory(third_party) 之前设置——目录属性只对之后创建的目标生效。
if(MSVC AND PMX_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "" FORCE)
    add_compile_options($<$<CONFIG:Debug>:/MTd> $<$<NOT:$<CONFIG:Debug>>:/MT>)
endif()

include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------- core
set(PMX_CORE_SOURCES
    src/core/pmx_io.cpp
    src/core/model.cpp
    src/core/pose.cpp
    src/core/skinning.cpp
    src/core/morph.cpp
    src/core/vpd.cpp
    src/core/vmd.cpp
    src/core/utf8_args.cpp
    src/core/name_zh.cpp
    src/core/json.cpp
)

if(PMX_MODULE_TIMELINE)
    list(APPEND PMX_CORE_SOURCES src/core/anim.cpp)
endif()
if(PMX_MODULE_BAKE AND PMX_ENABLE_PHYSICS)
    list(APPEND PMX_CORE_SOURCES src/core/bake.cpp)
endif()
if(PMX_MODULE_AUDIO)
    list(APPEND PMX_CORE_SOURCES src/core/audio.cpp)
endif()
if(PMX_MODULE_SCRIPT)
    list(APPEND PMX_CORE_SOURCES src/core/script.cpp)
endif()

add_library(pmx_core STATIC ${PMX_CORE_SOURCES})
target_include_directories(pmx_core PUBLIC src)
target_compile_features(pmx_core PUBLIC cxx_std_20)

# miniaudio 以单头文件方式随仓库提供；audio.cpp 自己定义 MINIAUDIO_IMPLEMENTATION。
if(PMX_MODULE_AUDIO)
    target_include_directories(pmx_core PRIVATE third_party/miniaudio)
    if(WIN32)
        target_link_libraries(pmx_core PRIVATE ole32)
    endif()
endif()

if(WIN32)
    # CommandLineToArgvW 用于把宽字符命令行还原成 UTF-8 参数。
    target_link_libraries(pmx_core PRIVATE shell32)
endif()

if(PMX_ENABLE_PHYSICS)
    target_sources(pmx_core PRIVATE src/physics/physics.cpp src/physics/softbody.cpp)
    target_link_libraries(pmx_core PRIVATE bullet)
    target_compile_definitions(pmx_core PUBLIC PMX_ENABLE_PHYSICS=1)
endif()

# ---------------------------------------------------------------- gpu
# 无界面构建不需要 GL 后端；核心库与 CLI 不依赖 GPU 代码。
if(PMX_BUILD_APP)
set(PMX_GPU_SOURCES
    src/gpu/gl_api.cpp
    src/gpu/shader.cpp
    src/gpu/renderer.cpp
)
if(PMX_MODULE_RENDER_OUT)
    list(APPEND PMX_GPU_SOURCES src/gpu/offscreen.cpp)
endif()

add_library(pmx_gpu STATIC ${PMX_GPU_SOURCES})
target_include_directories(pmx_gpu PUBLIC src)
target_link_libraries(pmx_gpu PUBLIC pmx_core glfw stb_image)
# 系统 GL 头只声明到 1.1，与自定义加载器的函数指针同名冲突；统一关掉 GLFW 的 GL 包含。
target_compile_definitions(pmx_gpu PUBLIC PMX_GPU_OPENGL=1 GLFW_INCLUDE_NONE)
endif()  # PMX_BUILD_APP

# ---------------------------------------------------------------- app
if(PMX_BUILD_APP)
    file(GLOB PMX_APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp src/app/panels/*.cpp)
    add_executable(pmxstudio ${PMX_APP_SOURCES})
    target_include_directories(pmxstudio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(pmxstudio PRIVATE
        PMXSTUDIO_VERSION="${PROJECT_VERSION}")
    target_link_libraries(pmxstudio PRIVATE pmx_gpu imgui glfw)
    if(WIN32)
        # Agent API 的本地 socket（ws2_32）。
        target_link_libraries(pmxstudio PRIVATE ws2_32)
    else()
        # 插件动态库加载（dlopen）。
        target_link_libraries(pmxstudio PRIVATE ${CMAKE_DL_LIBS})
    endif()
    # 模块宏：modules.hpp 用它决定默认开关；关闭的模块连 UI 标签都不出现。
    foreach(mod TIMELINE BAKE RENDER_OUT AUDIO SCRIPT)
        if(PMX_MODULE_${mod})
            target_compile_definitions(pmxstudio PRIVATE PMX_MODULE_${mod}=1)
        endif()
    endforeach()
    if(WIN32)
        # GUI 子系统：启动时不带控制台窗口，入口为 main.cpp 里的 WinMain。
        # 产物直接输出到仓库根目录，构建完即可双击运行；多配置生成器需要逐配置
        # 指定目录，否则 CMake 会自动追加 Release/Debug 子目录。
        set_target_properties(pmxstudio PROPERTIES
            WIN32_EXECUTABLE ON
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}")
        # 应用图标：GLFW_ICON 资源同时用于窗口图标与 exe 文件图标。
        target_sources(pmxstudio PRIVATE assets/icon.rc)
        # 资源管理器文件对话框（comdlg32）、宽字符命令行解析（shell32）、
        # 原生深色标题栏（DwmSetWindowAttribute，dwmapi）。
        target_link_libraries(pmxstudio PRIVATE comdlg32 shell32 dwmapi)
    endif()
endif()

# ---------------------------------------------------------------- cli
if(PMX_BUILD_CLI AND PMX_MODULE_SCRIPT)
    add_executable(pmxstudio-cli src/cli/main.cpp)
    target_link_libraries(pmxstudio-cli PRIVATE pmx_core)
    if(WIN32)
        set_target_properties(pmxstudio-cli PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
    endif()
endif()

# ---------------------------------------------------------------- tests
if(PMX_BUILD_TESTS)
    add_executable(pmx_tests tests/test_core.cpp)
    target_link_libraries(pmx_tests PRIVATE pmx_core)
    target_compile_definitions(pmx_tests PRIVATE
        PMX_TEST_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model.pmx")
    add_test(NAME core COMMAND pmx_tests)

    # 与 Python 版做读写一致性校验的命令行探针（tools/parity_check.py 调用）。
    add_executable(pmx_parity_probe tests/parity_probe.cpp)
    target_link_libraries(pmx_parity_probe PRIVATE pmx_core)

    # 2.0 新增的纯核心测试：文件存在才建目标，避免关闭模块时配置失败。
    foreach(extra anim bake script)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${extra}.cpp")
            add_executable(pmx_tests_${extra} tests/test_${extra}.cpp)
            target_link_libraries(pmx_tests_${extra} PRIVATE pmx_core)
            target_compile_definitions(pmx_tests_${extra} PRIVATE
                PMX_TEST_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/tests/test_model.pmx")
            add_test(NAME ${extra} COMMAND pmx_tests_${extra})
        endif()
    endforeach()
endif()
