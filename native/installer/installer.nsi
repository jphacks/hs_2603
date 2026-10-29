; e-editor（C++版）のインストーラー
; build-installer.ps1 から makensis で作る（VERSION / OUTFILE / ICON / INSTALL_FILES / UNINSTALL_FILES / VCREDIST_PS1 を渡す）
; Program Files に入れて、この PC のすべてのユーザーが使えるようにする（入れるときに管理者の許可が要る）。
; 自動保存・設定・ログイン・テンプレートなどは、これまでどおりユーザーごとの %LOCALAPPDATA%\JPHACKS\e-editor に置く。
Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!define APPNAME "e-editor"
!define APPID "e-editor-cpp"
!define EXE "e-editor.exe"
!define PROGID "EEditorProject.Cpp"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}"
; オブジェクトマスクの AI の部品（onnxruntime.dll）が使う Microsoft Visual C++ ランタイムの版（14.44 以上）
!define VCRT_KEY "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"
!define VCRT_MINOR 44

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

; Visual C++ ランタイム（x64）が入っているか（64 ビットと 32 ビットのレジストリの両方を見る）。$R0 に 1 / 0
Function CheckVcRuntime
  StrCpy $R0 0
  ReadRegDWORD $0 HKLM "${VCRT_KEY}" "Installed"
  ReadRegDWORD $1 HKLM "${VCRT_KEY}" "Major"
  ReadRegDWORD $2 HKLM "${VCRT_KEY}" "Minor"
  ${If} $0 == 1
  ${AndIf} $1 == 14
  ${AndIf} $2 >= ${VCRT_MINOR}
    StrCpy $R0 1
    Return
  ${EndIf}
  SetRegView 32
  ReadRegDWORD $0 HKLM "${VCRT_KEY}" "Installed"
  ReadRegDWORD $1 HKLM "${VCRT_KEY}" "Major"
  ReadRegDWORD $2 HKLM "${VCRT_KEY}" "Minor"
  SetRegView 64
  ${If} $0 == 1
  ${AndIf} $1 == 14
  ${AndIf} $2 >= ${VCRT_MINOR}
    StrCpy $R0 1
  ${EndIf}
FunctionEnd

; オブジェクトマスク（AI）は Microsoft Visual C++ ランタイムを使う。入っていない・古いときだけ、Microsoft の配布元から
; ダウンロードして入れる（インストーラーには入れない。Microsoft の署名を確かめてから動かす）。
; 入れられなくても、オブジェクトマスクのほかはそのまま使える（アプリが、使うときに理由を知らせる）
Function EnsureVcRuntime
  Call CheckVcRuntime
  ${If} $R0 == 1
    Return
  ${EndIf}
  DetailPrint "Microsoft Visual C++ ランタイムを、Microsoft からダウンロードして入れています…"
  InitPluginsDir
  File "/oname=$PLUGINSDIR\vcredist.ps1" "${VCREDIST_PS1}"
  nsExec::ExecToLog '"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "$PLUGINSDIR\vcredist.ps1" "$PLUGINSDIR"'
  Pop $0
  Call CheckVcRuntime
  ${If} $R0 == 1
    DetailPrint "Microsoft Visual C++ ランタイムを入れました"
  ${Else}
    DetailPrint "Microsoft Visual C++ ランタイムを入れられませんでした（$0）。オブジェクトマスク（AI）を使うときは、https://aka.ms/vc14/vc_redist.x64.exe から入れてください"
  ${EndIf}
FunctionEnd

Section "Install"
  ; ショートカットは、この PC のすべてのユーザーのスタートメニューとデスクトップに置く
  SetShellVarContext all
  !include "${INSTALL_FILES}"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  Call EnsureVcRuntime

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
