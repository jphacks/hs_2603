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
  Fonts  = Join-Path $KantanDev 'fonts'
}

# アプリに同梱する文字のフォント（YouTube の字幕・テロップでよく使われる無料のフォント）と、その使用許諾。
# どれも SIL Open Font License 1.1（小杉丸ゴシックだけ Apache License 2.0）で、アプリと一緒に配ってよい。
# 決まったコミットのファイルを使い、中身（SHA-256）も確かめる。build.ps1 がアプリの fonts と licenses\fonts に入れる
$KantanFontBase = @{
  Google = 'https://raw.githubusercontent.com/google/fonts/e7db6ca603fd4862010383ff17ffd6cc4ffac1c0'
  Noto   = 'https://raw.githubusercontent.com/notofonts/noto-cjk/f8d157532fbfaeda587e826d4cd5b21a49186f7c'
  MPlus  = 'https://raw.githubusercontent.com/coz-m/MPLUS_FONTS/eb604901d6f04b6f7f2a84b0378c58df84a9dba6'
}
$KantanFonts = @(
  @{ File = 'NotoSansJP-Regular.otf'; Url = "$($KantanFontBase.Noto)/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf"; Sha256 = 'dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073' },
  @{ File = 'NotoSansJP-Bold.otf'; Url = "$($KantanFontBase.Noto)/Sans/SubsetOTF/JP/NotoSansJP-Bold.otf"; Sha256 = '1b0edfb500b73a4fa8a4fcaae1bbbd403994e08e73e3e0da37e70d3853f42c5f' },
  @{ File = 'NotoSansJP-LICENSE.txt'; Url = "$($KantanFontBase.Noto)/Sans/LICENSE"; Sha256 = '6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2' },
  @{ File = 'MPLUSRounded1c-Regular.ttf'; Url = "$($KantanFontBase.Google)/ofl/mplusrounded1c/MPLUSRounded1c-Regular.ttf"; Sha256 = 'b75708b53e45b06d17d470aeeca5b766e3d1b3999f03f13ec4eb863ca846c14c' },
  @{ File = 'MPLUSRounded1c-Bold.ttf'; Url = "$($KantanFontBase.Google)/ofl/mplusrounded1c/MPLUSRounded1c-Bold.ttf"; Sha256 = 'c358630584e8e2d8fbd6121d0f4693255ffef6d1e6d4f3441fd6e5a963a11f9e' },
  # M PLUS Rounded 1c の配布元には使用許諾の文書が無いので、同じ OFL 1.1 の文書（M+ FONTS の配布元のもの）に、このフォントの著作権表示を付けて入れる
  @{ File = 'MPLUSRounded1c-OFL.txt'; Url = "$($KantanFontBase.MPlus)/OFL.txt"; Sha256 = '04971e3fcee60b247395150d93b3616f6a0b092572332c96187b472976553abc'; Prepend = 'M PLUS Rounded 1c: Copyright 2016 The Rounded M+ Project Authors.' },
  @{ File = 'ZenMaruGothic-Regular.ttf'; Url = "$($KantanFontBase.Google)/ofl/zenmarugothic/ZenMaruGothic-Regular.ttf"; Sha256 = 'a0c0b53543e0993ae2225e629c833f3d51495ad31720694ff112ce4ce11111ef' },
  @{ File = 'ZenMaruGothic-Bold.ttf'; Url = "$($KantanFontBase.Google)/ofl/zenmarugothic/ZenMaruGothic-Bold.ttf"; Sha256 = 'fe24426b9c8b5523a0146a8235c8674eccf0493af354a53ec895c3596d9eb745' },
  @{ File = 'ZenMaruGothic-OFL.txt'; Url = "$($KantanFontBase.Google)/ofl/zenmarugothic/OFL.txt"; Sha256 = '2a20cf7ce1909d8ee1e949095d340f7d7656705f7c810a2d6faf56800ad0cb3d' },
  @{ File = 'KosugiMaru-Regular.ttf'; Url = "$($KantanFontBase.Google)/apache/kosugimaru/KosugiMaru-Regular.ttf"; Sha256 = '4b8d0022c8dadd090ef67cd1f71f130714767af7806cba2eb4ebe4b0271c1d68' },
  @{ File = 'KosugiMaru-LICENSE.txt'; Url = "$($KantanFontBase.Google)/apache/kosugimaru/LICENSE.txt"; Sha256 = 'cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30' },
  @{ File = 'DelaGothicOne-Regular.ttf'; Url = "$($KantanFontBase.Google)/ofl/delagothicone/DelaGothicOne-Regular.ttf"; Sha256 = '4ff87a0965f1b0505e5a2c58424bc6ad3cff27e56a82f21c2fc9d6b0e3857ee2' },
  @{ File = 'DelaGothicOne-OFL.txt'; Url = "$($KantanFontBase.Google)/ofl/delagothicone/OFL.txt"; Sha256 = 'c0014792d4f4abc0508c295b277f2b17ae44465c8dc88d12af9cea48279b5fda' },
  @{ File = 'RocknRollOne-Regular.ttf'; Url = "$($KantanFontBase.Google)/ofl/rocknrollone/RocknRollOne-Regular.ttf"; Sha256 = 'dc0f5ff975851827f63f2c6bfed128ffbca14b6399a10fb5e1711215c0108526' },
  @{ File = 'RocknRollOne-OFL.txt'; Url = "$($KantanFontBase.Google)/ofl/rocknrollone/OFL.txt"; Sha256 = 'b2f42a005a6a48ead81e369021e300184f0cbecb48aee6f831e46c7897f6055b' },
  @{ File = 'MochiyPopOne-Regular.ttf'; Url = "$($KantanFontBase.Google)/ofl/mochiypopone/MochiyPopOne-Regular.ttf"; Sha256 = '9e009430e1316c271a5f34759c6b65fc343c4e806f193042528887e7235a92c6' },
  @{ File = 'MochiyPopOne-OFL.txt'; Url = "$($KantanFontBase.Google)/ofl/mochiypopone/OFL.txt"; Sha256 = '76be26178f13ef82866cf6a5c54272191bb583c203ad7035021c7681c9043558' },
  @{ File = 'ReggaeOne-Regular.ttf'; Url = "$($KantanFontBase.Google)/ofl/reggaeone/ReggaeOne-Regular.ttf"; Sha256 = 'aebe62598732d76036f30ec11bb0ec5f68938e373a06d1b4ceb6b9cf1abf3db2' },
  @{ File = 'ReggaeOne-OFL.txt'; Url = "$($KantanFontBase.Google)/ofl/reggaeone/OFL.txt"; Sha256 = '2756c3dcd85ad997b742330f91332fb478afad302708d4149f7bcd4600543ce2' },
  @{ File = 'RampartOne-Regular.ttf'; Url = "$($KantanFontBase.Google)/ofl/rampartone/RampartOne-Regular.ttf"; Sha256 = 'a4c60ee835175fa147eb483fac0dd8ff9007c37884ef77af86b0a868fa554d49' },
  @{ File = 'RampartOne-OFL.txt'; Url = "$($KantanFontBase.Google)/ofl/rampartone/OFL.txt"; Sha256 = '1286896706738557c1fb98f0f66006d4ca675e475d681ca85976fd0da9daa353' },
  @{ File = 'YuseiMagic-Regular.ttf'; Url = "$($KantanFontBase.Google)/ofl/yuseimagic/YuseiMagic-Regular.ttf"; Sha256 = '82098615f39ed9da6a8ccc674b9006e49c70dd5b775a7a1697f6bedd22ce25a2' },
  @{ File = 'YuseiMagic-OFL.txt'; Url = "$($KantanFontBase.Google)/ofl/yuseimagic/OFL.txt"; Sha256 = 'c74e8c47951ddd9c902f07097761cfa0457993e28d8e1e946e273c0250be77c9' }
)

# ツールがそろっているか（中身のファイルがあるかで見る）
function Test-KantanTool([string]$Name) {
  $files = switch ($Name) {
    'Qt'     { @(Join-Path $KantanTools.Qt 'lib\cmake\Qt6\Qt6Config.cmake') }
    'MinGW'  { @(Join-Path $KantanTools.MinGW 'bin\g++.exe') }
    'FFmpeg' { @((Join-Path $KantanTools.FFmpeg 'bin\ffmpeg.exe'), (Join-Path $KantanTools.FFmpeg 'include\libavformat\avformat.h')) }
    'Fonts'  { @($KantanFonts | ForEach-Object { Join-Path $KantanTools.Fonts $_.File }) }
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
