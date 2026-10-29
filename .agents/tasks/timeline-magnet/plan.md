# 実装計画: タイムラインのマグネット（吸着）機能

ユーザーの要望（原文）: 「キリがいい秒数とかでちょっと止まる機能（マグネット機能）みたいなものを追加してほしい」「１イ２ア３ア４イ」「以上の内容で実装お願い」「言語はC＋＋で」「それ以外は触らないで」

- 案は `.agents/tasks/timeline-magnet/proposal.md`（案B のオン/オフを Alt だけにしたもの。7 章の答えは 1イ 2ア 3ア 4イ）。
- 変更するのは `native/src/ui/timelinewidget.cpp` と `native/src/ui/timelinewidget.h` の 2 つだけ（C++ / Qt 6.8.3）。ほかのファイル（editor.cpp/.h、mainwindow.cpp、README.md、アイコン、app.qrc など）は触らない。どうしても必要だと判断したら、編集せずに止めて報告する。
- `c:\git\JPHACK\e-editor` で直接作業する（worktree なし）。コミット・push・stash・配置（deploy）はしない。
- 一時ファイルとハーネスは `C:\e-editor-dev\tmp\kv-magnet\` に作る。リポジトリにテストやビルドのターゲットを足さない。
- 計画・検証・レビューのファイルは `.agents/tasks/timeline-magnet/` に置く。proposal.md は消さない。

## 0. 始める前の状態（計画のときに確かめた結果）

- `git -C c:/git/JPHACK/e-editor status`: ブランチ `feat/ikumu1`。変更は `?? .agents/tasks/timeline-magnet/`（未追跡）だけで、ほかに変更はない。
- `git log -1 --oneline`: `eaac4b6 (HEAD -> feat/ikumu1) feat(timeline): make playhead visible over header column`（proposal.md と同じ）。
- `origin/feat/ikumu1` が 2 コミット先にある（`c3c5436` マージ、`0059f30` テキストのプリセット表示。変えたのは textstyles・textrender・inspector・librarypanel・widgets で、timelinewidget.cpp/.h には触れていない）。頼まれていないので pull はしない。今回の変更とはぶつからない。
- `timelinewidget.cpp` は UTF-8（BOM なし）・CRLF・937 行、`timelinewidget.h` は UTF-8（BOM なし）・CRLF・121 行（インデックスは LF、`core.autocrlf=true`）。編集後もこのままにする。
- ユーザーのアプリ `native\dist\e-editor\e-editor.exe` が起動している（計画のとき PID 13748）。終了させない。`build.ps1 -NoDeploy` は `native\build\` にだけ書くので、起動中のアプリには触れない。
- `C:\e-editor-dev\tmp\` はあり、`kv-magnet\` はまだない。ツール（`C:\e-editor-dev\Qt\6.8.3\mingw_64`、`Qt\Tools\mingw1310_64`、`cmake\bin\cmake.exe`、`bin\ninja.exe`、`ffmpeg`）はそろっている。
- 実装を始める前に、もう一度 `git -C c:\git\JPHACK\e-editor status --porcelain` を見る。`?? .agents/tasks/timeline-magnet/` 以外が出たら、何も触らずに止めて報告する。

## 1. 今のコード（HEAD eaac4b6 の行番号）

timelinewidget.cpp:
- 29 `kSnapPx = 8`。116-122 `xOf` / `tOf`。154-160 `followPlayhead`（再生中だけ）。
- 248-266 `snapPoints(exclude)`: `{0, currentTime()}`＋全クリップの start/end＋テキストとオーディオの端（exclude を除く）。
- 268-279 `snap(t, points)`: 8/pps 秒より近い（`<`）点のうち一番近いものを返す。なければ t のまま（吸着したかどうかがわからない）。
- 294 `kSteps`。296-328 `paintRuler`: 301-307 で「1 間隔が 70px 以上になる一番小さい値」を `step` にする（なければ 1200）。308 `minor = step / 5`。316-327 の目盛りのループ（320 `major`、321 線の色、322 線、323-326 数字。数字の時刻は `double(i / 5) * step`）。
- 556-634 `paintEvent`: 577-581 挿入位置の黄色い線 `QColor(0xff, 0xd4, 0x4a)`（ビデオの行だけ）、583-591 ビデオの終わりの破線、593-607 見出し、609-615 目盛りと左上の角、617-633 赤い線（最後に描くので見出しの上に出る）。
- 638-672 `mousePressEvent`: 645 `merge_`、647 見出しの列（目盛りの行以外）は無視、650-655 `Drag::Scrub`（652 `ed.seek(tOf(pos.x()))`）、661 `dragSnap_ = snapPoints(h.id)`。
- 674-775 `mouseMoveEvent`: 684 しきい値（ClipMove 6px、ItemMove 4px）、689-691 Scrub（690 `ed.seek(tOf(pos.x()))`）、697-719 ClipTrim、720-731 ItemMove（724-726 が右端の不具合）、732-771 ItemTrim（737・740・751・754 で snap）。
- 777-790 `mouseReleaseEvent`（781・783 は押した位置へ seek。788 `endMerge()`）。792-798 ダブルクリック。800-812 ホイール。

timelinewidget.h: 70 `snapPoints`、71 `snap`、72-74、96 `dragSnap_`。

ほかに読んだもの（変えない）: `timeline.cpp:29` `frameRound(t, fps) = floor(t*fps+0.5)/fps`、`timeline.cpp:82` `audioDuration = max(0, out-in)`、`editor.cpp:155-160` `seek`（0〜contentEnd に収めるだけ。値が範囲内ならそのまま入る）、`editor.cpp:115-127` `commit`（merge が同じなら 1 回の操作にまとめる）、`editor.cpp:149-153` `select` は `lastMerge_` を消す、`editor.cpp:325-336` `addText`（frameRound する）、`mainwindow.cpp:441-497`（handleKey と eventFilter は Alt 付きのキーを使わない）、`theme.cpp:116`（スタイルは Fusion）。

## 2. 決めたこと

- D1 吸着の計算は `snap()` 1 つにまとめる。戻り値を `std::optional<double>` にする（吸着しなかったら空。吸着したかどうかで目印を出すため。`t == 吸着先` のときも「吸着した」とわかる）。
  1. 内容の点（`points`）のうち、距離が `kSnapPx / pps()` 秒より小さい（今と同じ `<`）一番近いもの。同じ距離なら一覧で先のもの（今と同じ）。
  2. なければ目盛りの数字の位置 `g = frameRound(std::round(t / step) * step, fps)`。`g >= 0` で、距離が同じしきい値より小さいとき。`step = rulerStep()` は呼ぶたびに今の拡大率から計算するので、ドラッグ中に Ctrl+ホイールで拡大率が変わっても、次にマウスが動いたときの間隔になる。細かい線（step/5）は吸着先にしない。
  3. どちらもなければ空。呼ぶ側は今の値（`tOf(x)` など）をそのまま使う（フレームにはそろえない）。
  - frameRound は目盛りの数字の位置にだけ通す。proposal の 5 章の 4（「数字の位置は…フレームの境目だが、念のため frameRound を通す」）と 6 章の計算のイメージに合わせた。内容の点は今と同じくそのままの値を使う。クリップの境目・0:00・ビデオの終わり・addText や吸着で置いた端はもともとフレームの境目なので丸めても同じで、違いが出るのはフレームにそろっていない端だけ。そこを丸めると端同士がぴったり合わなくなる（proposal 5 章の 1 の目的に反する）ため。
- D2 `snapPoints(exclude, withPlayhead)` にする。点の順番は今と同じ（0 → 赤い線 → クリップ → テキスト → オーディオ）。ビデオの終わりは最後のクリップの `l.end`（layoutClips の total と同じ値）なので、点を足さなくても入っている。
  - テキスト・オーディオを押したとき: `snapPoints(h.id, true)`（押した時点の赤い線を含む。今と同じ）。
  - 赤い線（Scrub）を押したとき: `snapPoints(QString(), false)`（赤い線自身は入れない。テキストとオーディオは全部入る）。どちらも押したときに 1 回だけ作って `dragSnap_` に入れる（今と同じ作り）。
- D3 `rulerStep()` を新しく作り（kSteps の直後に定義）、paintRuler の 301-307 をこれに置き換える。値は前と同じなので、目盛りの見た目は変わらない。吸着の間隔も必ず同じ値になる。
- D4 目印の状態は `std::optional<double> snapMark_`（吸着している時刻）1 つ。描き方は `drag_` で分ける。
  - `drag_ == Drag::Scrub` のとき: paintRuler で、時刻 `double(i / 5) * step` が `snapMark_` と 1e-6 以内の数字（major の目盛り）だけ、線と数字の色を `QColor(0xff, 0xd4, 0x4a)` にする。数字の位置でない内容の点に吸着したときは、合う数字がないので何も変わらない。内容の点がたまたま数字の位置と同じ（例: 0:00）なら黄色になる（赤い線が実際に数字の上にあるため）。
  - `drag_ == Drag::ItemMove / ItemTrim` のとき: paintEvent の「終わりの位置」（583-591）のすぐあと、見出し（593）の前に `p.fillRect(QRectF(xOf(*snapMark_) - 1, kRulerH, 2, area.height() - kRulerH), QColor(0xff, 0xd4, 0x4a))`。赤い線と同じ形で、トラック全体（目盛りの下から一番下まで）に出る。見出しより前に描くので、見出しの列の下には隠れる。赤い線は今までどおり paintEvent の最後に描く。
  - 色は挿入位置の線（581）と同じ値を、ファイルのほかの所と同じく `QColor(...)` で直接書く（581 には触らない）。
  - `snapMark_` が空なら描画は前と同じ命令だけになるので、吸着していないときは画素まで同じ（ハーネスで比べる）。
  - セットとクリア: mousePressEvent の最初（`dragDx_ = 0;` のあと）で空にする。Scrub は press と move のたびに決め直す。ItemMove / ItemTrim は move のたびに決め直す（4px のしきい値の前は空のまま）。mouseReleaseEvent で空にする（そのあとの `viewport()->update()` で消える）。ClipMove / ClipTrim では使わない。
  - 赤い線が数字に吸着したときの目盛りの線は、赤い線のつまみと縦線の真下に重なるので、画面ではほとんど見えない（数字が黄色に見える）。仕様どおり線の色も変える。
- D5 赤い線は `scrubTo(x, snapOn)` を新しく作り、press と move の両方から呼ぶ（同じ処理を 2 か所に書かないため）。`t = tOf(x)` → `snapMark_ = snapOn ? snap(t, dragSnap_) : nullopt` → `ed.seek(snapMark_.value_or(t))` → `ed.currentTime() != *snapMark_` なら `snapMark_` を空にする（内容の終わりより後ろの数字に吸着して seek で終わりに止められたとき、黄色にしない）。`Editor::seek` は変えない。
- D6 Alt: press は `!(e->modifiers() & Qt::AltModifier)`、move は switch の前で `const bool snapOn = !(e->modifiers() & Qt::AltModifier);` を作って Scrub・ItemMove・ItemTrim で使う。キーのイベントは見ない（Alt を押したり離したりしただけでは変わらず、次にマウスが動いたときに効く）。Alt のときは目印も出ない。
- D7 ItemMove: `s1 = 左端の吸着先`、`s2 = 右端の吸着先`（どちらも optional）。`s1` があり、`s2` がないか `|s1 - start| <= |s2 - (start + dur)|` なら左端（同じなら今と同じく左）、そうでなく `s2` があれば `start = *s2 - dur`。これで「右端は左端も吸着できるときしか効かない」不具合（724-726）が直る。目印はそのとき使った端の時刻。最後の `start = max(0, start)` は残す（吸着先はどれも 0 以上で、0 は必ず内容の点にあるので、右端を使った結果が 0 未満になることはない）。
- D8 ItemTrim: 動かしている端（`raw`）を吸着させ、今ある制限（0 未満にしない、kMinTextDuration、kMinClipDuration、素材の長さ）はそのまま通す。制限のあとの実際の端が吸着先から 1e-6 秒以上ずれたら（制限で止められたら）目印を出さない。テキストの右端は `t.start + t.duration`、左端は `start`、オーディオの右端は `x.start + audioDuration(x)`、左端は `x.start` で比べる。
- D9 (j) Alt とメニューバー: Qt 6.8.3 のソースを読んだ結果、今のままで問題は出ないと見込む（7 章）。ハーネスで確かめ、問題が出たときだけ付録 C の最小限の変更を入れる。
- D10 変えないもの: Editor::seek、←→・Home/End（mainwindow）、再生中の動きと followPlayhead、元に戻す（`merge_` と `endMerge()`。吸着は updateText/updateAudio に渡す値を変えるだけなので、1 回のドラッグは 1 回の操作のまま）、拡大縮小とホイール、ビデオのクリップの移動・端の長さの変更、ライブラリからのドロップ、ダブルクリックでのテキストの追加、クリップやテキストを押して離したときの seek（781・783）、見出しと赤い線の描き方、README・ショートカット一覧（Alt の説明は足さない）。コメントは日本語で、ほかのリファクタリングはしない。

## 3. ユーザーに見える違い

1. 赤い線のドラッグ（目盛り・左上の角・トラックの空いた所から）: 目盛りの数字の位置から 8px 以内で止まり、その数字が黄色になる。0:00・ビデオの終わり・クリップの境目・テキストとオーディオの端でも止まる（数字の位置でなければ黄色にならない）。両方が近ければ内容の点が先。押した瞬間も吸着する（数字のすぐ近くをクリックすると、赤い線が数字にぴったり合う）。テキストの行の空いた所のダブルクリックでは、1 回目の押しで赤い線が吸着することがある（追加されるテキストの位置は今のまま）。
2. テキスト・オーディオの移動と長さの変更: 今の吸着先に加えて目盛りの数字にも止まる。右端だけが吸着先に近いときも止まる（不具合の修正）。止まっている間、黄色の縦線がトラック全体に出て、離すと消える。
3. Alt を押している間は、どれも吸着しない（今あるテキスト・オーディオの吸着も）。
4. 吸着先から 8px 以内にはマウスで置けない。置きたいときは Alt を押しながらドラッグするか、←→ で 1 フレームずつ動かす。

## 4. ビルドとハーネスの実行（出力はログに書いてから読む）

execute_pwsh の画面はコマンドを何度も表示して出力が切れることがあるので、出力はログファイルに書き、最後に終了コードの目印を足し、読み取りツールで読む。パスは絶対パスにし、git は `git -C c:\git\JPHACK\e-editor` で呼ぶ。

- アプリのビルド（コンパイルだけ。配置しない。`<n>` は 1, 2, …）:
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1 -NoDeploy 2>&1 | Out-File -Encoding utf8 C:\e-editor-dev\tmp\kv-magnet\build-<n>.log; "BUILD-EXIT=$LASTEXITCODE" | Out-File -Append -Encoding utf8 C:\e-editor-dev\tmp\kv-magnet\build-<n>.log
  ```
  成功の条件: 最後の行が `BUILD-EXIT=0`、`error:` がなく、`warning:` もない（`-Wall -Wno-unused-parameter`）。ヘッダーを変えると mainwindow.cpp もコンパイルし直される。もし変えていないファイルの警告が出たら、`git diff --stat` でそのファイルが変わっていないことを確かめ、「前からある警告」として記録する（今回の変更による警告は 0 にする）。GCC が `std::optional` について `-Wmaybe-uninitialized` を出したら、動きを変えずに書き方を変えて消す（例: optional の代わりに double と bool の組にする）。
