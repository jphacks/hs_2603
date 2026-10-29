# native\build.ps1・native\tools\setup.ps1・native\installer\build-installer.ps1 の共通部分

# 開発用ツールの置き場所（環境変数 E_EDITOR_DEV で変えられる）。
# MinGW は場所に日本語や空白が入ると動かないので（ld が自分のライブラリを見つけられない、windres が失敗する）、
# ユーザーフォルダーの名前に日本語や空白があるときは C:\e-editor-dev を使う。
function Test-EEditorSafePath([string]$Path) { return $Path -cmatch '^[\x21-\x7E]+$' }
$EEditorDev = if ($env:E_EDITOR_DEV) {
  $env:E_EDITOR_DEV
} else {
  $p = Join-Path $env:USERPROFILE 'e-editor-dev'
  if (Test-EEditorSafePath $p) { $p } else { Join-Path $env:SystemDrive 'e-editor-dev' }
}
$EEditorDev = [System.IO.Path]::GetFullPath($EEditorDev)
if (-not (Test-EEditorSafePath $EEditorDev)) {
  throw "開発用ツールの場所には、日本語や空白を含まないフォルダーを指定してください（MinGW が動かないため）: $EEditorDev"
}
if (-not (Test-Path -LiteralPath ([System.IO.Path]::GetPathRoot($EEditorDev)))) {
  throw "開発用ツールの場所のドライブが見つかりません: $EEditorDev"
}

$EEditorTools = [ordered]@{
  Qt     = Join-Path $EEditorDev 'Qt\6.8.3\mingw_64'
  MinGW  = Join-Path $EEditorDev 'Qt\Tools\mingw1310_64'
  FFmpeg = Join-Path $EEditorDev 'ffmpeg'
  CMake  = Join-Path $EEditorDev 'cmake\bin\cmake.exe'
  Ninja  = Join-Path $EEditorDev 'bin\ninja.exe'
  Aqt    = Join-Path $EEditorDev 'bin\aqt.exe'
  NSIS   = Join-Path $EEditorDev 'nsis\makensis.exe'
  Fonts  = Join-Path $EEditorDev 'fonts'
  # オブジェクトマスク（AI で被写体を切り抜く）: ONNX Runtime と、被写体を追いかけるモデル（EdgeTAM）
  Ort       = Join-Path $EEditorDev 'onnxruntime'
  MaskModel = Join-Path $EEditorDev 'models\edgetam'
}

# ONNX Runtime（CPU 版。MIT License）。アプリは onnxruntime.dll を起動中に読み込む（無ければオブジェクトマスクだけが使えない）
$EEditorOrt = @{
  Version = '1.31.1'
  Url     = 'https://github.com/microsoft/onnxruntime/releases/download/v1.31.1/onnxruntime-win-x64-1.31.1.zip'
  Sha256  = '5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c'
}

# オブジェクトマスクのモデル: facebook/EdgeTAM（Apache License 2.0）の動画の追跡を ONNX にしたもの（jax-image-tools/edgetam-video-onnx）。
# 決まったコミットのファイルを使い、中身（SHA-256）も確かめる。build.ps1 がアプリの models\edgetam に入れる
$EEditorMaskModelBase = 'https://huggingface.co/jax-image-tools/edgetam-video-onnx/resolve/8ca3d3e4169938e65b552cdf14542fde25e8badb'
$EEditorMaskModel = @(
  @{ File = 'vision_encoder.onnx'; Url = "$EEditorMaskModelBase/vision_encoder.onnx"; Sha256 = '57fe1a2b3d500813fe4987c4209b856920f187ab0b7723d81586b607ea2cffc1' },
  @{ File = 'mask_decoder.onnx'; Url = "$EEditorMaskModelBase/mask_decoder.onnx"; Sha256 = 'ee24ee1cae6ecc71889912c1893e08b09f645bb63875108ccd180f7218a3647d' },
  @{ File = 'memory_attention.onnx'; Url = "$EEditorMaskModelBase/memory_attention.onnx"; Sha256 = '6477d775639905945949508c08a2c652d01e5933c0eb00a02225ae5f6aa1602c' },
  @{ File = 'memory_encoder.onnx'; Url = "$EEditorMaskModelBase/memory_encoder.onnx"; Sha256 = '035b30f8fb1f7c99211ada6eb9d72f0819de819ef94310b14e8147dc97f120ec' },
  # モデルの使用許諾（EdgeTAM の配布元のもの。build.ps1 がアプリの licenses に入れる）
  @{ File = 'EdgeTAM-LICENSE.txt'; Url = 'https://raw.githubusercontent.com/facebookresearch/EdgeTAM/1c2ec2184a3313a5657ac924f450ddf72b56eebf/LICENSE'; Sha256 = 'c71d239df91726fc519c6eb72d318ec65820627232b2f796219e87dcf35d0ab4' }
)

