# 検証結果: 赤い再生ヘッドを見出しの列の上まで動かして見えるようにする（1 回目）

## まとめ
- 変更したのは `native/src/ui/timelinewidget.cpp` だけ（追加 20 行、削除 19 行）。`timelinewidget.h` は変えていない。計画の付録 B のとおりで、それ以外の変更はない。
- アプリのビルド（`-NoDeploy`）は終了コード 0。警告もエラーもない（`-Wall -Wno-unused-parameter`）。
- ハーネス: 修正前のコードでは 47 件のうち 11 件が FAIL（計画で予想した 11 行と同じ）。修正後は 49 件すべて OK（`RESULT: ALL OK`、終了コード 0）。
- 画像は read_file で目でも確認した（6 章）。
- 一時ファイルはすべて削除した（9 章）。コミット・push・stash・配置はしていない。

## 1. 始める前の状態
- `review.json` はなかったので 1 回目として作業した。タスクのフォルダーには `plan.md` だけがあった。
- git: ブランチ `feat/ikumu1`、`origin/feat/ikumu1` と同じ、HEAD は `1f7d805`。`git status --porcelain` は `?? .agents/tasks/timeline-playhead-left/` だけ。依頼文にある widgets.h・widgets.cpp・inspector.cpp の変更は 1f7d805 ですでにコミットされていて、作業ツリーには変更がない（計画 1.3 と同じ）。私はこれらに触っていない。
- ユーザーのアプリ: 作業を始めたとき（00:37:45）に `Get-Process -Name e-editor` を実行したが、何も出なかった。つまり、その時点でアプリはすでに起動していなかった（計画を書いた時点の PID 24972 は見つからなかった）。私はどのプロセスも終了させていない。最後の確認（00:49 ごろ）でも起動していない。
- `timelinewidget.cpp` は UTF-8（BOM なし）・CRLF（936 行、LF だけの行はなし）。修正後も UTF-8（BOM なし）・CRLF（937 行、LF だけの行はなし）のまま。

## 2. 変更の内容
`git -C c:\git\JPHACK\e-editor diff`（`git diff --stat`: 1 file changed, 20 insertions(+), 19 deletions(-)。`git diff --check` は何も出さず、終了コード 0。diff の中に printf・qDebug・ハーネスのコードはない）:

```diff
@@ -325,21 +325,6 @@ void TimelineView::paintRuler(QPainter& p, const QRect& area) {
       p.drawText(QPointF(x + 3, 11), formatTime(double(i / 5) * step, step < 1));
     }
   }
-  // 再生ヘッドのつまみ
-  const double px = xOf(ed.currentTime());
-  if (px >= kHeadW - 8 && px <= area.width() + 8) {
-    QPainterPath knob;
-    knob.moveTo(px - 6, 0);
-    knob.lineTo(px + 6, 0);
-    knob.lineTo(px + 6, 12);
-    knob.lineTo(px, 18);
-    knob.lineTo(px - 6, 12);
-    knob.closeSubpath();
-    p.setRenderHint(QPainter::Antialiasing);
-    p.fillPath(knob, theme::playhead);
-    p.setRenderHint(QPainter::Antialiasing, false);
-    p.fillRect(QRectF(px - 1, 17, 2, kRulerH - 17), theme::playhead);
-  }
 }
 
 void TimelineView::paintVideo(QPainter& p) {
@@ -604,9 +589,6 @@ void TimelineView::paintEvent(QPaintEvent*) {
       p.drawLine(QPointF(ex, kRulerH), QPointF(ex, area.height()));
     }
   }
-  // 再生ヘッド
-  const double px = xOf(ed.currentTime());
-  if (px >= kHeadW - 1) p.fillRect(QRectF(px - 1, kRulerH, 2, area.height() - kRulerH), theme::playhead);
 
   // 見出し（左に固定）
   p.fillRect(QRect(0, kRulerH, kHeadW, area.height()), theme::panel);
@@ -631,6 +613,24 @@ void TimelineView::paintEvent(QPaintEvent*) {
   p.fillRect(QRect(0, kRulerH - 1, kHeadW, 1), theme::border);
   p.setPen(theme::dim);
   p.drawText(QRect(10, 0, kHeadW - 12, kRulerH), Qt::AlignVCenter | Qt::AlignLeft, formatTime(total, false));
+
+  // 再生ヘッド（見出しの列と左上の角より上に描くので、見出しの上でも画面の左端まで見える）
+  const double px = xOf(ed.currentTime());
+  if (px >= -1) p.fillRect(QRectF(px - 1, kRulerH, 2, area.height() - kRulerH), theme::playhead);
+  // 再生ヘッドのつまみ（左上の合計時間より上に描く）
+  if (px >= -8 && px <= area.width() + 8) {
+    QPainterPath knob;
+    knob.moveTo(px - 6, 0);
+    knob.lineTo(px + 6, 0);
+    knob.lineTo(px + 6, 12);
+    knob.lineTo(px, 18);
+    knob.lineTo(px - 6, 12);
+    knob.closeSubpath();
+    p.setRenderHint(QPainter::Antialiasing);
+    p.fillPath(knob, theme::playhead);
+    p.setRenderHint(QPainter::Antialiasing, false);
+    p.fillRect(QRectF(px - 1, 17, 2, kRulerH - 17), theme::playhead);
+  }
 }
 
 // ---- 操作 -------------------------------------------------------------------
@@ -643,7 +643,8 @@ void TimelineView::mousePressEvent(QMouseEvent* e) {
   dragMoved_ = false;
   dragDx_ = 0;
   merge_ = QStringLiteral("tl-") + newId();
-  if (pos.x() < kHeadW) return;
+  // 見出しの列のクリックは無視する。左上の角（目盛りの行）だけは、目盛りと同じく再生位置を動かす
+  if (pos.x() < kHeadW && pos.y() >= kRulerH) return;
   ed.pause();
   const Hit h = hitTest(pos);
   if (h.row == Row::Ruler || h.type == Selection::None) {
```

