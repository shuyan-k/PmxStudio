cmake_minimum_required(VERSION 3.20)
project(pmxstudio_demo_plugin LANGUAGES CXX)

# 独立构建示例：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# 产物为 demo.so / demo.dll / demo.dylib，放到 PmxStudio 的 plugins/ 目录即可。
add_library(pmx_demo_plugin MODULE plugin.cpp)
target_include_directories(pmx_demo_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
set_target_properties(pmx_demo_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "demo"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
