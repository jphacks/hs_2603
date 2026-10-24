# 実装計画: 赤い再生ヘッドを見出しの列の上まで動かして見えるようにする

ユーザーの要望（原文）: 「一番左のテキストとか３つ書いてあるのは消さなくていいのでそれを貫通して一番左まで赤い線で動画を見れるようにしてほしいです」「一番左が0:00になるわけではないですけど自分で右に動かしたとき左をフルで見れるようにしてほしいです」「半透明にはしなくていい」「それ以外の作業はしないで」「言語はC＋＋で書いて」

変更するのは `native/src/ui/timelinewidget.cpp` の 1 ファイルだけ（C++、Qt 6.8.3）。`timelinewidget.h` は変えない。`c:\git\JPHACK\e-editor` で直接作業する。コミット・push・stash はしない。ハーネスはリポジトリの外に作り、最後に消す。

## 1. 調べてわかったこと

### 1.1 行番号（現在のファイルで確認。全 936 行、UTF-8 BOM なし、作業ツリーは CRLF・インデックスは LF）
オーケストレーターの行番号はいくつか 1 行ずれていた。正しくは次のとおり。
- 24 `static constexpr int kHeadW = 108;`、25 `static constexpr int kRulerH = 26;`（26 行目ではない）
- 118-120 `xOf`、122-124 `tOf`（式は依頼文のとおり）
- 153-159 `followPlayhead`（再生中だけ自動スクロール）
- 190 `hitTest` の `if (p.x() < kHeadW || h.row == Row::Ruler || h.row == Row::None) return h;`
- 296 `paintRuler` の始まり。297 `const Editor& ed = editor();`、300 `const double total = ed.layout().total;`（`ed` はつまみ以外でも使っている）。313-315 で painter の文字を 8pt にする。319 `if (x < kHeadW) continue;`
- 328-342 `paintRuler` の「// 再生ヘッドのつまみ」: `if (px >= kHeadW - 8 && px <= area.width() + 8) { ... }`。343 が `paintRuler` の閉じ括弧
- 571 `paintEvent` の始まり。598-606「// 終わりの位置」
- 607-609「// 再生ヘッド」: `if (px >= kHeadW - 1) p.fillRect(QRectF(px - 1, kRulerH, 2, area.height() - kRulerH), theme::playhead);`
- 611-625「// 見出し（左に固定）」: 赤い線のあとに描くので線を覆う。614-616 で文字を 8.5pt にする
- 627-633「// 目盛り（上に固定）」: `paintRuler` のあと左上の角を panel で塗り、合計時間を描く。この角がつまみを覆う。634 が `paintEvent` の閉じ括弧
- 638-671 `mousePressEvent`。646 `if (pos.x() < kHeadW) return;`、647 `ed.pause();`、649-654 目盛りか空いている場所なら `ed.seek(tOf(pos.x())); drag_ = Drag::Scrub;`
- 673-774 `mouseMoveEvent`。688-690 `case Drag::Scrub: ed.seek(tOf(pos.x()));`
- 776-789 `mouseReleaseEvent`、791-797 `mouseDoubleClickEvent`（`x > kHeadW` のときだけテキストを足す）、799 `wheelEvent`

### 1.2 再生ヘッドのドラッグの仕組み
- 赤い線そのものをつかむ仕組みはない。ドラッグ（`Drag::Scrub`）が始まるのは、x >= kHeadW で目盛りの行を押したとき（つまみを押したときもこれ）と、トラックの行の何もない場所を押したとき（このときは選択も外す）だけ。トラックの行で赤い線の上がクリップなら、クリップの操作になる。
- ドラッグ中（688-690）は x を制限していない。`Editor::seek`（editor.cpp:155-160）が `std::clamp(t, 0.0, std::max(0.0, contentEnd()))` で 0〜内容の終わりにそろえる。上限の制限はこれだけ。
- そのため、今のコードでも右にスクロールして目盛りからドラッグし、マウスを見出しの上（x < kHeadW）に入れると、時間は tOf(x)（0 未満は 0）になり、プレビューもその時間を出す（previewengine.cpp:32 が `timeChanged` を受ける）。見えないのは描き方のせい:
  - 線は `px >= kHeadW - 1` のときだけ描き（609）、そのあと見出し（612）で塗りつぶされる。
  - つまみは `px >= kHeadW - 8` のときだけ描き（330）、そのあと左上の角（629）で塗りつぶされる。
- 見出しの列の上（x < kHeadW）を押すと、行に関係なく 646 で何もせずに戻る。つまみが左上の角に入ると、つかみ直すことができない。
- 左上の角の合計時間は 633 で描いている。そのとき painter の文字は `paintRuler` が設定した 8pt のまま（614-616 の 8.5pt ではない）。

### 1.3 リポジトリと環境の状態（依頼文と違う点がある）
- ブランチ `feat/ikumu1`。HEAD は 1f7d805 で `origin/feat/ikumu1` と同じ。調べたときの `git status --porcelain` は空だった。
- 依頼文にある widgets.h・widgets.cpp・inspector.cpp の変更と `.agents/tasks/inspector-wheel/` は、1f7d805 ですでにコミットされている（作者 tanu196、2026-10-21 13:39）。`.agents/` は追跡されているので、作業が終わったときの `git status` は ` M native/src/ui/timelinewidget.cpp` と `?? .agents/tasks/timeline-playhead-left/` だけになるはず。ほかのファイルには触らない。
- ユーザーのアプリが起動している（PID 24972、`native\dist\kantan-video-editor\kantan-video-editor.exe`、13:38:17 から）。終了させない。配置もしない（`-NoDeploy` だけ）。`-NoDeploy` は `native\build\kantan-video-editor.exe` だけを作るので、起動中のアプリとはぶつからない。
- ツール: `C:\kantan-dev`（Qt 6.8.3 mingw_64、MinGW 13.1、FFmpeg、CMake、Ninja）。`Qt6TestConfig.cmake` と `Qt6Test.dll` がある。`native\build` は構成済み（`build.ninja` と `kantan-configure.txt` がある）なので、ビルドは差分だけ。アプリは `-Wall -Wno-unused-parameter` でビルドする。
- テストの仕組みはリポジトリにない（README の「開発の始め方」にもない）。前回と同じく、リポジトリの外に一時的なハーネスを作る。