要点:
- 線とつまみを `paintEvent` の一番最後（見出し、左上の角、合計時間を描いたあと）で描く。見出しより先に描くもの（クリップ、テキスト、オーディオ、目盛り、終わりの破線、挿入位置）は今までどおり見出しの下に隠れる。見出しと角の塗り方は変えていないので、不透明のまま。
- 描く条件の左側を、見出しの右端（`kHeadW - 1` / `kHeadW - 8`）から画面の左端（`-1` / `-8`）に変えた。ゆとりの幅と右側の条件は今のまま。
- `mousePressEvent` で無視する範囲を、見出しの列のトラックの行（`y >= kRulerH`）だけにした。左上の角（`y < kRulerH`）は `hitTest` で `Row::Ruler` になるので、目盛りを押したときと同じ処理（`ed.pause()`、`ed.seek(tOf(x))`、`Drag::Scrub`）に入る。0 未満は `Editor::seek` が 0 にそろえる。
- 変えていないもの: `kHeadW`・`kRulerH`・`xOf`・`tOf`・`hitTest`・`mouseMoveEvent`・`mouseReleaseEvent`・`mouseDoubleClickEvent`・`wheelEvent`・`followPlayhead`・`zoomAt`・ドラッグ＆ドロップ・見出しと角の描き方・`timelinewidget.h`。見出しの上までドラッグできる動き（作業 2）は、`mouseMoveEvent` の `Drag::Scrub` がもともと x を制限していないので、コードを変えずにできている（計画 1.2）。
- 現在のファイルでの位置: 再生ヘッドの描画は 617〜633 行目、`mousePressEvent` の条件は 646〜647 行目。`paintRuler` は 328 行目で閉じる。日本語のコメントは read_file と grep で UTF-8 のまま正しく読めることを確かめた。