- ハーネス（`<phase>` は `baseline`・`step`・`fixed`）:
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\e-editor-dev\tmp\kv-magnet\run.ps1 2>&1 | Out-File -Encoding utf8 C:\e-editor-dev\tmp\kv-magnet\run-<phase>.log; "RUN-EXIT=$LASTEXITCODE" | Out-File -Append -Encoding utf8 C:\e-editor-dev\tmp\kv-magnet\run-<phase>.log
  ```
  1 回目はアプリのソースを全部コンパイルするので、タイムアウトは 1800000 ms にする。ハーネスは小さなウィンドウ（900x約350）を数秒出す。その間、本物のマウスをそのウィンドウの上で動かさない（ウィンドウは今のカーソルの位置を避けて出す）。
- ファイルの形の確認（編集のあと）:
  ```powershell
  foreach ($p in 'c:\git\JPHACK\e-editor\native\src\ui\timelinewidget.cpp','c:\git\JPHACK\e-editor\native\src\ui\timelinewidget.h') { $b = [IO.File]::ReadAllBytes($p); $t = [Text.Encoding]::UTF8.GetString($b); "{0} CRLF={1} LFonly={2} BOM={3}" -f $p, ([regex]::Matches($t, "`r`n")).Count, ([regex]::Matches($t, "(?<!`r)`n")).Count, ($b[0] -eq 0xEF) }
  ```
  `LFonly=0`、`BOM=False` であること。

## 5. 計画

- [ ] 1. ハーネスを作り、修正前のコードで基準をとる（アプリのファイルにはまだ触らない）。
      `C:\e-editor-dev\tmp\kv-magnet\` に付録 A の 3 ファイル（`CMakeLists.txt`、`magnetcheck.cpp`（UTF-8 BOM なし）、`run.ps1`（ASCII だけ））を作る。作業の前に 0 章の最後の git status を確かめる。
      Files: C:\e-editor-dev\tmp\kv-magnet\CMakeLists.txt, magnetcheck.cpp, run.ps1（一時ファイル）
      Verify: 4 章のハーネスのコマンドを `baseline` で実行し `run-baseline.log` を読む。期待: 6 章の 6.3 の 36 行だけが FAIL、ほかはすべて OK、最後が `RESULT: 36 check(s) FAILED` と `RUN-EXIT=1`。`note: saved C:/e-editor-dev/tmp/kv-magnet/baseline/ref-*.png` が 7 つ、`note: device pixel ratio 1.25` が出て、`setup:` の 4 行が OK（`note: mouse arrived at ...` は 1 回も出ない）。`j: control: ...` も OK。
      - 違う結果になったら、アプリではなくハーネスを直す（チェックの意味と数値は変えない。原因がハーネスにあると示せるときだけ）。`baseline\ref-*.png` を消してから、基準が合うまで繰り返す。`j: control` だけがどうしても OK にならない場合は 7 章の扱いにする。
      - 合ったら修正前の画像を残す: `New-Item -ItemType Directory -Force C:\e-editor-dev\tmp\kv-magnet\baseline\shots-before | Out-Null; Copy-Item C:\e-editor-dev\tmp\kv-magnet\shots\*.png C:\e-editor-dev\tmp\kv-magnet\baseline\shots-before\`

- [ ] 2. 目盛りの間隔を `rulerStep()` に切り出す（見た目は変えない）。手順 1 のあとに行う。
      付録 B の B-1（cpp: kSteps の直後に `rulerStep()` を定義し、paintRuler の 301-307 を `const double step = rulerStep();` にする）と B-2（h: 宣言を 1 行足す）だけを入れる。str_replace で編集し、CRLF と UTF-8（BOM なし）を保つ。
      Files: native/src/ui/timelinewidget.cpp, native/src/ui/timelinewidget.h
      Verify: 4 章のアプリのビルド（`build-1.log`）が `BUILD-EXIT=0`、error と warning なし。続けてハーネスを `step` で実行し、FAIL は 6.3 の 36 行のまま（`RESULT: 36 check(s) FAILED`）、`i: ... pixel-identical ...` の 7 行がすべて OK（目盛りの見た目が変わっていない）。

- [ ] 3. 吸着と目印を入れる。手順 2 のあとに行う。
      付録 B の B-3〜B-9 を入れる: ヘッダー（`snapPoints(exclude, withPlayhead)`、`std::optional<double> snap(...)`、`scrubTo`、`snapMark_`）、snapPoints と snap（D1・D2）、paintRuler の数字の黄色（D4）、paintEvent のガイド線（D4）、`scrubTo` と mousePressEvent（D5・D6）、mouseMoveEvent の Scrub・ItemMove・ItemTrim（D6〜D8）、mouseReleaseEvent（D4）。コメントは日本語。それ以外は変えない。
      Files: native/src/ui/timelinewidget.cpp, native/src/ui/timelinewidget.h
      Verify: アプリのビルド（`build-2.log`）が `BUILD-EXIT=0`、error と warning なし。4 章のファイルの形の確認が `LFonly=0`・`BOM=False`。ハーネスを `fixed` で実行し、すべて OK で `RESULT: ALL OK`、`RUN-EXIT=0`（`pixel-identical` の 7 行も OK）。失敗したらアプリのコードを直して手順 3 をやり直す。ハーネスの期待値を変えて通すことはしない。

- [ ] 4. (j) の結果を確かめ、必要なときだけ対策を入れる。手順 3 のあとに行う。
      `run-fixed.log` の `j:` の行と `note: Alt tapped during a drag ...` と `note: menu bar Alt navigation style hint ...` を読む。扱いは 7 章のとおり。`j:` がすべて OK なら何も変えない（見込み）。7.3 の条件で FAIL したときだけ付録 C を入れ、ビルドとハーネス（`fixed`）をやり直す。
      Files: （対策が必要なときだけ）native/src/ui/timelinewidget.cpp, native/src/ui/timelinewidget.h
      Verify: `run-fixed.log` がすべて OK（`RESULT: ALL OK`）。対策を入れた場合は、`j: control: ...`（Alt だけでメニューバーが選ばれる）と、ウィンドウが前面のときの `j: Alt+V still opens the View menu` も OK のまま。

- [ ] 5. 画像を目で見て確かめる。手順 3（と 4）のあとに行う。
      `C:\e-editor-dev\tmp\kv-magnet\shots\` の PNG を読み取りツールで開く（DPR 1.25 の実ピクセル）。
      - `a-snap-2s.png`: 赤い線とつまみが 0:02（左から 228）にあり、「0:02」の数字（つまみの右側）が黄色。ほかの数字は灰色。トラックに黄色の線はない。
      - `a-alt.png`・`a-released.png`・`c-content.png`: 数字はすべて灰色。
      - `e-guide-left.png`: 黄色の縦線が 6.0（左から 468）に、テキストの箱の左端を通ってトラックの一番下まで出ている。見出しの列には出ていない。`e-guide-right.png`: 線が 10.5（738、テキストの右端・クリップの終わり）。`f-trim-right.png`: 708。`f-audio-move.png`: 228。
      - `e-released.png`・`e-alt.png`・`h-clip-trim.png`: 黄色の縦線がない。`h-clip-move.png`: 黄色はビデオの行の挿入位置の線だけ（今までと同じ）。
      - `b-corner.png`・`i-idle-head.png`: 赤い線が見出しの列の上（左から 78 と 18）に出ている。
      - `d-100.png`・`d-20.png`: 「0:01」と「0:05」が黄色。
      - `i-*.png` と `baseline\shots-before\i-*.png`: 見た目が同じ（画素の比較は手順 3 で OK）。
      Verify: 上の見た目をすべて確かめ、結果を verification.md に書く。

- [ ] 6. 差分と状態を確かめ、verification.md を書く。
      Verify:
      - `git -C c:\git\JPHACK\e-editor status --short` が ` M native/src/ui/timelinewidget.cpp`、` M native/src/ui/timelinewidget.h`、`?? .agents/tasks/timeline-magnet/` だけ。
      - `git -C c:\git\JPHACK\e-editor diff` に付録 B（と、入れた場合だけ付録 C）の変更だけがある。printf・qDebug・ハーネスのコードがない。`git diff --check` が何も出さない。README.md・mainwindow.cpp などは変わっていない。何もコミットしない。
      - ユーザーのアプリは起動したまま。配置していないので `native\dist` の exe は古いまま。
      - `.agents/tasks/timeline-magnet/verification.md` に書く: 始める前の git の状態、変更の内容（diff）、ビルドのログ（全文）、ハーネスのログ（baseline・step・fixed の全文と、FAIL の行が 6.3 と同じであること）、DPR と座標の確認、画像を見た結果、(j) の結果と `note:` の行、8 章の手順、9 章の確かめられないこと、最後の git の状態。
      - 一時フォルダー `C:\e-editor-dev\tmp\kv-magnet\` は、レビューで直すことになったときにハーネスと修正前の基準（baseline）をそのまま使えるように残す。verification.md に「残してあること」と消し方 `Remove-Item -Recurse -Force -LiteralPath 'C:\e-editor-dev\tmp\kv-magnet'` を書く。消すのは、レビューで承認されたあとの最後のステップ（このフォルダーだけ。`C:\e-editor-dev\tmp` のほかのものには触らない）。

## 6. ハーネスの設計と期待値

### 6.1 作り方（timeline-playhead-left の付録 A と verification.md 4 章に倣う）

- main.cpp 以外のアプリのソースをリポジトリから直接コンパイルし、Qt6::Test（Qt 6.8.3 / MinGW 13.1）を使う。MainWindow と MediaManager は使わない（ユーザーの自動保存に書き込まない）。素材は `Editor::addAsset` で Ready の情報だけを入れる。
- アプリと同じく、メニューバー（`&File`・`&View`）のある `QMainWindow` の中央に `TimelineWidget` を置く（(j) で使う）。幅 900、中央の高さ 328（ビューポートは約 900x278）。
- マウスは QTest の QWindow 版（`mousePress` / `mouseRelease` / `mouseDClick` と、修飾キーを渡せる `QTest::mouseEvent(QTest::MouseMove, ...)`）で、OS の入力と同じ `QWindowSystemInterface` の経路から入れる。DPR は掛けない（`qt_handleMouseEvent` が自分で直す）。ビューポートのイベントフィルターで、毎回、届いた位置と修飾キー（Alt）が渡したものと同じかを確かめる（`setup:` の最初と最後のチェック。ずれたら `note: mouse arrived at ...`）。
- 押すたびにタイムスタンプを 6 秒進め、意図しないダブルクリックを防ぐ。
- Alt のキーは `QTest::simulateEvent(QWindow*, ...)` で Windows と同じ形にする（押す: 保存する修飾キーなし → `QKeyEvent::modifiers()` は AltModifier。離す: AltModifier を保存 → NoModifier）。
- 画像は `viewport()->grab()`（DPR 1.25 の実ピクセル）。ガイド線は `#ffd44a` の塗りつぶしなので色で探す（論理座標 ±1.2）。数字の黄色さは、文字の画素の（赤−青）の平均で見る（灰色なら 0 付近、黄色なら 100 前後。ClearType の色のにじみは左右で打ち消し合う。赤い線のつまみ ±7.5 は数えない）。黄色は 50 以上、灰色は 25 未満とする。
- 修正前の画像との比較: 1 回目（修正前）は `C:\e-editor-dev\tmp\kv-magnet\baseline\ref-*.png` に保存し（数に入れない）、2 回目からは全画素を比べる。比べる画像は、ほかの確認より前に、決まった状態（時間・スクロール・拡大率・選択）でとる（あとの確認では修正の前と後で状態が違うため）。画像は毎回 `shots\` に保存する。
- 修正を始めたあとに基準をとり直す必要が出たら: `git show HEAD:native/src/ui/timelinewidget.cpp` と `.h` を `C:\e-editor-dev\tmp\kv-magnet\orig\` に書き出し、それをコンパイルする別のビルドで基準をとる（作業ツリーは戻さない。stash しない）。

### 6.2 配置と数値（proposal 3 章の表と同じ計算）

- クリップ A 0〜3.5 秒（素材 3.5 秒）、B 3.5〜10.5 秒（素材 7 秒）。テキスト 6.5〜8.8 秒（長さ 2.3）。オーディオ 0〜2.6 秒（素材 2.6 秒）。30fps。内容の点 = {0, 3.5, 10.5, 6.5, 8.8, 2.6}（テキスト・オーディオのドラッグでは自分を除き、押した時点の赤い線を足す）。
- 行（縦スクロール 0）: 目盛り 0〜26、テキスト 26〜58（箱 28〜52、真ん中 42）、ビデオ 58〜124（91）、オーディオ 124〜170（箱 126〜164、147）。見出しの幅 108。
- スクロール 0 のとき `x = 108 + t × pps`、`tOf(x) = (x − 108) / pps`。しきい値 8px = 8/pps 秒（60px/秒で 0.1333 秒、100 で 0.08、20 で 0.4、79.35 で 0.1008）。
- 数字の間隔: 60px/秒 → 2 秒、100 → 1 秒、20 → 5 秒、79.35（60 から Ctrl+ホイール 2 回）→ 1 秒。
- ちょうど 8px の所は浮動小数点の誤差で結果が揺れるので、確かめるのは 3〜7px と 9px 以上だけにする。

| 確認 | 操作 | tOf(x) | 近い点と距離 | 期待 |
|---|---|---|---|---|
| (a) | 目盛り 400 を押す | 4.867 | なし（4.0 まで 52px） | 4.867 |
| (a) | 216 / 219 / 221 / 223 / 233 へ | 1.8 / 1.85 / 1.883 / 1.917 / 2.083 | 0:02 まで 12 / 9 / 7 / 5 / 5px | 1.8 / 1.85 / 2.0 / 2.0 / 2.0 |
| (a) | 223 へ Alt 付き | 1.917 | 0:02 まで 5px | 1.917（吸着しない、数字は灰色） |
| (b) | 目盛り 225 を押す（Alt なし / あり） | 1.95 | 0:02 まで 3px | 2.0 / 1.95 |
| (b) | スクロール 150、左上の角 83 を押す（なし / あり） | 2.083 | 0:02（x=78）まで 5px | 2.0 / 2.083 |
| (b) | テキストの行の空き 705 を押す | 9.95 | 0:10 まで 3px（10.5 は 33px） | 10.0、選択が外れる |
| (c) | 目盛り 600（8.2 秒）を押して 314 / 501 / 633 / 268 / 113 / 734 へ | 3.433 / 6.55 / 8.75 / 2.667 / 0.083 / 10.433 | 3.5 / 6.5 / 8.8 / 2.6 / 0 / 10.5 まで 3〜5px（数字は 26px 以上） | その点 |
| (c) | テキストを 4.1〜6.4 にして 350 へ | 4.033 | 4.1 まで 4px、0:04 まで 2px | 4.1（内容の点が先） |
| (c) | 赤い線を 1.0 にしてから 600 を押し 171 へ | 1.05 | 前の赤い線 1.0 まで 3px | 1.05（赤い線自身は吸着先でない） |
| (d) | 60px/秒で 165 へ | 0.95 | 1.0 は数字でない | 0.95 |
| (d) | 100px/秒、440 を押し 203 へ | 0.95 | 0:01 まで 5px | 1.0、「0:01」が黄色 |
| (d) | 20px/秒、296 を押し 210 へ / 147 へ | 5.1 / 1.95 | 0:05 まで 2px / 2.0 は数字でない（2.6 まで 13px） | 5.0、「0:05」が黄色 / 1.95 |
| (d) | 60px/秒で 600 を押し、x=108 で Ctrl+ホイール 2 回（79.35、スクロール 0 のまま）、184 へ | 0.958 | 0:01 まで約 3px | 1.0 |
| (e) | テキストの真ん中 558 を押し 545 → 533 | 開始 6.283 → 6.083 | 左端が 0:06 まで 5px（右端 8.383 は 0:08 まで 23px） | 開始 6.0、ガイド線 x=468 |
| (e) | 558 → 600 → 664 | 開始 8.267、右端 10.567 | 左端は 0:08 まで 16px、右端は 10.5 まで 4px | 開始 8.2（10.5 − 2.3）、ガイド線 x=738 |
| (e) | 558 → 545 → 381 → 380（Alt）→ 379 | 開始 3.55 / 3.533 / 3.517 | 3.5 まで 3 / 2 / 1px | 3.5 / 3.533 / 3.5 |
| (e) | 赤い線 1.5 で 558 → 545 → 261 | 開始 1.55 | 赤い線 1.5 まで 3px（右端 3.85 は 0:04 まで 9px） | 1.5 |
| (f) | テキストの右端のつまみ 632 → 650 → 699 | 終わり 9.1 → 9.917 | 0:10 まで 5px | 終わり 10.0、ガイド線 708 |
| (f) | 左端のつまみ 502 → 490 → 476 | 開始 6.3 → 6.067 | 0:06 まで 4px | 開始 6.0、終わり 8.8 |
| (f) | 502 → 490 → 325 → 324（Alt） | 開始 3.55 / 3.533 | 3.5 まで 3 / 2px | 3.5 / 3.533 |
| (f) | オーディオの真ん中 186 → 200 → 301 | 開始 0.233 → 1.917 | 0:02 まで 5px | 開始 2.0、ガイド線 228 |
| (f) | 186 → 200 → 279（Alt） | 開始 1.55 | 赤い線 1.5 まで 3px | 1.55 |
| (f) | オーディオの右端 260 → 250 → 228 | 終わり 2.433 → 2.067 | 0:02 まで 4px | 終わり 2.0 |
| (f) | オーディオの左端 112 → 150 → 228 | 開始 0.633 → 1.933 | 0:02 まで 4px | 開始 2.0（in 2.0、終わり 2.6） |
| (g) | 上のテキスト・オーディオのドラッグのたびに元に戻すを 1 回 | | | 6.5〜8.8 と 0〜2.6 に戻る |
| (h) | クリップ A の真ん中 213 → 230 → 650 で離す | | | 並びが B, A。黄色はビデオの行の挿入線（x=738）だけ |
| (h) | クリップ B の右端 734 → 720 → 708 | | 0:10 まで 4px | 終わり 302/30 = 10.067（吸着しない） |
| (h) | クリップの 225 / テキストの 585 をクリック | 1.95 / 7.95 | 数字まで 3px | tOf(x)（吸着しない） |
| (h) | テキストの行の空き 225 をダブルクリック | 1.95 | 0:02 まで 3px | 追加されたテキストの開始は frameRound(1.95)（吸着しない） |
| (h) | ホイール / Ctrl+ホイール | | | スクロール +120 / 拡大率 ×1.15 |

### 6.3 修正前（baseline）と手順 2 のあと（step）に FAIL するはずの 36 行

```
FAIL a: 5 px before 0:02 snaps to 2.0
FAIL a: the 0:02 label turns yellow while snapped
FAIL a: 7 px before 0:02 snaps to 2.0
FAIL a: 5 px after 0:02 snaps to 2.0
FAIL a: after Alt is released, the next move snaps again
FAIL a: releasing the mouse keeps the snapped time
FAIL b: a ruler press 3 px before 0:02 snaps to 2.0 at once
FAIL b: a press on the top-left corner snaps to 2.0
FAIL b: the snapped red line shows over the header column (x=78)
FAIL b: a press on an empty track spot snaps to 10.0 (and deselects)
FAIL c: snaps to the clip boundary 3.5 (4 px away)
FAIL c: snaps to the text start 6.5 (3 px away)
FAIL c: snaps to the text end 8.8 (3 px away)
FAIL c: snaps to the audio end 2.6 (4 px away)
FAIL c: snaps to 0:00 (5 px away)
FAIL c: snaps to the end of the video 10.5 (4 px away)
FAIL c: the text start 4.1 (4 px) wins over the ruler number 4.0 (2 px)
FAIL d: at 100 px/s (step 1 s) snaps to 1.0
FAIL d: at 100 px/s the 0:01 label turns yellow
FAIL d: at 20 px/s (step 5 s) snaps to 5.0
FAIL d: at 20 px/s the 0:05 label turns yellow
FAIL d: after zooming during the drag, the next move uses the new step (1.0)
FAIL e: the left edge snaps to the ruler number 6.0
FAIL e: a yellow guide line runs across all tracks at 6.0
FAIL e: only the right edge is near the clip end: it snaps (start 8.2, end 10.5)
FAIL e: the guide line is at the right edge (10.5)
FAIL e: with Alt held, the existing edge snap is off too
FAIL f: trimming the text's right edge snaps to 10.0
FAIL f: the guide line is at the trimmed edge (10.0)
FAIL f: trimming the text's left edge snaps to 6.0 (end stays 8.8)
FAIL f: with Alt held, the left trim does not snap
FAIL f: moving the audio snaps its left edge to 2.0
FAIL f: the guide line is at 2.0 while moving the audio
FAIL f: with Alt held, the audio does not snap to the playhead (1.5)
FAIL f: trimming the audio's right edge snaps to 2.0
FAIL f: trimming the audio's left edge snaps to 2.0 (end stays 2.6)
```

- 修正前に OK になるはずのもの: `setup:` の 4 行、(i) の 2 行（`pixel-identical` の 7 行は修正前は数に入らない）、「吸着しない」「灰色のまま」「ガイド線が出ない」「今ある内容の点への吸着（3.5・押した時点の赤い線 1.5）」「Alt を離すとまた吸着（3.5）」「元に戻すが 1 回で戻る」（g）、(h) の 12 行、(j) の行。
- `e: with Alt held, the existing edge snap is off too` と `f: with Alt held, ...` の 3 行は、修正前は今ある吸着（3.5・赤い線 1.5）が Alt でも効いてしまうので FAIL になる（Alt の確認になっている）。
- `e: only the right edge ...` は修正前の不具合（proposal 1 章）を再現する（修正前は開始 8.267 のまま）。
- チェックの数: 修正前 101（ウィンドウが前面なら Alt+V の 1 行が増えて 102）、修正後はそれに `pixel-identical` の 7 行が加わる。

## 7. (j) Alt を押しながらのドラッグとメニューバー

### 7.1 Qt 6.8.3 の仕組み（ソースを読んだ結果。要約）

- [qmenubar.cpp の eventFilter](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/widgets/widgets/qmenubar.cpp): Alt の ShortcutOverride（`modifiers() == AltModifier`）が来るとメニューバーは「Alt が押された」と覚え、アプリ全体のイベントを見始める。その間にマウスの押す・離す・動かす、フォーカスの変化、ショートカット、Alt 以外のキーが来ると忘れる。忘れないまま Alt を離したときだけ、キーボード操作の状態（最初のメニューが選ばれる）になる。
- [qwindowskeymapper.cpp](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/plugins/platforms/windows/qwindowskeymapper.cpp): Windows では、Shift〜ScrollLock の修飾キーを押し続けても、繰り返しのキーイベントは送られない。Alt を押したイベントは「Alt なし」で作られ、[QKeyEvent::modifiers()](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/gui/kernel/qevent.cpp) が Alt キー自身の分を足すので、受け取る側では AltModifier になる。
- 見込み: 「Alt を押す → マウスを押す → ドラッグ → マウスを離す → Alt を離す」でも、「ドラッグ中に Alt を押す → 動かす → マウスを離す → Alt を離す」でも、Alt のあとにマウスのイベントがあるので、メニューバーは選ばれない（修正の前も後も）。メニューバーが選ばれるのは、Alt を押してから離すまでにマウスのイベントが 1 つもないとき（Windows のふつうの Alt の動きで、今回のコードは変えない）。

（ウェブの内容は言い換えて要約した）

### 7.2 ハーネスでの確かめ方（付録 A の (j)）

1. 対照: タイムラインにフォーカスを置き、Alt だけを押して離す → `menuBar()->activeAction()` が空でない（メニューバーが選ばれる）。Esc（メニューバーに直接送る）で戻す。ハーネスがメニューバーの動きを再現できていることと、Alt だけの動きが変わっていないことを示す。
2. Alt を押す → 目盛りを Alt 付きで押す → Alt 付きで 2 回動かす → Alt 付きで離す → Alt を離す → 選ばれていない。そのドラッグは吸着していない（tOf(223)）。
3. 目盛りを押して動かす → Alt を押す → Alt 付きで 2 回動かす → Alt 付きで離す → Alt を離す → 選ばれていない。
4. 2 と同じことをテキストの移動で → 選ばれていない。吸着していない（開始 6.083）。元に戻すを 1 回。
5. 参考（チェックにしない）: 目盛りを押したまま、マウスを動かさずに Alt を押して離す → 結果を `note:` に出すだけ（「selected」になる見込み。Windows のふつうの Alt の動き）。
6. ウィンドウが前面のときだけ: Alt+V で View のメニューが開く（ほかの Alt の使い方が変わっていない）。前面でなければ `note:` を出して飛ばし、8 章の手順で確かめる。
7. 最後にメニューバーが選ばれていない。

### 7.3 結果の扱い

- すべて OK（見込み）: 何も変えない。verification.md に 5 の `note:` の結果を書き、最後の報告で「マウスを止めたまま Alt だけを押して離すとメニューが選ばれるのは、Windows のふつうの動きのまま」と伝える。
- 1（対照）が OK にならない: まず `note: menu bar Alt navigation style hint` を見る。0 ならアプリ（Fusion）でも Alt だけでメニューは選ばれないので、(j) の心配はない。1 なのに OK にならないときは、フォーカス（`gView->setFocus()`）やキーの送り方を直してみる。それでも再現できなければ、そのチェックを `note:` に変え、理由を verification.md に書き、(j) は 8 章の手で確かめる手順だけにする。対策は入れない。
- 3 が FAIL（ドラッグ中に押した Alt でメニューが選ばれた）: 付録 C を入れる（ドラッグ中の Alt の ShortcutOverride だけをタイムラインで受け取り、メニューバーに渡さない。ドラッグしていないときの Alt と Alt+V などは変わらない）。ビルドとハーネス（`fixed`）をやり直し、1・6 が OK のままであることも確かめる。
- 2 か 4 が FAIL（Alt をマウスより先に押したとき）: 7.1 の見込みと合わない。付録 C では直らないので、変更を入れずに止め、ログを付けて報告する。

## 8. 手で確かめる手順（最後の報告にこのまま書く）

1. アプリを閉じてから `build.cmd` をダブルクリックします（今回は起動中のアプリを止めないよう配置していないので、`native\dist` の exe はまだ古いままです）。
2. 動画を 2 本タイムラインに置き、テキストと BGM を 1 つずつ足します。
3. 目盛り（時間の数字の行）をつかんでゆっくり左右にドラッグします。「0:02」「0:04」などの数字の所で赤い線が少し止まり、止まっている間はその数字が黄色になります。さらに動かすと離れ、数字は灰色に戻ります。マウスを離すと黄色は消えます。
4. 数字のすぐ近くをクリックすると、赤い線が数字にぴったり合います。左上の角（合計時間の所）やトラックの空いた所をクリックしても同じです。
5. 赤い線をクリップの境目、テキストや BGM の端、0:00、動画の終わりに近づけると、そこでも止まります（数字の位置でなければ黄色にはなりません）。
6. Alt を押したままドラッグすると、どこにも止まりません。Alt を離して動かすと、また止まります。
7. 拡大・縮小すると、止まる間隔が目盛りの数字に合わせて変わります（拡大すると 1 秒・0.5 秒、縮小すると 5 秒・10 秒など）。
8. テキストや BGM をドラッグすると、左端か右端が、数字の位置・つかんだときの赤い線・クリップの境目・ほかの端に近づいたときに止まり、黄色の縦線がタイムライン全体に出ます。離すと消えます。右端だけをクリップの境目に近づけても止まります（直した不具合）。端をつかんで長さを変えるときも同じです。
9. テキストを動かしたあと Ctrl+Z を 1 回押すと、動かす前に戻ります。
10. Alt を押したままテキストか赤い線をドラッグし、マウスを離してから 1 秒ほど待って Alt を離します。メニューバーの「ファイル」が選ばれた状態になりません。続けて ← → を押すと、赤い線が 1 フレームずつ動きます。
11. 何もしていないときに Alt だけを押して離すと、今までどおりメニューバーが選ばれます（Esc で戻ります）。Alt+V で「表示」メニューが開きます。
12. ビデオのクリップの並べ替えと端のドラッグ、ライブラリからのドラッグ＆ドロップ、テキストの行のダブルクリックでのテキストの追加、ホイール・Ctrl+ホイール、再生中の自動スクロールは今までどおりです（吸着しません）。
13. 数字や端のすぐ近く（8px 以内）に置きたいときは、Alt を押しながらドラッグするか、← → で 1 フレームずつ動かしてください。

## 9. 確かめられないこと（verification.md にも書く）

- 本物の Windows のキー入力（WM_SYSKEYDOWN/UP、修飾キーの繰り返し、DefWindowProc）: ハーネスは QWindowSystemInterface から入れるので、Windows のキーの変換は通らない。7.1 のソースの読み取りと 8 章の手順 10・11 で確かめる。
- MainWindow の ←→・Home/End、ライブラリからのドロップ、実際の再生と followPlayhead、プレビューの映像: ハーネスでは作っていない（コードは変えていない）。
- DPR 1.25 以外、タッチパッドでの感じ、実際のアプリでの手の操作（配置していないため）。
- 赤い線が数字に吸着したときの目盛りの線の黄色: 赤い線の真下に重なるので画像では確かめられない（コードで確かめる）。

---

## 付録 A: 一時的なハーネス（リポジトリの外。`C:\e-editor-dev\tmp\kv-magnet\`）

パスは ASCII にする（ユーザーの %TEMP% には日本語が入っていて MinGW が動かないため、run.ps1 で TEMP を差し替える）。コンパイルが通らないとき（include の不足など）はハーネスだけを直す。チェックの意味と数値は変えない。

`C:\e-editor-dev\tmp\kv-magnet\CMakeLists.txt`:
```cmake
# Temporary harness for the timeline magnet (snap). Delete this folder when done.
cmake_minimum_required(VERSION 3.21)
project(KvMagnetCheck LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network Test)