## 2. 決めたこと
- D1 描く順番: 再生ヘッド（トラック部分の線とつまみ）を `paintEvent` の一番最後、左上の角と合計時間を描いたあとに描く。つまみのブロック（328-342）は `paintRuler` から `paintEvent` の最後へ移し、線（607-609）もその直前へ移す。これで線は見出しの列の上に、つまみは左上の角と合計時間の上に出る。見出しを先に描くので、見出しの下のクリップ・テキスト・オーディオ・目盛り・終わりの破線・挿入位置は今までどおり隠れる。見出しは不透明のまま。
  - ほかの案はとらない。`paintRuler` を角のあとに呼ぶと、合計時間の文字が 8pt から 8.5pt に変わってしまう（1.2 の最後）。角の塗りを `paintRuler` に移す案でも動くが、左に固定の角が目盛りの関数に混ざる。最後に描く方法なら文字・ペン・アンチエイリアスの状態を変えずに済む（つまみは `fillPath` と `fillRect` だけを使い、今と同じく AA をオンにしてからオフに戻す）。
- D2 描く条件: 線は `px >= kHeadW - 1` を `px >= -1` に、つまみは `px >= kHeadW - 8` を `px >= -8` に変える。左側のゆとり（線 1px、つまみ 8px）は今と同じで、基準を見出しの右端から画面の左端に変えるだけ。画面の左より外なら描かない。右側の条件は今のまま（線には右側の条件がなく、はみ出した分は painter が切る）。
- D3 見出しの上までのドラッグ: `mouseMoveEvent` は変えない。1.2 のとおり、今と同じ方法で始めたドラッグは x < kHeadW でも `ed.seek(tOf(pos.x()))` になり、`Editor::seek` が 0 で止める。上限も今のまま。スクロール 0 のときは tOf(x < kHeadW) が負になるので 0:00（x = kHeadW、見出しのすぐ右）で止まる。
- D4 左上の角のクリック: 646 を `if (pos.x() < kHeadW && pos.y() >= kRulerH) return;` に変える。角（y < kRulerH）は `hitTest` の `Row::Ruler` になるので、目盛りを押したときと同じ処理（`ed.pause()`、`ed.seek(tOf(pos.x()))`、`drag_ = Drag::Scrub`）に入る。0 未満は `Editor::seek` が 0 にする。トラックの行（y >= kRulerH）の見出しは今までどおり無視する。新しくドラッグを始められる場所はこの角だけ。
- D5 変えないもの: `kHeadW`・`kRulerH`・`xOf`・`tOf`・`hitTest`・`mouseMoveEvent`・`mouseReleaseEvent`・`mouseDoubleClickEvent`・`wheelEvent`・`followPlayhead`・`zoomAt`・見出しと角の描き方・`timelinewidget.h`。
- D6 目に見える違いは次の 3 つで、すべて合意した作業 1 の結果。最後の報告でユーザーに伝える。
  1. 右にスクロールしたとき、見出しの下の時間でも赤い線とつまみが見出しの上に出る。線はラベルの上を通る。
  2. つまみが左上の角に入ると、合計時間の文字の上に重なる。
  3. スクロール 0 で 0:00 のとき、これまで角に隠れていたつまみの左半分も見える。線の左 1px が見出しの右の境界線（x = 107）に重なる。
  x >= 114 の位置では、画面は 1 画素も変わらない（ハーネスで確かめる）。

## 3. ビルドとハーネスの実行（出力が切れるのでログに書く）
- execute_pwsh の画面はコマンドを何度も表示して出力が切れることがある。ビルドとハーネスの出力はログファイルに書き、最後に終了コードの目印を足して、読み取りツールで読む。パスはすべて絶対パスで書き、git は `git -C c:\git\JPHACK\e-editor` で呼ぶ。
- アプリのビルド（コンパイルだけ。配置しない）:
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1 -NoDeploy 2>&1 | Out-File -Encoding utf8 C:\kantan-dev\tmp\kv-playhead\build.log; "BUILD-EXIT=$LASTEXITCODE" | Out-File -Append -Encoding utf8 C:\kantan-dev\tmp\kv-playhead\build.log
  ```
  成功の条件: ログの最後の行が `BUILD-EXIT=0` で、`error:` がなく、timelinewidget.cpp の `warning:` もない。ログの日本語は文字化けしてもかまわない。
- ハーネス（`<phase>` は `baseline` か `fixed`）:
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\kantan-dev\tmp\kv-playhead\run.ps1 2>&1 | Out-File -Encoding utf8 C:\kantan-dev\tmp\kv-playhead\run-<phase>.log; "RUN-EXIT=$LASTEXITCODE" | Out-File -Append -Encoding utf8 C:\kantan-dev\tmp\kv-playhead\run-<phase>.log
  ```
  1 回目はアプリのソースを全部コンパイルするので、タイムアウトは 1800000 ms にする。ハーネスは 900x328 の小さなウィンドウを 1〜2 秒出す。その間、マウスをそのウィンドウの上で動かさない（ドラッグ中に本物のマウスの動きが混ざるため。ウィンドウは今のカーソルの位置を避けて出す）。

## 4. 計画

