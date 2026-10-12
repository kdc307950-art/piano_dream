#pragma once
//
// ui/FileDialog.h —— 系统文件打开对话框（portable-file-dialogs 的薄封装）
//
// pfd 头文件在 Windows 会引入 <windows.h>，与 raylib 的 CloseWindow/DrawText
// 等符号冲突，因此只在 .cpp 里包含；对上层只暴露这组最小接口。
// 对话框在后台线程（Windows）/子进程（Linux）里运行：弹框期间主循环
// （界面与音频）继续运转，靠 ready() 轮询结果。
//
#include <memory>
#include <string>
#include <vector>

namespace piano {

class FileDialog {
public:
    /// 立即弹出「打开文件」对话框（非阻塞），过滤器 *.mid *.midi。title 为 UTF-8。
    explicit FileDialog(const std::string& title);
    ~FileDialog();

    FileDialog(const FileDialog&) = delete;
    FileDialog& operator=(const FileDialog&) = delete;

    /// 用户已关闭对话框（选了文件或点了取消）。
    bool ready() const;
    /// 选中的文件（UTF-8 路径）；取消则返回空。仅在 ready() 之后调用。
    std::vector<std::string> result();
    /// 强制关闭对话框。退出时必须先 kill 再销毁：pfd 的 executor 析构
    /// 会停等用户关框，不 kill 就退出会让进程卡在关机路径上。
    void kill();

    /// 后端是否可用。Windows 恒真；Linux 由 pfd 一次性检测
    /// zenity/matedialog/qarma/kdialog（静态缓存）。
    static bool available();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace piano