## 3. アプリのビルド
コマンド（出力はリポジトリの外のログに書いた）:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1 -NoDeploy 2>&1 | Out-File -Encoding utf8 C:\e-editor-dev\tmp\kv-playhead\build.log; "BUILD-EXIT=$LASTEXITCODE" | Out-File -Append -Encoding utf8 C:\e-editor-dev\tmp\kv-playhead\build.log
```
ログの全文:
```
[1/4] Automatic MOC for target e-editor
[2/3] Building CXX object CMakeFiles/e-editor.dir/src/ui/timelinewidget.cpp.obj
[3/3] Linking CXX executable e-editor.exe
BUILD-EXIT=0
```
`warning:` も `error:` もない。新しくできたのは `native\build\e-editor.exe`（2026-10-22 00:47:58。git では無視されるフォルダー）だけ。配置していないので、`native\dist\e-editor\e-editor.exe` は 2026-10-21 13:34:55 のまま。

## 4. ハーネス
- 場所は `C:\e-editor-dev\tmp\kv-playhead\`。計画の付録 A の `CMakeLists.txt`・`playheadcheck.cpp`（UTF-8、BOM なし）・`run.ps1`（ASCII だけ）を、そのまま作った。main.cpp 以外のアプリのソース（.cpp 23 本と .h 23 本）をリポジトリから直接コンパイルし、Qt6::Test（Qt 6.8.3、MinGW 13.1）を使う。MainWindow と MediaManager は使わないので、ユーザーの自動保存には書き込んでいない。
- マウスは `QTest::mousePress/mouseMove/mouseRelease/mouseDClick` の QWindow 版で入れた。OS からの入力と同じ `QWindowSystemInterface` の経路を通る。DPR は 1.25（このマシンの値）。ビューポートに付けたイベントフィルターで、マウスの全イベントが渡した位置にぴったり届いたことを確かめた（最初と最後の `setup:` のチェックが OK。`note: mouse arrived at ...` は 1 回も出ていない）。
- ハーネスの修正はしていない。修正前のコードで、最初の実行から計画どおりの結果が出た。
- コマンド: `powershell -NoProfile -ExecutionPolicy Bypass -File C:\e-editor-dev\tmp\kv-playhead\run.ps1 2>&1 | Out-File ... run-<phase>.log; "RUN-EXIT=$LASTEXITCODE" | Out-File -Append ...`

### 4.1 修正前のコード（00:45 ごろ。アプリのファイルに触る前）
```
note: device pixel ratio 1.25, viewport 900x278
OK   setup: 2 clips (12 s), 1 text, 1 audio at 60 px/s
OK   setup: timeline can scroll to 300 and shows all rows
OK   setup: mouse events land at the intended viewport position
FAIL e: red line is drawn over the header column at x=60 (all rows)
FAIL e: knob is drawn over the top-left corner at x=60
OK   e: header stays opaque (clip, text and audio under it stay hidden)
OK   e: header icons and labels are visible
OK   e: total time in the top-left corner is visible
OK   e: nothing else in the header column is red
FAIL e: red line and knob reach the left edge (x=1)
OK   e: scroll 0, time 0:00: line stays just right of the header (x=108)
OK   e: scroll 0: no red inside the header column
FAIL e: scroll 0: the whole knob shows (left half over the corner)
OK   e: normal position x=400 is drawn
OK   e: normal position leaves the header and the corner untouched
note: saved C:/e-editor-dev/tmp/kv-playhead/shots/ref-e-normal.png (the first run must be on the unmodified code)
OK   e: playhead left of the screen is not drawn
note: saved C:/e-editor-dev/tmp/kv-playhead/shots/ref-e-hidden.png (the first run must be on the unmodified code)
OK   a: ruler press seeks (x=400)
OK   a: dragging over the header sets the time to tOf(x) (x=60)
FAIL a: the red line is drawn under the mouse over the header (x=60)
OK   a: dragging to the left edge reaches tOf(0)
OK   a: dragging past the left edge stops at 0:00
OK   a: after release, moving the mouse does not scrub
OK   a: drag started on an empty track spot also follows into the header
OK   b: ruler press at scroll 0 (x=300)
OK   b: scroll 0: dragging over the header stops at 0:00
OK   b: scroll 0: dragging to the left edge stays at 0:00
OK   b: scroll 0: the line stays just right of the header
FAIL c: corner press pauses playback like a ruler press
FAIL c: corner press moves the playhead to tOf(x) (x=24)
FAIL c: dragging from the corner keeps scrubbing (x=70)
FAIL c: the drag continues into the ruler (x=250)
FAIL c: after a corner click the knob and line show at the click (x=24)
FAIL c: scroll 0: a corner press goes to 0:00, not below
OK   c: double-click in the corner adds no text
OK   d: click on the text header does nothing
OK   d: press on the video header and drag right does nothing
OK   d: click on the audio header does nothing
OK   d: click on the header below the tracks does nothing
OK   d: double-click on the text header adds no text
OK   f: ruler press right of the header seeks (x=300)
OK   f: ruler drag right of the header scrubs (x=420)
OK   f: clip click selects the clip and seeks on release
OK   f: dragging a clip past the next one reorders the clips
OK   f: click on an empty track spot deselects and seeks
OK   f: double-click on the empty text row adds a text there
OK   f: wheel still scrolls the timeline horizontally
OK   setup: every mouse event landed at the intended viewport position
RESULT: 11 check(s) FAILED
RUN-EXIT=1
```
- チェックは 47 件で、OK 36 件、FAIL 11 件。画素の比較 2 件（`pixel-identical`）は、修正前の 1 回目では基準の画像を保存するだけなので数に入らない（修正後は 49 件）。
- FAIL の 11 行は、計画の手順 1 に書いた 11 行と完全に同じ。ほかはすべて OK。
- (a) で修正前に FAIL になるのは描画のチェック 1 つだけで、時間のチェックは修正前でも OK になる。修正前のコードでも、目盛りから始めたドラッグは見出しの上で `ed.seek(tOf(x))` になっていて、時間とプレビューは動いていた。見えなかったのは、線とつまみが見出しと角に塗りつぶされていたため（計画 1.2）。ハーネスの間違いではない。
- (c) と (e) は、期待どおり修正前に失敗した。

### 4.2 修正後のコード（00:48 ごろ）
ハーネスの再ビルドでコンパイルされたのは timelinewidget.cpp だけ。
```
note: device pixel ratio 1.25, viewport 900x278
OK   setup: 2 clips (12 s), 1 text, 1 audio at 60 px/s
OK   setup: timeline can scroll to 300 and shows all rows
OK   setup: mouse events land at the intended viewport position
OK   e: red line is drawn over the header column at x=60 (all rows)
OK   e: knob is drawn over the top-left corner at x=60
OK   e: header stays opaque (clip, text and audio under it stay hidden)
OK   e: header icons and labels are visible
OK   e: total time in the top-left corner is visible
OK   e: nothing else in the header column is red
OK   e: red line and knob reach the left edge (x=1)
OK   e: scroll 0, time 0:00: line stays just right of the header (x=108)
OK   e: scroll 0: no red inside the header column
OK   e: scroll 0: the whole knob shows (left half over the corner)
OK   e: normal position x=400 is drawn
OK   e: normal position leaves the header and the corner untouched
OK   e: normal position is pixel-identical to the unmodified code
OK   e: playhead left of the screen is not drawn
OK   e: playhead off-screen: pixel-identical to the unmodified code
OK   a: ruler press seeks (x=400)
OK   a: dragging over the header sets the time to tOf(x) (x=60)
OK   a: the red line is drawn under the mouse over the header (x=60)
OK   a: dragging to the left edge reaches tOf(0)
OK   a: dragging past the left edge stops at 0:00
OK   a: after release, moving the mouse does not scrub
OK   a: drag started on an empty track spot also follows into the header
OK   b: ruler press at scroll 0 (x=300)
OK   b: scroll 0: dragging over the header stops at 0:00
OK   b: scroll 0: dragging to the left edge stays at 0:00
OK   b: scroll 0: the line stays just right of the header
OK   c: corner press pauses playback like a ruler press
OK   c: corner press moves the playhead to tOf(x) (x=24)
OK   c: dragging from the corner keeps scrubbing (x=70)
OK   c: the drag continues into the ruler (x=250)
OK   c: after a corner click the knob and line show at the click (x=24)
OK   c: scroll 0: a corner press goes to 0:00, not below
OK   c: double-click in the corner adds no text
OK   d: click on the text header does nothing
OK   d: press on the video header and drag right does nothing
OK   d: click on the audio header does nothing
OK   d: click on the header below the tracks does nothing
OK   d: double-click on the text header adds no text
OK   f: ruler press right of the header seeks (x=300)
OK   f: ruler drag right of the header scrubs (x=420)
OK   f: clip click selects the clip and seeks on release
OK   f: dragging a clip past the next one reorders the clips
OK   f: click on an empty track spot deselects and seeks
OK   f: double-click on the empty text row adds a text there
OK   f: wheel still scrolls the timeline horizontally
OK   setup: every mouse event landed at the intended viewport position
RESULT: ALL OK
RUN-EXIT=0
```

## 5. 項目ごとの結果（配置: 60 px/秒、クリップ 0〜6 秒と 6〜12 秒、テキスト 0〜3 秒、オーディオ 0〜8 秒）
- (a) 右スクロール（120）で目盛りから見出しの上へドラッグ: OK。x = 60 で `currentTime == tOf(60)`（0.2 秒）、x = 0 で `tOf(0)`、x = −30 で 0 で止まる。トラックの何もない場所から始めたドラッグも同じ。ドラッグ中の画像で、マウスの下（x = 60）に線とつまみが描かれている。離したあとはマウスを動かしても時間は変わらない。
- (b) スクロール 0: OK。見出しの上（x = 60）でも左端（x = 0）でも 0:00 で止まり、線は見出しのすぐ右（x = 108）にあって、見出しの中は赤くならない。
- (c) 右スクロール（120）で左上の角をクリック: OK。再生中に押すと止まり（目盛りと同じ）、時間が `tOf(24)` になり、そのまま x = 70、さらに目盛りの x = 250 までドラッグが続く。クリックしたあとの画像で、x = 24 につまみと線が出ている。スクロール 0 で角を押すと 0:00（0 未満にはならない）。角のダブルクリックではテキストは増えない。
- (d) トラックの行の見出しの上: OK。テキスト・ビデオ・オーディオの見出しと、その下の空き部分のクリック、ビデオの見出しから右へのドラッグ、テキストの見出しのダブルクリックで、時間・選択・編集の版（revision）・テキストの数のどれも変わらない（修正前も後も OK）。
- (e) 見た目（grab の画素で確認）: OK。x = 60 で線がテキスト・ビデオ・オーディオの 3 行と下の端まで描かれ、つまみが左上の角の中に描かれる。見出しの x = 4・96・103 の画素は 3 行とも panel 色（#1b1b1b）のまま（下のクリップが透けていない）。アイコンとラベル（線の列を除く）と左上の合計時間の文字の画素がある。線の列のほかに見出しの中で赤い画素はない。x = 1 で線とつまみが左端に出る。スクロール 0・0:00 では線は x = 108 で、つまみ全体（左半分は角の上）が見える。見出しから離れた位置（x = 400）と、再生ヘッドが画面の外にあるとき（スクロール 300、0:00）は、修正前の画像と画素まで同じ。
- (f) 変えていない動き: OK（修正前も後も）。目盛りのクリックとドラッグ、クリップのクリック（選択して、離したときにその時間へ移動）、クリップを次のクリップの後ろまでドラッグして並べ替え、トラックの何もない場所のクリック（選択を外して移動）、テキストの行の何もない場所のダブルクリックでその時間にテキストを追加、ホイールで横にスクロール。

## 6. 画像を目で見た確認（read_file。DPR 1.25 の実ピクセルで 1125x348）
修正前（`shots\baseline\`）:
- `e-head-60.png`（スクロール 120、再生位置 0.2 秒 = x 60）: 赤い線もつまみもどこにも見えない。見出しと左上の角に隠れている。修正前の問題を確認した。
- `c-corner.png`: 左上の角（x = 24）のクリックが無視され、赤い線は前の位置（5 秒、x = 288、0:04 と 0:06 の間）に残っている。角には何も出ていない。

修正後（`shots\`）:
- `e-head-60.png`: 赤い線が x = 60 で、見出しの列の上を、テキスト・ビデオ・オーディオの 3 行とその下の空き部分まで通っている。左上の角の中にも赤いつまみがある。見出しは不透明な濃い灰色のままで、アイコンと「テキスト」「ビデオ」「オーディオ」が読める（線が通るところだけ赤）。見出しの下のクリップ（茶・青・緑）は透けておらず、どれも見出しの右の境目から始まっている。左上の「0:12」が読める。
- `e-head-left.png`: 線が画面の一番左端にあり、つまみは左端で半分切れて見える。見出しのラベルは読める。
- `c-corner.png`: つまみが x = 24 で左上の「0:12」に重なっていて、重なっていない「0:」の部分は見えている。線は見出しのアイコンとラベルの間を通っている。
- `e-scroll0.png`・`b-scroll0-drag.png`: スクロール 0、0:00。線は見出しのすぐ右（0:00 の目盛り）にあり、見出しの中には入っていない。つまみは全体が見え、左半分が左上の角の右端にかかっている。クリップ・テキスト・オーディオの名前もそのまま見える。
- `e-normal.png` と `shots\baseline\e-normal.png`: 見た目が同じ（線とつまみは x = 400、0:06 と 0:08 の間）。画素の比較でも一致した。

## 7. ユーザーに見える違い（すべて合意した作業 1〜3 の結果。計画 D6 と同じ）
1. 右にスクロールしたとき、見出しの下の時間でも、赤い線とつまみが見出しの上に出る。線はラベルの上を通る。
2. つまみが左上の角に入ると、合計時間の文字の上に重なる。
3. スクロール 0 で 0:00 のとき、これまで角に隠れていたつまみの左半分も見える。線の左の 1px が、見出しの右の境界線（x = 107）に重なる。
4. 右にスクロールしているとき、左上の角をクリック・ドラッグすると赤い線が動く（これまでは何も起きなかった）。スクロール 0 で角を押すと 0:00 に動き、再生中なら止まる（目盛りを押したときと同じ）。

見出しから離れた位置では見た目は変わらない（x = 400 と画面の外で画素まで一致）。理屈の上では、線とつまみが見出しの列にかからない位置（px >= 114）では、描く順番を変えても画素は変わらない。

## 8. 確認できなかったこと
- プレビューの映像そのもの: ハーネスでは PreviewEngine とプレビュー画面を作っていない（MainWindow も実際の動画ファイルも使っていない）。確かめたのは `Editor::currentTime()` が `tOf(x)` になることまでで、プレビューがその時間の映像を出すことは、`previewengine.cpp:32` で `Editor::timeChanged` を受けていることをコードで確かめただけ。
- 実際のアプリでの手での操作: していない。配置もしていないので、`native\dist` の exe は古いまま。ユーザーがアプリを閉じてから `build.cmd` を実行する必要がある。
- 本物のマウス: 使っていない。QTest で `QWindowSystemInterface` の経路から入れた（位置がずれていないことは確かめた）。
- DPR 1.25 以外（100%、150%、200% など）の画面は試していない。
- ハーネスで動かしていない操作（どれもコードは変えていない）: クリップの端のドラッグ（長さ変更）、テキストとオーディオの移動と長さ変更、分割、Ctrl+ホイールの拡大縮小、Shift+ホイール、ライブラリからのドラッグ＆ドロップ、実際の再生中の自動スクロール（followPlayhead）、テキストやオーディオの段が多くて縦にスクロールしたとき。

## 9. 削除した一時ファイル
- `C:\e-editor-dev\tmp\kv-playhead\`（フォルダーごと）: `CMakeLists.txt`、`playheadcheck.cpp`、`run.ps1`、確認用の `probe.ps1` と `final.ps1`、ハーネスのビルドフォルダー `build\`、画像の `shots\`（`baseline\` と `ref-*.png` を含む）、ログ（`build.log`、`run-baseline.log`、`run-fixed.log`、`qtest.log`、`src-list.log`、`enc.log`、`eol.log`、`eol2.log`、`probe.log`、`shots-list.log`、`final.log`）。
- `C:\e-editor-dev\tmp\` の直下に作ったログ: `kv-gitstatus.log`、`kv-ls.log`、`kv-final.log`、`kv-status.log`。
- 削除したあと `Test-Path 'C:\e-editor-dev\tmp\kv-playhead'` は False。`C:\e-editor-dev\tmp` には、作業を始める前と同じく何も残っていない。リポジトリには、テストやビルドのターゲットを何も足していない。

## 10. 最後の git の状態
`git -C c:\git\JPHACK\e-editor status --short`:
```
 M native/src/ui/timelinewidget.cpp