- [ ] 1. ハーネスを作り、修正前のコードで基準をとる。
      `C:\kantan-dev\tmp\kv-playhead\` に付録 A の 3 ファイルを作る: `CMakeLists.txt`、`playheadcheck.cpp`（UTF-8 BOM なし）、`run.ps1`（ASCII だけ）。パスは ASCII にする（ユーザーの %TEMP% には日本語が入っていて、MinGW が動かない）。このステップではリポジトリのファイルに触らない。
      Files: C:\kantan-dev\tmp\kv-playhead\CMakeLists.txt, playheadcheck.cpp, run.ps1（一時ファイル）
      Verify: 3 章のハーネスのコマンドを `<phase>` = `baseline` で実行し、`run-baseline.log` を読む。期待する結果は、最後が `RESULT: 11 check(s) FAILED` と `RUN-EXIT=1`。
      - FAIL になる 11 行（修正前は線とつまみが見出しと角に隠れ、角のクリックは無視されるため）:
        - `e: red line is drawn over the header column at x=60 (all rows)`
        - `e: knob is drawn over the top-left corner at x=60`
        - `e: red line and knob reach the left edge (x=1)`
        - `e: scroll 0: the whole knob shows (left half over the corner)`
        - `a: the red line is drawn under the mouse over the header (x=60)`
        - `c: corner press pauses playback like a ruler press`
        - `c: corner press moves the playhead to tOf(x) (x=24)`
        - `c: dragging from the corner keeps scrubbing (x=70)`
        - `c: the drag continues into the ruler (x=250)`
        - `c: after a corner click the knob and line show at the click (x=24)`
        - `c: scroll 0: a corner press goes to 0:00, not below`
      - ほかはすべて OK になる。`setup:`、`e:` の残り（見出しが不透明、ラベル、合計時間、スクロール 0 の位置、ふつうの位置）、`a:` の時間のチェック、`b:`、`d:`、`f:`、`c: double-click in the corner adds no text`。
      - (a) は時間のチェックが修正前でも OK になる。1.2 のとおり、ドラッグはもう見出しの上まで時間を動かしているので、ハーネスの間違いではない。(a) で修正前に失敗するのは、描画のチェック（線が見えない）だけ。
      - `note: saved ...ref-e-normal.png` と `note: saved ...ref-e-hidden.png` が出る（修正前の画像を基準として保存する）。
      - `note: device pixel ratio 1.25`（このマシンの値）が出る。`setup: mouse events land at the intended viewport position` は OK になる。
      OK になるはずの行が FAIL になったり、FAIL の数が違ったりしたら、ハーネスが間違っている。アプリではなくハーネスを直し、`shots\ref-*.png` を消してから、基準が合うまで繰り返す。
      合ったら、修正前の画像を残す: `New-Item -ItemType Directory -Force C:\kantan-dev\tmp\kv-playhead\shots\baseline | Out-Null; Copy-Item C:\kantan-dev\tmp\kv-playhead\shots\*.png C:\kantan-dev\tmp\kv-playhead\shots\baseline\ -Exclude ref-*`
      `shots\baseline\e-head-60.png` と `shots\baseline\c-corner.png` を読み取りツールで開き、赤い線が x < 108 の見出しの上に出ていないこと（修正前の問題）を目で確かめる。

- [ ] 2. timelinewidget.cpp を付録 B のとおりに変える。手順 1 のあとに行う。
      `paintRuler` からつまみのブロックを消す。`paintEvent` から線の 3 行を消す。`paintEvent` の最後（合計時間の `drawText` のあと）に、線とつまみを新しい条件で描く。`mousePressEvent` の 646 行目の条件を変える。str_replace で編集し、CRLF と UTF-8（BOM なし）をそのまま保つ。コメントは日本語にする。ほかは変えない。
      Files: native/src/ui/timelinewidget.cpp
      Verify: 3 章のアプリのビルドを実行する。`build.log` の最後が `BUILD-EXIT=0` で、`error:` がなく、timelinewidget.cpp の `warning:` もない（ninja は timelinewidget.cpp だけをコンパイルしてリンクする）。

- [ ] 3. 修正後のコードでハーネスをもう一度実行する。手順 2 のあとに行う。
      Verify: 3 章のハーネスのコマンドを `<phase>` = `fixed` で実行する。`run-fixed.log` のすべてのチェックが `OK` で、`e: normal position is pixel-identical to the unmodified code` と `e: playhead off-screen: pixel-identical to the unmodified code` も OK、最後が `RESULT: ALL OK` と `RUN-EXIT=0` になる。ログの全文を最後の報告に使う。
      失敗したら、アプリのコードを直して手順 2 と 3 をやり直す。ハーネスの期待値を変えて通すことはしない（失敗の原因がハーネスにあると示せるときだけ、ハーネスを直し、手順 1 の基準からやり直す）。

- [ ] 4. 修正後の画像を目で見て確かめる。手順 3 のあとに行う。
      `C:\kantan-dev\tmp\kv-playhead\shots\` の次の PNG を読み取りツールで開く。画像は DPR 1.25 の実ピクセルで、横 1125px ぐらいある。
      - `e-head-60.png`: 赤い線が、見出しの列の上（左から 60 の位置）を、テキスト・ビデオ・オーディオの 3 行とその下まで通っている。目盛りの行の左上の角に赤いつまみがある。見出しは不透明な濃い灰色で、アイコンと「テキスト」「ビデオ」「オーディオ」が読める（線が通るところだけ赤）。見出しの下のクリップ（青）・テキスト（茶）・オーディオ（緑）は見出しから透けていない。左上の「0:12」が読める。
      - `e-head-left.png`: 線とつまみが画面の一番左端にある。
      - `c-corner.png`: つまみが左上の角の「0:12」の上に重なっていて、「0:12」は重なっていない部分が見えている。線が見出しの上を通っている。
      - `e-scroll0.png` と `b-scroll0-drag.png`: スクロール 0 では、線は見出しのすぐ右（0:00）にあり、見出しの中には入っていない。つまみは全体が見えている。
      - `e-normal.png` と、`shots\baseline\e-normal.png`: 見た目が同じ。
      見た結果を（チェックの OK と合わせて）最後の報告に書く。

- [ ] 5. 一時ファイルを消し、作業ツリーを確かめる。
      `Remove-Item -Recurse -Force -LiteralPath 'C:\kantan-dev\tmp\kv-playhead'` を実行する。手順 1 で作ったこのフォルダーだけを消す。`C:\kantan-dev\tmp` のほかのものには触らない。
      Verify:
      - `Test-Path 'C:\kantan-dev\tmp\kv-playhead'` が False。
      - `git -C c:\git\JPHACK\e-editor status --short` が ` M native/src/ui/timelinewidget.cpp` と、このワークフローの `?? .agents/tasks/timeline-playhead-left/` だけを出す。
      - `git -C c:\git\JPHACK\e-editor diff` に付録 B の変更だけがある（printf、qDebug、ハーネスのコードがない）。`git -C c:\git\JPHACK\e-editor diff --check` が何も出さない。何もコミットしない。
      - ユーザーのアプリ（kantan-video-editor.exe）は起動したまま。配置していないので `native\dist` は古いまま。

## 5. ユーザーが自分で確かめる手順（最後の報告にこのまま書く）
1. アプリを閉じてから `build.cmd` をダブルクリックします（今回は起動中のアプリを止めないよう、配置をしていないので、`native\dist` の exe はまだ古いままです）。
2. 動画を 1 本以上タイムラインに置き、タイムラインを右にスクロールします（下のスクロールバーか、ホイール）。
3. 目盛り（時間の数字がある行）をクリックし、そのまま左へドラッグします。赤い線は「テキスト」「ビデオ」「オーディオ」の見出しの上を通って、一番左まで動きます。見出しは消えず、赤い線だけが上を通ります。プレビューには赤い線の時間の映像が出ます。
4. 赤い線が見出しの上にあるとき、上のつまみは左上の角（合計時間が出ているところ）に重なって見えます。左上の角をクリックすると、赤い線がその位置に動き、そのまま左右にドラッグできます。
5. 見出しの「テキスト」「ビデオ」「オーディオ」の行をクリックしても、今までどおり何も起きません。
6. スクロールを一番左に戻すと、赤い線は今までどおり 0:00（見出しのすぐ右）で止まり、見出しの中には入りません。0:00 のつまみは、これまで左半分が角に隠れていましたが、全体が見えるようになります。
7. クリップのクリック・移動・端のドラッグ、ダブルクリックでのテキストの追加、ホイールでのスクロール、Ctrl+ホイールでの拡大縮小、再生中の自動スクロールは今までどおりです。

---

## 付録 A: 一時的なハーネス（リポジトリの外。手順 5 で消す）

作り方の要点:
- 前回（`.agents/tasks/inspector-wheel/plan.md` の付録 A）と同じく、main.cpp 以外のアプリのソースを、リポジトリから直接コンパイルする。Qt6::Test を使う。MainWindow は作らないので、ユーザーの自動保存には書き込まない。MediaManager も使わない（素材は `Editor::addAsset` で Ready の情報だけを入れる）。
- マウスは OS の入力と同じ経路（`QWindowSystemInterface`）から入れる。`QTest::mousePress/mouseMove/mouseRelease/mouseDClick` の QWindow 版を使い、ウィンドウの論理座標を渡す。
- DPR 1.25 によるずれへの対処: 前回ずれたのはホイールで、`qt_handleWheelEvent` が論理座標を実ピクセルに直さずに渡していたため。マウスの `qt_handleMouseEvent` は Qt 6.8.3 では `QHighDpi::toNativeLocalPosition` で自分で直す（qwindowsysteminterface.cpp）。そのため、ここでは DPR を掛けない。念のため、ビューポートにイベントフィルターを付けて、届いた位置を毎回、渡した位置と比べる。最初の `setup: mouse events land ...` と最後の `setup: every mouse event landed ...` でずれがないことを確かめる。ずれていたら（`note: mouse arrived at ...` が出る）、ハーネスを直す。たとえば `qt_handleMouseEvent` を直接呼び、座標を DPR で直す。
- 画像は `viewport()->grab()` でとる。DPR 1.25 の実ピクセルなので、論理座標に DPR を掛けて画素を読む。線（論理 2px）は実ピクセルでは 2.5px になるので、論理座標の左右 0.6 の範囲で赤い画素を探す。PNG は `shots\` に保存し、手順 4 で目で見る。
- 修正前の画像との比べ方: 1 回目（修正前）は `ref-*.png` を保存し、2 回目からは画素を比べる。
- 押すたびにタイムスタンプを 6 秒進め、意図しないダブルクリックが起きないようにする（`delay` は時刻を進めるだけで、実際には待たない）。
- 配置: 幅 900、viewport の高さ 270 前後。拡大率は 60 px/秒（「全体を表示」が走るので決め直す）。クリップ A 0〜6 秒、B 6〜12 秒、テキスト 0〜3 秒、オーディオ 0〜8 秒。スクロール 120 のとき、見出しの下は 0.2〜2.0 秒で、3 つともその下にある。行の位置: 目盛り 0〜26、テキスト 26〜58、ビデオ 58〜124、オーディオ 124〜170。

`C:\kantan-dev\tmp\kv-playhead\CMakeLists.txt`:
```cmake
# Temporary harness for the playhead fix. Delete this folder when done.
cmake_minimum_required(VERSION 3.21)
project(KvPlayheadCheck LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network Test)

