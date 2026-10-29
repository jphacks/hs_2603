# Notion 未着手 残り全部（2回目+3回目をまとめて）

ユーザー指示: 「やっぱり一気に全部やって」「時間は問わない」。C++、関係ない所は触らない、コミットしない、日本語で報告。
1回目（9項目）は実装済み・ビルド/リンク OK（2026-10-24 15:49 の exe）。実行時の確認はまだ。

## 対象
1. フォントのインポート（TTF/OTF/TTC、AppLocalData/fonts にコピーして起動時に登録、ファミリー名で保存）
2. 同梱フォント（OFL: Noto Sans JP R/B, M PLUS Rounded 1c R/B, Zen Maru Gothic R/B, Dela Gothic One, RocknRoll One,
   Rampart One, Reggae One, Mochiy Pop One, Kosugi Maru(Apache), Yusei Magic）。setup.ps1 で e-editor-dev にDL → build.ps1 で dist/fonts
3. テキストのテンプレート保存（見た目・登場アニメ・キーフレーム。文字は保存しない。テキスト欄に表示、名前変更・削除）
4. 文字の一部だけフォント変更（設定欄の入力欄で選んだ範囲 + フォント選択）
5. 重ねるトラック（OverlayItem : VideoClip {start, track, group}。音も鳴る）
6. サイズ自由変更（stretchX/Y と切り抜き crop L/T/R/B。切り抜きはその場に残す方式）
7. クロマキー（chromakey 相当: UV 距離、similarity/blend、despill）
8. グループ化（テキスト・オーディオ・重ねるトラックだけ。一緒に移動・削除・複製。Ctrl+クリックで複数選択、Ctrl+G / Ctrl+Shift+G）

## 決めたこと
- TextItem: fontFamily（空なら FontId）、fontSpans{start,length,font,family}、group
- 部分フォントの位置調整は Editor::commit で（文字が変わって spans が変わっていないとき、1 文字ごとの配列で付け直し）
- 重ねるトラック: track 0 がメインのすぐ上、数字が大きいほど前。文字はいつも一番前。新規は scale 0.5・中央
- 重ねるトラックは fit=Contain 固定（透明の余白）。表示方法の欄は出さない
- 選択: Selection::Overlay を追加。clip()/updateClip() は重ねるトラックも探す
- テンプレート ID は "user:<id>"、AppLocalData/text-templates.json
- textPreset(id) は値で返す
- フォントの固定 URL: google/fonts e7db6ca6、notofonts/noto-cjk f8d15753、coz-m/MPLUS_FONTS eb604901（SHA-256 は common.ps1）

## 追加で直したこと（実行確認で見つけたもの）
- 書き出しのクロマキー: ffmpeg の chromakey は画面の外の画素を前の値のまま使うので、上の端の 1 行と左の端の所々が半透明になる
  → pad 1px + fillborders(smear) → chromakey → crop で回避。キーの色は U・V（テレビの範囲）にして yuv=1 で渡す（RGB だとフル レンジの式でずれる）
- メインのトラックのぼかし背景 + クロマキー: 透明にした所は黒、ぼかし背景も黒にした映像から作る（書き出し: gbrap + premultiply、プレビュー: 黒の上に描く）
- スポイト（Editor::setChromaPick / chromaPickChanged、PreviewEngine::sampleSource、PreviewStage のスポイト表示、設定欄の「スポイト」）
- 設定欄の「色かぶりを抑える」の文字が長く、設定欄の幅からはみ出していた → 短くしてツールチップへ

## 進み具合
- [x] model / projectio / editor / timeline / exportplan / exporter / previewengine / imageops / textrender
- [x] inspector（サイズ・切り抜き・クロマキー・タイミング、テンプレート保存、フォント欄）、library（マイテンプレート、重ねて追加）
- [x] timeline（重ねる段、グループの印、Ctrl+クリック、まとめて移動）、preview（重ねるものの選択・辺のつまみ）、objectmenu、mainwindow、keymap
- [x] setup.ps1 / common.ps1 / build.ps1 のフォント
- [x] ビルド OK（UI まで）
- [x] 一時的なテスト（p2test.cpp, --p2-test）で実行確認（すべて OK）→ 消した。消したあともビルド・リンク OK
- [x] setup.ps1 でフォントを C:\e-editor-dev\fonts にダウンロード（SHA-256 確認 OK）
- [ ] build.ps1（デプロイ込み）でフォントのコピー確認 → dist のアプリが起動中のため未確認
