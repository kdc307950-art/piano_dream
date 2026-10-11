# ============================================================================
# Grand Studio · C++ 88 键钢琴模拟器
#
# 说明：本工程把第三方依赖直接放在 third_party/ 下（raylib 预编译静态库 +
#       miniaudio 单头文件），因此不依赖 vcpkg / FetchContent，也不需要联网即可构建。
#       如果你更希望由 CMake 自己拉取依赖，把下面的 RAYLIB 部分换成
#       FetchContent_Declare(raylib GIT_REPOSITORY https://github.com/raysan5/raylib.git ...)
#       即可，其余不用改。
#
# 用法：
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build -j
#   ./build/piano.exe --selftest
# ============================================================================
cmake_minimum_required(VERSION 3.16)

project(GrandStudioPiano VERSION 1.0 LANGUAGES CXX)

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
find_library(RAYLIB_LIB
    NAMES raylib libraylib.a
    PATHS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/raylib/lib"
    NO_DEFAULT_PATH)

if(NOT RAYLIB_LIB)
    message(FATAL_ERROR
        "找不到 raylib 静态库。请确认 third_party/raylib/lib/libraylib.a 存在，"
        "或改用 FetchContent 拉取 raylib。")
endif()

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
    target_link_libraries(${tgt} PRIVATE ${RAYLIB_LIB})
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

# 输出到工程根目录：用户一眼能看到，双击时工作目录就是工程根，
# assets/samples、exports/ 都能自然找到。
set_target_properties(piano piano-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

message(STATUS "Grand Studio Piano · raylib = ${RAYLIB_LIB}")
message(STATUS "  产物：piano.exe (GUI，双击玩) / piano-cli.exe (控制台，--selftest)")