set(KV "C:/git/JPHACK/e-editor/native")
set(FFMPEG_DIR "C:/kantan-dev/ffmpeg")
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_DIR}/include")
foreach(lib avformat avcodec avutil swscale swresample avfilter)
  target_link_libraries(ffmpeg INTERFACE "${FFMPEG_DIR}/lib/lib${lib}.dll.a")
endforeach()
add_library(miniaudio STATIC "${KV}/third_party/miniaudio/miniaudio.c")
target_include_directories(miniaudio PUBLIC "${KV}/third_party/miniaudio")
target_compile_definitions(miniaudio PRIVATE MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)

# All app sources except main.cpp, compiled straight from the repo (same as the app target)
file(GLOB_RECURSE KV_FILES CONFIGURE_DEPENDS "${KV}/src/*.cpp" "${KV}/src/*.h")
list(FILTER KV_FILES EXCLUDE REGEX "/src/main\\.cpp$")
qt_add_executable(kv-playhead playheadcheck.cpp ${KV_FILES} "${KV}/resources/app.qrc")
target_include_directories(kv-playhead PRIVATE "${KV}/src")
target_compile_definitions(kv-playhead PRIVATE UNICODE _UNICODE NOMINMAX KV_VERSION="0.1.0")
target_link_libraries(kv-playhead PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network Qt6::Test ffmpeg miniaudio ole32 shell32 uuid)
```

`C:\kantan-dev\tmp\kv-playhead\run.ps1`（ASCII だけ。子の powershell で動くので、環境変数の変更は残らない）:
```powershell
# Temporary: build and run the playhead check harness. Delete this folder when done.
$ErrorActionPreference = 'Continue'
$d = 'C:\kantan-dev'
$h = Join-Path $d 'tmp\kv-playhead'
$env:PATH = "$d\Qt\Tools\mingw1310_64\bin;$d\Qt\6.8.3\mingw_64\bin;$d\ffmpeg\bin;$env:PATH"
$env:TEMP = $env:TMP = Join-Path $d 'tmp'
$env:QT_PLUGIN_PATH = "$d\Qt\6.8.3\mingw_64\plugins"
$cmake = "$d\cmake\bin\cmake.exe"
if (-not (Test-Path "$h\build\build.ninja")) {
  & $cmake -S $h -B "$h\build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$d\bin\ninja.exe" -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_PREFIX_PATH=$d\Qt\6.8.3\mingw_64" "-DCMAKE_C_COMPILER=$d\Qt\Tools\mingw1310_64\bin\gcc.exe" `
    "-DCMAKE_CXX_COMPILER=$d\Qt\Tools\mingw1310_64\bin\g++.exe"
  if ($LASTEXITCODE -ne 0) { Write-Host "configure failed ($LASTEXITCODE)"; exit 3 }
}
& $cmake --build "$h\build"
if ($LASTEXITCODE -ne 0) { Write-Host "build failed ($LASTEXITCODE)"; exit 3 }
& "$h\build\kv-playhead.exe"
exit $LASTEXITCODE
```

`C:\kantan-dev\tmp\kv-playhead\playheadcheck.cpp`（UTF-8 BOM なし。表示する文字は ASCII だけ）:
```cpp
// 一時的な確認用（再生ヘッドを見出しの列の上まで動かせるか）。確認が終わったらフォルダーごと消す
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QScrollBar>
#include <QTest>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/editor.h"
#include "core/timeline.h"
#include "ui/theme.h"
#include "ui/timelinewidget.h"

