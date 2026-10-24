# 報告: マグネット（吸着）機能の説明を README とショートカット一覧に足した

## まとめ

- README.md の「できること」→「タイムライン」にある吸着の文を、今の動きに合わせて直した。Alt を押しながらドラッグすると吸い付かないことも足した。キーボードショートカットの表には「吸い付かせずに動かす | Alt+ドラッグ」を 1 行足した。
- アプリ内のショートカット一覧（`MainWindow::showShortcuts()` の rows）にも、同じ言葉の行を 1 行足した。
- 変えたのはこの 3 か所だけ（README.md の 2 か所と、mainwindow.cpp の 1 行）。timelinewidget.cpp と .h には触っていない（作業の前後で SHA-256 が同じ）。
- `build.ps1 -NoDeploy` は終了コード 0 で、警告はない。
- ダイアログを作る関数の中身だけを使い捨てのハーネスで表示した。15 行すべてが切れずに表示されることを、寸法と grab() の画像で確かめた（チェックはすべて OK）。
- コミット・push・stash・pull・fetch・配置はしていない。ユーザーのアプリは起動したまま。

## 1. 足した文と行

`git diff -- README.md native/src/ui/mainwindow.cpp`（`git diff --output` で UTF-8 のまま書き出したもの。全文）:

```diff
diff --git a/README.md b/README.md
index 48006b9..4185a49 100644
--- a/README.md
+++ b/README.md
@@ -67,7 +67,7 @@ Windows 向けの動画編集アプリです。動画や写真を並べて、テ
 
 ### できること
 
-- タイムライン: クリップをドラッグして並べ替え、端をドラッグして長さを調整、再生位置で分割、複製、削除。テキストとオーディオは好きな位置に置けて、重ねることもできます。ドラッグ中は再生位置やほかの端に吸い付きます。
+- タイムライン: クリップをドラッグして並べ替え、端をドラッグして長さを調整、再生位置で分割、複製、削除。テキストとオーディオは好きな位置に置けて、重ねることもできます。テキストやオーディオ、再生位置をドラッグすると、キリのいい秒（目盛りの数字）やほかの端に吸い付きます。Alt を押しながらドラッグすると吸い付きません。
 - クリップ: 速度（0.25〜4 倍）、音量（0〜200%）とミュート、表示方法（全体表示 / 拡大 / ぼかし背景）、フィルター 9 種類と明るさ・コントラスト・彩度・色温度の調整、フェードイン / アウト、写真の表示時間（0.5〜20 秒）。画面と縦横比が違う素材は、はじめから「ぼかし背景」になります。
 - テキスト: スタイル 8 種類（シンプル、太フチ、バラエティ、字幕、タイトル、ポップ、メモ、インパクト）、フォント 7 種類、サイズ、太字、影、文字色・縁取り・背景、位置（上部 / 中央 / 下部）と揃え、表示する時間、アニメーション（なし / フェード / スライド）。
 - オーディオ（BGM）: 音量、フェード、開始位置、「動画の長さに合わせる」。タイムラインには波形が出ます。
@@ -99,6 +99,7 @@ Windows 向けの動画編集アプリです。動画や写真を並べて、テ
 | 先頭 / 末尾へ移動 | Home / End |
 | タイムラインの拡大・縮小 | Ctrl+ホイール、Ctrl+= / Ctrl+- |
 | タイムラインの全体を表示 | Ctrl+0 |
+| 吸い付かせずに動かす | Alt+ドラッグ |
 | メディアを追加 / 書き出し | Ctrl+I / Ctrl+E |
 | 新規 / 開く / 保存 / 名前を付けて保存 | Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+Shift+S |
 
diff --git a/native/src/ui/mainwindow.cpp b/native/src/ui/mainwindow.cpp
index 8c8fc7d..de58ef3 100644
--- a/native/src/ui/mainwindow.cpp
+++ b/native/src/ui/mainwindow.cpp
@@ -330,6 +330,7 @@ void MainWindow::showShortcuts() {
       {QStringLiteral("1秒移動"), QStringLiteral("Shift+← / →")},
       {QStringLiteral("先頭 / 末尾へ移動"), QStringLiteral("Home / End")},
       {QStringLiteral("タイムラインの拡大・縮小"), QStringLiteral("Ctrl+ホイール")},
+      {QStringLiteral("吸い付かせずに動かす"), QStringLiteral("Alt+ドラッグ")},
       {QStringLiteral("メディアを追加"), QStringLiteral("Ctrl+I")},
       {QStringLiteral("書き出し"), QStringLiteral("Ctrl+E")},
       {QStringLiteral("新規 / 開く / 保存"), QStringLiteral("Ctrl+N / Ctrl+O / Ctrl+S")},
```

