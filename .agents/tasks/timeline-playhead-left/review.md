# レビュー: 赤い再生ヘッドを見出しの列の上まで動かして見えるようにする（1 回目）

判定: APPROVED（阻害する指摘なし）。前回のレビューはない。

## 確かめたこと
- 差分: `git diff -- native/src/ui/timelinewidget.cpp native/src/ui/timelinewidget.h`。変わったのは timelinewidget.cpp だけ（20 行追加、19 行削除）で、.h は変わっていない。計画の付録 B と、verification.md 2 章に記録された diff の両方と一致する。
- 作業ツリー: `git status --short` は ` M native/src/ui/timelinewidget.cpp` と `?? .agents/tasks/timeline-playhead-left/` だけ。widgets.h・widgets.cpp・inspector.cpp は今回変わっていない（作業を始める前の 1f7d805 にコミット済み）。ブランチは `feat/ikumu1...origin/feat/ikumu1` で ahead なし、stash なし、`git diff --check` は終了コード 0 で出力なし、`C:\e-editor-dev\tmp\kv-playhead` は存在しない。verification.md の更新時刻（11:20）が記録された作業の時刻（00:49 ごろ）より後だったので、最終状態だけを狭く確認した。ビルドとハーネスは再実行していない。

## 合意内容との照合
1. 描く順番（合意 1）: 線（旧 paintEvent 607-609）とつまみ（旧 paintRuler 328-342）を、見出し（593-607）、目盛り・角・合計時間（609-615）のあと、paintEvent の最後（617-633）に移した。paintRuler の呼び出し元は paintEvent:610 だけなので、つまみがほかで消えることはない。左側の条件は画面の左端を基準にした `px >= -1` / `px >= -8` で、右側の条件は変えていない。つまみは fillPath と fillRect だけを使い、AA をオンにしてからオフに戻す（前と同じ）。合計時間の文字は、paintRuler が設定した 8pt のまま描かれる。見出しと角の塗りは変えていないので不透明のままで、その下のクリップ・目盛り・終わりの破線・挿入位置は隠れたまま。見出しの列にかからない位置（px >= 114）では重なるものがないので、見た目は同じ。
2. 見出しの上までのドラッグ（合意 2）: `Drag::Scrub` のしきい値は 0（684）で、689-690 の `ed.seek(tOf(pos.x()))` は x を制限していない。`Editor::seek`（editor.cpp:155-157）が `std::clamp(t, 0.0, std::max(0.0, contentEnd()))` で 0 と今ある上限にそろえる。このコードは変えていない。前に見えなかった原因は描く順番だけ。
3. 左上の角（合意 3）: 647 `if (pos.x() < kHeadW && pos.y() >= kRulerH) return;`。角（y < kRulerH）は rowAt:170 で Row::Ruler になり、hitTest:190 がすぐ返す。そのため、目盛りと同じ `ed.pause()` → `ed.seek(tOf(x))` → `Drag::Scrub` に入り、選択は外さない（目盛りと同じ）。トラックの行とその下の見出しは、今までどおりここで戻る。mouseDoubleClickEvent:794 は Row::Text のときだけテキストを足すので、角のダブルクリックでテキストは増えない。新しくドラッグを始められる場所は角だけ。
4. 変えないもの（合意 4）: kHeadW・kRulerH・xOf・tOf・hitTest・mouseMoveEvent・mouseReleaseEvent・mouseDoubleClickEvent・wheelEvent・followPlayhead・zoomAt・ドラッグ＆ドロップ・見出しと角の描き方は、どれも diff に入っていない。
5. 最小限の変更・C++・日本語コメント・リファクタリングなし（合意 5）: 満たしている。
- 見た目の違い（0:00 で線の左 1px が見出しの右の境界線 x=107 に重なる、0:00 でつまみの左半分が見える、つまみが合計時間に重なる）は、合意 1 から直接起きる結果。計画 D6 と verification.md 7 章に書かれている。

## 検証の記録（verification.md）
- ビルド: `build.ps1 -NoDeploy` のログが全文ある。`BUILD-EXIT=0` で、warning も error もない。
- ハーネス: 修正前は 47 件中 11 件が FAIL（計画で予想した 11 行と一致）。修正後は 49 件すべて OK（`RESULT: ALL OK`、`RUN-EXIT=0`）。内容は次のとおり。
  - (a) 目盛りから見出しの上、左端、左端の外までのドラッグ。時間と描画の両方を確かめた。
  - (b) スクロール 0 で 0:00 で止まる。
  - (c) 角のクリックとドラッグ、再生中なら止まること、スクロール 0 で 0:00 になること、ダブルクリックでテキストが増えないこと。
  - (d) トラックの行の見出しのクリック・ドラッグ・ダブルクリックで、何も変わらない。
  - (e) 見た目。見出しが不透明、ラベルと合計時間が見える、左端まで描く、画面の外は描かない、x=400 と画面の外で修正前と画素が一致する。
  - (f) 変えていない操作。
  - マウスの位置のずれはない（DPR 1.25）。
- 画像の目視確認、一時ファイルの削除、最後の git の状態も記録されている。
- 記録で確かめていない項目（プレビューの映像そのもの、実際のアプリでの手操作、DPR 1.25 以外、端のドラッグや Ctrl+ホイールなど）は、どれも今回のコードが触れていない経路なので、阻害する指摘にはしない。

## 指摘
なし。
