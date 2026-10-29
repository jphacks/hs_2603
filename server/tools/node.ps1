# server\tools の PowerShell の共通部分: AI サーバーを動かす Node.js を用意する。
# Node.js は server\.runtime にダウンロードして使う（システムには何も入れない。いらなくなったら server\.runtime を消せばよい）。
# 版を上げるときは、$EEditorNodeVersion と、https://nodejs.org/dist/v<版>/SHASUMS256.txt の zip の SHA-256 を書き換える。

$EEditorNodeVersion = '24.22.1'
$EEditorNodeSha256 = @{
  'x64'   = 'd2046d28c2c3dcd211ac5145c0bf30e2047ef9dfb306693c8c6e2eccd8b95fa5'
  'arm64' = 'a0c807319fede24c4a0d8f84564c82a619bd313b16b10c007c383c2609e99171'
}
$EEditorServerDir = Split-Path -Parent $PSScriptRoot
$EEditorRuntime = Join-Path $EEditorServerDir '.runtime'
# サーバーの .ts を動かすときに付ける設定（TypeScript を変換せずにそのまま動かす。package.json の scripts と同じ）
$EEditorNodeFlags = @('--experimental-strip-types', '--disable-warning=ExperimentalWarning')

# Node.js（node.exe）の場所を返す。無ければダウンロードする（初回だけ）
function Get-EEditorNode {
  if (-not [Environment]::Is64BitOperatingSystem) { throw 'AI サーバーには 64 ビット版の Windows が必要です' }
  $arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') { 'arm64' } else { 'x64' }
  $name = "node-v$EEditorNodeVersion-win-$arch"
  $dir = Join-Path $EEditorRuntime $name
  $exe = Join-Path $dir 'node.exe'
  if (Test-Path -LiteralPath $exe) { return $exe }

  New-Item -ItemType Directory -Force -Path $EEditorRuntime | Out-Null
  $root = [System.IO.Path]::GetPathRoot($EEditorRuntime)
  if ((New-Object System.IO.DriveInfo $root).AvailableFreeSpace -lt 300MB) { throw "空き容量が足りません。$root に 300MB 以上の空きが必要です" }
  $url = "https://nodejs.org/dist/v$EEditorNodeVersion/$name.zip"
  $zip = Join-Path $EEditorRuntime "$name.zip"
  Write-Host "Node.js $EEditorNodeVersion を server\.runtime にダウンロードします（初回だけ。30MB ほど）"
  Write-Host "  $url"
  $ProgressPreference = 'SilentlyContinue'
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  try {
    Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
  } catch {
    Remove-Item -Force -LiteralPath $zip -ErrorAction SilentlyContinue
    throw "Node.js をダウンロードできませんでした。インターネットにつながっているか確かめてください（$($_.Exception.Message)）"
  }
  # 中身（SHA-256）も確かめる
  if ((Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash -ne $EEditorNodeSha256[$arch].ToUpperInvariant()) {
    Remove-Item -Force -LiteralPath $zip
    throw "ダウンロードした Node.js の中身が想定と違います: $url"
  }
  # 展開は .NET で行う（場所に日本語や空白があっても大丈夫）
  $tmp = Join-Path $EEditorRuntime 'extract'
  if (Test-Path -LiteralPath $tmp) { Remove-Item -Recurse -Force -LiteralPath $tmp }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::ExtractToDirectory($zip, $tmp)
  # 前の版の Node.js は消す（ほかのウィンドウで使っているときは残す）
  Get-ChildItem -LiteralPath $EEditorRuntime -Directory -Filter 'node-v*' | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
  Move-Item -LiteralPath (Join-Path $tmp $name) -Destination $dir
  Remove-Item -Recurse -Force -LiteralPath $tmp
  Remove-Item -Force -LiteralPath $zip
  if (-not (Test-Path -LiteralPath $exe)) { throw "Node.js を用意できませんでした: $exe" }
  Write-Host "Node.js を用意しました: $dir"
  return $exe
}