# アプリに同梱する文字のフォント（YouTube の字幕・テロップでよく使われる無料のフォント）と、その使用許諾。
# どれも SIL Open Font License 1.1（小杉丸ゴシックだけ Apache License 2.0）で、アプリと一緒に配ってよい。
# 決まったコミットのファイルを使い、中身（SHA-256）も確かめる。build.ps1 がアプリの fonts と licenses\fonts に入れる
$EEditorFontBase = @{
  Google = 'https://raw.githubusercontent.com/google/fonts/e7db6ca603fd4862010383ff17ffd6cc4ffac1c0'
  Noto   = 'https://raw.githubusercontent.com/notofonts/noto-cjk/f8d157532fbfaeda587e826d4cd5b21a49186f7c'
  MPlus  = 'https://raw.githubusercontent.com/coz-m/MPLUS_FONTS/eb604901d6f04b6f7f2a84b0378c58df84a9dba6'
}
$EEditorFonts = @(
  @{ File = 'NotoSansJP-Regular.otf'; Url = "$($EEditorFontBase.Noto)/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf"; Sha256 = 'dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073' },
  @{ File = 'NotoSansJP-Bold.otf'; Url = "$($EEditorFontBase.Noto)/Sans/SubsetOTF/JP/NotoSansJP-Bold.otf"; Sha256 = '1b0edfb500b73a4fa8a4fcaae1bbbd403994e08e73e3e0da37e70d3853f42c5f' },
  @{ File = 'NotoSansJP-LICENSE.txt'; Url = "$($EEditorFontBase.Noto)/Sans/LICENSE"; Sha256 = '6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2' },
  @{ File = 'MPLUSRounded1c-Regular.ttf'; Url = "$($EEditorFontBase.Google)/ofl/mplusrounded1c/MPLUSRounded1c-Regular.ttf"; Sha256 = 'b75708b53e45b06d17d470aeeca5b766e3d1b3999f03f13ec4eb863ca846c14c' },
  @{ File = 'MPLUSRounded1c-Bold.ttf'; Url = "$($EEditorFontBase.Google)/ofl/mplusrounded1c/MPLUSRounded1c-Bold.ttf"; Sha256 = 'c358630584e8e2d8fbd6121d0f4693255ffef6d1e6d4f3441fd6e5a963a11f9e' },
  # M PLUS Rounded 1c の配布元には使用許諾の文書が無いので、同じ OFL 1.1 の文書（M+ FONTS の配布元のもの）に、このフォントの著作権表示を付けて入れる
  @{ File = 'MPLUSRounded1c-OFL.txt'; Url = "$($EEditorFontBase.MPlus)/OFL.txt"; Sha256 = '04971e3fcee60b247395150d93b3616f6a0b092572332c96187b472976553abc'; Prepend = 'M PLUS Rounded 1c: Copyright 2016 The Rounded M+ Project Authors.' },
  @{ File = 'ZenMaruGothic-Regular.ttf'; Url = "$($EEditorFontBase.Google)/ofl/zenmarugothic/ZenMaruGothic-Regular.ttf"; Sha256 = 'a0c0b53543e0993ae2225e629c833f3d51495ad31720694ff112ce4ce11111ef' },
  @{ File = 'ZenMaruGothic-Bold.ttf'; Url = "$($EEditorFontBase.Google)/ofl/zenmarugothic/ZenMaruGothic-Bold.ttf"; Sha256 = 'fe24426b9c8b5523a0146a8235c8674eccf0493af354a53ec895c3596d9eb745' },
  @{ File = 'ZenMaruGothic-OFL.txt'; Url = "$($EEditorFontBase.Google)/ofl/zenmarugothic/OFL.txt"; Sha256 = '2a20cf7ce1909d8ee1e949095d340f7d7656705f7c810a2d6faf56800ad0cb3d' },
  @{ File = 'KosugiMaru-Regular.ttf'; Url = "$($EEditorFontBase.Google)/apache/kosugimaru/KosugiMaru-Regular.ttf"; Sha256 = '4b8d0022c8dadd090ef67cd1f71f130714767af7806cba2eb4ebe4b0271c1d68' },
  @{ File = 'KosugiMaru-LICENSE.txt'; Url = "$($EEditorFontBase.Google)/apache/kosugimaru/LICENSE.txt"; Sha256 = 'cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30' },
  @{ File = 'DelaGothicOne-Regular.ttf'; Url = "$($EEditorFontBase.Google)/ofl/delagothicone/DelaGothicOne-Regular.ttf"; Sha256 = '4ff87a0965f1b0505e5a2c58424bc6ad3cff27e56a82f21c2fc9d6b0e3857ee2' },
  @{ File = 'DelaGothicOne-OFL.txt'; Url = "$($EEditorFontBase.Google)/ofl/delagothicone/OFL.txt"; Sha256 = 'c0014792d4f4abc0508c295b277f2b17ae44465c8dc88d12af9cea48279b5fda' },
  @{ File = 'RocknRollOne-Regular.ttf'; Url = "$($EEditorFontBase.Google)/ofl/rocknrollone/RocknRollOne-Regular.ttf"; Sha256 = 'dc0f5ff975851827f63f2c6bfed128ffbca14b6399a10fb5e1711215c0108526' },
  @{ File = 'RocknRollOne-OFL.txt'; Url = "$($EEditorFontBase.Google)/ofl/rocknrollone/OFL.txt"; Sha256 = 'b2f42a005a6a48ead81e369021e300184f0cbecb48aee6f831e46c7897f6055b' },
  @{ File = 'MochiyPopOne-Regular.ttf'; Url = "$($EEditorFontBase.Google)/ofl/mochiypopone/MochiyPopOne-Regular.ttf"; Sha256 = '9e009430e1316c271a5f34759c6b65fc343c4e806f193042528887e7235a92c6' },
  @{ File = 'MochiyPopOne-OFL.txt'; Url = "$($EEditorFontBase.Google)/ofl/mochiypopone/OFL.txt"; Sha256 = '76be26178f13ef82866cf6a5c54272191bb583c203ad7035021c7681c9043558' },
  @{ File = 'ReggaeOne-Regular.ttf'; Url = "$($EEditorFontBase.Google)/ofl/reggaeone/ReggaeOne-Regular.ttf"; Sha256 = 'aebe62598732d76036f30ec11bb0ec5f68938e373a06d1b4ceb6b9cf1abf3db2' },
  @{ File = 'ReggaeOne-OFL.txt'; Url = "$($EEditorFontBase.Google)/ofl/reggaeone/OFL.txt"; Sha256 = '2756c3dcd85ad997b742330f91332fb478afad302708d4149f7bcd4600543ce2' },
  @{ File = 'RampartOne-Regular.ttf'; Url = "$($EEditorFontBase.Google)/ofl/rampartone/RampartOne-Regular.ttf"; Sha256 = 'a4c60ee835175fa147eb483fac0dd8ff9007c37884ef77af86b0a868fa554d49' },
  @{ File = 'RampartOne-OFL.txt'; Url = "$($EEditorFontBase.Google)/ofl/rampartone/OFL.txt"; Sha256 = '1286896706738557c1fb98f0f66006d4ca675e475d681ca85976fd0da9daa353' },
  @{ File = 'YuseiMagic-Regular.ttf'; Url = "$($EEditorFontBase.Google)/ofl/yuseimagic/YuseiMagic-Regular.ttf"; Sha256 = '82098615f39ed9da6a8ccc674b9006e49c70dd5b775a7a1697f6bedd22ce25a2' },
  @{ File = 'YuseiMagic-OFL.txt'; Url = "$($EEditorFontBase.Google)/ofl/yuseimagic/OFL.txt"; Sha256 = 'c74e8c47951ddd9c902f07097761cfa0457993e28d8e1e946e273c0250be77c9' }
)

