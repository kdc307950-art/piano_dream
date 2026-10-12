#pragma once
//
// fx/FxSettings.h —— 音符特效设置的持久化格式（fx.txt）
//
// 简单键值文本，每行一条：
//     level=2            档位：0=关 1=少 2=标准 3=多
//     reducedMotion=0    减少动态效果：0/1
// 解析容忍缺行与坏值，一律回落默认（Standard / 关）。
// header-only、raylib-free，可被离线自测直接验证。
//
#include <string>

namespace piano {
namespace fx {

struct FxSettings {
    int  level = 2;              // 对应 Level 枚举值
    bool reducedMotion = false;
};

inline FxSettings parseFxSettings(const std::string& text) {
    FxSettings s;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t eol = text.find('\n', pos);
        const std::string line = text.substr(pos, eol == std::string::npos ? eol : eol - pos);
        pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!val.empty() && (val.back() == '\r' || val.back() == ' ')) val.pop_back();
        while (!key.empty() && key.back() == ' ') key.pop_back();

        if (key == "level") {
            // 只接受纯数字且落在 0..3，其余（空串/负号/字母/越界）保持默认
            if (!val.empty() && val.size() == 1 && val[0] >= '0' && val[0] <= '3')
                s.level = val[0] - '0';
        } else if (key == "reducedMotion") {
            if (val == "1" || val == "true") s.reducedMotion = true;
            else if (val == "0" || val == "false") s.reducedMotion = false;
        }
    }
    return s;
}

inline std::string formatFxSettings(const FxSettings& s) {
    return "level=" + std::to_string(s.level) +
           "\nreducedMotion=" + (s.reducedMotion ? "1" : "0") + "\n";
}

} // namespace fx
} // namespace piano
