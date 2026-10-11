#pragma once
//
// songs/BuiltinSongs.h —— 内置曲目
//
// 用「音高序列 + 节奏模板」描述，再展开成 tick 制的 NoteEvent，
// 好处是加曲子只要写一行音高数组，不需要手工算 tick。
//
#include <string>
#include <vector>

#include "core/Song.h"

namespace piano {

/// 全部内置曲目（旋律 + 简单伴奏）。
std::vector<Song> builtinSongs();

/// 供 --selftest 校验曲目数据合法性。
bool validateSong(const Song& song, std::string* why);

} // namespace piano
