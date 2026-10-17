; Grand Studio Piano · Windows installer
; Compile with:
;   makensis /DINPUT_ROOT=build\installer-stage /DOUTPUT_DIR=build installer\GrandStudioPiano.nsi

Unicode True
!include "MUI2.nsh"

!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "1.0.0"
!endif
!ifndef INPUT_ROOT
  !error "INPUT_ROOT must point to a cmake --install staging directory"
!endif
!ifndef OUTPUT_DIR
  !define OUTPUT_DIR "."
!endif
!ifndef INSTALLER_ROOT
  !define INSTALLER_ROOT "."
!endif

Name "Grand Studio Piano"
Caption "安装 Grand Studio Piano"
OutFile "${OUTPUT_DIR}\GrandStudioPiano-${PRODUCT_VERSION}-setup.exe"
InstallDir "$PROGRAMFILES64\Grand Studio Piano"
InstallDirRegKey HKLM "Software\Grand Studio Piano" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
SetDatablockOptimize on
BrandingText "Grand Studio · Digital Piano Experience"

VIProductVersion "${PRODUCT_VERSION}.0"
VIAddVersionKey "ProductName" "Grand Studio Piano"
VIAddVersionKey "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey "CompanyName" "Grand Studio"
VIAddVersionKey "FileDescription" "C++ 88-key digital piano"
VIAddVersionKey "LegalCopyright" "Grand Studio"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_BITMAP "${INSTALLER_ROOT}\installer-sidebar.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${INSTALLER_ROOT}\installer-header.bmp"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_WELCOMEPAGE_TITLE "欢迎安装 Grand Studio Piano"
!define MUI_WELCOMEPAGE_TEXT "离线运行的 88 键数字钢琴，支持真实采样、合成音色、MIDI 导入导出与跟练。\r\n\r\n安装程序将把程序、界面资源和 CC0 钢琴采样一起复制到本机。"
!define MUI_DIRECTORYPAGE_TEXT_TOP "选择 Grand Studio Piano 的安装位置。"
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "安装目录："
!define MUI_FINISHPAGE_TITLE "Grand Studio Piano 安装完成"
!define MUI_FINISHPAGE_TEXT "Grand Studio Piano 已安装完成。点击完成后即可开始演奏。"
!define MUI_FINISHPAGE_RUN "$INSTDIR\piano.exe"
!define MUI_FINISHPAGE_RUN_TEXT "启动 Grand Studio Piano"
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.md"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "打开使用说明"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${INPUT_ROOT}\assets\samples\FREEPATS_CC0_LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Section "Grand Studio Piano" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${INPUT_ROOT}\*"

  CreateDirectory "$SMPROGRAMS\Grand Studio Piano"
  CreateShortCut "$SMPROGRAMS\Grand Studio Piano\Grand Studio Piano.lnk" "$INSTDIR\piano.exe"
  CreateShortCut "$DESKTOP\Grand Studio Piano.lnk" "$INSTDIR\piano.exe"

  WriteRegStr HKLM "Software\Grand Studio Piano" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\GrandStudioPiano" "DisplayName" "Grand Studio Piano"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\GrandStudioPiano" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\GrandStudioPiano" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\GrandStudioPiano" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\GrandStudioPiano" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\GrandStudioPiano" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\Grand Studio Piano.lnk"
  Delete "$SMPROGRAMS\Grand Studio Piano\Grand Studio Piano.lnk"
  RMDir "$SMPROGRAMS\Grand Studio Piano"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\GrandStudioPiano"
  DeleteRegKey HKLM "Software\Grand Studio Piano"
  RMDir /r "$INSTDIR"
SectionEnd
