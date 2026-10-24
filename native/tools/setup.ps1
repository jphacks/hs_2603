# 開発用ツール（Qt・MinGW・FFmpeg・CMake・Ninja・NSIS）をダウンロードする。
#   powershell -ExecutionPolicy Bypass -File native\tools\setup.ps1
# 入る場所は %USERPROFILE%\kantan-dev（環境変数 KANTAN_DEV で変えられる）。システムには何も入れないので、
# いらなくなったらこのフォルダーを消せば元に戻る。そろっているものは飛ばす。
# native\build.ps1 は、ツールが足りないときにこれを自動で実行する。
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
. (Join-Path $PSScriptRoot 'common.ps1')

$all = @('CMake', 'Ninja', 'Qt', 'MinGW', 'FFmpeg', 'NSIS')
$need = @($all | Where-Object { -not (Test-KantanTool $_) })
if ($need.Count -eq 0) {
  Write-Host "開発用ツールはそろっています: $KantanDev"
  return
}

New-Item -ItemType Directory -Force -Path $KantanDev | Out-Null
$root = [System.IO.Path]::GetPathRoot($KantanDev)
if ((New-Object System.IO.DriveInfo $root).AvailableFreeSpace -lt 2GB) { throw "空き容量が足りません。$root に 2GB 以上の空きが必要です" }
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (-not (Test-Path -LiteralPath $tar)) { throw 'tar.exe が見つかりません。Windows 10 (1803) 以降が必要です' }
$tmp = Join-Path $KantanDev 'download'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
Write-Host "開発用ツールを $KantanDev にダウンロードします（$($need -join '・')）"
if (@($need | Where-Object { $_ -in 'Qt', 'MinGW', 'FFmpeg' }).Count -gt 0) {
  Write-Host '合わせて 1GB ほどあるので、しばらくかかります（回線の速さによりますが、5 分ほど）'
}

function Get-Download([string]$Url, [string]$Sha256 = '') {
  $out = Join-Path $tmp (Split-Path $Url -Leaf)
  Write-Host "  $Url"
  try {
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $out
  } catch {
    throw "ダウンロードできませんでした: $Url ($($_.Exception.Message))"
  }
  # 決まったファイルは中身（SHA-256）も確かめる
  if ($Sha256 -and (Get-FileHash -Algorithm SHA256 -LiteralPath $out).Hash -ne $Sha256.ToUpperInvariant()) {
    Remove-Item -Force -LiteralPath $out
    throw "ダウンロードしたファイルの中身が想定と違います: $Url"
  }
  return $out
}

# ダウンロードして展開し、展開したフォルダーを返す
function Expand-Download([string]$Url, [string]$Sha256 = '') {
  $file = Get-Download $Url $Sha256
  $dir = Join-Path $tmp ('x-' + [System.IO.Path]::GetFileNameWithoutExtension($file))
  if (Test-Path -LiteralPath $dir) { Remove-Item -Recurse -Force -LiteralPath $dir }
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  # Windows の tar.exe は zip と 7z を展開できる
  Invoke-Tool $tar @('-xf', $file, '-C', $dir) "$(Split-Path $file -Leaf) の展開" | Out-Null
  Remove-Item -Force -LiteralPath $file
  return $dir
}

function Move-Folder([string]$From, [string]$To) {
  if (Test-Path -LiteralPath $To) { Remove-Item -Recurse -Force -LiteralPath $To }
  Move-Item -LiteralPath $From -Destination $To
}

New-Item -ItemType Directory -Force -Path (Join-Path $KantanDev 'bin') | Out-Null

if ($need -contains 'CMake') {
  Write-Host 'CMake 4.4.4'
  $x = Expand-Download 'https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-windows-x86_64.zip' 'bace36e94b31c68ab6fa295f26dfa11219e0701cf7c94b0284a7d1cb13dac536'
  Move-Folder (Join-Path $x 'cmake-4.4.4-windows-x86_64') (Join-Path $KantanDev 'cmake')
  Remove-Item -Recurse -Force -LiteralPath $x
}