?? .agents/tasks/timeline-playhead-left/
```
増えた変更は timelinewidget.cpp だけ（`.agents/tasks/timeline-playhead-left/` はこのワークフローのフォルダー）。ブランチは `feat/ikumu1` のまま、`origin/feat/ikumu1` と同じ（HEAD 1f7d805）。コミット・push・stash はしていない。

## 11. ユーザーが自分で確かめる手順（計画 5 章のとおり）
1. アプリを閉じてから `build.cmd` をダブルクリックします（今回は配置をしていないので、`native\dist` の exe はまだ古いままです）。
2. 動画を 1 本以上タイムラインに置き、タイムラインを右にスクロールします（下のスクロールバーか、ホイール）。
3. 目盛り（時間の数字がある行）をクリックし、そのまま左へドラッグします。赤い線は「テキスト」「ビデオ」「オーディオ」の見出しの上を通って、一番左まで動きます。見出しは消えず、赤い線だけが上を通ります。プレビューには赤い線の時間の映像が出ます。
4. 赤い線が見出しの上にあるとき、上のつまみは左上の角（合計時間が出ているところ）に重なって見えます。左上の角をクリックすると、赤い線がその位置に動き、そのまま左右にドラッグできます。
5. 見出しの「テキスト」「ビデオ」「オーディオ」の行をクリックしても、今までどおり何も起きません。
6. スクロールを一番左に戻すと、赤い線は今までどおり 0:00（見出しのすぐ右）で止まり、見出しの中には入りません。0:00 のつまみは、これまで左半分が角に隠れていましたが、全体が見えるようになります。
7. クリップのクリック・移動・端のドラッグ、ダブルクリックでのテキストの追加、ホイールでのスクロール、Ctrl+ホイールでの拡大縮小、再生中の自動スクロールは今までどおりです。
