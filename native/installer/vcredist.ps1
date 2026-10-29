# インストーラー（installer.nsi）から呼ぶ: Microsoft Visual C++ ランタイム（x64。オブジェクトマスクの AI の部品 onnxruntime.dll が使う）を、
# Microsoft の配布元からダウンロードして、静かに入れる。ダウンロードしたファイルは、Microsoft の署名を確かめてから動かす。
# インストーラーの画面に出すので、メッセージは英語（ASCII）にする。終了コード: 0 入れた / それ以外は入れられなかった
param([string]$WorkDir = $env:TEMP)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try {
  [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
  $file = Join-Path $WorkDir 'vc_redist.x64.exe'
  Invoke-WebRequest -UseBasicParsing -Uri 'https://aka.ms/vc14/vc_redist.x64.exe' -OutFile $file
  $sig = Get-AuthenticodeSignature -LiteralPath $file
  if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    Remove-Item -Force -LiteralPath $file -ErrorAction SilentlyContinue
    Write-Output "vc_redist.x64.exe: signature check failed ($($sig.Status))"
    exit 2
  }
  $p = Start-Process -FilePath $file -ArgumentList '/install', '/quiet', '/norestart' -Wait -PassThru
  Remove-Item -Force -LiteralPath $file -ErrorAction SilentlyContinue
  # 0: 入れた / 1638: もっと新しい版が入っている / 3010: 入れた（再起動のあとで使える）
  if (@(0, 1638, 3010) -contains $p.ExitCode) { exit 0 }
  Write-Output "vc_redist.x64.exe: exit code $($p.ExitCode)"
  exit 3
} catch {
  Write-Output "vc_redist.x64.exe: $($_.Exception.Message)"
  exit 1
}
