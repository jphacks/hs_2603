# 検証結果: タイムラインのマグネット（吸着）機能（1 回目）

## まとめ

- 変更したのは `native/src/ui/timelinewidget.cpp` と `native/src/ui/timelinewidget.h` だけ（2 files changed, 86 insertions(+), 27 deletions(-)）。中身は計画の付録 B（B-1〜B-9）のとおりで、それ以外の変更はない。付録 C（(j) の対策）は必要なかったので入れていない。
- アプリのビルド（`build.ps1 -NoDeploy`）は 2 回とも終了コード 0。`warning:` も `error:` もない（手順 2 のあとと、手順 3 のあと）。
- ハーネス（リポジトリの外の QTest）:
  - 修正前: 102 件中 37 件が FAIL。計画 6.3 の予想（36 件）とは 5 行が違う。原因は修正前のコードの動きで、ハーネスの誤りではない（5.2）。
  - 手順 2（rulerStep の切り出しだけ）のあと: 109 件中 37 件が FAIL（修正前とまったく同じ 37 行）。画素比較の 7 件はすべて OK（目盛りの見た目は変わっていない）。
  - 修正後: 109 件すべて OK（`RESULT: ALL OK`、`RUN-EXIT=0`）。(a)〜(j) の全項目が通った。
- 画像の目視も期待どおり（8 章）。吸着していないときの画面は、修正前と画素まで同じ（7 枚、違う画素 0）。
- (j): Alt を押しながらドラッグし、マウスを離してから Alt を離しても、メニューバーは選ばれない（修正前も後も）。対策は入れていない。
- 気づいたこと: 修正前のコードでは、テキストやオーディオを「移動」するとき、片方の端だけが吸着先に近くても吸着しなかった（右端だけでなく左端も。両端が同時に近いときだけ効いていた）。今回の修正で、どちらか片方の端だけが近いときも吸着するようになった（合意した「実際に吸着したほうの端を使う」のとおり）。くわしくは 5.2。
- コミット・push・stash・配置はしていない。ユーザーのアプリ（PID 13748）は起動したまま。一時フォルダー `C:\e-editor-dev\tmp\kv-magnet\` はレビューのために残してある（13 章）。

## 1. 始める前の状態

- `review.json` はなかったので、1 回目として作業した。タスクのフォルダーには `plan.md` と `proposal.md` だけがあった。
- `git -C c:/git/JPHACK/e-editor status`（作業前）:
  ```
  On branch feat/ikumu1
  Your branch is behind 'origin/feat/ikumu1' by 3 commits, and can be fast-forwarded.
    (use "git pull" to update your local branch)
  Untracked files:
    (use "git add <file>..." to include in what will be committed)
  	.agents/tasks/timeline-magnet/
  nothing added to commit but untracked files present (use "git add" to track)
  ```
  `git status --porcelain` は `?? .agents/tasks/timeline-magnet/` だけで、ほかの変更はなかった。HEAD は `eaac4b6 feat(timeline): make playhead visible over header column`。stash はなし。
- `timelinewidget.cpp`: UTF-8（BOM なし）・CRLF 937 行（LF だけの行は 0）。`timelinewidget.h`: UTF-8（BOM なし）・CRLF 121 行（LF だけの行は 0）。
- ユーザーのアプリ `native\dist\e-editor\e-editor.exe` が起動中だった（PID 13748、11:29:33 起動）。exe の時刻は `native\dist` も `native\build` も 2026-10-22 00:47:58。
- `C:\e-editor-dev\tmp\` は空で、`kv-magnet\` はなかった。リポジトリに書式の設定（.clang-format など）はない。

## 2. 変更の内容

`git -C c:\git\JPHACK\e-editor diff`（`git diff --output` で UTF-8 のまま書き出したもの。全文）:

```diff
diff --git a/native/src/ui/timelinewidget.cpp b/native/src/ui/timelinewidget.cpp
index e7f1f52..62b5e36 100644
--- a/native/src/ui/timelinewidget.cpp
+++ b/native/src/ui/timelinewidget.cpp
@@ -245,9 +245,12 @@ double TimelineView::indicatorX(int index, const QString& exclude) const {
   return xOf(others[size_t(index)]->start);
 }
 
