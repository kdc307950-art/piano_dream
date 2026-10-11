#pragma once
//
// core/Console.h —— 让 Windows 控制台正确显示中文
//
// 问题：程序向 stdout 写的是 UTF-8 字节，而 Windows 控制台默认按 **OEM 代码页**
// （简体中文系统是 CP936 / GBK）解释这些字节 —— 于是「钢琴」变成「閿挗鐞?」。
//
// 解决：把控制台输出代码页临时切到 CP_UTF8，退出时恢复原值
// （不恢复会影响同一个控制台窗口里后续运行的其他程序）。
//
// 实现放在 Console.cpp 里单独一个编译单元，避免把 <windows.h> 带进
// 已经包含 raylib.h 的文件 —— wingdi.h 里的 Rectangle() 函数会和
// raylib 的 Rectangle 结构体撞名。
//
namespace piano {

/// RAII：构造时切到 UTF-8，析构时恢复。在 main() 开头放一个实例即可。
class ConsoleUtf8 {
public:
    ConsoleUtf8() noexcept;
    ~ConsoleUtf8() noexcept;
    ConsoleUtf8(const ConsoleUtf8&) = delete;
    ConsoleUtf8& operator=(const ConsoleUtf8&) = delete;

    /// 是否成功切换（非 Windows 平台或没有控制台时为 false）。
    bool active() const noexcept { return active_; }

private:
    bool active_ = false;
    unsigned long savedCodepage_ = 0;
};

} // namespace piano