// timelinewidget.cpp:24-28 と同じ値。行の位置はテキスト 1 段・オーディオ 1 段、縦スクロール 0 のとき
static constexpr int kHeadW = 108;
static constexpr int kRulerH = 26;
static constexpr int kTextMid = 42;    // テキストの行 26..58
static constexpr int kVideoMid = 91;   // ビデオの行 58..124
static constexpr int kAudioMid = 147;  // オーディオの行 124..170
static constexpr int kTracksEnd = 170;
static constexpr double kPps = 60;
static const char* const kShots = "C:/kantan-dev/tmp/kv-playhead/shots";

static int failures = 0;
static int mismatches = 0;

static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "OK  " : "FAIL", what);
  std::fflush(stdout);
  if (!ok) ++failures;
}

// ビューポートに届いたマウスの位置を覚えておく（座標のずれを見つけるため）
class Recorder : public QObject {
 public:
  QPointF last{-9999, -9999};

 protected:
  bool eventFilter(QObject*, QEvent* e) override {
    const QEvent::Type t = e->type();
    if (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonRelease || t == QEvent::MouseButtonDblClick || t == QEvent::MouseMove)
      last = static_cast<QMouseEvent*>(e)->position();
    return false;
  }
};

static kv::TimelineView* gView = nullptr;
static QWindow* gWin = nullptr;
static Recorder* gRec = nullptr;

// ---- マウス（OS から来るのと同じ QWindowSystemInterface の経路） ----
// qt_handleMouseEvent は論理座標を自分で実ピクセルに直すので、DPR は掛けない（ホイールとは違う）
static QPoint toWin(QPoint vp) {
  return gView->viewport()->mapTo(gView->window(), vp);
}
static void expectAt(QPoint vp) {
  if (std::abs(gRec->last.x() - vp.x()) > 0.01 || std::abs(gRec->last.y() - vp.y()) > 0.01) {
    ++mismatches;
    std::printf("note: mouse arrived at (%.2f, %.2f), intended (%d, %d)\n", gRec->last.x(), gRec->last.y(), vp.x(), vp.y());
  }
  gRec->last = QPointF(-9999, -9999);
}
static void press(QPoint vp) {
  QTest::mousePress(gWin, Qt::LeftButton, Qt::KeyboardModifiers(), toWin(vp), 6000);  // 時刻を 6 秒進めてダブルクリックにしない
  expectAt(vp);
}
static void moveTo(QPoint vp) {
  QTest::mouseMove(gWin, toWin(vp));
  expectAt(vp);
}
static void release(QPoint vp) {
  QTest::mouseRelease(gWin, Qt::LeftButton, Qt::KeyboardModifiers(), toWin(vp));
  expectAt(vp);
}
static void click(QPoint vp) {
  press(vp);
  release(vp);
}
static void dclick(QPoint vp) {
  QTest::mouseDClick(gWin, Qt::LeftButton, Qt::KeyboardModifiers(), toWin(vp), 6000);
  expectAt(vp);
}

// ---- 時間とスクロール ----
static int scrollX() {
  return gView->horizontalScrollBar()->value();
}
static void setScroll(int v) {
  gView->horizontalScrollBar()->setValue(v);
  QApplication::processEvents();
}
// TimelineView::tOf と同じ式
static double tAt(double x) {
  return (x - kHeadW + scrollX()) / kPps;
}
static double now() {
  return kv::editor().currentTime();
}
static bool same(double a, double b) {
  return std::abs(a - b) < 1e-6;
}