set(KV "C:/git/JPHACK/e-editor/native")
set(FFMPEG_DIR "C:/e-editor-dev/ffmpeg")
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
qt_add_executable(kv-magnet magnetcheck.cpp ${KV_FILES} "${KV}/resources/app.qrc")
target_include_directories(kv-magnet PRIVATE "${KV}/src")
target_compile_definitions(kv-magnet PRIVATE UNICODE _UNICODE NOMINMAX KV_VERSION="0.1.0")
target_link_libraries(kv-magnet PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network Qt6::Test ffmpeg miniaudio ole32 shell32 uuid)
```

`C:\e-editor-dev\tmp\kv-magnet\run.ps1`（ASCII だけ。子の powershell で動くので、環境変数の変更は残らない）:
```powershell
# Temporary: build and run the magnet check harness. Delete this folder when done.
$ErrorActionPreference = 'Continue'
$d = 'C:\e-editor-dev'
$h = Join-Path $d 'tmp\kv-magnet'
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
& "$h\build\kv-magnet.exe"
exit $LASTEXITCODE
```

`C:\e-editor-dev\tmp\kv-magnet\magnetcheck.cpp`（UTF-8 BOM なし。表示する文字は ASCII だけ）。前半（道具）:
```cpp
// 一時的な確認用（タイムラインのマグネット＝吸着）。確認が終わったらフォルダーごと消す
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QScrollBar>
#include <QStyle>
#include <QTest>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "core/editor.h"
#include "core/timeline.h"
#include "ui/theme.h"
#include "ui/timelinewidget.h"

