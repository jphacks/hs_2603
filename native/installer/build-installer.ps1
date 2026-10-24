# インストーラーを作る。先に native\build.ps1 でビルドしてから、NSIS で native\dist\kantan-video-editor-cpp-<バージョン>-setup.exe にまとめる。
#   powershell -ExecutionPolicy Bypass -File native\installer\build-installer.ps1
# makensis.exe は開発用ツールのフォルダー（tools\setup.ps1 が入れる）のものを使う。-MakeNsis で別のものも指定できる。
param([string]$MakeNsis = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$native = Split-Path -Parent $here
. (Join-Path $native 'tools\common.ps1')

& (Join-Path $native 'build.ps1')
# 配る exe に AI サーバーのアドレスが入っていないと、使う人は .env を書かないと AI を使えない
if (-not (Read-KantanReleaseServer (Join-Path $native 'release.env'))) {
  Write-Warning 'native\release.env に KANTAN_AI_SERVER が無いので、AI サーバーのアドレスを組み込まずに作ります（使う人は .env を書かないと AI を使えません）'
}
if (-not $MakeNsis) {
  if (-not (Test-KantanTool 'NSIS')) { & (Join-Path $native 'tools\setup.ps1') }
  $MakeNsis = $KantanTools.NSIS
}
$src = Join-Path $native 'dist\kantan-video-editor'

$cmakeLists = [System.IO.File]::ReadAllText((Join-Path $native 'CMakeLists.txt'))
$version = [regex]::Match($cmakeLists, 'project\(\w+ VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $version) { throw 'CMakeLists.txt からバージョンを読めません' }

# インストールするファイルと、アンインストールで消すファイルの一覧（アンインストールではこの一覧のファイルだけを消す）
$work = Join-Path $native 'build\installer'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$install = New-Object System.Collections.Generic.List[string]
$uninstall = New-Object System.Collections.Generic.List[string]
$dirs = @('') + @(Get-ChildItem -LiteralPath $src -Recurse -Directory | ForEach-Object { $_.FullName.Substring($src.Length + 1) } | Sort-Object)
foreach ($d in $dirs) {
  $full = if ($d) { Join-Path $src $d } else { $src }
  $files = @(Get-ChildItem -LiteralPath $full -File)
  if ($files.Count -eq 0) { continue }
  $install.Add('SetOutPath "$INSTDIR' + $(if ($d) { '\' + $d } else { '' }) + '"')
  foreach ($f in $files) {
    $install.Add('File "' + $f.FullName + '"')
    $rel = if ($d) { $d + '\' + $f.Name } else { $f.Name }
    $uninstall.Add('Delete "$INSTDIR\' + $rel + '"')
  }
}
$dirs | Where-Object { $_ } | Sort-Object { $_.Split('\').Count } -Descending | ForEach-Object { $uninstall.Add('RMDir "$INSTDIR\' + $_ + '"') }
$utf8 = New-Object System.Text.UTF8Encoding($true)
$installFile = Join-Path $work 'install-files.nsh'
$uninstallFile = Join-Path $work 'uninstall-files.nsh'
[System.IO.File]::WriteAllLines($installFile, $install, $utf8)
[System.IO.File]::WriteAllLines($uninstallFile, $uninstall, $utf8)

$out = Join-Path $native "dist\kantan-video-editor-cpp-$version-setup.exe"
Write-Host 'インストーラーを作っています（圧縮に数分かかります）'
Invoke-Tool $MakeNsis @('/V2', '/INPUTCHARSET', 'UTF8', "/DVERSION=$version", "/DOUTFILE=$out", "/DICON=$(Join-Path $native 'resources\app.ico')",
  "/DINSTALL_FILES=$installFile", "/DUNINSTALL_FILES=$uninstallFile", (Join-Path $here 'installer.nsi')) 'インストーラーの作成'
Write-Host "インストーラー: $out"