if ($need -contains 'Ninja') {
  Write-Host 'Ninja 1.13.2'
  $x = Expand-Download 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip' '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'
  Move-Item -Force -LiteralPath (Join-Path $x 'ninja.exe') -Destination $KantanTools.Ninja
  Remove-Item -Recurse -Force -LiteralPath $x
}

if (($need -contains 'Qt') -or ($need -contains 'MinGW')) {
  if (-not (Test-KantanTool 'Aqt')) {
    Write-Host 'aqtinstall 3.3.0（Qt をダウンロードする道具）'
    $f = Get-Download 'https://github.com/miurahr/aqtinstall/releases/download/v3.3.0/aqt_x64.exe' '4f74d4c95c464d238d7e17ec2d9b7f22a7c333f0f5270a62584e2b47fc765150'
    Move-Item -Force -LiteralPath $f -Destination $KantanTools.Aqt
  }
  $qtRoot = Join-Path $KantanDev 'Qt'
  # aqt の細かい進み具合や、動作に関係のない警告（pkg_resources など）は出さない
  function Invoke-Aqt([string[]]$Arguments, [string]$What) {
    Invoke-Tool $KantanTools.Aqt $Arguments $What | Where-Object { $_ -match 'Downloading|Finished installation|WARNING|ERROR' }
  }
  # aqt の中の別プロセスが出す警告は止められないので、先に断っておく
  Write-Host '（途中で「pkg_resources is deprecated」という警告が出ることがありますが、問題ありません）'
  # aqt は今いるフォルダーに aqtinstall.log を作るので、作業用フォルダーで動かす
  Push-Location -LiteralPath $tmp
  try {
    if ($need -contains 'Qt') {
      Write-Host 'Qt 6.8.3（MinGW 版）'
      Invoke-Aqt @('install-qt', 'windows', 'desktop', '6.8.3', 'win64_mingw', '-O', $qtRoot,
        '--archives', 'qtbase', 'qtsvg', 'qttranslations', '-m', 'qtimageformats') 'Qt のダウンロード'
    }
    if ($need -contains 'MinGW') {
      Write-Host 'MinGW 13.1'
      Invoke-Aqt @('install-tool', 'windows', 'desktop', 'tools_mingw1310', 'qt.tools.win64_mingw1310', '-O', $qtRoot) 'MinGW のダウンロード'
    }
  } finally {
    Pop-Location
  }
}

if ($need -contains 'FFmpeg') {
  # 8.1 系の最新のビルド（GPL 版・共有ライブラリ）。中身は毎日更新されるので SHA-256 は確かめない
  Write-Host 'FFmpeg 8.1'
  $x = Expand-Download 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip'
  $inner = Get-ChildItem -LiteralPath $x -Directory | Select-Object -First 1
  Move-Folder $inner.FullName $KantanTools.FFmpeg
  Remove-Item -Recurse -Force -LiteralPath $x
}

if ($need -contains 'NSIS') {
  Write-Host 'NSIS 3（インストーラーを作るときに使う）'
  $x = Expand-Download 'https://github.com/electron-userland/electron-builder-binaries/releases/download/nsis-3.0.4.1/nsis-3.0.4.1.7z' '9877df902530f96357d13a7a31ae2b9df67f48b11ffc9a1700a7c961574ec5fa'
  Move-Folder $x (Split-Path $KantanTools.NSIS)
}

Remove-Item -Recurse -Force -LiteralPath $tmp -ErrorAction SilentlyContinue
$still = @($all | Where-Object { -not (Test-KantanTool $_) })
if ($still.Count -gt 0) { throw "用意できなかったツールがあります: $($still -join ', ')" }
Write-Host "開発用ツールがそろいました: $KantanDev"