-std::vector<double> TimelineView::snapPoints(const QString& exclude) const {
+// 吸着先の「内容の点」: 0:00、クリップの境目（最後のクリップの終わりがビデオの終わり）、テキストとオーディオの端（exclude の自分は除く）。
+// withPlayhead のときは今の再生位置も入れる（赤い線のドラッグでは、赤い線自身は入れない）
+std::vector<double> TimelineView::snapPoints(const QString& exclude, bool withPlayhead) const {
   const Editor& ed = editor();
-  std::vector<double> pts{0, ed.currentTime()};
+  std::vector<double> pts{0};
+  if (withPlayhead) pts.push_back(ed.currentTime());
   for (const auto& l : ed.layout().items) {
     pts.push_back(l.start);
     pts.push_back(l.end);
@@ -265,9 +268,12 @@ std::vector<double> TimelineView::snapPoints(const QString& exclude) const {
   return pts;
 }
 
-double TimelineView::snap(double t, const std::vector<double>& points) const {
-  double best = t;
-  double bestD = kSnapPx / pps();
+// t を吸着させた時刻。しきい値は画面上の 8px。内容の点のうち一番近いものを先に使い、
+// なければ目盛りの数字の位置（細かい線は使わない）。どちらもなければ空
+std::optional<double> TimelineView::snap(double t, const std::vector<double>& points) const {
+  const double range = kSnapPx / pps();
+  std::optional<double> best;
+  double bestD = range;
   for (double p : points) {
     const double d = std::abs(p - t);
     if (d < bestD) {
@@ -275,7 +281,12 @@ double TimelineView::snap(double t, const std::vector<double>& points) const {
       bestD = d;
     }
   }
-  return best;
+  if (best) return best;
+  // 目盛りの数字の位置（paintRuler と同じ間隔）。念のためフレームの境目にそろえる
+  const double step = rulerStep();
+  const double g = frameRound(std::round(t / step) * step, editor().project().settings.fps);
+  if (g >= 0 && std::abs(g - t) < range) return g;
+  return std::nullopt;
 }
 
 const std::vector<QPixmap>& TimelineView::thumbPixmaps(const QString& assetId, int height) {
@@ -293,18 +304,20 @@ const std::vector<QPixmap>& TimelineView::thumbPixmaps(const QString& assetId, i
 
 static const double kSteps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200};
 
+// 目盛りの数字の間隔: 1 間隔が 70px 以上になる一番小さい値。吸着する「キリのいい秒」もこの間隔にする
+double TimelineView::rulerStep() const {
+  for (double s : kSteps) {
+    if (s * pps() >= 70) return s;
+  }
+  return 1200;
+}
+
 void TimelineView::paintRuler(QPainter& p, const QRect& area) {
   const Editor& ed = editor();
   p.fillRect(QRect(0, 0, area.width(), kRulerH), theme::panel);
   p.fillRect(QRect(0, kRulerH - 1, area.width(), 1), theme::border);
   const double total = ed.layout().total;
-  double step = 1200;
-  for (double s : kSteps) {
-    if (s * pps() >= 70) {
-      step = s;
-      break;
-    }
-  }
+  const double step = rulerStep();
   const double minor = step / 5;
   const double left = horizontalScrollBar()->value();
   const int w = area.width() - kHeadW;
@@ -318,10 +331,12 @@ void TimelineView::paintRuler(QPainter& p, const QRect& area) {
     const double x = std::round(xOf(t)) + 0.5;
     if (x < kHeadW) continue;
     const bool major = i % 5 == 0;
-    p.setPen(t > total + 1e-6 && total > 0 ? QColor(0x35, 0x35, 0x35) : major ? QColor(0x7a, 0x7a, 0x7a) : QColor(0x45, 0x45, 0x45));
+    // 赤い線のドラッグで目盛りの数字に吸着しているときは、その数字と線を挿入位置の線と同じ黄色にする
+    const bool snapped = major && drag_ == Drag::Scrub && snapMark_ && std::abs(double(i / 5) * step - *snapMark_) < 1e-6;
+    p.setPen(snapped ? QColor(0xff, 0xd4, 0x4a) : t > total + 1e-6 && total > 0 ? QColor(0x35, 0x35, 0x35) : major ? QColor(0x7a, 0x7a, 0x7a) : QColor(0x45, 0x45, 0x45));
     p.drawLine(QPointF(x, major ? 12 : 18), QPointF(x, kRulerH - 1));
     if (major) {
-      p.setPen(QColor(0x9a, 0x9a, 0x9a));
+      p.setPen(snapped ? QColor(0xff, 0xd4, 0x4a) : QColor(0x9a, 0x9a, 0x9a));
       p.drawText(QPointF(x + 3, 11), formatTime(double(i / 5) * step, step < 1));
     }
   }
@@ -590,6 +605,9 @@ void TimelineView::paintEvent(QPaintEvent*) {
     }
   }
 
+  // 吸着した位置（テキストとオーディオのドラッグ中だけ。挿入位置の線と同じ色。見出しの列の下には隠れる）
+  if (snapMark_ && (drag_ == Drag::ItemMove || drag_ == Drag::ItemTrim)) p.fillRect(QRectF(xOf(*snapMark_) - 1, kRulerH, 2, area.height() - kRulerH), QColor(0xff, 0xd4, 0x4a));
+
   // 見出し（左に固定）
   p.fillRect(QRect(0, kRulerH, kHeadW, area.height()), theme::panel);
   p.fillRect(QRect(kHeadW - 1, 0, 1, area.height()), theme::border);
@@ -635,6 +653,16 @@ void TimelineView::paintEvent(QPaintEvent*) {
 
 // ---- 操作 -------------------------------------------------------------------
 
+// 赤い線を x の位置へ動かす。snapOn のときは、押したときに作った吸着先（dragSnap_）と目盛りの数字に吸着させる
+void TimelineView::scrubTo(double x, bool snapOn) {
+  Editor& ed = editor();
+  const double t = tOf(x);
+  snapMark_ = snapOn ? snap(t, dragSnap_) : std::nullopt;
+  ed.seek(snapMark_.value_or(t));
+  // 内容の終わりより後ろの数字に吸着して、seek で終わりに止められたときは目印を出さない
+  if (snapMark_ && ed.currentTime() != *snapMark_) snapMark_.reset();
+}
+
 void TimelineView::mousePressEvent(QMouseEvent* e) {
   if (e->button() != Qt::LeftButton) return;
   Editor& ed = editor();
@@ -642,6 +670,7 @@ void TimelineView::mousePressEvent(QMouseEvent* e) {
   pressPos_ = pos;
   dragMoved_ = false;
   dragDx_ = 0;
+  snapMark_.reset();
   merge_ = QStringLiteral("tl-") + newId();
   // 見出しの列のクリックは無視する。左上の角（目盛りの行）だけは、目盛りと同じく再生位置を動かす
   if (pos.x() < kHeadW && pos.y() >= kRulerH) return;
@@ -649,7 +678,9 @@ void TimelineView::mousePressEvent(QMouseEvent* e) {
   const Hit h = hitTest(pos);
   if (h.row == Row::Ruler || h.type == Selection::None) {
     if (h.row != Row::Ruler) ed.select({});
-    ed.seek(tOf(pos.x()));
+    // 押した瞬間も吸着させる（Alt を押しているときは吸着しない）。赤い線自身は吸着先にしない
+    dragSnap_ = snapPoints(QString(), false);
+    scrubTo(pos.x(), !(e->modifiers() & Qt::AltModifier));
     drag_ = Drag::Scrub;
     return;
   }
@@ -658,7 +689,7 @@ void TimelineView::mousePressEvent(QMouseEvent* e) {
   dragType_ = h.type;
   dragSide_ = h.handle;
   pressTime_ = tOf(pos.x());
-  dragSnap_ = snapPoints(h.id);
+  dragSnap_ = snapPoints(h.id, true);
   if (h.type == Selection::Clip) {
     origClip_ = *ed.clip(h.id);
     drag_ = h.handle != 0 ? Drag::ClipTrim : Drag::ClipMove;
@@ -685,9 +716,11 @@ void TimelineView::mouseMoveEvent(QMouseEvent* e) {
   if (!dragMoved_ && std::abs(dx) < threshold) return;
   dragMoved_ = true;
   const int fps = ed.project().settings.fps;
+  // Alt を押している間は、どの吸着も効かない
+  const bool snapOn = !(e->modifiers() & Qt::AltModifier);
   switch (drag_) {
     case Drag::Scrub:
-      ed.seek(tOf(pos.x()));
+      scrubTo(pos.x(), snapOn);
       break;
     case Drag::ClipMove:
       dragDx_ = dx;
@@ -721,25 +754,41 @@ void TimelineView::mouseMoveEvent(QMouseEvent* e) {
       const double origStart = dragType_ == Selection::Text ? origText_.start : origAudio_.start;
       const double dur = dragType_ == Selection::Text ? origText_.duration : audioDuration(origAudio_);
       double start = std::max(0.0, origStart + dt);
-      const double s1 = snap(start, dragSnap_);
-      const double s2 = snap(start + dur, dragSnap_) - dur;
-      start = std::abs(s1 - start) <= std::abs(s2 - start) ? s1 : s2;
+      // 左端と右端のうち、実際に吸着したほうで、ずれが小さいほうに合わせる
+      const std::optional<double> s1 = snapOn ? snap(start, dragSnap_) : std::nullopt;        // 左端の吸着先
+      const std::optional<double> s2 = snapOn ? snap(start + dur, dragSnap_) : std::nullopt;  // 右端の吸着先
+      snapMark_.reset();
+      if (s1 && (!s2 || std::abs(*s1 - start) <= std::abs(*s2 - (start + dur)))) {
+        start = *s1;
+        snapMark_ = s1;
+      } else if (s2) {
+        start = *s2 - dur;
+        snapMark_ = s2;
+      }
       start = std::max(0.0, start);
       if (dragType_ == Selection::Text) ed.updateText(dragId_, [&](TextItem& t) { t.start = start; }, merge_);
       else ed.updateAudio(dragId_, [&](AudioItem& a) { a.start = start; }, merge_);
       break;
     }
     case Drag::ItemTrim: {
+      // 動かしている端を吸着させる。長さの制限などで端が吸着先からずれたときは、目印を出さない
+      snapMark_.reset();
       if (dragType_ == Selection::Text) {
         const TextItem o = origText_;
         ed.updateText(o.id, [&](TextItem& t) {
           if (dragSide_ > 0) {
-            const double end = snap(o.start + o.duration + dt, dragSnap_);
+            const double raw = o.start + o.duration + dt;
+            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
+            const double end = s.value_or(raw);
             t.duration = std::max(kMinTextDuration, end - o.start);
+            if (s && std::abs(t.start + t.duration - *s) < 1e-6) snapMark_ = s;
           } else {
-            const double start = std::clamp(snap(o.start + dt, dragSnap_), 0.0, o.start + o.duration - kMinTextDuration);
+            const double raw = o.start + dt;
+            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
+            const double start = std::clamp(s.value_or(raw), 0.0, o.start + o.duration - kMinTextDuration);
             t.start = start;
             t.duration = o.start + o.duration - start;
+            if (s && std::abs(start - *s) < 1e-6) snapMark_ = s;
           }
         }, merge_);
       } else {
@@ -748,10 +797,15 @@ void TimelineView::mouseMoveEvent(QMouseEvent* e) {
         const double assetDur = a ? a->duration : o.out;
         ed.updateAudio(o.id, [&](AudioItem& x) {
           if (dragSide_ > 0) {
-            const double end = snap(o.start + (o.out - o.in) + dt, dragSnap_);
+            const double raw = o.start + (o.out - o.in) + dt;
+            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
+            const double end = s.value_or(raw);
             x.out = std::min(assetDur, std::max(o.in + kMinClipDuration, o.in + (end - o.start)));
+            if (s && std::abs(x.start + audioDuration(x) - *s) < 1e-6) snapMark_ = s;
           } else {
-            double start = snap(o.start + dt, dragSnap_);
+            const double raw = o.start + dt;
+            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
+            double start = s.value_or(raw);
             double inPoint = o.in + (start - o.start);
             if (inPoint < 0) {
               start -= inPoint;
@@ -764,6 +818,7 @@ void TimelineView::mouseMoveEvent(QMouseEvent* e) {
             inPoint = std::min(inPoint, o.out - kMinClipDuration);
             x.start = o.start + (inPoint - o.in);
             x.in = inPoint;
+            if (s && std::abs(x.start - *s) < 1e-6) snapMark_ = s;
           }
         }, merge_);
       }
@@ -785,6 +840,7 @@ void TimelineView::mouseReleaseEvent(QMouseEvent* e) {
   drag_ = Drag::None;
   dragMoved_ = false;
   dragDx_ = 0;
+  snapMark_.reset();
   ed.endMerge();
   viewport()->update();
 }
diff --git a/native/src/ui/timelinewidget.h b/native/src/ui/timelinewidget.h
index a159ad8..f5de683 100644
--- a/native/src/ui/timelinewidget.h
+++ b/native/src/ui/timelinewidget.h
@@ -67,11 +67,13 @@ class TimelineView : public QAbstractScrollArea {
   QRectF itemRect(Selection::Type type, double start, double duration, int lane) const;
   int insertionIndex(double x, const QString& exclude) const;
   double indicatorX(int index, const QString& exclude) const;
-  std::vector<double> snapPoints(const QString& exclude) const;
-  double snap(double t, const std::vector<double>& points) const;
+  std::vector<double> snapPoints(const QString& exclude, bool withPlayhead) const;
+  std::optional<double> snap(double t, const std::vector<double>& points) const;
+  double rulerStep() const;
   const std::vector<QPixmap>& thumbPixmaps(const QString& assetId, int height);
   void followPlayhead();
   void zoomAt(double factor, double mouseX);
+  void scrubTo(double x, bool snapOn);
 
   void paintRuler(QPainter& p, const QRect& area);
   void paintVideo(QPainter& p);
@@ -94,6 +96,7 @@ class TimelineView : public QAbstractScrollArea {
   TextItem origText_;
   AudioItem origAudio_;
   std::vector<double> dragSnap_;
+  std::optional<double> snapMark_;  // 吸着している時刻（目印を描く）。吸着していないときは空
   double dragDx_ = 0;
   int dragIndex_ = -1;
   std::optional<double> dropX_;
```

- 付録 B との対応（行番号は修正後のファイル）: B-1 `rulerStep()` の切り出し（cpp 307-313）と paintRuler の `const double step = rulerStep();`（cpp 320）、B-2・B-3 の宣言（h 70-76）とメンバー `snapMark_`（h 99）、B-4 `snapPoints`・`snap`（cpp 248-290）、B-5 目盛りの数字と線の黄色（cpp 334-341）、B-6 ガイド線（cpp 608-609）、B-7 `scrubTo` と mousePressEvent（cpp 656-692）、B-8 mouseMoveEvent の Scrub・ItemMove・ItemTrim（cpp 719-824）、B-9 mouseReleaseEvent（cpp 843）。付録 B の文字どおりで、書き方の変更（折り返しなど）もしていない。
- `git diff --check`: 出力なし、終了コード 0。追加した行に printf・qDebug・qWarning・std::cout はない。ハーネスのコードもない。
- 編集後のファイルの形: cpp は CRLF 993 行・LF だけの行 0・BOM なし、h は CRLF 124 行・LF だけの行 0・BOM なし（UTF-8 のまま）。
- frameRound について: 計画 D1 のとおり、frameRound は目盛りの数字の位置にだけ通している。指示の「吸着先の時刻は、念のため frameRound を通す」は、proposal 5 章の 4（「数字の位置は 24/30/60fps でフレームの境目だが、念のため frameRound を通す」）と 6 章の計算のイメージ（数字の位置の `g` だけに frameRound）と同じ意味と読んだ。内容の点（クリップの境目・テキストとオーディオの端・0:00・押したときの赤い線）は今と同じくそのままの値を使う。クリップの境目と 0:00 はもともとフレームの境目なので丸めても同じで、違いが出るのはフレームにそろっていない端だけ。そこを丸めると、端同士がぴったり合わなくなる（すき間や重なりができる）ため。
- 吸着しなかったときは、赤い線は `tOf(x)`、テキストとオーディオは今と同じ計算の値のまま（フレームにはそろえない）。しきい値は今と同じ `kSnapPx = 8`（`8 / pps()` 秒、比べ方も今と同じ `<`）。

## 3. アプリのビルド

コマンド（ターミナルの出力が切れるので、計画 4 章のコマンドを一時フォルダーの `appbuild.ps1` に書いて実行した。中身は次のとおりで、計画のコマンドと同じ）:

```powershell
param([int]$N = 1)
$log = "C:\e-editor-dev\tmp\kv-magnet\build-$N.log"
powershell -NoProfile -ExecutionPolicy Bypass -File 'c:\git\JPHACK\e-editor\native\build.ps1' -NoDeploy 2>&1 | Out-File -Encoding utf8 $log
"BUILD-EXIT=$LASTEXITCODE" | Out-File -Append -Encoding utf8 $log
```

`build-1.log`（手順 2 のあと。rulerStep の切り出しだけを入れた状態。全文）:
```
[1/5] Automatic MOC for target e-editor
[2/5] Building CXX object CMakeFiles/e-editor.dir/e-editor_autogen/mocs_compilation.cpp.obj
[3/5] Building CXX object CMakeFiles/e-editor.dir/src/ui/timelinewidget.cpp.obj
[4/5] Building CXX object CMakeFiles/e-editor.dir/src/ui/mainwindow.cpp.obj
[5/5] Linking CXX executable e-editor.exe
BUILD-EXIT=0
```

`build-2.log`（手順 3 のあと。最終のコード。全文）:
```
[1/5] Automatic MOC for target e-editor
[2/5] Building CXX object CMakeFiles/e-editor.dir/e-editor_autogen/mocs_compilation.cpp.obj
[3/5] Building CXX object CMakeFiles/e-editor.dir/src/ui/timelinewidget.cpp.obj
[4/5] Building CXX object CMakeFiles/e-editor.dir/src/ui/mainwindow.cpp.obj
[5/5] Linking CXX executable e-editor.exe
BUILD-EXIT=0
```

- どちらも終了コード 0、`warning:` も `error:` もない（`-Wall -Wno-unused-parameter`）。`std::optional` についての `-Wmaybe-uninitialized` も出ていないので、書き方は計画のまま。ヘッダーを変えたので mainwindow.cpp もコンパイルし直されたが、警告はない。
- 新しくなったのは `native\build\e-editor.exe`（2026-10-22 14:43:29。git では無視されるフォルダー）だけ。配置していないので、`native\dist\e-editor\e-editor.exe` は 2026-10-22 00:47:58 のまま。

## 4. ハーネス

- 場所: `C:\e-editor-dev\tmp\kv-magnet\`（リポジトリの外。パスは ASCII だけ）。計画の付録 A の `CMakeLists.txt`・`magnetcheck.cpp`（UTF-8、BOM なし）・`run.ps1`（ASCII だけ）を、そのまま作った。リポジトリにはテストもビルドのターゲットも足していない。
- 作り: main.cpp 以外のアプリのソース（.cpp 23 本と .h）をリポジトリから直接コンパイルし、Qt6::Test（Qt 6.8.3 / MinGW 13.1）を使う。MainWindow と MediaManager は使わない（ユーザーの自動保存に書き込まない）。素材は `Editor::addAsset` で Ready の情報だけを入れる。アプリと同じく、メニューバー（`&File`・`&View`）のある `QMainWindow` の中央に `TimelineWidget` を置いた（(j) で使う）。
- マウスは QTest の QWindow 版（`mousePress`・`mouseRelease`・`mouseDClick`、修飾キー付きの移動は `QTest::mouseEvent(QTest::MouseMove, ...)`）で入れた。OS の入力と同じ `QWindowSystemInterface` の経路を通る。DPR は掛けていない。
- DPR と座標: `note: device pixel ratio 1.25, viewport 900x278, window active 1`。ビューポートのイベントフィルターで、マウスの全イベントの位置と修飾キー（Alt）が渡したものと同じことを毎回確かめた。4 回の実行すべてで `note: mouse arrived at ...` は 0 回、`setup:` の最初と最後のチェックは OK。
- Alt のキーは `QTest::simulateEvent(QWindow*, ...)` で Windows と同じ形（押す: 保存する修飾キーなし、離す: AltModifier）で入れた。ウィンドウは 4 回とも前面になった（`window active 1`）ので、Alt+V の確認もできた。
- 実行（計画 4 章のコマンドを `harness.ps1` に書いて実行。中身は次のとおり）:
  ```powershell
  param([string]$Phase = 'fixed')
  $log = "C:\e-editor-dev\tmp\kv-magnet\run-$Phase.log"
  powershell -NoProfile -ExecutionPolicy Bypass -File 'C:\e-editor-dev\tmp\kv-magnet\run.ps1' 2>&1 | Out-File -Encoding utf8 $log
  "RUN-EXIT=$LASTEXITCODE" | Out-File -Append -Encoding utf8 $log
  ```
- 実行した順番: `baseline`（修正前のコード）→ `step`（手順 2 のあと）→ `fixed`（手順 3 のあと）→ `fixed-values`（同じコードで、実測値を出すようにしたハーネス）。
- ハーネスを変えたのは 1 回だけ: `fixed` のあとで、チェックごとの実測値を `measured:` の行に出す処理（`logVal`。`same()` の比べた 2 つの値、数字の黄色さ、ガイド線の画素数、ガイド線の有無、違う画素の数、拡大率とスクロール、revision）を足した。チェックの条件・数値・順番は変えていない。`fixed` と `fixed-values` の 109 行の OK/FAIL の行がまったく同じであることを `Compare-Object` で確かめた（True）。基準の画像（`baseline\ref-*.png`）は修正前の 1 回目に保存したもので、そのあと作り直していない。
- 4 回の実行のログに `warning:`・`error:`・`Mouse event at ... outside` の行はない（ハーネスのビルドも警告なし）。

## 5. 修正前（baseline）の結果

### 5.1 ログ（`run-baseline.log` 全文。アプリのファイルに触る前に実行）

```
-- The C compiler identification is GNU 13.1.0
-- The CXX compiler identification is GNU 13.1.0
-- Detecting C compiler ABI info
-- Detecting C compiler ABI info - done
-- Check for working C compiler: C:/e-editor-dev/Qt/Tools/mingw1310_64/bin/gcc.exe - skipped
-- Detecting C compile features
-- Detecting C compile features - done
-- Detecting CXX compiler ABI info
-- Detecting CXX compiler ABI info - done
-- Check for working CXX compiler: C:/e-editor-dev/Qt/Tools/mingw1310_64/bin/g++.exe - skipped
-- Detecting CXX compile features
-- Detecting CXX compile features - done
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD
-- Performing Test CMAKE_HAVE_LIBC_PTHREAD - Success
-- Found Threads: TRUE
-- Performing Test HAVE_STDATOMIC
-- Performing Test HAVE_STDATOMIC - Success
-- Found WrapAtomic: TRUE
-- Could NOT find WrapVulkanHeaders (missing: Vulkan_INCLUDE_DIR) 
-- Configuring done (17.4s)
-- Generating done (0.7s)
-- Build files have been written to: C:/e-editor-dev/tmp/kv-magnet/build
[0/2] Re-checking globbed directories...
[1/34] Automatic MOC for target miniaudio
[2/34] Building CXX object CMakeFiles/miniaudio.dir/miniaudio_autogen/mocs_compilation.cpp.obj
[3/34] Building C object CMakeFiles/miniaudio.dir/C_/git/JPHACK/e-editor/native/third_party/miniaudio/miniaudio.c.obj
[4/34] Linking CXX static library libminiaudio.a
[5/34] Automatic MOC for target kv-magnet
[6/34] Automatic RCC for ../../../git/JPHACK/e-editor/native/resources/app.qrc
[7/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/core/model.cpp.obj
[8/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/core/timeline.cpp.obj
[9/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/core/textstyles.cpp.obj
[10/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/core/filters.cpp.obj
[11/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/render/imageops.cpp.obj
[12/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/media/videosource.cpp.obj
[13/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/render/textrender.cpp.obj
[14/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/media/audioengine.cpp.obj
[15/34] Building CXX object CMakeFiles/kv-magnet.dir/kv-magnet_autogen/ACB6F6S2LK/qrc_app.cpp.obj
[16/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/core/exportplan.cpp.obj
[17/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/exportdialog.cpp.obj
[18/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/render/previewengine.cpp.obj
[19/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/export/exporter.cpp.obj
[20/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/core/projectio.cpp.obj
[21/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/core/editor.cpp.obj
[22/34] Building CXX object CMakeFiles/kv-magnet.dir/kv-magnet_autogen/mocs_compilation.cpp.obj
[23/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/media/mediamanager.cpp.obj
[24/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/media/ffutil.cpp.obj
[25/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/theme.cpp.obj
[26/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/previewwidget.cpp.obj
[27/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/widgets.cpp.obj
[28/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/librarypanel.cpp.obj
[29/34] Building CXX object CMakeFiles/kv-magnet.dir/magnetcheck.cpp.obj
[30/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/inspector.cpp.obj
[31/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/timelinewidget.cpp.obj
[32/34] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/mainwindow.cpp.obj
[33/34] Linking CXX executable kv-magnet.exe
note: device pixel ratio 1.25, viewport 900x278, window active 1
OK   setup: clips 0-3.5-10.5 s, text 6.5-8.8 s, audio 0-2.6 s at 60 px/s
OK   setup: timeline can scroll to 300 and shows all rows
OK   setup: mouse events (and the Alt modifier) land at the intended viewport position
note: saved C:/e-editor-dev/tmp/kv-magnet/baseline/ref-i-idle.png (the first run must be on the unmodified code)
note: saved C:/e-editor-dev/tmp/kv-magnet/baseline/ref-i-scrub-free.png (the first run must be on the unmodified code)
note: saved C:/e-editor-dev/tmp/kv-magnet/baseline/ref-i-scrub-alt.png (the first run must be on the unmodified code)
note: saved C:/e-editor-dev/tmp/kv-magnet/baseline/ref-i-move-free.png (the first run must be on the unmodified code)
OK   i: (setup) one undo puts the text back
note: saved C:/e-editor-dev/tmp/kv-magnet/baseline/ref-i-idle-100.png (the first run must be on the unmodified code)
note: saved C:/e-editor-dev/tmp/kv-magnet/baseline/ref-i-idle-20.png (the first run must be on the unmodified code)
note: saved C:/e-editor-dev/tmp/kv-magnet/baseline/ref-i-idle-head.png (the first run must be on the unmodified code)
OK   i: the red line and knob are still drawn over the header column
OK   a: a ruler press away from snap points seeks to tOf(x)
OK   a: 12 px before 0:02 does not snap (stays tOf(x))
FAIL a: 5 px before 0:02 snaps to 2.0
FAIL a: the 0:02 label turns yellow while snapped
OK   a: the other labels stay gray
OK   a: no guide line across the tracks while scrubbing
OK   a: the red line and knob are drawn at the playhead
FAIL a: 7 px before 0:02 snaps to 2.0
OK   a: 9 px before 0:02 does not snap
FAIL a: 5 px after 0:02 snaps to 2.0
OK   a: with Alt held, 5 px before 0:02 does not snap
OK   a: with Alt held, no label is yellow
FAIL a: after Alt is released, the next move snaps again
FAIL a: releasing the mouse keeps the snapped time
OK   a: after release every label is gray again
OK   a: scrubbing adds nothing to the undo history
FAIL b: a ruler press 3 px before 0:02 snaps to 2.0 at once
OK   b: a ruler press with Alt held does not snap
FAIL b: a press on the top-left corner snaps to 2.0
FAIL b: the snapped red line shows over the header column (x=78)
OK   b: a corner press with Alt held does not snap
FAIL b: a press on an empty track spot snaps to 10.0 (and deselects)
FAIL c: snaps to the clip boundary 3.5 (4 px away)
FAIL c: snaps to the text start 6.5 (3 px away)
FAIL c: snaps to the text end 8.8 (3 px away)
FAIL c: snaps to the audio end 2.6 (4 px away)
FAIL c: snaps to 0:00 (5 px away)
FAIL c: snaps to the end of the video 10.5 (4 px away)
OK   c: a snap to a point that is not a ruler number shows no yellow
FAIL c: the text start 4.1 (4 px) wins over the ruler number 4.0 (2 px)
OK   c: (setup) one undo puts the text back
OK   c: the playhead's own old position (1.0, 3 px) is not a snap point
OK   d: at 60 px/s (step 2 s) 0.95 s does not snap to 1.0
FAIL d: at 100 px/s (step 1 s) snaps to 1.0
FAIL d: at 100 px/s the 0:01 label turns yellow
FAIL d: at 20 px/s (step 5 s) snaps to 5.0
FAIL d: at 20 px/s the 0:05 label turns yellow
OK   d: at 20 px/s 2.0 is not a snap point
OK   d: (setup) Ctrl+wheel during the drag zoomed in to the 1 s step
FAIL d: after zooming during the drag, the next move uses the new step (1.0)
OK   e: (setup) pressing the middle of the text selects it
FAIL e: the left edge snaps to the ruler number 6.0
FAIL e: a yellow guide line runs across all tracks at 6.0
OK   e: no ruler label turns yellow while moving a text
OK   e: the guide line disappears after release
OK   g: one undo after a text move restores it
FAIL e: only the right edge is near the clip end: it snaps (start 8.2, end 10.5)
FAIL e: the guide line is at the right edge (10.5)
OK   g: one undo after a right-edge snap restores the text
FAIL e: the left edge still snaps to the clip boundary 3.5 (existing snap)
OK   e: with Alt held, the existing edge snap is off too
OK   e: no guide line while Alt is held
FAIL e: releasing Alt snaps again
OK   g: one undo after an Alt drag restores the text
FAIL e: the playhead position at press (1.5) is still a snap point
OK   g: one undo after a playhead snap restores the text
FAIL f: trimming the text's right edge snaps to 10.0
FAIL f: the guide line is at the trimmed edge (10.0)
OK   g: one undo after a text right trim restores it
FAIL f: trimming the text's left edge snaps to 6.0 (end stays 8.8)
OK   g: one undo after a text left trim restores it
OK   f: the left trim still snaps to the clip boundary 3.5 (existing snap)
FAIL f: with Alt held, the left trim does not snap
OK   g: one undo after an Alt trim restores the text
OK   f: (setup) pressing the middle of the audio selects it
FAIL f: moving the audio snaps its left edge to 2.0
FAIL f: the guide line is at 2.0 while moving the audio
OK   g: one undo after an audio move restores it
OK   f: with Alt held, the audio does not snap to the playhead (1.5)
OK   g: one undo after an Alt audio move restores it
FAIL f: trimming the audio's right edge snaps to 2.0
OK   g: one undo after an audio right trim restores it
FAIL f: trimming the audio's left edge snaps to 2.0 (end stays 2.6)
OK   g: one undo after an audio left trim restores it
OK   h: moving a clip shows only the insertion line in the video row
OK   h: dragging a clip past the next one reorders the clips
OK   h: (setup) one undo puts the clips back
OK   h: trimming a clip's edge does not snap (end 10.067, not 10.0)
OK   h: no guide line while trimming a clip
OK   h: (setup) one undo restores the clip length
OK   h: clicking a clip moves the playhead to tOf(x), not snapped
OK   h: clicking a text moves the playhead to tOf(x), not snapped
OK   h: double-click adds a text at tOf(x) (frame-rounded), not snapped
note: after the double-click the playhead is at 1.9500 (the first press scrubs like a single press)
OK   h: (setup) one undo removes the added text
OK   h: wheel still scrolls the timeline horizontally
OK   h: Ctrl+wheel still zooms by 1.15
note: menu bar Alt navigation style hint 1, window active 1
OK   j: control: an Alt tap alone selects the menu bar (standard Windows behavior, unchanged)
OK   j: control: Escape leaves the menu bar
OK   j: Alt held from before the press; mouse released first, then Alt: the menu bar is not selected
OK   j: that Alt drag did not snap
OK   j: Alt pressed during the drag; mouse released first, then Alt: the menu bar is not selected
OK   j: after an Alt text drag (mouse released first, then Alt) the menu bar is not selected
OK   j: that Alt text drag did not snap
OK   j: (setup) one undo puts the text back
note: Alt tapped during a drag without moving the mouse: the menu bar is selected
OK   j: Alt+V still opens the View menu
OK   j: the menu bar is left unselected at the end
OK   setup: every mouse event landed at the intended position with the intended modifiers
RESULT: 37 check(s) FAILED
RUN-EXIT=1
```

- チェックは 102 件（ウィンドウが前面だったので Alt+V の 1 件を含む）。OK 65 件、FAIL 37 件。画素比較の 7 件は、1 回目では基準の画像を保存するだけなので数に入らない（`note: saved ...` が 7 つ）。
- `setup:` の 4 行、(i) の 2 行、(h) の 12 行、(j) の 10 行（Alt+V を含む）はすべて OK。「吸着しない」「灰色のまま」「ガイド線が出ない」「元に戻すが 1 回で戻る」（g）も OK。
- 修正前の画像は `C:\e-editor-dev\tmp\kv-magnet\baseline\` に残した: 画素比較の基準 `ref-*.png` 7 枚と、この実行のすべての画像のコピー `shots-before\*.png` 22 枚。

### 5.2 計画 6.3 の予想（36 行）との違い

37 行のうち 34 行は計画 6.3 の行と同じ。違うのは次の 5 行で、どれもテキストかオーディオの「移動」（`Drag::ItemMove`）のチェック。

| チェック | 計画の予想（修正前） | 修正前の実際 | 修正前の値 |
|---|---|---|---|
| e: the left edge still snaps to the clip boundary 3.5 (existing snap) | OK | FAIL | 開始 3.55（3.5 にならない） |
| e: releasing Alt snaps again | OK | FAIL | 開始 3.517 |
| e: the playhead position at press (1.5) is still a snap point | OK | FAIL | 開始 1.55 |
| e: with Alt held, the existing edge snap is off too | FAIL | OK | 開始 3.533（もともと吸着しない） |
| f: with Alt held, the audio does not snap to the playhead (1.5) | FAIL | OK | 開始 1.55（もともと吸着しない） |

（値はコードからの計算。修正前のログには値を出していない。）

原因は修正前のコード（timelinewidget.cpp の旧 724-726 行）にある:

```cpp
const double s1 = snap(start, dragSnap_);              // 左端を合わせたときの開始位置
const double s2 = snap(start + dur, dragSnap_) - dur;  // 右端を合わせたときの開始位置
start = std::abs(s1 - start) <= std::abs(s2 - start) ? s1 : s2;
```

吸着しなかった側の端は、ずれが 0（浮動小数点の誤差で 1e-16 程度）になる。そのため、
- 右端だけが近いとき: 左端のずれ 0 ≤ 右端のずれ → s1（吸着しない）。proposal 1 章に書かれていた不具合。
- 左端だけが近いとき: 左端のずれ（例 0.05 秒）≤ 右端のずれ（0）が成り立たない → s2（吸着しない）。proposal には書かれていなかったが、同じ式から起きる。

つまり修正前は、テキストやオーディオを移動したとき、両端が同時に吸着先に近いときにしか吸着しなかった。例えば e の 381 の位置では、左端 3.55 はクリップの境目 3.5 まで 3px だが、右端 5.85 の近くに吸着先がないので、開始は 3.55 のままだった。Alt のチェック 2 つは、修正前はもともと吸着しない位置なので OK になった。長さの変更（`Drag::ItemTrim`）は端が 1 つなのでこの問題はなく、`f: the left trim still snaps to the clip boundary 3.5 (existing snap)` は修正前も OK、`f: with Alt held, the left trim does not snap` は修正前は FAIL（Alt でも吸着してしまう）で、計画の予想どおり。

ハーネスの誤りではない（チェックの意味・数値は計画どおりで、修正前のコードの動きを正しく測っている）ので、計画の手順 1 の扱い（「原因がハーネスにあると示せるときだけハーネスを直す」）に従い、ハーネスは変えていない。修正後のコードでは、この 5 行も含めて全部 OK になる（7 章）。修正後は `e: the left edge still snaps ...`・`e: releasing Alt snaps again`（Alt なしなら 3.5 に吸着）と `e: with Alt held, ...`（Alt なら 3.533 のまま）が並んで通るので、Alt で移動の吸着が外れることも確かめられている。

## 6. 手順 2（rulerStep の切り出し）のあと

`run-step.log`（全文）:

```
[0/2] Re-checking globbed directories...
[1/7] Automatic MOC for target kv-magnet
[2/7] Building CXX object CMakeFiles/kv-magnet.dir/kv-magnet_autogen/mocs_compilation.cpp.obj
[3/7] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/timelinewidget.cpp.obj
[4/7] Building CXX object CMakeFiles/kv-magnet.dir/C_/git/JPHACK/e-editor/native/src/ui/mainwindow.cpp.obj
[5/7] Building CXX object CMakeFiles/kv-magnet.dir/magnetcheck.cpp.obj
[6/7] Linking CXX executable kv-magnet.exe
note: device pixel ratio 1.25, viewport 900x278, window active 1
OK   setup: clips 0-3.5-10.5 s, text 6.5-8.8 s, audio 0-2.6 s at 60 px/s
OK   setup: timeline can scroll to 300 and shows all rows
OK   setup: mouse events (and the Alt modifier) land at the intended viewport position
OK   i: idle timeline is pixel-identical to the unmodified code
OK   i: scrubbing away from snap points is pixel-identical
OK   i: scrubbing with Alt held is pixel-identical
OK   i: moving a text away from snap points is pixel-identical
OK   i: (setup) one undo puts the text back
OK   i: the ruler at 100 px/s is pixel-identical
OK   i: the ruler at 20 px/s is pixel-identical
OK   i: the red line over the header column is pixel-identical
OK   i: the red line and knob are still drawn over the header column
OK   a: a ruler press away from snap points seeks to tOf(x)
OK   a: 12 px before 0:02 does not snap (stays tOf(x))
FAIL a: 5 px before 0:02 snaps to 2.0
FAIL a: the 0:02 label turns yellow while snapped
OK   a: the other labels stay gray
OK   a: no guide line across the tracks while scrubbing
OK   a: the red line and knob are drawn at the playhead
FAIL a: 7 px before 0:02 snaps to 2.0
OK   a: 9 px before 0:02 does not snap
FAIL a: 5 px after 0:02 snaps to 2.0
OK   a: with Alt held, 5 px before 0:02 does not snap
OK   a: with Alt held, no label is yellow
FAIL a: after Alt is released, the next move snaps again
FAIL a: releasing the mouse keeps the snapped time
OK   a: after release every label is gray again
OK   a: scrubbing adds nothing to the undo history
FAIL b: a ruler press 3 px before 0:02 snaps to 2.0 at once
OK   b: a ruler press with Alt held does not snap
FAIL b: a press on the top-left corner snaps to 2.0
FAIL b: the snapped red line shows over the header column (x=78)
OK   b: a corner press with Alt held does not snap
FAIL b: a press on an empty track spot snaps to 10.0 (and deselects)
FAIL c: snaps to the clip boundary 3.5 (4 px away)
FAIL c: snaps to the text start 6.5 (3 px away)
FAIL c: snaps to the text end 8.8 (3 px away)
FAIL c: snaps to the audio end 2.6 (4 px away)
FAIL c: snaps to 0:00 (5 px away)
FAIL c: snaps to the end of the video 10.5 (4 px away)
OK   c: a snap to a point that is not a ruler number shows no yellow
FAIL c: the text start 4.1 (4 px) wins over the ruler number 4.0 (2 px)
OK   c: (setup) one undo puts the text back
OK   c: the playhead's own old position (1.0, 3 px) is not a snap point
OK   d: at 60 px/s (step 2 s) 0.95 s does not snap to 1.0
FAIL d: at 100 px/s (step 1 s) snaps to 1.0
FAIL d: at 100 px/s the 0:01 label turns yellow
FAIL d: at 20 px/s (step 5 s) snaps to 5.0
FAIL d: at 20 px/s the 0:05 label turns yellow
OK   d: at 20 px/s 2.0 is not a snap point
OK   d: (setup) Ctrl+wheel during the drag zoomed in to the 1 s step
FAIL d: after zooming during the drag, the next move uses the new step (1.0)
OK   e: (setup) pressing the middle of the text selects it
FAIL e: the left edge snaps to the ruler number 6.0
FAIL e: a yellow guide line runs across all tracks at 6.0
OK   e: no ruler label turns yellow while moving a text
OK   e: the guide line disappears after release
OK   g: one undo after a text move restores it
FAIL e: only the right edge is near the clip end: it snaps (start 8.2, end 10.5)
FAIL e: the guide line is at the right edge (10.5)
OK   g: one undo after a right-edge snap restores the text
FAIL e: the left edge still snaps to the clip boundary 3.5 (existing snap)
OK   e: with Alt held, the existing edge snap is off too
OK   e: no guide line while Alt is held
FAIL e: releasing Alt snaps again
OK   g: one undo after an Alt drag restores the text
FAIL e: the playhead position at press (1.5) is still a snap point
OK   g: one undo after a playhead snap restores the text
FAIL f: trimming the text's right edge snaps to 10.0
FAIL f: the guide line is at the trimmed edge (10.0)
OK   g: one undo after a text right trim restores it
FAIL f: trimming the text's left edge snaps to 6.0 (end stays 8.8)
OK   g: one undo after a text left trim restores it
OK   f: the left trim still snaps to the clip boundary 3.5 (existing snap)
FAIL f: with Alt held, the left trim does not snap
OK   g: one undo after an Alt trim restores the text
OK   f: (setup) pressing the middle of the audio selects it
FAIL f: moving the audio snaps its left edge to 2.0
FAIL f: the guide line is at 2.0 while moving the audio
OK   g: one undo after an audio move restores it
OK   f: with Alt held, the audio does not snap to the playhead (1.5)
OK   g: one undo after an Alt audio move restores it
FAIL f: trimming the audio's right edge snaps to 2.0
OK   g: one undo after an audio right trim restores it
FAIL f: trimming the audio's left edge snaps to 2.0 (end stays 2.6)
OK   g: one undo after an audio left trim restores it
OK   h: moving a clip shows only the insertion line in the video row
OK   h: dragging a clip past the next one reorders the clips
OK   h: (setup) one undo puts the clips back
OK   h: trimming a clip's edge does not snap (end 10.067, not 10.0)
OK   h: no guide line while trimming a clip
OK   h: (setup) one undo restores the clip length
OK   h: clicking a clip moves the playhead to tOf(x), not snapped
OK   h: clicking a text moves the playhead to tOf(x), not snapped
OK   h: double-click adds a text at tOf(x) (frame-rounded), not snapped
note: after the double-click the playhead is at 1.9500 (the first press scrubs like a single press)
OK   h: (setup) one undo removes the added text
OK   h: wheel still scrolls the timeline horizontally
OK   h: Ctrl+wheel still zooms by 1.15
note: menu bar Alt navigation style hint 1, window active 1
OK   j: control: an Alt tap alone selects the menu bar (standard Windows behavior, unchanged)
OK   j: control: Escape leaves the menu bar
OK   j: Alt held from before the press; mouse released first, then Alt: the menu bar is not selected
OK   j: that Alt drag did not snap
OK   j: Alt pressed during the drag; mouse released first, then Alt: the menu bar is not selected
OK   j: after an Alt text drag (mouse released first, then Alt) the menu bar is not selected
OK   j: that Alt text drag did not snap
OK   j: (setup) one undo puts the text back
note: Alt tapped during a drag without moving the mouse: the menu bar is selected
OK   j: Alt+V still opens the View menu
OK   j: the menu bar is left unselected at the end
OK   setup: every mouse event landed at the intended position with the intended modifiers
RESULT: 37 check(s) FAILED
RUN-EXIT=1
```

- 109 件中 OK 72 件、FAIL 37 件。FAIL の 37 行は修正前とまったく同じ（`Compare-Object` で True）。
- `i: ... pixel-identical ...` の 7 行はすべて OK。目盛りの間隔の計算を `rulerStep()` に移しても、目盛りの見た目は 1 画素も変わっていない（60・100・20px/秒、見出しの上の赤い線を含む）。

## 7. 修正後の結果

### 7.1 ログ（`run-fixed-values.log` 全文。チェックごとの期待値と実測値つき）

`measured:` の行は、その上のチェックで測った値。`actual` が実測値、`expected` が期待値（`same()` で比べた 2 つ。1e-6 秒以内なら同じとする）。`warmth@t` は目盛りの数字 t の黄色さ（黄色は 50 以上、灰色は 25 未満）、`guideColoredPixelsInTracks` はトラックにあるガイド線の色の画素の数、`guideIn...` はその行にガイド線があるか（1 = ある）、`differingPixels` は修正前の画像と違う画素の数（-1 は大きさが違う）。真偽だけのチェック（赤い線の画素、選択、メニューバー、クリップの順番など）には `measured:` の行がない。

```
[0/2] Re-checking globbed directories...
[1/5] Automatic MOC for target kv-magnet
[2/4] Building CXX object CMakeFiles/kv-magnet.dir/magnetcheck.cpp.obj
[3/4] Linking CXX executable kv-magnet.exe
note: device pixel ratio 1.25, viewport 900x278, window active 1
OK   setup: clips 0-3.5-10.5 s, text 6.5-8.8 s, audio 0-2.6 s at 60 px/s
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   setup: timeline can scroll to 300 and shows all rows
OK   setup: mouse events (and the Alt modifier) land at the intended viewport position
OK   i: idle timeline is pixel-identical to the unmodified code
       measured: differingPixels=0.000000
OK   i: scrubbing away from snap points is pixel-identical
       measured: differingPixels=0.000000
OK   i: scrubbing with Alt held is pixel-identical
       measured: differingPixels=0.000000
OK   i: moving a text away from snap points is pixel-identical
       measured: differingPixels=0.000000
OK   i: (setup) one undo puts the text back
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   i: the ruler at 100 px/s is pixel-identical
       measured: differingPixels=0.000000
OK   i: the ruler at 20 px/s is pixel-identical
       measured: differingPixels=0.000000
OK   i: the red line over the header column is pixel-identical
       measured: differingPixels=0.000000
OK   i: the red line and knob are still drawn over the header column
OK   a: a ruler press away from snap points seeks to tOf(x)
       measured: actual=4.866667, expected=4.866667
OK   a: 12 px before 0:02 does not snap (stays tOf(x))
       measured: actual=1.800000, expected=1.800000
OK   a: 5 px before 0:02 snaps to 2.0
       measured: actual=2.000000, expected=2.000000
OK   a: the 0:02 label turns yellow while snapped
       measured: warmth@2.0=110.028037
OK   a: the other labels stay gray
       measured: warmth@0.0=0.391892, warmth@4.0=-0.021127, warmth@6.0=1.121622, warmth@8.0=1.000000, warmth@10.0=0.166667, warmth@12.0=1.710526
OK   a: no guide line across the tracks while scrubbing
       measured: guideColoredPixelsInTracks=0.000000
OK   a: the red line and knob are drawn at the playhead
OK   a: 7 px before 0:02 snaps to 2.0
       measured: actual=2.000000, expected=2.000000
OK   a: 9 px before 0:02 does not snap
       measured: actual=1.850000, expected=1.850000
OK   a: 5 px after 0:02 snaps to 2.0
       measured: actual=2.000000, expected=2.000000
OK   a: with Alt held, 5 px before 0:02 does not snap
       measured: actual=1.916667, expected=1.916667
OK   a: with Alt held, no label is yellow
       measured: warmth@0.0=0.391892, warmth@2.0=1.705882, warmth@4.0=-0.021127, warmth@6.0=1.121622, warmth@8.0=1.000000, warmth@10.0=0.166667, warmth@12.0=1.710526
OK   a: after Alt is released, the next move snaps again
       measured: actual=2.000000, expected=2.000000
OK   a: releasing the mouse keeps the snapped time
       measured: actual=2.000000, expected=2.000000
OK   a: after release every label is gray again
       measured: warmth@0.0=0.391892, warmth@2.0=3.128440, warmth@4.0=-0.021127, warmth@6.0=1.121622, warmth@8.0=1.000000, warmth@10.0=0.166667, warmth@12.0=1.710526
OK   a: scrubbing adds nothing to the undo history
       measured: revision=6.000000, revisionBefore=6.000000
OK   b: a ruler press 3 px before 0:02 snaps to 2.0 at once
       measured: actual=2.000000, expected=2.000000
OK   b: a ruler press with Alt held does not snap
       measured: actual=1.950000, expected=1.950000
OK   b: a press on the top-left corner snaps to 2.0
       measured: actual=2.000000, expected=2.000000
OK   b: the snapped red line shows over the header column (x=78)
OK   b: a corner press with Alt held does not snap
       measured: actual=2.083333, expected=2.083333
OK   b: a press on an empty track spot snaps to 10.0 (and deselects)
       measured: actual=10.000000, expected=10.000000
OK   c: snaps to the clip boundary 3.5 (4 px away)
       measured: actual=3.500000, expected=3.500000
OK   c: snaps to the text start 6.5 (3 px away)
       measured: actual=6.500000, expected=6.500000
OK   c: snaps to the text end 8.8 (3 px away)
       measured: actual=8.800000, expected=8.800000
OK   c: snaps to the audio end 2.6 (4 px away)
       measured: actual=2.600000, expected=2.600000
OK   c: snaps to 0:00 (5 px away)
       measured: actual=0.000000, expected=0.000000
OK   c: snaps to the end of the video 10.5 (4 px away)
       measured: actual=10.500000, expected=10.500000
OK   c: a snap to a point that is not a ruler number shows no yellow
       measured: warmth@0.0=0.391892, warmth@2.0=1.705882, warmth@4.0=-0.021127, warmth@6.0=1.121622, warmth@8.0=1.000000, warmth@10.0=0.166667, warmth@12.0=1.710526, guideColoredPixelsInTracks=0.000000
OK   c: the text start 4.1 (4 px) wins over the ruler number 4.0 (2 px)
       measured: actual=4.100000, expected=4.100000
OK   c: (setup) one undo puts the text back
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   c: the playhead's own old position (1.0, 3 px) is not a snap point
       measured: actual=1.050000, expected=1.050000
OK   d: at 60 px/s (step 2 s) 0.95 s does not snap to 1.0
       measured: actual=0.950000, expected=0.950000
OK   d: at 100 px/s (step 1 s) snaps to 1.0
       measured: actual=1.000000, expected=1.000000
OK   d: at 100 px/s the 0:01 label turns yellow
       measured: warmth@1.0=99.566038
OK   d: at 20 px/s (step 5 s) snaps to 5.0
       measured: actual=5.000000, expected=5.000000
OK   d: at 20 px/s the 0:05 label turns yellow
       measured: warmth@5.0=113.436893
OK   d: at 20 px/s 2.0 is not a snap point
       measured: actual=1.950000, expected=1.950000
OK   d: (setup) Ctrl+wheel during the drag zoomed in to the 1 s step
       measured: scrollX=0.000000, pps=79.350000
OK   d: after zooming during the drag, the next move uses the new step (1.0)
       measured: actual=1.000000, expected=1.000000
OK   e: (setup) pressing the middle of the text selects it
OK   e: the left edge snaps to the ruler number 6.0
       measured: actual=6.000000, expected=6.000000
OK   e: a yellow guide line runs across all tracks at 6.0
       measured: guideX=468.000000, guideInTextRow=1.000000, guideInVideoRow=1.000000, guideInAudioRow=1.000000, guideAtBottom=1.000000
OK   e: no ruler label turns yellow while moving a text
       measured: warmth@0.0=0.391892, warmth@2.0=1.705882, warmth@4.0=-0.021127, warmth@6.0=1.121622, warmth@8.0=1.000000, warmth@10.0=0.166667, warmth@12.0=1.710526
OK   e: the guide line disappears after release
       measured: guideColoredPixelsInTracks=0.000000
OK   g: one undo after a text move restores it
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   e: only the right edge is near the clip end: it snaps (start 8.2, end 10.5)
       measured: actual=8.200000, expected=8.200000, actual=10.500000, expected=10.500000
OK   e: the guide line is at the right edge (10.5)
       measured: guideX=738.000000, guideInTextRow=1.000000, guideInVideoRow=1.000000, guideInAudioRow=1.000000, guideAtBottom=1.000000
OK   g: one undo after a right-edge snap restores the text
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   e: the left edge still snaps to the clip boundary 3.5 (existing snap)
       measured: actual=3.500000, expected=3.500000
OK   e: with Alt held, the existing edge snap is off too
       measured: actual=3.533333, expected=3.533333
OK   e: no guide line while Alt is held
       measured: guideColoredPixelsInTracks=0.000000
OK   e: releasing Alt snaps again
       measured: actual=3.500000, expected=3.500000
OK   g: one undo after an Alt drag restores the text
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   e: the playhead position at press (1.5) is still a snap point
       measured: actual=1.500000, expected=1.500000
OK   g: one undo after a playhead snap restores the text
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   f: trimming the text's right edge snaps to 10.0
       measured: actual=6.500000, expected=6.500000, actual=10.000000, expected=10.000000
OK   f: the guide line is at the trimmed edge (10.0)
       measured: guideX=708.000000, guideInTextRow=1.000000, guideInVideoRow=1.000000, guideInAudioRow=1.000000, guideAtBottom=1.000000
OK   g: one undo after a text right trim restores it
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   f: trimming the text's left edge snaps to 6.0 (end stays 8.8)
       measured: actual=6.000000, expected=6.000000, actual=8.800000, expected=8.800000
OK   g: one undo after a text left trim restores it
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   f: the left trim still snaps to the clip boundary 3.5 (existing snap)
       measured: actual=3.500000, expected=3.500000
OK   f: with Alt held, the left trim does not snap
       measured: actual=3.533333, expected=3.533333
OK   g: one undo after an Alt trim restores the text
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   f: (setup) pressing the middle of the audio selects it
OK   f: moving the audio snaps its left edge to 2.0
       measured: actual=2.000000, expected=2.000000
OK   f: the guide line is at 2.0 while moving the audio
       measured: guideX=228.000000, guideInTextRow=1.000000, guideInVideoRow=1.000000, guideInAudioRow=1.000000, guideAtBottom=1.000000
OK   g: one undo after an audio move restores it
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   f: with Alt held, the audio does not snap to the playhead (1.5)
       measured: actual=1.550000, expected=1.550000
OK   g: one undo after an Alt audio move restores it
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   f: trimming the audio's right edge snaps to 2.0
       measured: actual=0.000000, expected=0.000000, actual=2.000000, expected=2.000000
OK   g: one undo after an audio right trim restores it
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   f: trimming the audio's left edge snaps to 2.0 (end stays 2.6)
       measured: actual=2.000000, expected=2.000000, actual=2.000000, expected=2.000000, actual=2.600000, expected=2.600000
OK   g: one undo after an audio left trim restores it
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
OK   h: moving a clip shows only the insertion line in the video row
OK   h: dragging a clip past the next one reorders the clips
OK   h: (setup) one undo puts the clips back
       measured: actual=10.500000, expected=10.500000
OK   h: trimming a clip's edge does not snap (end 10.067, not 10.0)
       measured: actual=10.066667, expected=10.066667
OK   h: no guide line while trimming a clip
       measured: guideColoredPixelsInTracks=0.000000
OK   h: (setup) one undo restores the clip length
       measured: actual=10.500000, expected=10.500000
OK   h: clicking a clip moves the playhead to tOf(x), not snapped
       measured: actual=1.950000, expected=1.950000
OK   h: clicking a text moves the playhead to tOf(x), not snapped
       measured: actual=7.950000, expected=7.950000
OK   h: double-click adds a text at tOf(x) (frame-rounded), not snapped
       measured: actual=1.966667, expected=1.966667
note: after the double-click the playhead is at 2.0000 (the first press scrubs like a single press)
OK   h: (setup) one undo removes the added text
OK   h: wheel still scrolls the timeline horizontally
       measured: scrollX=240.000000
OK   h: Ctrl+wheel still zooms by 1.15
       measured: actual=69.000000, expected=69.000000
note: menu bar Alt navigation style hint 1, window active 1
OK   j: control: an Alt tap alone selects the menu bar (standard Windows behavior, unchanged)
OK   j: control: Escape leaves the menu bar
OK   j: Alt held from before the press; mouse released first, then Alt: the menu bar is not selected
OK   j: that Alt drag did not snap
       measured: actual=1.916667, expected=1.916667
OK   j: Alt pressed during the drag; mouse released first, then Alt: the menu bar is not selected
OK   j: after an Alt text drag (mouse released first, then Alt) the menu bar is not selected
OK   j: that Alt text drag did not snap
       measured: actual=6.083333, expected=6.083333
OK   j: (setup) one undo puts the text back
       measured: actual=6.500000, expected=6.500000, actual=8.800000, expected=8.800000, actual=0.000000, expected=0.000000, actual=0.000000, expected=0.000000, actual=2.600000, expected=2.600000
note: Alt tapped during a drag without moving the mouse: the menu bar is selected
OK   j: Alt+V still opens the View menu
OK   j: the menu bar is left unselected at the end
OK   setup: every mouse event landed at the intended position with the intended modifiers
RESULT: ALL OK
RUN-EXIT=0
```

- 109 件すべて OK（修正前の 102 件＋画素比較の 7 件）。
- 実測値を出す前の `run-fixed.log`（計画どおりのハーネス）も `RESULT: ALL OK`・`RUN-EXIT=0` で、109 行の OK/FAIL の行は上と完全に同じ。そちらは `[2/7] magnetcheck.cpp`・`[4/7] timelinewidget.cpp`・`[5/7] mainwindow.cpp` をコンパイルし直してから実行した（修正後のアプリのソースでビルドされたことの確認）。

### 7.2 項目ごとのまとめ（配置: 30fps、クリップ 0〜3.5〜10.5 秒、テキスト 6.5〜8.8 秒、オーディオ 0〜2.6 秒。x は見出しの幅 108 を含むビューポートの座標）

| 項目 | 操作 | 期待 | 実測 |
|---|---|---|---|
| (a) 60px/秒（間隔 2 秒） | 目盛り 400 を押す | 4.866667（tOf） | 4.866667 |
| | 216（0:02 の 12px 手前）/ 219（9px）へ | 1.8 / 1.85（吸着しない） | 1.800000 / 1.850000 |
| | 223（5px 手前）/ 221（7px）/ 233（5px 後ろ）へ | 2.0 | 2.000000 ×3 |
| | 223 へ Alt 付き | 1.916667（吸着しない）、数字は灰色 | 1.916667、0:02 の黄色さ 1.7 |
| | Alt を離して 225 へ、離す | 2.0 のまま | 2.000000、2.000000 |
| | 吸着中の「0:02」/ ほかの数字 | 黄色（50 以上）/ 灰色（25 未満） | 110.0 / −0.02〜1.71 |
| | 離したあとの「0:02」 | 灰色 | 3.1 |
| | 元に戻すの履歴 | 増えない | revision 6 → 6 |
| (b) 押した瞬間 | 目盛り 225 を押す（なし / Alt） | 2.0 / 1.95 | 2.000000 / 1.950000 |
| | スクロール 150、左上の角 83 を押す（なし / Alt） | 2.0 / 2.083333 | 2.000000 / 2.083333、赤い線は見出しの上（x=78） |
| | テキストの行の空いた所 705 を押す | 10.0、選択が外れる | 10.000000、選択なし |
| (c) 内容の点 | 314 / 501 / 633 / 268 / 113 / 734 へ | 3.5 / 6.5 / 8.8 / 2.6 / 0 / 10.5 | 同じ値 |
| | 内容の点 4.1（4px）と数字 4.0（2px） | 4.1（内容の点が先） | 4.100000 |
| | 押す前の赤い線 1.0 の 3px 横（171） | 1.05（赤い線自身は吸着先でない） | 1.050000 |
| | 数字でない点（3.5）に吸着中 | 黄色なし | 数字の黄色さ 2 未満、ガイドの画素 0 |
| (d) 拡大率 | 60px/秒で 165（1.0 は数字でない） | 0.95 | 0.950000 |
| | 100px/秒で 203 | 1.0、「0:01」が黄色 | 1.000000、99.6 |
| | 20px/秒で 210 / 147 | 5.0（「0:05」が黄色）/ 1.95（2.0 は細かい線で吸着しない） | 5.000000（113.4）/ 1.950000 |
| | ドラッグ中に Ctrl+ホイール 2 回のあと 184 | 拡大率 79.35（間隔 1 秒）、1.0 | 79.35・スクロール 0、1.000000 |
| (e) テキストの移動 | 558 → 545 → 533 | 開始 6.0、ガイド線 x=468 | 6.000000、全部の行と一番下に線あり |
| | 558 → 600 → 664（右端だけが 10.5 に近い） | 開始 8.2・終わり 10.5、ガイド線 738 | 8.200000・10.500000、線あり |
| | 381 / 380（Alt）/ 379 | 3.5 / 3.533333（ガイドなし）/ 3.5 | 3.500000 / 3.533333（画素 0）/ 3.500000 |
| | 押したときの赤い線 1.5 の 3px 横（261） | 1.5 | 1.500000 |
| | 離したあと | ガイド線が消える | 画素 0 |
| (f) 長さの変更など | テキストの右端 632 → 650 → 699 | 終わり 10.0（開始 6.5）、ガイド線 708 | 10.000000（6.500000）、線あり |
| | テキストの左端 502 → 490 → 476 | 開始 6.0、終わり 8.8 | 6.000000、8.800000 |
| | 左端 325 / 324（Alt） | 3.5 / 3.533333 | 3.500000 / 3.533333 |
| | オーディオの移動 186 → 200 → 301 | 開始 2.0、ガイド線 228 | 2.000000、線あり |
| | オーディオの移動 279（Alt） | 1.55（赤い線 1.5 に吸着しない） | 1.550000 |
| | オーディオの右端 260 → 250 → 228 | 終わり 2.0（開始 0） | 2.000000（0.000000） |
| | オーディオの左端 112 → 150 → 228 | 開始 2.0・in 2.0・終わり 2.6 | 2.000000・2.000000・2.600000 |
| (g) 元に戻す | テキスト・オーディオのドラッグのたび（12 回）に 1 回だけ元に戻す | 6.5〜8.8、0〜2.6（in 0）に戻る | 12 回とも戻った |
| (h) 変えていない動き | クリップ A を 650 へ並べ替え | 並びが B, A。黄色はビデオの行の挿入線（738）だけ | そのとおり |
| | クリップ B の右端 734 → 720 → 708 | 終わり 302/30 = 10.066667（0:10 に吸着しない）、ガイドなし | 10.066667、画素 0 |
| | クリップの 225 / テキストの 585 をクリック | 1.95 / 7.95（吸着しない） | 1.950000 / 7.950000 |
| | テキストの行の 225 をダブルクリック | 追加されたテキストの開始 frameRound(1.95) = 1.966667（吸着しない） | 1.966667 |
| | ホイール / Ctrl+ホイール | スクロール 120 → 240 / 拡大率 60 → 69 | 240 / 69.000000 |
| (i) 見た目 | 吸着していない 7 つの状態の画像 | 修正前と同じ | 違う画素 0（7 枚とも） |
| (j) Alt とメニュー | 9 章 | | |

## 8. 画素比較と目視

- 画素比較: `i-idle`（何もしていない）、`i-scrub-free`（吸着しない所までの赤い線のドラッグ中）、`i-scrub-alt`（Alt 付きのドラッグ中）、`i-move-free`（吸着しない所までのテキストの移動中）、`i-idle-100`・`i-idle-20`（100・20px/秒の目盛り）、`i-idle-head`（見出しの上の赤い線）の 7 枚が、修正前の画像（`baseline\ref-*.png`）と全画素で同じ（違う画素 0）。手順 2 のあとも同じ。
- 目視（読み取りツールで PNG を開いた。DPR 1.25 の実ピクセルで 1125x348。見たのは `fixed` の実行の画像。`fixed-values` の実行で同じ名前で上書きされたが、コードは同じ）:
  - `a-snap-2s.png`: 赤い線とつまみが 0:02（実ピクセルで約 285 = 論理 228）にあり、つまみの右の「0:02」の数字が黄色。ほかの数字は灰色。トラックに黄色の線はない。
  - `a-released.png`・`c-content.png`: 数字はすべて灰色。`c-content.png` では赤い線がクリップの境目 3.5（論理 318）にぴったり重なっている。`a-released.png` の赤い線は 0:02 のまま。
  - `e-guide-left.png`: 黄色の縦線が 0:06（論理 468）に、目盛りのすぐ下から一番下まで出ていて、テキストの箱の左端を通る。見出しの列には出ていない。赤い線は 1.5 のまま。
  - `e-guide-right.png`: 線がビデオの終わり 10.5（論理 738）にあり、テキストの箱の右端がちょうどそこにある。
  - `f-trim-right.png`: 線が 0:10（論理 708）で、テキストの右端と一致。`f-audio-move.png`: 線が 0:02（論理 228）で、オーディオの左端と一致。
  - `e-released.png`・`e-alt.png`・`h-clip-trim.png`: 黄色の縦線はない（`e-alt.png` ではテキストが 3.5 のすぐ右、3.533 にある）。
  - `h-clip-move.png`: 黄色はビデオの行の挿入位置の線（738）だけで、修正前と同じ見た目。
  - `b-corner.png`・`i-idle-head.png`: 赤い線が見出しの列の上（論理 78 と 18）に出ていて、つまみは左上の角の上にある（今までどおり）。
  - `d-100.png`・`d-20.png`: 「0:01」と「0:05」が黄色で、赤い線はその数字の位置にある。
  - 修正前の `baseline\shots-before\i-idle.png` も開いて、目盛り・クリップ・テキスト・オーディオ・赤い線が正しく描かれていることを確かめた（基準として妥当）。
- 赤い線が数字に吸着したときの目盛りの線（数字の左の縦線）の黄色は、赤い線とつまみの真下に重なるので画像では見えない（コードで確かめた。11 章）。

## 9. (j) Alt を押しながらのドラッグとメニューバー

- 結果: すべて OK（修正前も修正後も同じ）。対策（計画の付録 C）は入れていない。
  - 対照: タイムラインにフォーカスがある状態で Alt だけを押して離すと、メニューバーが選ばれる（Windows のふつうの動き）。Esc で戻る。→ ハーネスがメニューバーの動きを再現できている。
  - Alt を押してから目盛りを押し、Alt 付きで動かし、マウスを離してから Alt を離す → メニューバーは選ばれない。このドラッグは吸着しない（1.916667 = tOf(223)）。
  - 目盛りのドラッグの途中で Alt を押し、Alt 付きで動かし、マウスを離してから Alt を離す → 選ばれない。
  - テキストを Alt を押しながら動かし、マウスを離してから Alt を離す → 選ばれない。吸着しない（開始 6.083333）。元に戻す 1 回で戻る。
  - Alt+V で View のメニューが開く（ウィンドウが前面だったので確かめられた）。最後にメニューバーは選ばれていない。
- `note: menu bar Alt navigation style hint 1`（Fusion でも Alt でメニューバーを選ぶ設定）。
- `note: Alt tapped during a drag without moving the mouse: the menu bar is selected`（参考。チェックにはしていない）: マウスを押したまま、まったく動かさずに Alt だけを押して離すと、メニューバーが選ばれる。これは Windows のふつうの Alt の動きで、修正前も同じ結果（Qt のメニューバーは「Alt を押してから離すまでにマウスのイベントがなかった」ときにメニューを選ぶ）。Alt を押したあとにマウスを少しでも動かせば選ばれない。今回のコードはキーのイベントを見ていないので、この動きは変えていない。

## 10. ユーザーに見える違い

1. 赤い線のドラッグ（目盛り・左上の角・トラックの空いた所から）: 目盛りの数字の位置から 8px 以内で止まり、その数字が黄色になる。0:00・ビデオの終わり・クリップの境目・テキストとオーディオの端でも止まる（数字の位置でなければ黄色にならない）。両方が近ければ内容の点が先。押した瞬間も吸着する。
2. テキストの行の空いた所のダブルクリックでは、1 回目の押しで赤い線が吸着することがある（ハーネスでは 1.95 の所で、赤い線が 2.0 になった。修正前は 1.95）。追加されるテキストの位置は今のまま（1.966667 = frameRound(1.95)）。
3. テキスト・オーディオの移動と長さの変更: 今の吸着先に加えて目盛りの数字にも止まる。移動では、左端だけ・右端だけが吸着先に近いときも止まる（修正前は 5.2 のとおり、両端が同時に近いときしか止まらなかった）。止まっている間、黄色の縦線がトラック全体に出て、離すと消える。
4. Alt を押している間は、どれも吸着しない（今あるテキスト・オーディオの吸着も）。
5. 吸着先から 8px 以内にはマウスで置けない。置きたいときは Alt を押しながらドラッグするか、← → で 1 フレームずつ動かす。
6. 変わらないもの: ← → と Home/End、再生中の動きと自動スクロール、元に戻す（1 回のドラッグは 1 回の操作）、拡大縮小とホイール、ビデオのクリップの並べ替えと端の長さの変更、ライブラリからのドロップ、ダブルクリックで追加するテキストの位置、クリップやテキストを押して離したときの赤い線の移動、見出しと赤い線の描き方、README とショートカット一覧（Alt の説明は足していない）。

## 11. 確認できなかったこと

- 本物の Windows のキー入力（WM_SYSKEYDOWN/UP、修飾キーの繰り返し、DefWindowProc）と、本物のマウスでの Alt の届き方: ハーネスは `QWindowSystemInterface` から入れるので、Windows のキーやマウスの変換は通らない。12 章の手順 10・11 で確かめてほしい。
- MainWindow の ← →・Home/End、ライブラリからのドロップ、実際の再生と自動スクロール（followPlayhead）、プレビューの映像: ハーネスでは作っていない（コードは変えていない）。
- 24fps と 60fps: ハーネスは 30fps だけ。目盛りの数字の位置は 0.5 秒の倍数なので 24・30・60fps のどれでもフレームの境目になり、frameRound を通しても値は変わらない（コードと計算で確かめた）。
- 拡大率 140px/秒以上（数字の間隔 0.5 秒、「0:00.5」のような表示）と、10 秒以上の間隔: 吸着と黄色の処理は同じコードだが、ハーネスでは 60・100・20・79.35px/秒だけを試した。
- テキストやオーディオが何段もあるとき・縦にスクロールしたとき: ハーネスは 1 段ずつ。ガイド線は目盛りの下からビューポートの一番下まで引くので、段の数には関係しない（コード）。
- DPR 1.25 以外、タッチパッドでの感じ、実際のアプリでの手の操作（配置していないため）。
- 赤い線が数字に吸着したときの目盛りの線の黄色: 赤い線とつまみの真下に重なるので画像では確かめられない（コードで、数字と同じ条件 `snapped` で線の色も変えていることを確かめた）。
- 修正前のハーネスのログには値を出していないので、5.2 の表の「修正前の値」はコードからの計算（FAIL/OK の結果はログのとおり）。

## 12. 手で確かめる手順（最後の報告にこのまま書く）

1. アプリを閉じてから `build.cmd` をダブルクリックします（今回は起動中のアプリを止めないよう配置していないので、`native\dist` の exe はまだ古いままです）。
2. 動画を 2 本タイムラインに置き、テキストと BGM を 1 つずつ足します。
3. 目盛り（時間の数字の行）をつかんでゆっくり左右にドラッグします。「0:02」「0:04」などの数字の所で赤い線が少し止まり、止まっている間はその数字が黄色になります。さらに動かすと離れ、数字は灰色に戻ります。マウスを離すと黄色は消えます。
4. 数字のすぐ近くをクリックすると、赤い線が数字にぴったり合います。左上の角（合計時間の所）やトラックの空いた所をクリックしても同じです。
5. 赤い線をクリップの境目、テキストや BGM の端、0:00、動画の終わりに近づけると、そこでも止まります（数字の位置でなければ黄色にはなりません）。
6. Alt を押したままドラッグすると、どこにも止まりません。Alt を離して動かすと、また止まります。
7. 拡大・縮小すると、止まる間隔が目盛りの数字に合わせて変わります（拡大すると 1 秒・0.5 秒、縮小すると 5 秒・10 秒など）。
8. テキストや BGM をドラッグすると、左端か右端が、数字の位置・つかんだときの赤い線・クリップの境目・ほかの端に近づいたときに止まり、黄色の縦線がタイムライン全体に出ます。離すと消えます。右端だけ、または左端だけを近づけても止まります（直した不具合）。端をつかんで長さを変えるときも同じです。
9. テキストを動かしたあと Ctrl+Z を 1 回押すと、動かす前に戻ります。
10. Alt を押したままテキストか赤い線をドラッグし、マウスを離してから 1 秒ほど待って Alt を離します。メニューバーの「ファイル」が選ばれた状態になりません。続けて ← → を押すと、赤い線が 1 フレームずつ動きます。
11. 何もしていないときに Alt だけを押して離すと、今までどおりメニューバーが選ばれます（Esc で戻ります）。Alt+V で「表示」メニューが開きます。マウスを押したまま動かさずに Alt だけを押して離したときも、メニューバーが選ばれます（Windows のふつうの動きで、前と同じです）。
12. ビデオのクリップの並べ替えと端のドラッグ、ライブラリからのドラッグ＆ドロップ、テキストの行のダブルクリックでのテキストの追加、ホイール・Ctrl+ホイール、再生中の自動スクロールは今までどおりです（吸着しません）。ダブルクリックの 1 回目で赤い線が数字に吸着することがありますが、追加されるテキストの位置は今までどおりです。
13. 数字や端のすぐ近く（8px 以内）に置きたいときは、Alt を押しながらドラッグするか、← → で 1 フレームずつ動かしてください。

## 13. 最後の状態と一時ファイル

- `git -C c:\git\JPHACK\e-editor status --short`:
  ```
   M native/src/ui/timelinewidget.cpp
   M native/src/ui/timelinewidget.h
  ?? .agents/tasks/timeline-magnet/
  ```
  `git diff --name-only` は上の 2 つだけ（README.md・mainwindow.cpp・editor.cpp/.h・アイコン・app.qrc などは変わっていない）。ブランチは `feat/ikumu1`、HEAD は `eaac4b6` のまま。コミット・push・stash はしていない（stash の一覧は空）。
- ユーザーのアプリは起動したまま（PID 13748、11:29:33 起動）。配置していないので `native\dist` の exe は 2026-10-22 00:47:58 のまま。
- 取り込んでいない上流の変更（参考。頼まれていないので pull はしていない）: `origin/feat/ikumu1` は作業の始めは 3 コミット先だったが、作業中に 5 コミット先になった（追跡ブランチが更新された。私は fetch していない）。増えた `72c2e3f`（テキストのプリセットのドラッグ＆ドロップ）と `7136035`（音量キーフレーム。14:42:48）は、どちらも timelinewidget.cpp/.h を大きく変えている。今回の変更と同じ所（mousePressEvent の `Drag::Scrub` の分岐、オーディオの左端の長さ変更の `x.in = inPoint;` の直後など）にも手が入っているので、あとで pull やマージをすると timelinewidget.cpp で衝突する見込み。また上流ではオーディオの段の高さ（`kAudioLaneH` 42 → 52）が変わるので、このハーネスの行の位置は今の HEAD 用。
- 一時フォルダー `C:\e-editor-dev\tmp\kv-magnet\` は、レビューで直すことになったときに、ハーネスと修正前の基準をそのまま使えるように残してある。中身:
  - ハーネス: `CMakeLists.txt`、`magnetcheck.cpp`（4 章の実測値の出力を足したもの）、`run.ps1`、ハーネスのビルド `build\`。
  - 基準と画像: `baseline\ref-*.png`（7 枚、修正前のコードで保存）、`baseline\shots-before\*.png`（修正前の 22 枚）、`shots\*.png`（最後の実行の 22 枚）。
  - ログ: `build-1.log`、`build-2.log`、`run-baseline.log`、`run-step.log`、`run-fixed.log`、`run-fixed-values.log`、`compare.log`、`final.log`、`diff.log`、`diff.patch`、`env.log`、`shape.log`、`keep-before.log`、`ls.log`、`ping.log`、`qtest-api.log`、`upstream.log`、`upstream-head.log`、`upstream-timeline.log`、`upstream-timeline.patch`、`diffout.log`。
  - 補助のスクリプト（ターミナルの出力が切れるので、コマンドをファイルにして実行した）: `appbuild.ps1`、`harness.ps1`、`compare.ps1`、`final.ps1`、`diffout.ps1`、`env.ps1`、`shape.ps1`、`keep-before.ps1`、`ls.ps1`、`ping.ps1`、`upstream.ps1`。どれも読み取りかログの書き出しだけで、リポジトリには書き込まない（`build.ps1 -NoDeploy` が `native\build\` に書くのを除く）。
  - 消し方（レビューで承認されたあとの最後のステップで行う。このフォルダーだけを消し、`C:\e-editor-dev\tmp` のほかのものには触らない）: `Remove-Item -Recurse -Force -LiteralPath 'C:\e-editor-dev\tmp\kv-magnet'`
