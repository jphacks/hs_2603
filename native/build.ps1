# かんたん動画エディタ（C++ / Qt / MinGW）をビルドして、動かすのに必要なファイルを native\dist\kantan-video-editor に集める。
# 開発用ツールが無いときは、先に tools\setup.ps1 でダウンロードする（初回だけ）。
#   powershell -ExecutionPolicy Bypass -File native\build.ps1             ビルド
#   powershell -ExecutionPolicy Bypass -File native\build.ps1 -Run        ビルドして起動
#   powershell -ExecutionPolicy Bypass -File native\build.ps1 -NoDeploy   コンパイルだけ
param(
  [switch]$Run,
  [switch]$NoDeploy
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\common.ps1')
$here = $PSScriptRoot

if (@('CMake', 'Ninja', 'Qt', 'MinGW', 'FFmpeg') | Where-Object { -not (Test-KantanTool $_) }) {
  & (Join-Path $here 'tools\setup.ps1')
}
$qt = $KantanTools.Qt
$mingw = $KantanTools.MinGW
$ffmpeg = $KantanTools.FFmpeg
$env:PATH = "$mingw\bin;$qt\bin;$env:PATH"
# 一時フォルダーの場所に日本語や空白があっても MinGW が困らないよう、ビルド中はツールのフォルダーの中を使う
$userTemp = @($env:TEMP, $env:TMP)
$env:TEMP = $env:TMP = Join-Path $KantanDev 'tmp'
New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null

# このフォルダーの場所に日本語や空白があると、MinGW の windres や Qt の moc が失敗する。
# そのときはソースを開発用ツールのフォルダーの中にコピーして、そちらでビルドする（変わったファイルだけコピーする）
$srcDir = $here
if (-not (Test-KantanSafePath $here)) {
  $hash = [System.Security.Cryptography.SHA256]::Create().ComputeHash([System.Text.Encoding]::UTF8.GetBytes($here.ToLowerInvariant()))
  $srcDir = Join-Path $KantanDev ('work\' + (-join ($hash[0..5] | ForEach-Object { $_.ToString('x2') })))
  Write-Host 'このフォルダーの場所には日本語か空白が含まれているので、ソースを次の場所にコピーしてビルドします'
  Write-Host "  $srcDir（エラーの場所もこちらのパスで表示されます）"
  & (Join-Path $env:SystemRoot 'System32\robocopy.exe') $here $srcDir /MIR /XD build dist /NFL /NDL /NJH /NJS /NP /R:2 /W:1 | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "ソースをコピーできませんでした（robocopy の終了コード $LASTEXITCODE）" }
}
$build = Join-Path $srcDir 'build'
$configure = @('-S', $srcDir, '-B', $build, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$($KantanTools.Ninja)", '-DCMAKE_BUILD_TYPE=Release',
  "-DCMAKE_PREFIX_PATH=$qt", "-DFFMPEG_DIR=$ffmpeg", "-DCMAKE_C_COMPILER=$mingw\bin\gcc.exe", "-DCMAKE_CXX_COMPILER=$mingw\bin\g++.exe")
# ツールやフォルダーの場所が前回と違うときは、前のビルド設定を消して作り直す
$stamp = Join-Path $build 'kantan-configure.txt'
$signature = (@($KantanTools.CMake) + $configure) -join "`n"
$sameConfig = (Test-Path -LiteralPath $stamp) -and ([System.IO.File]::ReadAllText($stamp) -eq $signature)
if ((Test-Path -LiteralPath $build) -and -not $sameConfig) {
  Write-Host 'ビルドの設定が変わったので、native\build を作り直します'
  Remove-Item -Recurse -Force -LiteralPath $build
}
if (-not (Test-Path -LiteralPath (Join-Path $build 'build.ninja'))) {
  Invoke-Tool $KantanTools.CMake $configure 'CMake の設定'
  [System.IO.File]::WriteAllText($stamp, $signature)
}
Invoke-Tool $KantanTools.CMake @('--build', $build) 'ビルド'
if ($NoDeploy) { return }

# 動かすのに必要なファイルを集める
$out = Join-Path $here 'dist\kantan-video-editor'
$exe = Join-Path $out 'kantan-video-editor.exe'
$exeFull = [System.IO.Path]::GetFullPath($exe)
$running = foreach ($p in @(Get-Process -Name 'kantan-video-editor' -ErrorAction SilentlyContinue)) {
  try { if ($p.Path -and ([System.IO.Path]::GetFullPath($p.Path) -ieq $exeFull)) { $p } } catch { }
}
if ($running) { throw "アプリが起動したままなので上書きできません。閉じてからもう一度実行してください: $exe" }
# 前のビルドの古いファイルが残らないよう、空のフォルダーから集め直す
if (Test-Path -LiteralPath $out) { Remove-Item -Recurse -Force -LiteralPath $out }
New-Item -ItemType Directory -Force -Path $out | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'kantan-video-editor.exe') -Destination $out -Force
# tls は AI 機能（Bedrock への HTTPS）で使う。Windows 標準の暗号化（Schannel）を使うので OpenSSL は要らない
Invoke-Tool (Join-Path $qt 'bin\windeployqt.exe') @('--release', '--compiler-runtime', '--no-system-d3d-compiler', '--no-opengl-sw',
  '--no-translations', '--skip-plugin-types', 'networkinformation', '--dir', $out, $exe) 'windeployqt' | Out-Null
# OpenSSL の版は、OpenSSL の DLL を入れていないので使えない。紛らわしいので消しておく
Remove-Item -LiteralPath (Join-Path $out 'tls\qopensslbackend.dll') -Force -ErrorAction SilentlyContinue
# Qt の標準の部品（右クリックメニューや色の選択画面など）を日本語にする
New-Item -ItemType Directory -Force -Path (Join-Path $out 'translations') | Out-Null
Copy-Item -LiteralPath (Join-Path $qt 'translations\qtbase_ja.qm') -Destination (Join-Path $out 'translations') -Force
Get-ChildItem -LiteralPath (Join-Path $ffmpeg 'bin') -Filter '*.dll' | Copy-Item -Destination $out -Force
Copy-Item -LiteralPath (Join-Path $ffmpeg 'bin\ffmpeg.exe') -Destination $out -Force
$lic = Join-Path $out 'licenses'
New-Item -ItemType Directory -Force -Path $lic | Out-Null
Copy-Item -LiteralPath (Join-Path $ffmpeg 'LICENSE.txt') -Destination (Join-Path $lic 'FFmpeg-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $here 'third_party\miniaudio\LICENSE') -Destination (Join-Path $lic 'miniaudio-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $here 'resources\icons\LICENSE-lucide.txt') -Destination (Join-Path $lic 'lucide-LICENSE.txt') -Force
Write-Host "できあがり: $exe"
if ($Run) {
  $env:TEMP, $env:TMP = $userTemp
  Start-Process -FilePath $exe
}
