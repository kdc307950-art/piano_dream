# NSIS 安装向导

`GrandStudioPiano.nsi` 是实际可编译的 Windows NSIS 安装脚本，包含欢迎、许可、安装位置、安装进度和完成启动页面，并创建开始菜单、桌面快捷方式和卸载项。

Windows 开发机安装 NSIS 3.x 后运行：

```powershell
.\build.cmd -Installer
```

脚本会先执行 `cmake --install` 生成 staging 目录，再调用 `makensis.exe` 生成：

```text
build/GrandStudioPiano-1.0.0-setup.exe
```

CI 的 Windows job 会自动安装 NSIS 并上传安装器制品。安装包包含程序、UI 资源、FreePats CC0 钢琴采样、许可证和使用说明。