// ---- 画像（grab は DPR 倍の実ピクセル） ----
struct Shot {
  QImage img;
  qreal dpr = 1;
};
static Shot grabView(const char* name) {
  QApplication::processEvents();
  const QPixmap pm = gView->viewport()->grab();
  Shot s{pm.toImage().convertToFormat(QImage::Format_RGB32), pm.devicePixelRatio()};
  s.img.save(QStringLiteral("%1/%2.png").arg(QLatin1String(kShots), QLatin1String(name)));
  return s;
}
static bool isRed(const QColor& c) {  // theme::playhead = #ff4057
  return c.red() >= 200 && c.green() <= 120 && c.blue() <= 140;
}
static bool isPanel(const QColor& c) {  // theme::panel = #1b1b1b
  return std::abs(c.red() - 0x1b) <= 2 && std::abs(c.green() - 0x1b) <= 2 && std::abs(c.blue() - 0x1b) <= 2;
}
static bool isInk(const QColor& c) {  // アイコンと文字（#9b9b9b / #6b6b6b）
  return !isRed(c) && qGray(c.rgb()) >= 0x45;
}
static QColor at(const Shot& s, double x, double y) {
  return s.img.pixelColor(std::clamp(int(std::floor(x * s.dpr)), 0, s.img.width() - 1),
                          std::clamp(int(std::floor(y * s.dpr)), 0, s.img.height() - 1));
}
// 論理座標 (x, y) の左右 0.6 の範囲に赤い画素があるか
static bool redNear(const Shot& s, double x, double y) {
  const int py = std::clamp(int(std::floor(y * s.dpr)), 0, s.img.height() - 1);
  const int a = std::max(0, int(std::floor((x - 0.6) * s.dpr)));
  const int b = std::min(s.img.width() - 1, int(std::floor((x + 0.6) * s.dpr)));
  for (int px = a; px <= b; ++px) {
    if (isRed(s.img.pixelColor(px, py))) return true;
  }
  return false;
}
static bool redRows(const Shot& s, double x) {
  return redNear(s, x, kTextMid) && redNear(s, x, kVideoMid) && redNear(s, x, kAudioMid);
}
// 論理座標の範囲 [x0, x1) x [y0, y1) で pred に合う画素を数える。論理座標 [skip0, skip1) の列は数えない
static int countPx(const Shot& s, double x0, double x1, double y0, double y1, bool (*pred)(const QColor&), double skip0 = 0,
                   double skip1 = 0) {
  int n = 0;
  const int py1 = std::min(s.img.height(), int(std::ceil(y1 * s.dpr)));
  const int px1 = std::min(s.img.width(), int(std::ceil(x1 * s.dpr)));
  for (int py = std::max(0, int(std::floor(y0 * s.dpr))); py < py1; ++py) {
    for (int px = std::max(0, int(std::floor(x0 * s.dpr))); px < px1; ++px) {
      const double lx = (px + 0.5) / s.dpr;
      if (lx >= skip0 && lx < skip1) continue;
      if (pred(s.img.pixelColor(px, py))) ++n;
    }
  }
  return n;
}
// 1 回目（修正前のコード）の画像を基準として保存し、2 回目からは画素を比べる
static void compareRef(const Shot& s, const char* name, const char* what) {
  const QString path = QStringLiteral("%1/ref-%2.png").arg(QLatin1String(kShots), QLatin1String(name));
  if (!QFile::exists(path)) {
    s.img.save(path);
    std::printf("note: saved %s (the first run must be on the unmodified code)\n", qPrintable(path));
    return;
  }
  const QImage ref = QImage(path).convertToFormat(QImage::Format_RGB32);
  int diff = ref.size() == s.img.size() ? 0 : -1;
  for (int y = 0; diff >= 0 && y < ref.height(); ++y) {
    for (int x = 0; x < ref.width(); ++x) {
      if ((ref.pixel(x, y) & 0xffffff) != (s.img.pixel(x, y) & 0xffffff)) ++diff;
    }
  }
  if (diff != 0) std::printf("note: %s differs from the reference (%d pixels, -1 = size)\n", name, diff);
  check(diff == 0, what);
}

static kv::Asset makeAsset(const char* id, const char* name, kv::AssetKind kind, double duration) {
  kv::Asset a;
  a.id = QString::fromLatin1(id);
  a.name = QString::fromLatin1(name);
  a.kind = kind;
  a.duration = duration;
  if (kind == kv::AssetKind::Video) {
    a.width = 1920;
    a.height = 1080;
  }
  a.hasAudio = true;
  a.status = kv::AssetStatus::Ready;
  return a;
}