# ツールがそろっているか（中身のファイルがあるかで見る）
function Test-EEditorTool([string]$Name) {
  $files = switch ($Name) {
    'Qt'     { @(Join-Path $EEditorTools.Qt 'lib\cmake\Qt6\Qt6Config.cmake') }
    'MinGW'  { @(Join-Path $EEditorTools.MinGW 'bin\g++.exe') }
    'FFmpeg' { @((Join-Path $EEditorTools.FFmpeg 'bin\ffmpeg.exe'), (Join-Path $EEditorTools.FFmpeg 'include\libavformat\avformat.h')) }
    'Fonts'  { @($EEditorFonts | ForEach-Object { Join-Path $EEditorTools.Fonts $_.File }) }
    'Ort'    { @((Join-Path $EEditorTools.Ort 'include\onnxruntime_c_api.h'), (Join-Path $EEditorTools.Ort 'lib\onnxruntime.dll')) }
    'MaskModel' { @($EEditorMaskModel | ForEach-Object { Join-Path $EEditorTools.MaskModel $_.File }) }
    default  { @($EEditorTools[$Name]) }
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

# ---- 配る exe に組み込む AI サーバーのアドレス（native\release.env の E_EDITOR_AI_SERVER） ----

# release.env の E_EDITOR_AI_SERVER の値（ファイルが無い・書いていなければ空）。読み方はアプリの .env と同じ
# （# で始まる行は無視、値の前後の引用符を外す、値のあとの「 #」からはコメント。UTF-8 でも UTF-16 でもよい）
function Read-EEditorReleaseServer([string]$File) {
  if (-not (Test-Path -LiteralPath $File)) { return '' }
  $value = ''
  foreach ($raw in ([System.IO.File]::ReadAllText($File) -split "`n")) {
    $line = $raw.Trim()
    if (-not $line -or $line.StartsWith('#')) { continue }
    if ($line -notmatch '^(?:export\s+)?E_EDITOR_AI_SERVER\s*=(.*)$') { continue }
    $v = $Matches[1].Trim()
    if ($v.Length -ge 2 -and (($v[0] -eq '"' -and $v[-1] -eq '"') -or ($v[0] -eq "'" -and $v[-1] -eq "'"))) {
      $v = $v.Substring(1, $v.Length - 2)
    } else {
      $i = $v.IndexOf(' #')
      if ($i -ge 0) { $v = $v.Substring(0, $i) }
    }
    $value = $v.Trim()
  }
  return $value
}

# 家の中（この PC・家のネットワーク・.local・Tailscale の 100.64.0.0/10）のアドレスか（アプリの privateHost と同じ決まり）
function Test-EEditorPrivateHost([string]$HostName) {
  $h = $HostName.ToLowerInvariant().Trim('[', ']')
  if ($h -eq 'localhost' -or $h.EndsWith('.local')) { return $true }
  $ip = $null
  if (-not [System.Net.IPAddress]::TryParse($h, [ref]$ip)) { return $false }
  if ([System.Net.IPAddress]::IsLoopback($ip)) { return $true }
  $b = $ip.GetAddressBytes()
  if ($ip.AddressFamily -eq [System.Net.Sockets.AddressFamily]::InterNetworkV6) {
    return $ip.IsIPv6LinkLocal -or (($b[0] -band 0xFE) -eq 0xFC)
  }
  return ($b[0] -eq 10) -or ($b[0] -eq 172 -and $b[1] -ge 16 -and $b[1] -le 31) -or ($b[0] -eq 192 -and $b[1] -eq 168) -or
    ($b[0] -eq 169 -and $b[1] -eq 254) -or ($b[0] -eq 100 -and ($b[1] -band 0xC0) -eq 64)
}

# 組み込むアドレスを確かめる（だめなら止める。配ったあとでは、使う人には直せないため）。アドレスそのものは表示しない
function Assert-EEditorReleaseServer([string]$Value, [string]$File) {
  $hasScheme = $Value -match '^[A-Za-z][A-Za-z0-9+.-]*://'
  $probe = if ($hasScheme) { $Value } else { 'http://' + $Value }
  $u = $null
  if (-not [Uri]::TryCreate($probe, [UriKind]::Absolute, [ref]$u) -or -not $u.Host) {
    throw "$File の E_EDITOR_AI_SERVER の書き方が正しくありません（例: https://ai.example.com）"
  }
  $scheme = if ($hasScheme) { $u.Scheme } elseif (Test-EEditorPrivateHost $u.Host) { 'http' } else { 'https' }
  if ($scheme -ne 'http' -and $scheme -ne 'https') {
    throw "$File の E_EDITOR_AI_SERVER は http:// か https:// のアドレスにしてください"
  }
  if ($u.UserInfo -or $u.Query -or $u.Fragment) {
    throw "$File の E_EDITOR_AI_SERVER には、サーバーのアドレスだけを書いてください（例: https://ai.example.com）"
  }
  if ($scheme -eq 'http' -and -not (Test-EEditorPrivateHost $u.Host)) {
    throw "$File の E_EDITOR_AI_SERVER が、HTTP で家の外のアドレスになっています。パスワードが読まれないよう、https:// にしてください"
  }
}

# 組み込むアドレスを、ビルドのフォルダーの generated\kv_embedded_server.h に書く（aiconfig.cpp が読む）。
# exe を文字で眺めても見えないよう、毎回作る鍵と混ぜたバイトの並びにする。空なら、組み込まない形にする。
# 中身が前と同じなら書き換えない（書き換えると aiconfig.cpp をコンパイルし直すため）
function Write-EEditorEmbeddedServer([string]$Value, [string]$Dir) {
  New-Item -ItemType Directory -Force -Path $Dir | Out-Null
  $header = Join-Path $Dir 'kv_embedded_server.h'
  $stampFile = Join-Path $Dir 'kv_embedded_server.stamp'
  $sha = [System.Security.Cryptography.SHA256]::Create()
  $stamp = -join ($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes('kv-embed-v1:' + $Value)) | ForEach-Object { $_.ToString('x2') })
  if ((Test-Path -LiteralPath $header) -and (Test-Path -LiteralPath $stampFile) -and ([System.IO.File]::ReadAllText($stampFile) -eq $stamp)) { return }
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($Value)
  $n = $bytes.Length
  $key = New-Object byte[] ([Math]::Max(1, $n))
  [System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($key)
  $data = New-Object byte[] ([Math]::Max(1, $n))
  for ($i = 0; $i -lt $n; $i++) { $data[$i] = $bytes[$i] -bxor $key[$i] -bxor (($i * 0x9D + 0x5B) -band 0xFF) }
  $hex = { param($a) ($a | ForEach-Object { '0x{0:x2}' -f $_ }) -join ',' }
  $text = @(
    '// build.ps1 makes this file from native/release.env. Do not edit. Not in git.',
    '#pragma once',
    'namespace kv::ai::embedded {',
    "inline constexpr int kSize = $n;",
    "inline constexpr unsigned char kKey[] = {$(& $hex $key)};",
    "inline constexpr unsigned char kData[] = {$(& $hex $data)};",
    '}  // namespace kv::ai::embedded',
    ''
  ) -join "`n"
  [System.IO.File]::WriteAllText($header, $text, (New-Object System.Text.UTF8Encoding($false)))
  [System.IO.File]::WriteAllText($stampFile, $stamp)
}
