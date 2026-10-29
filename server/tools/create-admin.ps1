# AI サーバーの管理者を作る（server\create-admin.cmd から呼ばれる）。管理者のパスワードを忘れたときの、作り直しにも使う。
#   create-admin.cmd                                     メールアドレスと名前を聞く
#   create-admin.cmd -Email you@example.com -Name 名前   聞かずに作る（-Reset で、登録済みの人を管理者にして作り直す）
param(
  [string]$Email = '',
  [string]$Name = '',
  [switch]$Reset
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'node.ps1')
try { $Host.UI.RawUI.WindowTitle = 'e-editor AI サーバー - 管理者を作る' } catch { }

$code = 1
try {
  $node = Get-EEditorNode
  if (-not (Test-Path -LiteralPath (Join-Path $EEditorServerDir '.env'))) {
    throw 'server\.env がありません。先に start.cmd を起動して、server\.env を書いてください'
  }
  # ライブラリが無ければ入れる（start.cmd を起動したことがあれば、入っている）
  if (-not (Test-Path -LiteralPath (Join-Path $EEditorServerDir 'node_modules\pg\package.json'))) {
    & $node @EEditorNodeFlags (Join-Path $PSScriptRoot 'deps.ts')
    if ($LASTEXITCODE -ne 0) { throw 'ライブラリを入れられませんでした' }
  }
  $ask = -not $Email
  if ($ask) {
    Write-Host 'AI サーバーの管理者を作ります。仮のパスワードを作って表示するので、管理画面にそのパスワードでログインしてください。'
    Write-Host '（登録済みのメールアドレスを入れると、管理者にして、仮のパスワードを作り直せます）'
    Write-Host ''
    for ($i = 0; $i -lt 5 -and $Email -notmatch '^[^\s@]+@[^\s@]+\.[^\s@]+$'; $i++) {
      if ($Email) { Write-Host 'メールアドレスの書き方が正しくありません' -ForegroundColor Yellow }
      $Email = (Read-Host 'メールアドレス').Trim()
    }
    if ($Email -notmatch '^[^\s@]+@[^\s@]+\.[^\s@]+$') { throw 'メールアドレスが入力されませんでした' }
    $Name = (Read-Host '名前（空のままでもかまいません）').Trim()
  }
  $cli = Join-Path $EEditorServerDir 'src\cli\create-admin.ts'
  $cliArgs = @('--email', $Email)
  if ($Name) { $cliArgs += @('--name', $Name) }
  if ($Reset) { $cliArgs += '--reset' }
  & $node @EEditorNodeFlags $cli @cliArgs
  $code = $LASTEXITCODE
  # 2: もう登録されている
  if ($code -eq 2 -and $ask) {
    $answer = Read-Host 'このメールアドレスは、もう登録されています。管理者にして、仮のパスワードを作り直しますか？（y で作り直す）'
    if ($answer.Trim() -match '^(y|yes)$') {
      & $node @EEditorNodeFlags $cli @cliArgs '--reset'
      $code = $LASTEXITCODE
    }
  }
} catch {
  Write-Host $_.Exception.Message -ForegroundColor Red
  $code = 1
}
exit $code
