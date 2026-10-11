# ============================================================================
# Grand Studio · C++ 88 键钢琴模拟器
#
# 说明：所有平台统一从 raylib 6.0 源码构建，避免把某个平台的静态库
#       （例如 Windows COFF）误用于 Linux 或 macOS。
#
# 用法：
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build -j
#   ./build/piano.exe --selftest
# ============================================================================
cmake_minimum_required(VERSION 3.16)

project(GrandStudioPiano VERSION 1.0 LANGUAGES CXX)

option(PIANO_TSAN "Build with ThreadSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --------------------------------------------------------------------- 源文件
file(GLOB_RECURSE PIANO_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest.cpp")

# 用 OBJECT 库编译一次、链接两次 —— 两个可执行文件共享同一批目标文件
add_library(piano_objs OBJECT ${PIANO_SOURCES})

target_include_directories(piano_objs PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/raylib/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio")

# --------------------------------------------------------------------- raylib
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
set(PLATFORM "Desktop" CACHE STRING "" FORCE)
if(UNIX AND NOT APPLE)
    # raylib 的 GLFW 后端在 Linux 同时编译 X11 与 Wayland，运行时由 GLFW 选择。
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
    set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/6.0.tar.gz)
FetchContent_MakeAvailable(raylib)
set(PIANO_RAYLIB_TARGET raylib)

# ------------------------------------------------------- 两个可执行文件
#
#   piano       GUI 子系统 —— 双击运行不弹命令框，给用户玩的成品
#   piano-cli   控制台子系统 —— 保留 stdout，供 --selftest 与诊断
#
# 子系统在链接时写死在 PE 头里，GUI 程序的 stdout 没地方去，
# 所以自测必须用控制台版。
add_executable(piano     $<TARGET_OBJECTS:piano_objs>)
add_executable(piano-cli $<TARGET_OBJECTS:piano_objs>)

if(WIN32)
    # WIN32_EXECUTABLE = TRUE 等价于给链接器加 -mwindows
    set_target_properties(piano PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

foreach(tgt piano piano-cli)
    target_include_directories(${tgt} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/raylib/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio")
    target_link_libraries(${tgt} PRIVATE ${PIANO_RAYLIB_TARGET})
endforeach()

# --------------------------------------------------------------- 平台系统库
if(WIN32)
    foreach(tgt piano piano-cli)
        target_link_libraries(${tgt} PRIVATE
            opengl32 gdi32 winmm ole32 oleaut32 uuid shell32 user32)
    endforeach()
    if(MINGW)
        foreach(tgt piano piano-cli)
            target_link_options(${tgt} PRIVATE
                -static -static-libgcc -static-libstdc++)
        endforeach()
    endif()
elseif(APPLE)
    foreach(tgt piano piano-cli)
        target_link_libraries(${tgt} PRIVATE
            "-framework OpenGL" "-framework Cocoa" "-framework IOKit"
            "-framework CoreAudio" "-framework CoreVideo" "-framework CoreFoundation")
    endforeach()
else()
    foreach(tgt piano piano-cli)
        target_link_libraries(${tgt} PRIVATE GL m pthread dl rt X11)
    endforeach()
endif()

# --------------------------------------------------------------------- 编译选项
if(MSVC)
    target_compile_options(piano_objs PRIVATE /W4 /permissive-)
else()
    target_compile_options(piano_objs PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
endif()

if(PIANO_TSAN AND NOT MSVC)
    target_compile_options(piano_objs PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    foreach(tgt piano piano-cli)
        target_link_options(${tgt} PRIVATE -fsanitize=thread)
    endforeach()
endif()

# 输出到工程根目录：用户一眼能看到，双击时工作目录就是工程根，
# assets/samples、exports/ 都能自然找到。
set_target_properties(piano piano-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

enable_testing()
add_test(NAME piano_selftest COMMAND piano-cli --selftest)

message(STATUS "Grand Studio Piano · raylib = FetchContent 6.0")
message(STATUS "  产物：piano.exe (GUI，双击玩) / piano-cli.exe (控制台，--selftest)")
