// miniaudio 实现单元。
// 整个工程中只有这一个文件定义 MINIAUDIO_IMPLEMENTATION，
// 其余文件只包含声明（见 audio/MaConfig.h 的统一配置）。
#include "audio/MaConfig.h"

// miniaudio 的 OGG 后端依赖 stb_vorbis 的声明与实现。raylib 已随工程
// 提供同一份公共领域实现，这里只在本编译单元引入一次，避免采样文件
// 被扫描到后无声回退到合成音色。
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "../../third_party/raylib-src/src/external/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
