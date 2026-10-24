; かんたん動画エディタ（C++版）のインストーラー
; build-installer.ps1 から makensis で作る（VERSION / OUTFILE / ICON / INSTALL_FILES / UNINSTALL_FILES を渡す）
; Program Files に入れて、この PC のすべてのユーザーが使えるようにする（入れるときに管理者の許可が要る）。
; 自動保存・設定・ログイン・テンプレートなどは、これまでどおりユーザーごとの %LOCALAPPDATA%\JPHACKS\kantan-video-editor に置く。
Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!define APPNAME "かんたん動画エディタ"
!define APPID "kantan-video-editor-cpp"
!define EXE "kantan-video-editor.exe"
!define PROGID "KantanVideoProject.Cpp"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPID}"
InstallDirRegKey HKLM "Software\${APPID}" "InstallDir"
BrandingText "${APPNAME} ${VERSION}"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
; 完了の画面の「起動する」は、管理者ではないふつうの権限で起動する（LaunchApp）
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp
!define MUI_FINISHPAGE_NOREBOOTSUPPORT

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Japanese"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=${LANG_JAPANESE} "ProductName" "${APPNAME}"
VIAddVersionKey /LANG=${LANG_JAPANESE} "FileDescription" "${APPNAME} セットアップ"
VIAddVersionKey /LANG=${LANG_JAPANESE} "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_JAPANESE} "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=${LANG_JAPANESE} "CompanyName" "JPHACKS Team"
VIAddVersionKey /LANG=${LANG_JAPANESE} "LegalCopyright" "MIT License"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "このアプリは 64 ビット版の Windows でだけ動きます。"
    Abort
  ${EndIf}
  ; 64 ビット版のレジストリ（WOW6432Node ではない方）に書く
  SetRegView 64
FunctionEnd

Function un.onInit
  SetRegView 64
FunctionEnd

; インストーラーは管理者として動いているので、そのまま起動するとアプリも管理者になり、
; エクスプローラーからのドラッグ＆ドロップが効かなくなる。エクスプローラーを通して、ふつうの権限で起動する
Function LaunchApp
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${EXE}"'
FunctionEnd

; 以前の版は、ユーザーごとに %LOCALAPPDATA%\Programs に入れていた（管理者の許可が要らない入れ方）。
; 残っていると、スタートメニューやデスクトップのショートカットと「アプリ」の一覧が 2 つになるので、先にアンインストールする
Function RemovePerUserInstall
  ReadRegStr $0 HKCU "${UNINST_KEY}" "InstallLocation"
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\uninstall.exe"
    DetailPrint "以前の版（ユーザーごとのインストール）をアンインストールしています: $0"
    ExecWait '"$0\uninstall.exe" /S _?=$0'
    Delete "$0\uninstall.exe"
    RMDir "$0"
  ${EndIf}
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\${APPID}"
FunctionEnd

Section "Install"
  Call RemovePerUserInstall
  ; ショートカットは、この PC のすべてのユーザーのスタートメニューとデスクトップに置く
  SetShellVarContext all
  !include "${INSTALL_FILES}"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\${EXE}"
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXE}"

  ; .kvproj をダブルクリックで開けるようにする（すべてのユーザー）
  WriteRegStr HKLM "Software\Classes\.kvproj" "" "${PROGID}"
  WriteRegStr HKLM "Software\Classes\${PROGID}" "" "${APPNAME} プロジェクト"
  WriteRegStr HKLM "Software\Classes\${PROGID}\DefaultIcon" "" "$INSTDIR\${EXE},0"
  WriteRegStr HKLM "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\${EXE}" "%1"'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  ; 「アプリと機能」に出す
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APPNAME} (C++版)"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "JPHACKS Team"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
  WriteRegStr HKLM "Software\${APPID}" "InstallDir" "$INSTDIR"
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  Delete "$SMPROGRAMS\${APPNAME}.lnk"
  Delete "$DESKTOP\${APPNAME}.lnk"

  ; .kvproj がこのアプリを指しているときだけ外す
  ReadRegStr $0 HKLM "Software\Classes\.kvproj" ""
  StrCmp $0 "${PROGID}" 0 +2
  DeleteRegKey HKLM "Software\Classes\.kvproj"
  DeleteRegKey HKLM "Software\Classes\${PROGID}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  ; インストールしたファイルだけを消す（ユーザーごとの自動保存・設定・テンプレート、作業中のプロジェクトや書き出した動画は残る）
  !include "${UNINSTALL_FILES}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\${APPID}"
SectionEnd