int main(int argc, char** argv) {
  QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  QApplication app(argc, argv);
  kv::theme::apply(app);
  QDir().mkpath(QLatin1String(kShots));

  kv::TimelineWidget tw;
  tw.resize(900, 328);
  // 本物のカーソルの下には出さない（ドラッグ中に本物のマウスの動きが混ざらないように）
  const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
  QPoint topLeft(avail.left() + 40, avail.top() + 40);
  if (QRect(topLeft, tw.size()).adjusted(-40, -40, 40, 40).contains(QCursor::pos())) topLeft.setY(avail.bottom() - tw.height() - 80);
  tw.move(topLeft);
  tw.show();
  if (!QTest::qWaitForWindowExposed(&tw)) std::printf("note: window was not exposed\n");
  gView = tw.view();
  gWin = tw.windowHandle();
  Recorder recorder;
  gRec = &recorder;
  gView->viewport()->installEventFilter(&recorder);

  kv::Editor& ed = kv::editor();
  ed.addAsset(makeAsset("va", "clip-a.mp4", kv::AssetKind::Video, 6));
  ed.addAsset(makeAsset("vb", "clip-b.mp4", kv::AssetKind::Video, 6));
  ed.addAsset(makeAsset("au", "bgm.mp3", kv::AssetKind::Audio, 8));
  ed.addToTimeline({QStringLiteral("va"), QStringLiteral("vb")});  // 0..6 秒と 6..12 秒
  ed.addToTimeline({QStringLiteral("au")});                         // 0..8 秒
  ed.addText(QStringLiteral("basic"), 0.0);                          // 0..3 秒
  ed.select({});
  ed.setPxPerSec(kPps);  // 最初のクリップで「全体を表示」が走るので、拡大率を決め直す
  setScroll(0);
  ed.seek(0);
  QApplication::processEvents();
  const QString clipA = ed.project().clips.at(0).id;
  const QString clipB = ed.project().clips.at(1).id;
  const int viewH = gView->viewport()->height();
  std::printf("note: device pixel ratio %.2f, viewport %dx%d\n", gWin->devicePixelRatio(), gView->viewport()->width(), viewH);
  check(ed.layout().total == 12 && ed.contentEnd() == 12 && ed.project().texts.size() == 1 && ed.project().audios.size() == 1 &&
            ed.pxPerSec() == kPps,
        "setup: 2 clips (12 s), 1 text, 1 audio at 60 px/s");
  check(gView->horizontalScrollBar()->maximum() >= 300 && gView->verticalScrollBar()->value() == 0 && viewH >= kTracksEnd + 40,
        "setup: timeline can scroll to 300 and shows all rows");
  click({20, 160});  // 見出しのオーディオの行の何もないところ（修正の前も後も何も起きない）
  check(mismatches == 0, "setup: mouse events land at the intended viewport position");

  // ---- (e) 見た目（時間とスクロールは直接決める） ----
  setScroll(120);
  ed.seek(tAt(60));
  {
    const Shot s = grabView("e-head-60");
    check(redRows(s, 60) && redNear(s, 60, viewH - 3), "e: red line is drawn over the header column at x=60 (all rows)");
    check(redNear(s, 60, 4) && redNear(s, 60, 10) && redNear(s, 60, 15) && redNear(s, 60, 21), "e: knob is drawn over the top-left corner at x=60");
    bool opaque = true;
    for (int y : {kTextMid, kVideoMid, kAudioMid}) {
      for (double x : {4.0, 96.0, 103.0}) opaque = opaque && isPanel(at(s, x, y));
    }
    check(opaque, "e: header stays opaque (clip, text and audio under it stay hidden)");
    const bool labels = countPx(s, 10, 100, 28, 56, isInk, 57, 63) >= 20 && countPx(s, 10, 100, 60, 122, isInk, 57, 63) >= 20 &&
                        countPx(s, 10, 100, 126, 168, isInk, 57, 63) >= 20;
    check(labels, "e: header icons and labels are visible");
    check(countPx(s, 8, 48, 3, 23, isInk) >= 10, "e: total time in the top-left corner is visible");
    check(countPx(s, 0, 106, kRulerH, kTracksEnd, isRed, 58, 62) == 0, "e: nothing else in the header column is red");
  }
  ed.seek(tAt(1));
  {
    const Shot s = grabView("e-head-left");
    check(redRows(s, 1) && redNear(s, 1, 4), "e: red line and knob reach the left edge (x=1)");
  }
  setScroll(0);
  ed.seek(0);
  {
    const Shot s = grabView("e-scroll0");
    check(redNear(s, 108, kVideoMid) && redNear(s, 108, 4), "e: scroll 0, time 0:00: line stays just right of the header (x=108)");
    check(countPx(s, 0, 106, kRulerH, kTracksEnd, isRed) == 0, "e: scroll 0: no red inside the header column");
    check(redNear(s, 104, 4), "e: scroll 0: the whole knob shows (left half over the corner)");
  }
  setScroll(120);
  ed.seek(tAt(400));
  {
    const Shot s = grabView("e-normal");
    check(redNear(s, 400, kVideoMid) && redNear(s, 400, 4), "e: normal position x=400 is drawn");
    check(countPx(s, 0, 106, 0, kTracksEnd, isRed) == 0, "e: normal position leaves the header and the corner untouched");
    compareRef(s, "e-normal", "e: normal position is pixel-identical to the unmodified code");
  }
  setScroll(300);
  ed.seek(0);  // xOf(0) = -192（画面の外）
  {
    const Shot s = grabView("e-hidden");
    check(countPx(s, 0, s.img.width() / s.dpr, 0, viewH, isRed) == 0, "e: playhead left of the screen is not drawn");
    compareRef(s, "e-hidden", "e: playhead off-screen: pixel-identical to the unmodified code");
  }

  // ---- (a) 右にスクロールして、目盛りから見出しの上までドラッグ ----
  setScroll(120);
  ed.seek(0);
  press({400, 10});
  check(same(now(), tAt(400)), "a: ruler press seeks (x=400)");
  moveTo({150, 10});
  moveTo({60, 10});
  check(same(now(), tAt(60)), "a: dragging over the header sets the time to tOf(x) (x=60)");
  {
    const Shot s = grabView("a-drag-60");
    check(redNear(s, 60, kVideoMid) && redNear(s, 60, 4), "a: the red line is drawn under the mouse over the header (x=60)");
  }
  moveTo({0, 10});
  check(same(now(), tAt(0)), "a: dragging to the left edge reaches tOf(0)");
  moveTo({-30, 10});
  check(same(now(), 0.0), "a: dragging past the left edge stops at 0:00");
  release({-30, 10});
  moveTo({60, 10});
  check(same(now(), 0.0), "a: after release, moving the mouse does not scrub");
  press({500, kTextMid});  // テキストの行の何もないところ
  moveTo({60, kTextMid});
  check(same(now(), tAt(60)), "a: drag started on an empty track spot also follows into the header");
  release({60, kTextMid});

  // ---- (b) スクロール 0 では 0:00 で止まる ----
  setScroll(0);
  press({300, 10});
  check(same(now(), tAt(300)), "b: ruler press at scroll 0 (x=300)");
  moveTo({60, 10});
  check(same(now(), 0.0), "b: scroll 0: dragging over the header stops at 0:00");
  moveTo({0, 10});
  check(same(now(), 0.0), "b: scroll 0: dragging to the left edge stays at 0:00");
  {
    const Shot s = grabView("b-scroll0-drag");
    check(redNear(s, 108, kVideoMid) && countPx(s, 0, 106, kRulerH, kTracksEnd, isRed) == 0, "b: scroll 0: the line stays just right of the header");
  }
  release({0, 10});

  // ---- (c) 右にスクロールして、左上の角をクリックしてドラッグ ----
  setScroll(120);
  ed.seek(5.0);
  ed.play();
  const bool wasPlaying = ed.playing();
  press({24, 10});
  check(wasPlaying && !ed.playing(), "c: corner press pauses playback like a ruler press");
  check(same(now(), tAt(24)), "c: corner press moves the playhead to tOf(x) (x=24)");
  moveTo({70, 10});
  check(same(now(), tAt(70)), "c: dragging from the corner keeps scrubbing (x=70)");
  moveTo({250, 12});
  check(same(now(), tAt(250)), "c: the drag continues into the ruler (x=250)");
  release({250, 12});
  ed.pause();  // 修正前は止まらないので、ここで止めておく
  click({24, 10});
  {
    const Shot s = grabView("c-corner");
    check(redNear(s, 24, 4) && redNear(s, 24, 10) && redNear(s, 24, kVideoMid), "c: after a corner click the knob and line show at the click (x=24)");
  }
  setScroll(0);
  ed.seek(3.0);
  press({24, 10});
  check(same(now(), 0.0), "c: scroll 0: a corner press goes to 0:00, not below");
  release({24, 10});
  setScroll(120);
  const size_t textsC = ed.project().texts.size();
  dclick({24, 10});
  check(ed.project().texts.size() == textsC, "c: double-click in the corner adds no text");

  // ---- (d) トラックの行の見出しの上のクリックは何もしない ----
  setScroll(120);
  ed.seek(5.0);
  ed.select({kv::Selection::Clip, clipA});
  const int rev0 = ed.revision();
  const size_t textsD = ed.project().texts.size();
  auto unchanged = [&] { return same(now(), 5.0) && ed.selection() == kv::Selection{kv::Selection::Clip, clipA} && ed.revision() == rev0; };
  click({60, kTextMid});
  check(unchanged(), "d: click on the text header does nothing");
  press({60, kVideoMid});
  moveTo({300, kVideoMid});
  release({300, kVideoMid});
  check(unchanged(), "d: press on the video header and drag right does nothing");
  click({60, kAudioMid});
  check(unchanged(), "d: click on the audio header does nothing");
  click({60, kTracksEnd + 20});
  check(unchanged(), "d: click on the header below the tracks does nothing");
  dclick({60, kTextMid});
  check(unchanged() && ed.project().texts.size() == textsD, "d: double-click on the text header adds no text");

  // ---- (f) 変えていない操作 ----
  setScroll(120);
  ed.select({});
  press({300, 10});
  check(same(now(), tAt(300)), "f: ruler press right of the header seeks (x=300)");
  moveTo({420, 10});
  check(same(now(), tAt(420)), "f: ruler drag right of the header scrubs (x=420)");
  release({420, 10});
  click({200, kVideoMid});
  check(ed.selection() == kv::Selection{kv::Selection::Clip, clipA} && same(now(), tAt(200)), "f: clip click selects the clip and seeks on release");
  press({200, kVideoMid});
  moveTo({230, kVideoMid});
  moveTo({600, kVideoMid});
  release({600, kVideoMid});
  check(ed.project().clips.size() == 2 && ed.project().clips[0].id == clipB && ed.project().clips[1].id == clipA,
        "f: dragging a clip past the next one reorders the clips");
  click({500, kTextMid});
  check(!ed.selection() && same(now(), tAt(500)), "f: click on an empty track spot deselects and seeks");
  const size_t textsF = ed.project().texts.size();
  dclick({500, kTextMid});
  check(ed.project().texts.size() == textsF + 1 && same(ed.project().texts.back().start, kv::frameRound(tAt(500), ed.project().settings.fps)),
        "f: double-click on the empty text row adds a text there");
  {
    const int s0 = scrollX();
    const QPointF p(400, kVideoMid);
    QWheelEvent we(p, gView->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(gView->viewport(), &we);
    check(scrollX() == s0 + 120, "f: wheel still scrolls the timeline horizontally");
  }

  check(mismatches == 0, "setup: every mouse event landed at the intended viewport position");
  if (failures) std::printf("RESULT: %d check(s) FAILED\n", failures);
  else std::printf("RESULT: ALL OK\n");
  return failures ? 1 : 0;
}
```

ハーネスについての補足:
- コンパイルが通らないとき（include の不足など）は、ハーネスだけを直す。チェックの意味は変えない。
- ハーネスの準備（配置・大きさ・時間）を変えたら、`shots\ref-*.png` を消し、修正前のコードで基準をとり直す。
- ダブルクリックでは、Qt 6 の QWidgetWindow がダブルクリックを作った 2 回目の press をウィジェットに渡さない。それでも渡された場合は、2 回目の press もふつうのクリックとして動くだけなので、結果は同じ。

## 付録 B: timelinewidget.cpp の変更（これ以外は変えない）

1) `paintRuler` の最後（328-342 行目）。次のブロックを丸ごと消す。直前の `for` の閉じ括弧（327 行目）と、関数の閉じ括弧（343 行目）は残す:
```cpp
  // 再生ヘッドのつまみ
  const double px = xOf(ed.currentTime());
  if (px >= kHeadW - 8 && px <= area.width() + 8) {
    QPainterPath knob;
    knob.moveTo(px - 6, 0);
    knob.lineTo(px + 6, 0);
    knob.lineTo(px + 6, 12);
    knob.lineTo(px, 18);
    knob.lineTo(px - 6, 12);
    knob.closeSubpath();
    p.setRenderHint(QPainter::Antialiasing);
    p.fillPath(knob, theme::playhead);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(QRectF(px - 1, 17, 2, kRulerH - 17), theme::playhead);
  }
```
`paintRuler` の `ed` は 300 行目の `ed.layout().total` で使い続けるので、使わない変数の警告は出ない。

2) `paintEvent` の 607-609 行目。次の 3 行を消す。その次の空行（610 行目）は残す:
```cpp
  // 再生ヘッド
  const double px = xOf(ed.currentTime());
  if (px >= kHeadW - 1) p.fillRect(QRectF(px - 1, kRulerH, 2, area.height() - kRulerH), theme::playhead);
```

3) `paintEvent` の最後。633 行目の合計時間の `p.drawText(...)` のあと、関数の閉じ括弧の前に足す。つまみの中身は 1) と同じで、条件だけが違う:
```cpp

  // 再生ヘッド（見出しの列と左上の角より上に描くので、見出しの上でも画面の左端まで見える）
  const double px = xOf(ed.currentTime());
  if (px >= -1) p.fillRect(QRectF(px - 1, kRulerH, 2, area.height() - kRulerH), theme::playhead);
  // 再生ヘッドのつまみ（左上の合計時間より上に描く）
  if (px >= -8 && px <= area.width() + 8) {
    QPainterPath knob;
    knob.moveTo(px - 6, 0);
    knob.lineTo(px + 6, 0);
    knob.lineTo(px + 6, 12);
    knob.lineTo(px, 18);
    knob.lineTo(px - 6, 12);
    knob.closeSubpath();
    p.setRenderHint(QPainter::Antialiasing);
    p.fillPath(knob, theme::playhead);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(QRectF(px - 1, 17, 2, kRulerH - 17), theme::playhead);
  }
```

4) `mousePressEvent` の 646 行目:
```cpp
  if (pos.x() < kHeadW) return;
```
を次に置き換える:
```cpp
  // 見出しの列のクリックは無視する。左上の角（目盛りの行）だけは、目盛りと同じく再生位置を動かす
  if (pos.x() < kHeadW && pos.y() >= kRulerH) return;
```

`git diff` の大きさの目安: 削除 19 行（1 の 15 行、2 の 3 行、4 の 1 行）、追加 20 行前後。xOf / tOf・定数・見出し・角・ほかのイベント処理・ヘッダーは変えない。
