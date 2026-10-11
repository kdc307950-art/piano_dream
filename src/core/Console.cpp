#include "core/Console.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOGDI
#    define NOGDI          // 避开 wingdi.h 的 Rectangle()，它和 raylib 的结构体撞名
#  endif
#  include <windows.h>
#endif

namespace piano {

ConsoleUtf8::ConsoleUtf8() noexcept {
#ifdef _WIN32
    const UINT current = GetConsoleOutputCP();
    if (current == 0 || current == CP_UTF8) return;   // 没有控制台，或已经是 UTF-8

    if (SetConsoleOutputCP(CP_UTF8) != 0) {
        savedCodepage_ = current;
        active_ = true;
    }
#endif
}

ConsoleUtf8::~ConsoleUtf8() noexcept {
#ifdef _WIN32
    if (active_ && savedCodepage_ != 0) {
        SetConsoleOutputCP(static_cast<UINT>(savedCodepage_));
    }
#endif
}

} // namespace piano