言葉と置き場所の決め方:
- 動詞は、README の元の文にある「吸い付く」にそろえた（「吸着」は使っていない）。表の行は、README とアプリで同じ「吸い付かせずに動かす」にした。
- キーは、既存の「Ctrl+ホイール」と同じく + の前後に空白を入れない「Alt+ドラッグ」にした。列の順（操作 | キー）も既存のとおり。
- 置き場所は、タイムラインの行のすぐ次。README は「タイムラインの全体を表示」の次、アプリは「タイムラインの拡大・縮小」の次にした。アプリの一覧には Ctrl+0 の行がないので、そこがタイムラインの最後の行になる。
- README の文は、吸い付くもの（テキスト・オーディオ・再生位置）、吸い付く先（キリのいい秒＝目盛りの数字、ほかの端）、Alt の 3 つに絞った。元の文にあった「再生位置に吸い付く」（テキストやオーディオをつかんだときの再生位置）は、文を短くするために省いた。残したいときは「（テキストとオーディオは再生位置にも）」を足せる。

## 2. ビルド

コマンド（出力はリポジトリの外のログに書き、最後に終了コードを足した）:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File 'c:\git\JPHACK\e-editor\native\build.ps1' -NoDeploy *>&1 | Out-File -Encoding utf8 C:\kantan-dev\tmp\kv-docs\build-1.log; "BUILD-EXIT=$LASTEXITCODE" | Out-File -Append -Encoding utf8 C:\kantan-dev\tmp\kv-docs\build-1.log
```

ログ（全文）:

```
[1/4] Automatic MOC for target kantan-video-editor
[2/3] Building CXX object CMakeFiles/kantan-video-editor.dir/src/ui/mainwindow.cpp.obj
[3/3] Linking CXX executable kantan-video-editor.exe
BUILD-EXIT=0
```

- 終了コード 0。`warning` も `error` もない（`-Wall -Wno-unused-parameter`）。コンパイルし直したのは mainwindow.cpp だけ。
- 新しくなったのは `native\build\kantan-video-editor.exe`（2026-10-22 15:29:43）だけ。配置していないので、`native\dist\kantan-video-editor\kantan-video-editor.exe` は 14:43:29 のまま。

## 3. ショートカット一覧

### 3.1 開き方（コードで確認）

- メニューの「ヘルプ(H)」→「キーボードショートカット」（mainwindow.cpp の `buildMenus()`）。README の「アプリの『ヘルプ』→『キーボードショートカット』でも確認できます」と同じ。
- 「?」キーでも開く（`handleKey()` の `Qt::Key_Question`）。文字の入力欄にフォーカスがあるとき、ほかのダイアログやメニューが開いているとき、Ctrl か Alt を押しているときは効かない（Shift は効く）。

### 3.2 行が増えると見切れる作りか（コードで確認）

- ダイアログは `setMinimumWidth(420)` だけで、高さは固定していない（setFixedSize・setMaximumHeight はない）。中は QVBoxLayout。
- 表の高さは `setMinimumHeight(int(rows.size()) * 30 + 4)` で、行の数に合わせて増える（14 行で 424px、15 行で 454px）。ダイアログの高さもそれに合わせて増える。
- 行の高さは表の既定のまま（行ごとに変えていない）。列は、操作の列が Stretch、キーの列が ResizeToContents。
- そのため、1 行足すとダイアログが 30px 高くなり、行の数で見切れることはない。ただし 1 行 30px と決め打ちしているので、行の高さが 30px を超える環境では見切れる（4 章）。

### 3.3 ハーネスでの表示の確認

作り:
- 場所は `C:\kantan-dev\tmp\kv-docs\harness\`（リポジトリの外。終わったあと消した）。
- `extract.ps1` が、`MainWindow::showShortcuts()` の中身を HEAD（315〜362 行）と作業ツリー（315〜363 行）からそのまま写して、.inc ファイルにする。変えたのは 1 行目のクラス名だけ（QMainWindow を継いだ空のクラスにした）。ハーネスはこの関数を呼び、`dlg.exec()` の中で 0.8 秒後に寸法を測り、`grab()` の画像を保存してからダイアログを閉じる。
- MainWindow は作っていない（プレビュー・メディアの管理・自動保存のタイマーも作られない）。アプリ名は別の名前にし、QStandardPaths をテストモードにした。終わったあと、`%LOCALAPPDATA%` に `qttest` や `kv-docs-harness` のフォルダーはできていなかった。自動保存のファイルの時刻は 15:23:08 で、作業を始める前のまま。
- アプリの main.cpp と同じく、`HighDpiScaleFactorRoundingPolicy::PassThrough` にしてから `theme::apply` を呼んだ（リポジトリの theme.cpp をそのままコンパイル）。翻訳ファイルは読んでいない（Qt の標準の文言にしか効かず、このダイアログの文字はすべてアプリが書いたもの）。
- ハーネスのビルドも警告なし。結果は `RESULT: ALL OK`、終了コード 0。

結果（画面 1536x864、使える範囲 1536x816、DPR 1.25）:

| | 前（HEAD） | 後（今回） |
| --- | --- | --- |
| 行の数 | 14 | 15 |
| ダイアログ（枠を含む） | 420x482（420x512） | 420x512（420x542） |
| 表の高さ / 行の高さの合計 | 424 / 420 | 454 / 450 |
| 1 行の高さ | すべて 30 | すべて 30 |
| スクロールバー | なし | なし |

- 足した行は 0 から数えて 11 行目で、「タイムラインの拡大・縮小」（10 行目）のすぐ次。y=330〜360 で、表の中に収まっている。最後の行（新規 / 開く / 保存）も y=420〜450 で、表の高さ 454 の中。
- 文字の幅: 「吸い付かせずに動かす」は 118px で列の幅 256px の中、「Alt+ドラッグ」は 72px で列の幅 142px の中。全部のセルで、必要な幅が列の幅以下（「…」で省略されない）。高さも全部のセルで 25px（行は 30px）。
- 足した行のキーのセルは、ほかのキーのセルと同じく右寄せの灰色。
- ダイアログの幅は 420 のままで、高さが 30px 増えた。画面に収まっている（542 ≤ 816）。

画像（`grab()`。実ピクセルで 525x640）を開いて見た結果:
- 15 行が上から並び、「タイムラインの拡大・縮小 / Ctrl+ホイール」と「メディアを追加 / Ctrl+I」の間に「吸い付かせずに動かす / Alt+ドラッグ」がある。キーはほかと同じく右寄せの灰色。
- 最後の「新規 / 開く / 保存」「Ctrl+N / Ctrl+O / Ctrl+S」も、その下の線まで見えていて、その下に「閉じる」ボタンがある。スクロールバーも「…」もない。
- 前（HEAD）の画像（525x603）は 14 行で、足した行がないこと以外は同じ見た目。

ハーネスのログ（`result.log` のうち、後（今回）の部分と最後の比較。前（HEAD）の部分も同じチェックがすべて OK）:

```
OK   after: the dialog is shown
OK   after: the dialog has the table
note: after: title キーボードショートカット, visible 1
note: after: screen \\.\DISPLAY1 geometry 1536x864 available 1536x816 dpr 1.25 logicalDpi 96
note: after: dialog 420x512 frame 420x542 minimum 420x512 maximum 16777215x16777215 sizeHint 278x512
note: after: table 398x454 minimum 0x454 maximum 16777215x16777215 viewport 398x454 frameWidth 0 columns 256+142 font Yu Gothic UI 9pt height 16
OK   after: the table has 15 rows (actual 15)
note: after: row 0 y=0 h=30 | 再生 / 一時停止 need 98x25 of 256 | Space need 46x25 of 142
note: after: row 1 y=30 h=30 | 分割 need 38x25 of 256 | S need 21x25 of 142
note: after: row 2 y=60 h=30 | 削除 need 38x25 of 256 | Delete need 49x25 of 142
note: after: row 3 y=90 h=30 | 複製 need 38x25 of 256 | Ctrl+D need 50x25 of 142
note: after: row 4 y=120 h=30 | 元に戻す need 58x25 of 256 | Ctrl+Z need 48x25 of 142
note: after: row 5 y=150 h=30 | やり直し need 53x25 of 256 | Ctrl+Y need 48x25 of 142
note: after: row 6 y=180 h=30 | テキストを追加 need 83x25 of 256 | T need 21x25 of 142
note: after: row 7 y=210 h=30 | 1フレーム移動 need 79x25 of 256 | ← / → need 50x25 of 142
note: after: row 8 y=240 h=30 | 1秒移動 need 57x25 of 256 | Shift+← / → need 82x25 of 142
note: after: row 9 y=270 h=30 | 先頭 / 末尾へ移動 need 108x25 of 256 | Home / End need 78x25 of 142
note: after: row 10 y=300 h=30 | タイムラインの拡大・縮小 need 131x25 of 256 | Ctrl+ホイール need 77x25 of 142
note: after: row 11 y=330 h=30 | 吸い付かせずに動かす need 118x25 of 256 | Alt+ドラッグ need 72x25 of 142
note: after: row 12 y=360 h=30 | メディアを追加 need 82x25 of 256 | Ctrl+I need 44x25 of 142
note: after: row 13 y=390 h=30 | 書き出し need 56x25 of 256 | Ctrl+E need 47x25 of 142
note: after: row 14 y=420 h=30 | 新規 / 開く / 保存 need 104x25 of 256 | Ctrl+N / Ctrl+O / Ctrl+S need 142x25 of 142
note: after: rows total height 450, viewport height 454
OK   after: every row lies fully inside the visible area of the table
OK   after: the rows' total height fits the viewport
OK   after: no vertical scroll bar and nothing to scroll
OK   after: no horizontal scroll bar and nothing to scroll
OK   after: every text fits the width of its column (nothing is elided)
OK   after: every text fits the height of its row
OK   after: neither the dialog nor the table has a fixed or capped height
OK   after: the whole dialog (with its frame) fits the screen's available area
OK   after: the new row comes right after the zoom row (zoom 10, new 11)
OK   after: the new row's key is Alt+ドラッグ
OK   after: the new key cell is drawn like the other key cells (right-aligned, muted color)
OK   after: saved C:\kantan-dev\tmp\kv-docs\harness\out/shortcuts-after.png (525x640 px)
note: dialog before 420x482, after 420x512, row height 30
OK   the dialog keeps its width
OK   the dialog grows by the 30 px that showShortcuts reserves per row
RESULT: ALL OK
```

## 4. 確かめられなかったこと

- 実際のアプリでショートカット一覧を開くこと。ユーザーのアプリが起動していて、配置もしない約束なので試していない。今起動しているアプリ（`native\dist` の 14:43:29 の exe）には、この行はまだ入っていない。アプリを閉じて `build.cmd` でビルドし直すと出る。
- MainWindow を親にしたときのダイアログ。ハーネスは、関数の中身をそのまま写して、QMainWindow を継いだ空のクラスで動かした。大きさと中身は、この関数とアプリ全体のテーマ（`theme::apply`）だけで決まるので同じになるはず（コードで確認）。ダイアログが出る位置は見ていない。
- ほかの画面（DPR 1.25 以外）や、Windows の文字を大きくする設定。表の文字は Windows の一覧用のフォント（この PC では Yu Gothic UI 9pt）で、行の高さはここでは 30px だった。コードは 1 行 30px と決め打ちで場所をとっているので、文字が大きくて行が 30px を超える環境では、最後の行が見切れるおそれがある（試していない）。これは前からの作りで（14 行のときも同じ）、今回は直していない。この環境では、15 行で 450px / 454px（余り 4px）だった。
- README の GitHub での見た目。表の書き方は前後の行と同じ。
- 吸着の動きそのものは、今回は試していない。README の文は、`.agents/tasks/timeline-magnet/verification.md` の 10 章と review.md の内容に合わせて書いた。

## 5. 最後の状態と一時ファイル

- `git status --porcelain`（この報告を書く前）:
  ```
   M README.md
   M native/src/ui/mainwindow.cpp
   M native/src/ui/timelinewidget.cpp
   M native/src/ui/timelinewidget.h
  ?? .agents/tasks/timeline-magnet/plan.md
  ?? .agents/tasks/timeline-magnet/proposal.md
  ?? .agents/tasks/timeline-magnet/review.json
  ?? .agents/tasks/timeline-magnet/review.md
  ?? .agents/tasks/timeline-magnet/verification.md
  ```
  このほかに、この報告（`.agents/tasks/timeline-magnet-docs/report.md`）が増えた。
- ブランチは `feat/ikumu1`、HEAD は `eaac4b6` のまま。stash はなし。コミット・push・stash・pull・fetch・merge はしていない。
- `git diff --stat`: README.md が `3 +-`、mainwindow.cpp が `1 +`。timelinewidget.cpp と .h は前の作業のまま（106 行と 7 行。合計から今回の分を引くと、前回の 86 行追加・27 行削除と同じ）。`git diff --check` は出力なし・終了コード 0。
- timelinewidget.cpp と .h の SHA-256 は、作業の前後で同じ（`EA70B2EE…`、`A501EED2…`）。
- README.md と mainwindow.cpp は、どちらも編集の前後で UTF-8・BOM なし・CRLF（CRLF の行が 252→253、499→500。LF だけの行は 0）。
- ユーザーのアプリ（PID 30500、15:21:56 起動）は起動したまま。
- 一時フォルダー `C:\kantan-dev\tmp\kv-docs\`（ビルドとハーネスのログ、ハーネス、画像）は、この報告を書いたあとに消した。