// timelinewidget.cpp:24-29 と同じ値。行の位置はテキスト 1 段・オーディオ 1 段、縦スクロール 0 のとき
static constexpr int kHeadW = 108;
static constexpr int kRulerH = 26;
static constexpr int kTextMid = 42;    // テキストの行 26..58（箱は 28..52）
static constexpr int kVideoMid = 91;   // ビデオの行 58..124
static constexpr int kAudioMid = 147;  // オーディオの行 124..170（箱は 126..164）
static constexpr int kTracksEnd = 170;
static const char* const kShots = "C:/e-editor-dev/tmp/kv-magnet/shots";
static const char* const kRefs = "C:/e-editor-dev/tmp/kv-magnet/baseline";

static int failures = 0;
static int mismatches = 0;

static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "OK  " : "FAIL", what);
  std::fflush(stdout);
  if (!ok) ++failures;
}

// ビューポートに届いたマウスの位置と修飾キーを覚えておく（座標のずれと Alt の届き方を確かめるため）
class Recorder : public QObject {
 public:
  QPointF last{-9999, -9999};
  Qt::KeyboardModifiers mods;

 protected:
  bool eventFilter(QObject*, QEvent* e) override {
    const QEvent::Type t = e->type();
    if (t == QEvent::MouseButtonPress || t == QEvent::MouseButtonRelease || t == QEvent::MouseButtonDblClick || t == QEvent::MouseMove) {
      last = static_cast<QMouseEvent*>(e)->position();
      mods = static_cast<QMouseEvent*>(e)->modifiers();
    }
    return false;
  }
};

static kv::TimelineView* gView = nullptr;
static QWindow* gWin = nullptr;
static Recorder* gRec = nullptr;

// ---- マウス（OS から来るのと同じ QWindowSystemInterface の経路。qt_handleMouseEvent が自分で実ピクセルに直すので DPR は掛けない） ----
static QPoint toWin(QPoint vp) {
  return gView->viewport()->mapTo(gView->window(), vp);
}
static void expectAt(QPoint vp, Qt::KeyboardModifiers m) {
  if (std::abs(gRec->last.x() - vp.x()) > 0.01 || std::abs(gRec->last.y() - vp.y()) > 0.01 || gRec->mods != m) {
    ++mismatches;
    std::printf("note: mouse arrived at (%.2f, %.2f) mods 0x%x, intended (%d, %d) mods 0x%x\n", gRec->last.x(), gRec->last.y(),
                unsigned(gRec->mods.toInt()), vp.x(), vp.y(), unsigned(m.toInt()));
  }
  gRec->last = QPointF(-9999, -9999);
}
static void press(QPoint vp, Qt::KeyboardModifiers m = Qt::NoModifier) {
  QTest::mousePress(gWin, Qt::LeftButton, m, toWin(vp), 6000);  // 時刻を 6 秒進めてダブルクリックにしない
  expectAt(vp, m);
}
static void moveTo(QPoint vp, Qt::KeyboardModifiers m = Qt::NoModifier) {
  QTest::mouseEvent(QTest::MouseMove, gWin, Qt::NoButton, m, toWin(vp));  // QTest::mouseMove は修飾キーを渡せない
  expectAt(vp, m);
}
static void release(QPoint vp, Qt::KeyboardModifiers m = Qt::NoModifier) {
  QTest::mouseRelease(gWin, Qt::LeftButton, m, toWin(vp));
  expectAt(vp, m);
}
static void click(QPoint vp) {
  press(vp);
  release(vp);
}
static void dclick(QPoint vp) {
  QTest::mouseDClick(gWin, Qt::LeftButton, Qt::NoModifier, toWin(vp), 6000);
  expectAt(vp, Qt::NoModifier);
}

