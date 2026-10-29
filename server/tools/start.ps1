# AI サーバーを起動する（server\start.cmd から呼ばれる）。止めるときは Ctrl+C か、このウィンドウを閉じる。
# 初回だけ Node.js を server\.runtime にダウンロードする。そのあとは tools\supervisor.ts が、
# ライブラリを入れてサーバーを起動し、server フォルダーのファイルが変わったら、自動で取り込んで起動し直す。
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'node.ps1')
try { $Host.UI.RawUI.WindowTitle = 'e-editor AI サーバー' } catch { }

# 古いコンソールでは、ウィンドウの中をクリックすると「簡易編集モード」で表示が止まり、サーバーも書き込めずに止まって、
# AI の依頼に返事をしなくなる。このウィンドウだけ、簡易編集モードを切っておく（できなければ、そのまま）
try {
  Add-Type -Namespace EEditorConsole -Name Mode -MemberDefinition @'
[DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int nStdHandle);
[DllImport("kernel32.dll")] static extern bool GetConsoleMode(IntPtr h, out uint mode);
[DllImport("kernel32.dll")] static extern bool SetConsoleMode(IntPtr h, uint mode);
public static void DisableQuickEdit() {
  IntPtr h = GetStdHandle(-10);
  uint mode;
  if (!GetConsoleMode(h, out mode)) return;
  SetConsoleMode(h, (mode & ~0x0040u) | 0x0080u);
}
'@
  [EEditorConsole.Mode]::DisableQuickEdit()
} catch { }

$code = 1
try {
  $node = Get-EEditorNode
  # 設定ファイル（初回だけ、見本から作る）
  $envFile = Join-Path $EEditorServerDir '.env'
  if (-not (Test-Path -LiteralPath $envFile)) {
    Copy-Item -LiteralPath (Join-Path $EEditorServerDir '.env.example') -Destination $envFile
    Write-Host ''
    Write-Host 'server\.env を作りました（中身は server\.env.example と同じ）。次の 2 つを書いて保存してください。'
    Write-Host '  DATABASE_URL              PostgreSQL への接続先（ユーザー名・パスワード・データベース名）'
    Write-Host '  AWS_BEARER_TOKEN_BEDROCK  Amazon Bedrock の API キー'
    Write-Host '保存すると、自動でサーバーが起動します（このウィンドウは開いたままにしてください）。'
    Write-Host ''
    # 人が見ているときだけ、メモ帳で開く
    if (-not [Console]::IsInputRedirected) { Start-Process -FilePath 'notepad.exe' -ArgumentList ('"' + $envFile + '"') }
  }
  $supervisor = Join-Path $PSScriptRoot 'supervisor.ts'
  while ($true) {
    & $node @EEditorNodeFlags $supervisor
    $code = $LASTEXITCODE
    # 75: 起動の仕組み（tools）が変わったので、新しい中身で起動し直す
    if ($code -ne 75) { break }
  }
} catch {
  Write-Host $_.Exception.Message -ForegroundColor Red
  $code = 1
}
exit $code
