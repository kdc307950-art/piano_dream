#pragma once
//
// audio/MaConfig.h —— miniaudio 编译配置
//
// 必须在任何 #include "miniaudio.h" 之前包含，且所有编译单元保持一致，
// 否则会出现 ODR 冲突。这里只保留设备输出与解码器，其余功能全部裁剪以缩短编译时间。
//
#ifndef PIANO_MA_CONFIG_H
#define PIANO_MA_CONFIG_H

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_NULL

#endif // PIANO_MA_CONFIG_H