// ---- Alt キー（Windows と同じ形。押したイベントは修飾キーなしで作り、QKeyEvent::modifiers() が AltModifier を足す） ----
static void altDown() {
  QTest::simulateEvent(gWin, true, Qt::Key_Alt, Qt::NoModifier, QString(), false);
}
static void altUp() {
  QTest::simulateEvent(gWin, false, Qt::Key_Alt, Qt::AltModifier, QString(), false);
}

// ---- 時間とスクロール（TimelineView::tOf / xOf と同じ式） ----
static int scrollX() {
  return gView->horizontalScrollBar()->value();
}
static void setScroll(int v) {
  gView->horizontalScrollBar()->setValue(v);
  QApplication::processEvents();
}
static double pps() {
  return kv::editor().pxPerSec();
}
static void setZoom(double v) {
  kv::editor().setPxPerSec(v);
  QApplication::processEvents();
}
static double tAt(double x) {
  return (x - kHeadW + scrollX()) / pps();
}
static double xAt(double t) {
  return kHeadW + t * pps() - scrollX();
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
static bool isGuide(const QColor& c) {  // 挿入位置の線と同じ #ffd44a（塗りつぶしなので色はそのまま）
  return std::abs(c.red() - 0xff) <= 6 && std::abs(c.green() - 0xd4) <= 6 && std::abs(c.blue() - 0x4a) <= 6;
}
// 論理座標 (x, y) の左右 1.2 の範囲に pred に合う画素があるか
static bool colorNear(const Shot& s, double x, double y, bool (*pred)(const QColor&)) {
  const int py = std::clamp(int(std::floor(y * s.dpr)), 0, s.img.height() - 1);
  const int a = std::max(0, int(std::floor((x - 1.2) * s.dpr)));
  const int b = std::min(s.img.width() - 1, int(std::floor((x + 1.2) * s.dpr)));
  for (int px = a; px <= b; ++px) {
    if (pred(s.img.pixelColor(px, py))) return true;
  }
  return false;
}
// 黄色のガイド線が、テキスト・ビデオ・オーディオの行とその下まで x に通っているか
static bool guideAcross(const Shot& s, double x) {
  const double bottom = s.img.height() / s.dpr - 4;
  return colorNear(s, x, kTextMid, isGuide) && colorNear(s, x, kVideoMid, isGuide) && colorNear(s, x, kAudioMid, isGuide) &&
         colorNear(s, x, bottom, isGuide);
}
// トラック（目盛りより下）にある、ガイド線の色の画素の数
static int guidePx(const Shot& s) {
  int n = 0;
  for (int py = int(std::ceil(kRulerH * s.dpr)); py < s.img.height(); ++py) {
    for (int px = 0; px < s.img.width(); ++px) {
      if (isGuide(s.img.pixelColor(px, py))) ++n;
    }
  }
  return n;
}
// 目盛りの数字 t のラベルの黄色さ: 文字の画素の (赤 - 青) の平均。灰色の文字なら 0 付近、黄色なら 100 前後。
// ClearType の色のにじみは文字の左右で打ち消し合う。赤い線のつまみ（今の再生位置 ±7.5）は数えない
static double labelWarmth(const Shot& s, double t) {
  const double x = std::round(xAt(t)) + 0.5;
  const double knob = xAt(now());
  long long sum = 0;
  int n = 0;
  const int px1 = std::min(s.img.width(), int(std::ceil((x + 30) * s.dpr)));
  for (int py = int(std::floor(1 * s.dpr)); py < int(std::ceil(12 * s.dpr)); ++py) {
    for (int px = std::max(0, int(std::floor((x + 3) * s.dpr))); px < px1; ++px) {
      if (std::abs((px + 0.5) / s.dpr - knob) < 7.5) continue;
      const QColor c = s.img.pixelColor(px, py);
      if (std::max({c.red(), c.green(), c.blue()}) < 0x40) continue;  // 背景（#1b1b1b）は数えない
      sum += c.red() - c.blue();
      ++n;
    }
  }
  return n >= 4 ? double(sum) / n : 0;
}
static bool labelsGray(const Shot& s, std::initializer_list<double> ts) {
  for (double t : ts) {
    const double w = labelWarmth(s, t);
    if (w >= 25) {
      std::printf("note: label %.1f looks yellow (warmth %.1f)\n", t, w);
      return false;
    }
  }
  return true;
}
// 1 回目（修正前のコード）の画像を基準として baseline に保存し、2 回目からは全画素を比べる
static void compareRef(const Shot& s, const char* name, const char* what) {
  const QString path = QStringLiteral("%1/ref-%2.png").arg(QLatin1String(kRefs), QLatin1String(name));
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
```

後半（main。前半のすぐあとに続ける）:
```cpp
int main(int argc, char** argv) {
  QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  QApplication app(argc, argv);
  kv::theme::apply(app);
  QDir().mkpath(QLatin1String(kShots));
  QDir().mkpath(QLatin1String(kRefs));

  // アプリと同じく、メニューバーのあるメインウィンドウにタイムラインを置く（(j) で使う）
  QMainWindow mw;
  mw.menuBar()->addMenu(QStringLiteral("&File"))->addAction(QStringLiteral("New"));
  QMenu* viewMenu = mw.menuBar()->addMenu(QStringLiteral("&View"));
  viewMenu->addAction(QStringLiteral("Zoom in"));
  auto* tw = new kv::TimelineWidget;
  mw.setCentralWidget(tw);
  mw.resize(900, 328 + mw.menuBar()->sizeHint().height());
  // 本物のカーソルの下には出さない（ドラッグ中に本物のマウスの動きが混ざらないように）
  const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
  QPoint topLeft(avail.left() + 40, avail.top() + 40);
  if (QRect(topLeft, mw.size()).adjusted(-40, -40, 40, 40).contains(QCursor::pos())) topLeft.setY(avail.bottom() - mw.height() - 80);
  mw.move(topLeft);
  mw.show();
  if (!QTest::qWaitForWindowExposed(&mw)) std::printf("note: window was not exposed\n");
  mw.activateWindow();
  const bool active = QTest::qWaitForWindowActive(&mw, 2000);
  gView = tw->view();
  gWin = mw.windowHandle();
  Recorder recorder;
  gRec = &recorder;
  gView->viewport()->installEventFilter(&recorder);

  kv::Editor& ed = kv::editor();
  ed.addAsset(makeAsset("va", "clip-a.mp4", kv::AssetKind::Video, 3.5));
  ed.addAsset(makeAsset("vb", "clip-b.mp4", kv::AssetKind::Video, 7));
  ed.addAsset(makeAsset("au", "bgm.mp3", kv::AssetKind::Audio, 2.6));
  ed.addToTimeline({QStringLiteral("va"), QStringLiteral("vb")});  // 0..3.5 秒と 3.5..10.5 秒
  ed.addToTimeline({QStringLiteral("au")});                         // 0..2.6 秒
  const QString textId = ed.addText(QStringLiteral("basic"), 0.0);
  ed.updateText(textId, [](kv::TextItem& t) {
    t.start = 6.5;  // 6.5..8.8 秒
    t.duration = 2.3;
  });
  const QString audioId = ed.project().audios.at(0).id;
  const QString clipA = ed.project().clips.at(0).id;
  const QString clipB = ed.project().clips.at(1).id;
  ed.select({});
  setZoom(60);  // 最初のクリップで「全体を表示」が走るので、拡大率を決め直す
  setScroll(0);
  ed.seek(0);
  const int viewH = gView->viewport()->height();
  std::printf("note: device pixel ratio %.2f, viewport %dx%d, window active %d\n", gWin->devicePixelRatio(), gView->viewport()->width(), viewH,
              int(active));
  auto textStart = [&] { return ed.text(textId)->start; };
  auto textEnd = [&] { return ed.text(textId)->start + ed.text(textId)->duration; };
  auto audioStart = [&] { return ed.audio(audioId)->start; };
  auto audioEnd = [&] { return ed.audio(audioId)->start + kv::audioDuration(*ed.audio(audioId)); };
  check(ed.layout().total == 10.5 && ed.contentEnd() == 10.5 && same(textStart(), 6.5) && same(textEnd(), 8.8) && same(audioStart(), 0) &&
            same(audioEnd(), 2.6) && pps() == 60,
        "setup: clips 0-3.5-10.5 s, text 6.5-8.8 s, audio 0-2.6 s at 60 px/s");
  check(gView->horizontalScrollBar()->maximum() >= 300 && gView->verticalScrollBar()->value() == 0 && viewH >= kTracksEnd + 40 &&
            gView->viewport()->width() >= 860,
        "setup: timeline can scroll to 300 and shows all rows");
  click({20, 160});  // 見出しのオーディオの行の何もないところ（何も起きない）
  press({20, 160}, Qt::AltModifier);
  release({20, 160}, Qt::AltModifier);
  check(mismatches == 0, "setup: mouse events (and the Alt modifier) land at the intended viewport position");

  // ドラッグのあと: 元に戻すを 1 回だけして、ドラッグの前に戻るかを確かめる。違ったら、次の確認のために置き直す
  auto undoOnce = [&](const char* what) {
    ed.undo();
    const bool ok =
        same(textStart(), 6.5) && same(textEnd(), 8.8) && same(audioStart(), 0) && same(ed.audio(audioId)->in, 0) && same(audioEnd(), 2.6);
    check(ok, what);
    if (!ok) {
      ed.updateText(textId, [](kv::TextItem& t) {
        t.start = 6.5;
        t.duration = 2.3;
      });
      ed.updateAudio(audioId, [](kv::AudioItem& a) {
        a.start = 0;
        a.in = 0;
        a.out = 2.6;
      });
    }
    ed.select({});
  };
  // 目盛りの 600（8.2 秒。近くに吸着先はない）を押して x までドラッグし、時間を確かめて離す
  auto scrubCase = [&](int x, double expect, const char* what) {
    press({600, 10});
    moveTo({x, 10});
    check(same(now(), expect), what);
    release({x, 10});
  };

  // ---- (i) 吸着していないときの見た目は修正前と画素まで同じ（ほかの確認より前に、決まった状態でとる） ----
  ed.seek(tAt(400));
  compareRef(grabView("i-idle"), "i-idle", "i: idle timeline is pixel-identical to the unmodified code");
  press({600, 10});
  moveTo({216, 10});  // 1.8 秒（0:02 から 12px）
  compareRef(grabView("i-scrub-free"), "i-scrub-free", "i: scrubbing away from snap points is pixel-identical");
  moveTo({223, 10}, Qt::AltModifier);  // 0:02 から 5px でも Alt なので吸着しない
  compareRef(grabView("i-scrub-alt"), "i-scrub-alt", "i: scrubbing with Alt held is pixel-identical");
  release({223, 10}, Qt::AltModifier);
  ed.seek(tAt(400));
  press({558, kTextMid});   // テキストの真ん中
  moveTo({540, kTextMid});  // 開始 6.2 秒（どちらの端も吸着しない）
  compareRef(grabView("i-move-free"), "i-move-free", "i: moving a text away from snap points is pixel-identical");
  release({540, kTextMid});
  undoOnce("i: (setup) one undo puts the text back");
  setZoom(100);
  compareRef(grabView("i-idle-100"), "i-idle-100", "i: the ruler at 100 px/s is pixel-identical");
  setZoom(20);
  compareRef(grabView("i-idle-20"), "i-idle-20", "i: the ruler at 20 px/s is pixel-identical");
  setZoom(60);
  setScroll(150);
  ed.seek(tAt(18));  // 1.0 秒。赤い線は見出しの列の上（x = 18）
  {
    const Shot s = grabView("i-idle-head");
    compareRef(s, "i-idle-head", "i: the red line over the header column is pixel-identical");
    check(colorNear(s, 18, kVideoMid, isRed) && colorNear(s, 18, 4, isRed), "i: the red line and knob are still drawn over the header column");
  }
  setScroll(0);

  // ---- (a) 起動したときの拡大率（60px/秒、数字の間隔 2 秒）で目盛りをドラッグする ----
  ed.select({});
  const int rev0 = ed.revision();
  press({400, 10});  // 4.87 秒（近くに吸着先はない）
  check(same(now(), tAt(400)), "a: a ruler press away from snap points seeks to tOf(x)");
  moveTo({216, 10});
  check(same(now(), tAt(216)), "a: 12 px before 0:02 does not snap (stays tOf(x))");
  moveTo({223, 10});
  check(same(now(), 2.0), "a: 5 px before 0:02 snaps to 2.0");
  {
    const Shot s = grabView("a-snap-2s");
    check(labelWarmth(s, 2.0) >= 50, "a: the 0:02 label turns yellow while snapped");
    check(labelsGray(s, {0, 4, 6, 8, 10, 12}), "a: the other labels stay gray");
    check(guidePx(s) == 0, "a: no guide line across the tracks while scrubbing");
    check(colorNear(s, xAt(now()), kVideoMid, isRed) && colorNear(s, xAt(now()), 4, isRed), "a: the red line and knob are drawn at the playhead");
  }
  moveTo({221, 10});
  check(same(now(), 2.0), "a: 7 px before 0:02 snaps to 2.0");
  moveTo({219, 10});
  check(same(now(), tAt(219)), "a: 9 px before 0:02 does not snap");
  moveTo({233, 10});
  check(same(now(), 2.0), "a: 5 px after 0:02 snaps to 2.0");
  moveTo({223, 10}, Qt::AltModifier);
  check(same(now(), tAt(223)), "a: with Alt held, 5 px before 0:02 does not snap");
  {
    const Shot s = grabView("a-alt");
    check(labelsGray(s, {0, 2, 4, 6, 8, 10, 12}), "a: with Alt held, no label is yellow");
  }
  moveTo({225, 10});
  check(same(now(), 2.0), "a: after Alt is released, the next move snaps again");
  release({225, 10});
  check(same(now(), 2.0), "a: releasing the mouse keeps the snapped time");
  {
    const Shot s = grabView("a-released");
    check(labelsGray(s, {0, 2, 4, 6, 8, 10, 12}), "a: after release every label is gray again");
  }
  check(ed.revision() == rev0, "a: scrubbing adds nothing to the undo history");

  // ---- (b) 押した瞬間も吸着する（目盛り・左上の角・トラックの空いた所） ----
  press({225, 10});  // 1.95 秒（0:02 まで 3px）
  check(same(now(), 2.0), "b: a ruler press 3 px before 0:02 snaps to 2.0 at once");
  release({225, 10});
  press({225, 10}, Qt::AltModifier);
  check(same(now(), tAt(225)), "b: a ruler press with Alt held does not snap");
  release({225, 10}, Qt::AltModifier);
  setScroll(150);  // 0:02 が左上の角の下（x = 78）に来る
  ed.seek(5.0);
  press({83, 10});  // 2.083 秒（0:02 まで 5px）
  check(same(now(), 2.0), "b: a press on the top-left corner snaps to 2.0");
  {
    const Shot s = grabView("b-corner");
    check(colorNear(s, 78, kVideoMid, isRed), "b: the snapped red line shows over the header column (x=78)");
  }
  release({83, 10});
  press({83, 10}, Qt::AltModifier);
  check(same(now(), tAt(83)), "b: a corner press with Alt held does not snap");
  release({83, 10}, Qt::AltModifier);
  setScroll(0);
  ed.select({kv::Selection::Text, textId});
  press({705, kTextMid});  // テキストの行の空いた所、9.95 秒（0:10 まで 3px）
  check(same(now(), 10.0) && !ed.selection(), "b: a press on an empty track spot snaps to 10.0 (and deselects)");
  release({705, kTextMid});

  // ---- (c) 赤い線は内容の点にも吸着する。両方が近いときは内容の点が先 ----
  scrubCase(314, 3.5, "c: snaps to the clip boundary 3.5 (4 px away)");
  scrubCase(501, 6.5, "c: snaps to the text start 6.5 (3 px away)");
  scrubCase(633, 8.8, "c: snaps to the text end 8.8 (3 px away)");
  scrubCase(268, 2.6, "c: snaps to the audio end 2.6 (4 px away)");
  scrubCase(113, 0.0, "c: snaps to 0:00 (5 px away)");
  scrubCase(734, 10.5, "c: snaps to the end of the video 10.5 (4 px away)");
  press({600, 10});
  moveTo({314, 10});
  {
    const Shot s = grabView("c-content");
    check(labelsGray(s, {0, 2, 4, 6, 8, 10, 12}) && guidePx(s) == 0, "c: a snap to a point that is not a ruler number shows no yellow");
  }
  release({314, 10});
  ed.updateText(textId, [](kv::TextItem& t) { t.start = 4.1; });  // 4.1..6.4 秒
  scrubCase(350, 4.1, "c: the text start 4.1 (4 px) wins over the ruler number 4.0 (2 px)");
  undoOnce("c: (setup) one undo puts the text back");
  ed.seek(1.0);
  scrubCase(171, tAt(171), "c: the playhead's own old position (1.0, 3 px) is not a snap point");

  // ---- (d) 拡大率で数字の間隔が変わる ----
  scrubCase(165, tAt(165), "d: at 60 px/s (step 2 s) 0.95 s does not snap to 1.0");
  setZoom(100);  // 数字の間隔 1 秒
  press({440, 10});  // 3.32 秒（近くに吸着先はない）
  moveTo({203, 10});  // 0.95 秒（0:01 まで 5px）
  check(same(now(), 1.0), "d: at 100 px/s (step 1 s) snaps to 1.0");
  {
    const Shot s = grabView("d-100");
    check(labelWarmth(s, 1.0) >= 50, "d: at 100 px/s the 0:01 label turns yellow");
  }
  release({203, 10});
  setZoom(20);  // 数字の間隔 5 秒
  press({296, 10});  // 9.4 秒（近くに吸着先はない）
  moveTo({210, 10});  // 5.1 秒（0:05 まで 2px）
  check(same(now(), 5.0), "d: at 20 px/s (step 5 s) snaps to 5.0");
  {
    const Shot s = grabView("d-20");
    check(labelWarmth(s, 5.0) >= 50, "d: at 20 px/s the 0:05 label turns yellow");
  }
  moveTo({147, 10});  // 1.95 秒（2.0 は数字ではない）
  check(same(now(), tAt(147)), "d: at 20 px/s 2.0 is not a snap point");
  release({147, 10});
  // ドラッグ中に Ctrl+ホイールで拡大したら、次に動かしたときの間隔を使う
  setZoom(60);
  press({600, 10});
  for (int k = 0; k < 2; ++k) {
    const QPointF p(kHeadW, 10);  // 見出しの境目で拡大するので、スクロールは 0 のまま
    QWheelEvent we(p, gView->viewport()->mapToGlobal(p), QPoint(), QPoint(0, 120), Qt::LeftButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(gView->viewport(), &we);
  }
  check(scrollX() == 0 && pps() > 70 && pps() < 140, "d: (setup) Ctrl+wheel during the drag zoomed in to the 1 s step");
  moveTo({184, 10});  // 0.958 秒（0:01 まで約 3px）
  check(same(now(), 1.0), "d: after zooming during the drag, the next move uses the new step (1.0)");
  release({184, 10});
  setZoom(60);
  setScroll(0);

  // ---- (e) テキストの移動（左端・右端・Alt・押したときの赤い線） ----
  ed.select({});
  ed.seek(1.5);  // 押したときの赤い線（1.5 秒）も吸着先になる
  press({558, kTextMid});
  check(ed.selection() == kv::Selection{kv::Selection::Text, textId}, "e: (setup) pressing the middle of the text selects it");
  moveTo({545, kTextMid});  // 開始 6.283 秒（吸着しない）
  moveTo({533, kTextMid});  // 開始 6.083 秒 → 左端が 0:06 に吸着
  check(same(textStart(), 6.0), "e: the left edge snaps to the ruler number 6.0");
  {
    const Shot s = grabView("e-guide-left");
    check(guideAcross(s, 468), "e: a yellow guide line runs across all tracks at 6.0");
    check(labelsGray(s, {0, 2, 4, 6, 8, 10, 12}), "e: no ruler label turns yellow while moving a text");
  }
  release({533, kTextMid});
  {
    const Shot s = grabView("e-released");
    check(guidePx(s) == 0, "e: the guide line disappears after release");
  }
  undoOnce("g: one undo after a text move restores it");
  press({558, kTextMid});
  moveTo({600, kTextMid});  // 開始 7.2 秒（吸着しない）
  moveTo({664, kTextMid});  // 開始 8.267 秒、右端 10.567 秒 → 右端だけがクリップの終わり 10.5 に近い
  check(same(textStart(), 10.5 - 2.3) && same(textEnd(), 10.5), "e: only the right edge is near the clip end: it snaps (start 8.2, end 10.5)");
  {
    const Shot s = grabView("e-guide-right");
    check(guideAcross(s, 738), "e: the guide line is at the right edge (10.5)");
  }
  release({664, kTextMid});
  undoOnce("g: one undo after a right-edge snap restores the text");
  press({558, kTextMid});
  moveTo({545, kTextMid});
  moveTo({381, kTextMid});  // 開始 3.55 秒 → 今ある吸着でクリップの境目 3.5 に
  check(same(textStart(), 3.5), "e: the left edge still snaps to the clip boundary 3.5 (existing snap)");
  moveTo({380, kTextMid}, Qt::AltModifier);  // 開始 3.533 秒。Alt なので吸着しない
  check(same(textStart(), 6.5 + (380.0 - 558.0) / 60.0), "e: with Alt held, the existing edge snap is off too");
  {
    const Shot s = grabView("e-alt");
    check(guidePx(s) == 0, "e: no guide line while Alt is held");
  }
  moveTo({379, kTextMid});  // 開始 3.517 秒 → また 3.5 に
  check(same(textStart(), 3.5), "e: releasing Alt snaps again");
  release({379, kTextMid});
  undoOnce("g: one undo after an Alt drag restores the text");
  press({558, kTextMid});
  moveTo({545, kTextMid});
  moveTo({261, kTextMid});  // 開始 1.55 秒 → 押したときの赤い線 1.5 に（今と同じ）
  check(same(textStart(), 1.5), "e: the playhead position at press (1.5) is still a snap point");
  release({261, kTextMid});
  undoOnce("g: one undo after a playhead snap restores the text");

  // ---- (f) テキストの長さの変更（両端）と、オーディオの移動と長さの変更 ----
  press({632, kTextMid});   // テキストの右端のつまみ
  moveTo({650, kTextMid});  // 終わり 9.1 秒（吸着しない）
  moveTo({699, kTextMid});  // 終わり 9.917 秒 → 0:10 に吸着
  check(same(textStart(), 6.5) && same(textEnd(), 10.0), "f: trimming the text's right edge snaps to 10.0");
  {
    const Shot s = grabView("f-trim-right");
    check(guideAcross(s, 708), "f: the guide line is at the trimmed edge (10.0)");
  }
  release({699, kTextMid});
  undoOnce("g: one undo after a text right trim restores it");
  press({502, kTextMid});   // テキストの左端のつまみ
  moveTo({490, kTextMid});  // 開始 6.3 秒（吸着しない）
  moveTo({476, kTextMid});  // 開始 6.067 秒 → 0:06 に吸着
  check(same(textStart(), 6.0) && same(textEnd(), 8.8), "f: trimming the text's left edge snaps to 6.0 (end stays 8.8)");
  release({476, kTextMid});
  undoOnce("g: one undo after a text left trim restores it");
  press({502, kTextMid});
  moveTo({490, kTextMid});
  moveTo({325, kTextMid});  // 開始 3.55 秒 → クリップの境目 3.5（今ある吸着）
  check(same(textStart(), 3.5), "f: the left trim still snaps to the clip boundary 3.5 (existing snap)");
  moveTo({324, kTextMid}, Qt::AltModifier);  // 開始 3.533 秒。Alt なので吸着しない
  check(same(textStart(), 6.5 + (324.0 - 502.0) / 60.0), "f: with Alt held, the left trim does not snap");
  release({324, kTextMid}, Qt::AltModifier);
  undoOnce("g: one undo after an Alt trim restores the text");
  press({186, kAudioMid});  // オーディオの真ん中
  check(ed.selection() == kv::Selection{kv::Selection::Audio, audioId}, "f: (setup) pressing the middle of the audio selects it");
  moveTo({200, kAudioMid});  // 開始 0.233 秒（吸着しない）
  moveTo({301, kAudioMid});  // 開始 1.917 秒 → 0:02 に吸着
  check(same(audioStart(), 2.0), "f: moving the audio snaps its left edge to 2.0");
  {
    const Shot s = grabView("f-audio-move");
    check(guideAcross(s, 228), "f: the guide line is at 2.0 while moving the audio");
  }
  release({301, kAudioMid});
  undoOnce("g: one undo after an audio move restores it");
  press({186, kAudioMid});
  moveTo({200, kAudioMid});
  moveTo({279, kAudioMid}, Qt::AltModifier);  // 開始 1.55 秒。Alt がなければ赤い線 1.5 に吸着する
  check(same(audioStart(), (279.0 - 186.0) / 60.0), "f: with Alt held, the audio does not snap to the playhead (1.5)");
  release({279, kAudioMid}, Qt::AltModifier);
  undoOnce("g: one undo after an Alt audio move restores it");
  press({260, kAudioMid});   // オーディオの右端のつまみ
  moveTo({250, kAudioMid});  // 終わり 2.433 秒（吸着しない）
  moveTo({228, kAudioMid});  // 終わり 2.067 秒 → 0:02 に吸着
  check(same(audioStart(), 0.0) && same(audioEnd(), 2.0), "f: trimming the audio's right edge snaps to 2.0");
  release({228, kAudioMid});
  undoOnce("g: one undo after an audio right trim restores it");
  press({112, kAudioMid});   // オーディオの左端のつまみ
  moveTo({150, kAudioMid});  // 開始 0.633 秒（吸着しない）
  moveTo({228, kAudioMid});  // 開始 1.933 秒 → 0:02 に吸着
  check(same(audioStart(), 2.0) && same(ed.audio(audioId)->in, 2.0) && same(audioEnd(), 2.6),
        "f: trimming the audio's left edge snaps to 2.0 (end stays 2.6)");
  release({228, kAudioMid});
  undoOnce("g: one undo after an audio left trim restores it");

  // ---- (h) 変えていない操作 ----
  // ビデオのクリップの並べ替え（吸着しない。黄色はビデオの行の挿入位置の線だけ）
  press({213, kVideoMid});  // クリップ A の真ん中
  moveTo({230, kVideoMid});
  moveTo({650, kVideoMid});
  {
    const Shot s = grabView("h-clip-move");
    check(colorNear(s, 738, kVideoMid, isGuide) && !colorNear(s, 738, kTextMid, isGuide) && !colorNear(s, 738, kAudioMid, isGuide),
          "h: moving a clip shows only the insertion line in the video row");
  }
  release({650, kVideoMid});
  check(ed.project().clips.size() == 2 && ed.project().clips[0].id == clipB && ed.project().clips[1].id == clipA,
        "h: dragging a clip past the next one reorders the clips");
  ed.undo();
  check(ed.project().clips[0].id == clipA && same(ed.layout().total, 10.5), "h: (setup) one undo puts the clips back");
  // ビデオのクリップの端（長さの変更）は吸着しない
  press({734, kVideoMid});  // クリップ B の右端のつまみ
  moveTo({720, kVideoMid});
  moveTo({708, kVideoMid});  // 終わり 10.067 秒（0:10 まで 4px でも吸着しない）
  check(same(ed.layout().total, 302.0 / 30), "h: trimming a clip's edge does not snap (end 10.067, not 10.0)");
  {
    const Shot s = grabView("h-clip-trim");
    check(guidePx(s) == 0, "h: no guide line while trimming a clip");
  }
  release({708, kVideoMid});
  ed.undo();
  check(same(ed.layout().total, 10.5), "h: (setup) one undo restores the clip length");
  // クリップやテキストを押して離すと、押した位置へ赤い線が動く（吸着しない）
  ed.select({});
  click({225, kVideoMid});  // クリップ A の上、1.95 秒（0:02 まで 3px）
  check(same(now(), tAt(225)), "h: clicking a clip moves the playhead to tOf(x), not snapped");
  click({585, kTextMid});  // テキストの上、7.95 秒（0:08 まで 3px）
  check(same(now(), tAt(585)), "h: clicking a text moves the playhead to tOf(x), not snapped");
  // ダブルクリックで追加するテキストの位置は今のまま
  ed.select({});
  const size_t texts0 = ed.project().texts.size();
  dclick({225, kTextMid});  // テキストの行の空いた所、1.95 秒
  check(ed.project().texts.size() == texts0 + 1 && same(ed.project().texts.back().start, kv::frameRound(tAt(225), 30)),
        "h: double-click adds a text at tOf(x) (frame-rounded), not snapped");
  std::printf("note: after the double-click the playhead is at %.4f (the first press scrubs like a single press)\n", now());
  ed.undo();
  check(ed.project().texts.size() == texts0, "h: (setup) one undo removes the added text");
  // ホイール（横スクロール）と Ctrl+ホイール（拡大縮小）
  {
    setScroll(120);
    const QPointF p(400, kVideoMid);
    QWheelEvent we(p, gView->viewport()->mapToGlobal(p), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(gView->viewport(), &we);
    check(scrollX() == 240, "h: wheel still scrolls the timeline horizontally");
    const double z0 = pps();
    QWheelEvent wz(p, gView->viewport()->mapToGlobal(p), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(gView->viewport(), &wz);
    check(same(pps(), z0 * 1.15), "h: Ctrl+wheel still zooms by 1.15");
    setZoom(60);
    setScroll(0);
  }

  // ---- (j) Alt を押しながらドラッグしたあと、メニューバーが選ばれた状態にならないか ----
  ed.select({});
  QMenuBar* mb = mw.menuBar();
  auto menuSelected = [&] { return mb->activeAction() != nullptr; };
  auto leaveMenu = [&] {
    for (int k = 0; k < 3 && menuSelected(); ++k) QTest::keyClick(mb, Qt::Key_Escape);  // メニューバーに直接送る
    gView->setFocus();
    QApplication::processEvents();
  };
  std::printf("note: menu bar Alt navigation style hint %d, window active %d\n",
              mb->style()->styleHint(QStyle::SH_MenuBar_AltKeyNavigation, nullptr, mb), int(mw.isActiveWindow()));
  gView->setFocus();
  QApplication::processEvents();
  altDown();
  altUp();
  check(menuSelected(), "j: control: an Alt tap alone selects the menu bar (standard Windows behavior, unchanged)");
  leaveMenu();
  check(!menuSelected(), "j: control: Escape leaves the menu bar");
  // 1) Alt を押してからドラッグし、マウスを離してから Alt を離す
  altDown();
  press({400, 10}, Qt::AltModifier);
  moveTo({300, 10}, Qt::AltModifier);
  moveTo({223, 10}, Qt::AltModifier);
  release({223, 10}, Qt::AltModifier);
  altUp();
  check(!menuSelected(), "j: Alt held from before the press; mouse released first, then Alt: the menu bar is not selected");
  check(same(now(), tAt(223)), "j: that Alt drag did not snap");
  leaveMenu();
  // 2) ドラッグを始めてから Alt を押し、マウスを離してから Alt を離す
  press({400, 10});
  moveTo({300, 10});
  altDown();
  moveTo({250, 10}, Qt::AltModifier);
  moveTo({223, 10}, Qt::AltModifier);
  release({223, 10}, Qt::AltModifier);
  altUp();
  check(!menuSelected(), "j: Alt pressed during the drag; mouse released first, then Alt: the menu bar is not selected");
  leaveMenu();
  // 3) テキストを Alt を押しながら動かしたときも同じ
  altDown();
  press({558, kTextMid}, Qt::AltModifier);
  moveTo({545, kTextMid}, Qt::AltModifier);
  moveTo({533, kTextMid}, Qt::AltModifier);
  release({533, kTextMid}, Qt::AltModifier);
  altUp();
  check(!menuSelected(), "j: after an Alt text drag (mouse released first, then Alt) the menu bar is not selected");
  check(same(textStart(), 6.5 + (533.0 - 558.0) / 60.0), "j: that Alt text drag did not snap");
  leaveMenu();
  undoOnce("j: (setup) one undo puts the text back");
  // 参考: ドラッグ中にマウスを動かさずに Alt だけを押して離す（Windows のふつうの Alt の動き。チェックにはしない）
  press({400, 10});
  altDown();
  altUp();
  std::printf("note: Alt tapped during a drag without moving the mouse: the menu bar is %s\n", menuSelected() ? "selected" : "not selected");
  release({400, 10});
  leaveMenu();
  // 4) Alt+V（メニューのキー操作）は今までどおり（ウィンドウが前面のときだけ確かめられる）
  if (mw.isActiveWindow()) {
    QTest::keyClick(gWin, Qt::Key_V, Qt::AltModifier);
    QWidget* popup = QApplication::activePopupWidget();
    check(popup == viewMenu, "j: Alt+V still opens the View menu");
    if (popup) popup->close();
    leaveMenu();
  } else {
    std::printf("note: the window is not active, so Alt+V was not checked (check it by hand)\n");
  }
  check(!menuSelected(), "j: the menu bar is left unselected at the end");

  check(mismatches == 0, "setup: every mouse event landed at the intended position with the intended modifiers");
  if (failures) std::printf("RESULT: %d check(s) FAILED\n", failures);
  else std::printf("RESULT: ALL OK\n");
  return failures ? 1 : 0;
}
```

ハーネスについての補足:
- 配置（大きさ・時間・位置）を変えたら、`baseline\ref-*.png` を消して、修正前のコードで基準をとり直す（6.1 の最後）。
- ダブルクリックでは、Qt 6 の QWidgetWindow がダブルクリックを作った 2 回目の press をウィジェットに渡さない（前回のハーネスと同じ）。
- `undoOnce` は、元に戻すで戻らなかったときに値を置き直すので、1 つの失敗がほかの確認に広がらない。

---

## 付録 B: timelinewidget.cpp / .h の変更（str_replace で入れる。これ以外は変えない）

手順 2 で B-1・B-2、手順 3 で B-3〜B-9 を入れる。「前」はファイルにある文字どおり（CRLF は str_replace がそのまま保つ）。

### B-1（手順 2, cpp 294-308）目盛りの間隔を rulerStep() に切り出す

前:
```cpp
static const double kSteps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200};

void TimelineView::paintRuler(QPainter& p, const QRect& area) {
  const Editor& ed = editor();
  p.fillRect(QRect(0, 0, area.width(), kRulerH), theme::panel);
  p.fillRect(QRect(0, kRulerH - 1, area.width(), 1), theme::border);
  const double total = ed.layout().total;
  double step = 1200;
  for (double s : kSteps) {
    if (s * pps() >= 70) {
      step = s;
      break;
    }
  }
  const double minor = step / 5;
```
後:
```cpp
static const double kSteps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200};

// 目盛りの数字の間隔: 1 間隔が 70px 以上になる一番小さい値。吸着する「キリのいい秒」もこの間隔にする
double TimelineView::rulerStep() const {
  for (double s : kSteps) {
    if (s * pps() >= 70) return s;
  }
  return 1200;
}

void TimelineView::paintRuler(QPainter& p, const QRect& area) {
  const Editor& ed = editor();
  p.fillRect(QRect(0, 0, area.width(), kRulerH), theme::panel);
  p.fillRect(QRect(0, kRulerH - 1, area.width(), 1), theme::border);
  const double total = ed.layout().total;
  const double step = rulerStep();
  const double minor = step / 5;
```

### B-2（手順 2, h 71-72）宣言

前:
```cpp
  double snap(double t, const std::vector<double>& points) const;
  const std::vector<QPixmap>& thumbPixmaps(const QString& assetId, int height);
```
後:
```cpp
  double snap(double t, const std::vector<double>& points) const;
  double rulerStep() const;
  const std::vector<QPixmap>& thumbPixmaps(const QString& assetId, int height);
```

### B-3（手順 3, h）宣言とメンバー

前:
```cpp
  std::vector<double> snapPoints(const QString& exclude) const;
  double snap(double t, const std::vector<double>& points) const;
  double rulerStep() const;
  const std::vector<QPixmap>& thumbPixmaps(const QString& assetId, int height);
  void followPlayhead();
  void zoomAt(double factor, double mouseX);
```
後:
```cpp
  std::vector<double> snapPoints(const QString& exclude, bool withPlayhead) const;
  std::optional<double> snap(double t, const std::vector<double>& points) const;
  double rulerStep() const;
  const std::vector<QPixmap>& thumbPixmaps(const QString& assetId, int height);
  void followPlayhead();
  void zoomAt(double factor, double mouseX);
  void scrubTo(double x, bool snapOn);
```
前:
```cpp
  std::vector<double> dragSnap_;
  double dragDx_ = 0;
```
後:
```cpp
  std::vector<double> dragSnap_;
  std::optional<double> snapMark_;  // 吸着している時刻（目印を描く）。吸着していないときは空
  double dragDx_ = 0;
```

### B-4（手順 3, cpp 248-279）snapPoints と snap

前（248 行目の `std::vector<double> TimelineView::snapPoints(const QString& exclude) const {` から 279 行目の snap の閉じ括弧まで）を、次に置き換える:
```cpp
// 吸着先の「内容の点」: 0:00、クリップの境目（最後のクリップの終わりがビデオの終わり）、テキストとオーディオの端（exclude の自分は除く）。
// withPlayhead のときは今の再生位置も入れる（赤い線のドラッグでは、赤い線自身は入れない）
std::vector<double> TimelineView::snapPoints(const QString& exclude, bool withPlayhead) const {
  const Editor& ed = editor();
  std::vector<double> pts{0};
  if (withPlayhead) pts.push_back(ed.currentTime());
  for (const auto& l : ed.layout().items) {
    pts.push_back(l.start);
    pts.push_back(l.end);
  }
  for (const auto& t : ed.project().texts) {
    if (t.id == exclude) continue;
    pts.push_back(t.start);
    pts.push_back(t.start + t.duration);
  }
  for (const auto& a : ed.project().audios) {
    if (a.id == exclude) continue;
    pts.push_back(a.start);
    pts.push_back(a.start + audioDuration(a));
  }
  return pts;
}

// t を吸着させた時刻。しきい値は画面上の 8px。内容の点のうち一番近いものを先に使い、
// なければ目盛りの数字の位置（細かい線は使わない）。どちらもなければ空
std::optional<double> TimelineView::snap(double t, const std::vector<double>& points) const {
  const double range = kSnapPx / pps();
  std::optional<double> best;
  double bestD = range;
  for (double p : points) {
    const double d = std::abs(p - t);
    if (d < bestD) {
      best = p;
      bestD = d;
    }
  }
  if (best) return best;
  // 目盛りの数字の位置（paintRuler と同じ間隔）。念のためフレームの境目にそろえる
  const double step = rulerStep();
  const double g = frameRound(std::round(t / step) * step, editor().project().settings.fps);
  if (g >= 0 && std::abs(g - t) < range) return g;
  return std::nullopt;
}
```

### B-5（手順 3, cpp paintRuler の目盛りのループ。B-1 のあとの位置）数字と線を黄色にする

前:
```cpp
    const bool major = i % 5 == 0;
    p.setPen(t > total + 1e-6 && total > 0 ? QColor(0x35, 0x35, 0x35) : major ? QColor(0x7a, 0x7a, 0x7a) : QColor(0x45, 0x45, 0x45));
    p.drawLine(QPointF(x, major ? 12 : 18), QPointF(x, kRulerH - 1));
    if (major) {
      p.setPen(QColor(0x9a, 0x9a, 0x9a));
      p.drawText(QPointF(x + 3, 11), formatTime(double(i / 5) * step, step < 1));
    }
```
後:
```cpp
    const bool major = i % 5 == 0;
    // 赤い線のドラッグで目盛りの数字に吸着しているときは、その数字と線を挿入位置の線と同じ黄色にする
    const bool snapped = major && drag_ == Drag::Scrub && snapMark_ && std::abs(double(i / 5) * step - *snapMark_) < 1e-6;
    p.setPen(snapped ? QColor(0xff, 0xd4, 0x4a) : t > total + 1e-6 && total > 0 ? QColor(0x35, 0x35, 0x35) : major ? QColor(0x7a, 0x7a, 0x7a) : QColor(0x45, 0x45, 0x45));
    p.drawLine(QPointF(x, major ? 12 : 18), QPointF(x, kRulerH - 1));
    if (major) {
      p.setPen(snapped ? QColor(0xff, 0xd4, 0x4a) : QColor(0x9a, 0x9a, 0x9a));
      p.drawText(QPointF(x + 3, 11), formatTime(double(i / 5) * step, step < 1));
    }
```

### B-6（手順 3, cpp paintEvent 591 と 593 の間）テキスト・オーディオのガイド線

前:
```cpp
      p.drawLine(QPointF(ex, kRulerH), QPointF(ex, area.height()));
    }
  }

  // 見出し（左に固定）
```
後:
```cpp
      p.drawLine(QPointF(ex, kRulerH), QPointF(ex, area.height()));
    }
  }

  // 吸着した位置（テキストとオーディオのドラッグ中だけ。挿入位置の線と同じ色。見出しの列の下には隠れる）
  if (snapMark_ && (drag_ == Drag::ItemMove || drag_ == Drag::ItemTrim)) p.fillRect(QRectF(xOf(*snapMark_) - 1, kRulerH, 2, area.height() - kRulerH), QColor(0xff, 0xd4, 0x4a));

  // 見出し（左に固定）
```

### B-7（手順 3, cpp 636-661）scrubTo と mousePressEvent

前:
```cpp
// ---- 操作 -------------------------------------------------------------------

void TimelineView::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  Editor& ed = editor();
  const QPointF pos = e->position();
  pressPos_ = pos;
  dragMoved_ = false;
  dragDx_ = 0;
  merge_ = QStringLiteral("tl-") + newId();
```
後:
```cpp
// ---- 操作 -------------------------------------------------------------------

// 赤い線を x の位置へ動かす。snapOn のときは、押したときに作った吸着先（dragSnap_）と目盛りの数字に吸着させる
void TimelineView::scrubTo(double x, bool snapOn) {
  Editor& ed = editor();
  const double t = tOf(x);
  snapMark_ = snapOn ? snap(t, dragSnap_) : std::nullopt;
  ed.seek(snapMark_.value_or(t));
  // 内容の終わりより後ろの数字に吸着して、seek で終わりに止められたときは目印を出さない
  if (snapMark_ && ed.currentTime() != *snapMark_) snapMark_.reset();
}

void TimelineView::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  Editor& ed = editor();
  const QPointF pos = e->position();
  pressPos_ = pos;
  dragMoved_ = false;
  dragDx_ = 0;
  snapMark_.reset();
  merge_ = QStringLiteral("tl-") + newId();
```
前:
```cpp
    if (h.row != Row::Ruler) ed.select({});
    ed.seek(tOf(pos.x()));
    drag_ = Drag::Scrub;
```
後:
```cpp
    if (h.row != Row::Ruler) ed.select({});
    // 押した瞬間も吸着させる（Alt を押しているときは吸着しない）。赤い線自身は吸着先にしない
    dragSnap_ = snapPoints(QString(), false);
    scrubTo(pos.x(), !(e->modifiers() & Qt::AltModifier));
    drag_ = Drag::Scrub;
```
前:
```cpp
  dragSnap_ = snapPoints(h.id);
```
後:
```cpp
  dragSnap_ = snapPoints(h.id, true);
```

### B-8（手順 3, cpp mouseMoveEvent 687-771）

前:
```cpp
  const int fps = ed.project().settings.fps;
  switch (drag_) {
    case Drag::Scrub:
      ed.seek(tOf(pos.x()));
      break;
```
後:
```cpp
  const int fps = ed.project().settings.fps;
  // Alt を押している間は、どの吸着も効かない
  const bool snapOn = !(e->modifiers() & Qt::AltModifier);
  switch (drag_) {
    case Drag::Scrub:
      scrubTo(pos.x(), snapOn);
      break;
```
前（ItemMove の 724-727）:
```cpp
      double start = std::max(0.0, origStart + dt);
      const double s1 = snap(start, dragSnap_);
      const double s2 = snap(start + dur, dragSnap_) - dur;
      start = std::abs(s1 - start) <= std::abs(s2 - start) ? s1 : s2;
      start = std::max(0.0, start);
```
後:
```cpp
      double start = std::max(0.0, origStart + dt);
      // 左端と右端のうち、実際に吸着したほうで、ずれが小さいほうに合わせる
      const std::optional<double> s1 = snapOn ? snap(start, dragSnap_) : std::nullopt;        // 左端の吸着先
      const std::optional<double> s2 = snapOn ? snap(start + dur, dragSnap_) : std::nullopt;  // 右端の吸着先
      snapMark_.reset();
      if (s1 && (!s2 || std::abs(*s1 - start) <= std::abs(*s2 - (start + dur)))) {
        start = *s1;
        snapMark_ = s1;
      } else if (s2) {
        start = *s2 - dur;
        snapMark_ = s2;
      }
      start = std::max(0.0, start);
```
前（ItemTrim の 732-755。`case Drag::ItemTrim: {` から、オーディオの左端の `double inPoint = ...` まで）:
```cpp
    case Drag::ItemTrim: {
      if (dragType_ == Selection::Text) {
        const TextItem o = origText_;
        ed.updateText(o.id, [&](TextItem& t) {
          if (dragSide_ > 0) {
            const double end = snap(o.start + o.duration + dt, dragSnap_);
            t.duration = std::max(kMinTextDuration, end - o.start);
          } else {
            const double start = std::clamp(snap(o.start + dt, dragSnap_), 0.0, o.start + o.duration - kMinTextDuration);
            t.start = start;
            t.duration = o.start + o.duration - start;
          }
        }, merge_);
      } else {
        const AudioItem o = origAudio_;
        const Asset* a = ed.asset(o.assetId);
        const double assetDur = a ? a->duration : o.out;
        ed.updateAudio(o.id, [&](AudioItem& x) {
          if (dragSide_ > 0) {
            const double end = snap(o.start + (o.out - o.in) + dt, dragSnap_);
            x.out = std::min(assetDur, std::max(o.in + kMinClipDuration, o.in + (end - o.start)));
          } else {
            double start = snap(o.start + dt, dragSnap_);
            double inPoint = o.in + (start - o.start);
```
後:
```cpp
    case Drag::ItemTrim: {
      // 動かしている端を吸着させる。長さの制限などで端が吸着先からずれたときは、目印を出さない
      snapMark_.reset();
      if (dragType_ == Selection::Text) {
        const TextItem o = origText_;
        ed.updateText(o.id, [&](TextItem& t) {
          if (dragSide_ > 0) {
            const double raw = o.start + o.duration + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            const double end = s.value_or(raw);
            t.duration = std::max(kMinTextDuration, end - o.start);
            if (s && std::abs(t.start + t.duration - *s) < 1e-6) snapMark_ = s;
          } else {
            const double raw = o.start + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            const double start = std::clamp(s.value_or(raw), 0.0, o.start + o.duration - kMinTextDuration);
            t.start = start;
            t.duration = o.start + o.duration - start;
            if (s && std::abs(start - *s) < 1e-6) snapMark_ = s;
          }
        }, merge_);
      } else {
        const AudioItem o = origAudio_;
        const Asset* a = ed.asset(o.assetId);
        const double assetDur = a ? a->duration : o.out;
        ed.updateAudio(o.id, [&](AudioItem& x) {
          if (dragSide_ > 0) {
            const double raw = o.start + (o.out - o.in) + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            const double end = s.value_or(raw);
            x.out = std::min(assetDur, std::max(o.in + kMinClipDuration, o.in + (end - o.start)));
            if (s && std::abs(x.start + audioDuration(x) - *s) < 1e-6) snapMark_ = s;
          } else {
            const double raw = o.start + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            double start = s.value_or(raw);
            double inPoint = o.in + (start - o.start);
```
さらに、オーディオの左端の最後（`x.in = inPoint;` の直後）に 1 行足す:
前:
```cpp
            x.start = o.start + (inPoint - o.in);
            x.in = inPoint;
          }
```
後:
```cpp
            x.start = o.start + (inPoint - o.in);
            x.in = inPoint;
            if (s && std::abs(x.start - *s) < 1e-6) snapMark_ = s;
          }
```
（`inPoint < 0`・`start < 0`・`std::min(inPoint, o.out - kMinClipDuration)` の行はそのまま。）

### B-9（手順 3, cpp mouseReleaseEvent 785-788）

前:
```cpp
  drag_ = Drag::None;
  dragMoved_ = false;
  dragDx_ = 0;
  ed.endMerge();
```
後:
```cpp
  drag_ = Drag::None;
  dragMoved_ = false;
  dragDx_ = 0;
  snapMark_.reset();
  ed.endMerge();
```

---

## 付録 C: (j) で 7.3 の「3 が FAIL」になったときだけ入れる変更

h（protected の `void scrollContentsBy(int dx, int dy) override;` の次の行）:
```cpp
  bool event(QEvent* e) override;
```
cpp（`#include <QDragEnterEvent>` の並びに `#include <QKeyEvent>` を足し、`scrubTo` の前に置く）:
```cpp
// ドラッグ中に押した Alt は吸着を外すためのものなので、メニューバーに渡さない（Alt を離したときにメニューが選ばれないように）。
// ドラッグしていないときの Alt と、Alt+V などほかのキーは今までどおり
bool TimelineView::event(QEvent* e) {
  if (e->type() == QEvent::ShortcutOverride && drag_ != Drag::None && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Alt) {
    e->accept();
    return true;
  }
  return QAbstractScrollArea::event(e);
}
```
仕組み: ShortcutOverride はフォーカスのあるウィジェット（クリックでフォーカスを持つタイムライン）から親へ順に渡り、受け取られなかったときだけメインウィンドウのメニューバーに届く。ドラッグ中の Alt だけをここで受け取るので、メニューバーは「Alt が押された」と覚えない。入れたら手順 3 と同じビルドとハーネス（`fixed`）をやり直し、`j:` がすべて OK、`j: control: ...` と（前面のとき）`j: Alt+V still opens the View menu` が OK のままであることを確かめる。
