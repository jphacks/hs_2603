# native\build.ps1・native\tools\setup.ps1・native\installer\build-installer.ps1 の共通部分

# 開発用ツールの置き場所（環境変数 KANTAN_DEV で変えられる）。
# MinGW は場所に日本語や空白が入ると動かないので（ld が自分のライブラリを見つけられない、windres が失敗する）、
# ユーザーフォルダーの名前に日本語や空白があるときは C:\kantan-dev を使う。
function Test-KantanSafePath([string]$Path) { return $Path -cmatch '^[\x21-\x7E]+$' }
$KantanDev = if ($env:KANTAN_DEV) {
  $env:KANTAN_DEV
} else {
  $p = Join-Path $env:USERPROFILE 'kantan-dev'
  if (Test-KantanSafePath $p) { $p } else { Join-Path $env:SystemDrive 'kantan-dev' }
}
$KantanDev = [System.IO.Path]::GetFullPath($KantanDev)
if (-not (Test-KantanSafePath $KantanDev)) {
  throw "開発用ツールの場所には、日本語や空白を含まないフォルダーを指定してください（MinGW が動かないため）: $KantanDev"
}
if (-not (Test-Path -LiteralPath ([System.IO.Path]::GetPathRoot($KantanDev)))) {
  throw "開発用ツールの場所のドライブが見つかりません: $KantanDev"
}

$KantanTools = [ordered]@{
  Qt     = Join-Path $KantanDev 'Qt\6.8.3\mingw_64'
  MinGW  = Join-Path $KantanDev 'Qt\Tools\mingw1310_64'
  FFmpeg = Join-Path $KantanDev 'ffmpeg'
  CMake  = Join-Path $KantanDev 'cmake\bin\cmake.exe'
  Ninja  = Join-Path $KantanDev 'bin\ninja.exe'
  Aqt    = Join-Path $KantanDev 'bin\aqt.exe'
  NSIS   = Join-Path $KantanDev 'nsis\makensis.exe'
}

# ツールがそろっているか（中身のファイルがあるかで見る）
function Test-KantanTool([string]$Name) {
  $files = switch ($Name) {
    'Qt'     { @(Join-Path $KantanTools.Qt 'lib\cmake\Qt6\Qt6Config.cmake') }
    'MinGW'  { @(Join-Path $KantanTools.MinGW 'bin\g++.exe') }
    'FFmpeg' { @((Join-Path $KantanTools.FFmpeg 'bin\ffmpeg.exe'), (Join-Path $KantanTools.FFmpeg 'include\libavformat\avformat.h')) }
    default  { @($KantanTools[$Name]) }
  }
  foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) { return $false }
  }
  return $true
}

# 外部のツールを動かす。標準エラーに出る警告では止めず、終了コードだけで成否を見る
function Invoke-Tool([string]$Exe, [string[]]$Arguments, [string]$What) {
  $prev = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $Exe @Arguments 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
  } finally {
    $ErrorActionPreference = $prev
  }
  if ($code -ne 0) { throw "$What に失敗しました（終了コード $code）" }
}
