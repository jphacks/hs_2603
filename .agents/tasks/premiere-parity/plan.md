# Premiere Pro 相当の基本機能を足す（作業計画と進み具合）

依頼 1: 「プレミアプロにあるような編集機能を全部調べて実装してほしい（時間は無限にかかっていい）」。最初の依頼はモザイク。
依頼 2（2026-10-26）: 「基本機能が足りない。プレミアプロのように基本機能がたくさんほしい。調べて追加して」
→ 依頼 1 の途中（エフェクトの土台。UI なし）から続けて、基本機能を優先して足す。コミットはしない。

## 調べたこと（Premiere の基本機能。出典）
- ショートカット: Ripple Delete = Shift+Delete、Add Edit = Ctrl+K、Q/W = リップルトリム、J/K/L、I/O、M（マーカー）
  https://helpx.adobe.com/nz/premiere-pro/using/keyboard-shortcuts.html
- トランジション: ディゾルブ（クロスディゾルブ・暗転・ホワイトアウト・フィルムディゾルブ）、アイリス、スライド（プッシュ・スライド・分割）、ワイプ、ズーム
  https://helpx.adobe.com/ph_fil/premiere/desktop/add-video-effects/effects-and-transitions-library/list-of-video-transitions.html
- 速度・デュレーション: 逆再生、オーディオのピッチを維持、リップル編集
  https://helpx.adobe.com/ca/premiere/desktop/edit-projects/change-clip-speed/change-clip-speed-using-the-speedduration-option.html

## 環境
- ビルド: `powershell -File C:\Users\yunre\e-editor-dev\tmp\kv-pp\build.ps1`（-NoDeploy でコンパイルだけ。ログは build.log、UTF-16）。
  -Deploy を付けると dist に配置（アプリが起動中だと失敗する）。
- ターミナルの出力は乱れる → 結果はファイルに書いてから read_file で読む。PowerShell のスクリプトは ASCII だけで書く（日本語のコメントで行が壊れる）。
- 同梱 FFmpeg 8.1.3（xfade 58 種類、libx265/libvpx-vp9/prores_ks/gif/libmp3lame/libopus/pcm など）。FFmpeg のソース: `C:\Users\yunre\e-editor-dev\tmp\kv-pp\ffsrc\`

## 仕組みのメモ（調べた結果）
- model.h: VideoClip（メイン。隙間なく並ぶ = 削除・挿入で自然に詰まる）、OverlayItem : VideoClip（start, track, group, kind, region）、
  TextItem（track）、AudioItem（track。速度なし）、ProjectData。新しい項目は既定値つきで足せば、丸ごとコピーする操作では残る。
- editor.cpp: commit(fn, merge) → clampVolumes（値の範囲をそろえる所）→ resolveText/AudioOverlaps → 履歴。fn が true なら必ず履歴が増える。
  validSelection（新しい Selection の種類はここに足さないと消える）。splitAtPlayhead（1460〜。second = c の丸ごとコピー）。
  deleteSelection / duplicateSelection / copySelection / pasteClipboard / moveClipToOverlay / moveOverlayToMain。layout() は revision で作り直す（commit のあと参照は無効）。
- timeline.cpp: clipDuration = round((out-in)/speed*fps)/fps（写真は out-in）、layoutClips はフレームで積む、findClipAt（切れ目ちょうどは後ろのクリップ）。
- 書き出し: exportplan.cpp buildExportPlan。メインは clip ごとに -ss in -t dur*speed+0.5 -i → [cNsrc] → pre（反転・エフェクト）→ fit → tail（色・709・tpad/trim）→ fade → [vN]、
  音 [aN]、最後に concat → [vcat][acat]。重ねるトラック（エフェクトレイヤーは split+alphamerge）、文字、範囲・全体の速度、amix、出力引数（libx264 固定）。
  exporter.cpp: ExportRequest を作る（toExportClip）、ffmpeg を `-f mp4` 固定で起動、.part → rename。exportdialog.cpp で .mp4 固定。
- プレビュー: render/compositor（planFrame → フレームを用意 → composeFrame）。PreviewEngine::compose がデコーダー（key ごと）を管理。音は updateAudioSnapshot → AudioEngine::mixBlock（1 サンプルごとの gain の所に pan・ミュートを足せる）。
- UI: timelinewidget.cpp（paintRuler/paintVideo/paintOverlays/hitTest/mousePress/dragTo/finishDrag/contextMenuEvent/drag&drop）、
  inspector.cpp（ClipPanel など。Section/SliderRow/Segmented/ColorSwatches、upd(fn, merge) + released → endMerge）、
  objectmenu.cpp（execObjectMenu。addVisualMenus）、librarypanel.cpp（createMediaView/createAudioView/createTextView）、
  mainwindow.cpp（buildMenus、bindCommand(id, action)、addHiddenCommand、keymap の commands() に id と既定キー）、previewwidget.cpp（PreviewStage::paintEvent、TransportBar）。
- AI の道具（ai/tools.cpp）: split_clip は丸ごとコピー（新しい項目は残る。トランジションは消す必要あり）、add_media は項目ごとに作る（既定値で OK）。

## 機能の一覧と進み具合（凡例: [ ] まだ / [~] 作業中 / [x] 済み）

### 前回まで（依頼 1）に入れたもの（ビルドは通る。UI なし）
- [x] 合成の共通化（render/compositor）、反転・クリップのエフェクト・エフェクトレイヤーのデータ・保存・プレビュー・書き出し（core/effects）

### 今回やること（優先順）
- [x] 1. エフェクトの UI: 設定欄・反転・エフェクトレイヤー・タイムライン・左の「エフェクト」タブ（ui/effectsview。検索・ダブルクリック・ドラッグ・
        トランジションはカーソルで動く見本）・右クリック（objectmenu の addClipActions）・perspective の known（新しいビューを古い並びにも出す）
- [~] 2. トランジション: [x] データ〜UI・設定欄（inspector の TransitionPanel）・シーケンスのメニュー、[ ] パリティ確認（ハーネスに tr-* を足す）
- [x] 3. マーカー: データ・保存・表示・ドラッグ・ダイアログ・色・メニュー（マーカー）・M / Shift+M / Ctrl+Shift+M・Delete で削除
- [x] 4. 編集コマンド: メニュー（編集・クリップ・シーケンス・マーカー。mainwindow の buildEditingMenus）とキー
- [x] 5. 吸着のオン/オフ、ループ再生（Editor::play と PreviewEngine::tick）、セーフマージン・グリッド（PreviewStage::paintEvent、TransportBar）
- [x] 6. 静止画の書き出し（render/stillframe + ui/clipdialogs）、フレームの保持、カラーマット（MediaManager::addGeneratedImage）、音声を切り離す
- [x] 7. 書き出しの形式（ExportFormat。H.265・WebM・ProRes・GIF（フレームごとの 256 色）・MP3・WAV（映像は nullsink・小さく組み立てる））
- [x] 8. 速度・デュレーションの画面（Ctrl+R。逆再生はまだ）
- [x] 9. 字幕（SRT・WebVTT）の読み込み・書き出し（core/subtitles、Editor::importSubtitles、ファイルのメニュー）
- [~] 10. [x] 属性のペースト（画面も）、[x] リンクし直す（メディアの一覧の右クリック。Editor::relinkAsset）、[ ] パン
- [ ] 11. （時間があれば）色の基本補正の追加、トラックの非表示・ミュート、逆再生
- [x] 12. README（できること・ショートカット）
- [x] 13. 確かめる: ハーネス ops 37 件・io 7 件すべて PASS、tr-* はほかの機能と同じ程度の差（ノイズディゾルブは
        点が細かく H.264 の圧縮で崩れるだけ。grainprobe で xfade と 99.999% 一致を確認）、formats 7 形式すべて ffprobe で確認、
        still OK、ui（MainWindow を出して画面・メニュー・書き出しの画面を PNG に。out\ui）で見た目を確認。
        メニューに Ctrl・Alt を使わないキー（Q・W・PgUp など）も出すようにした（applyShortcuts の kvBaseText）

2026-10-26: build.ps1 -Deploy で native\dist\e-editor に配置済み（BUILD-EXIT=0）。コミットはしていない。

### 依頼 3（2026-10-26）: 「動いていない機能がないか、バグがないかしっかり検査して直して」「まだやってないこともやって」
- [x] 検査（5 つに分けて調べた: 書き出し・編集の中心・タイムライン・プレビュー・ほかの画面）→ 見つかったものを直した:
  - 編集の中心: commit が「何も変わらない」なら履歴に残さない（ProjectData に operator==）。リップル（Q・W・リップル削除）で取り除いた時間の
    中のもの（消す・頭や終わりを切る・前から続くものは短く）・マーカー（詰めた所へ）、重ねるものの段は上へだけ探す（liftOverlayToFreeLane）。
    フレームの保持で後ろもずらす。属性のペースト（速度を先に・使えるエフェクトだけ・種類の違う所は写さない・attributesChanged）。
    トランジションの長さの既定は離したときだけ覚える（rememberTransitionDefault）。1 フレームずらすのまとめ方。フレームレートを変えたときの丸め。
    字幕の重なり。見えていないタブのリンクし直し。AI に「トランジションを選んでいる」と伝える。
  - タイムライン: トランジションの端のドラッグ（相対・しきい値・小さい帯は選ぶだけ）、マーカーのドラッグ（しきい値・再生位置のつまみを優先・
    終わりまで）、押す順（フェード → クリップの端 → 帯）、自動スクロール（エフェクトのドラッグ・マーカー・トランジション）、描く順
    （マーカーの名前の上に印、帯の上にクリップの名前）、ツールバーの削除・複製の状態。
  - プレビュー: 静止画の書き出しのフレームの選び方をプレビューと同じに、トランジションの範囲の外だけ素材に収める・最初のフレーム（映像が遅れて
    始まる素材）、先読みをトランジションの始まりから、ループ再生（終わりの先読み・戻った直後は古いフレームを出さない・範囲の外へ動かしても
    くり返す・短すぎる範囲はくり返さない）、再生中のアイコンの描き直しを減らす、AI のコマのデコーダーをクリップごとに、エフェクトレイヤーの
    動かない範囲のマスクを使い回す。
  - 書き出し: 映像・音の時刻を、読み始めの位置（ファイルの始まり）を 0 にしたまま使う（プレビューと同じ。トランジションの前に足す分も端数のまま）。
    黒の背景にも BT.709 の印。
  - 画面: 速度・デュレーション（長さを入れたときの速度の精度・短くなりすぎない・写真の長さをフレームに）、フレームの保持（重ねるトラックでは
    使えない）、PgUp/PgDn（一覧の中では一覧の操作）、マーカーの名前の長さの制限をなくす、リンクし直しの画面の後で消えた素材を使わない。
- [x] パン（VideoClip / AudioItem の pan。プレビュー: AudioEngine の mixSourceGain と左右の倍率、書き出し: pan フィルター。設定欄のスライダー）
- [x] トラックの非表示・ミュート（ProjectData の mainVideoHidden ほか。見出しの目・スピーカーの印・右クリック、プレビュー・書き出し・音、
      うすく描く、保存は使っているときだけ "trackStates"）
- [x] トランジションの音のクロスフェード（Transition::audioCrossfade。新しく付けるものはオン、以前のファイルはオフ。書き出しは acrossfade の qsin、
      プレビューは MixSource の headExt/tailExt と同じカーブ。設定欄・帯の右クリック）
- [x] 逆再生（media/reverser: 区切って reverse・areverse → concat。generated に作って使い回す。Editor::setClipReversed で素材と in/out を
      入れ替え（reversedFrom に元の動画）。クリップのメニュー・右クリック・速度の画面。進み具合の画面でキャンセルできる）
- ハーネス: fixes（直した所 21 件）・features（新しい機能 26 件。音はプレビューの mixOffline と書き出しの WAV を比べる）を足した。
  2026-10-26 の結果: ops 38/38・io 7/7・fixes 21/21・features 26/26 PASS。all（映像の比較）は前と同じ程度の差（ノイズディゾルブだけ大きい。既知）。
  formats 7 形式 OK。still は静止画 = プレビューが完全に一致（mad 0.00。書き出しとの差は回転の縁の圧縮だけ）。ui の画面（out\ui\tracks.png ほか）も確認。
  ついでに直したもの: プレビューの音の読み始め（AudioDecoder::seek を 0.1 秒手前から。AAC の最初の音が 10ms ほど欠けていた）。
  build.ps1 -Deploy で配置済み（BUILD-EXIT=0）。コミットはしていない。

ハーネスの新しいシナリオ: tr-basic / tr-slides / tr-wipes / tr-wipes2 / tr-edge / tr-short / tr-layers、formats、still、io、ops、grainprobe、ui
（hbr.ps1 -Scenarios a,b -N <ログ番号>）。"all" は scenarios() の映像比較だけ。
ビルド: 2026-10-26 ここまで全部通った（BUILD-EXIT=0）。

### 依頼 4（2026-10-26）: 「AI に操作説明もできるようにしてほしい。どうやってするか聞かれたら、薄くポインターで案内するとか」
- [x] 道具 show_guide（ai/tools.cpp）: steps = [{target, text}] を 1〜6 個。編集はせず、画面の案内（ToolResult::guide）を返す。
  道具の説明（場所の ID の enum と、ID の始まりごとの短い説明）は ai/guidecatalog.cpp の一覧（101 か所）から作る。
  結果には場所の名前（place）と、ショートカットキー・今は使えない理由（画面の側が setGuideInfoProvider で答える）を付ける。
- [x] システムプロンプトに「操作の案内」（使い方を聞かれたら show_guide。準備はアプリが足す。道具でできない編集は、できないと伝えて案内）。
  できないことの行も直した（トランジション・重ねる映像などは、アプリではできるので案内する）。3,917 文字（上限 8,000）。
- [x] ui/guide.cpp: GuideController（手順ごとに 0.12 秒おきに見直す。need・view・via で準備の場所を先に示す。押す→離す・落とす、
  メニューの操作の triggered（キーでも）で次へ。済んだタブの手順は飛ばす。開いたダイアログの手順へ進む。Esc・2 分でやめる）と
  GuideOverlay（マウスを通す透明な最前面の窓。矢印が動いていく・押す合図の輪・枠・吹き出し。ドラッグは運ぶ動きをくり返す）。
- [x] 探すための名前: objectName（タイムラインのボタン・右上・メディア/オーディオの追加と一覧・エフェクトのカード・書き出しの形式 など）、
  Segmented は objectName を見た目に使うので kvGuide プロパティ。Section::title()、PerspectiveManager::reachView、
  TimelineView::guideRect / revealGuidePart、MainWindow::commandAction（objectName の QAction も）。
- [x] AI の欄のカード（手順の一覧・今の手順・やめる・次へ・もう一度見せる）。新しい会話・ログアウトで案内もやめる。
- [x] サーバーはそのまま（道具 19 個・JSON 18,420 文字 < 24,000）。guard.ts のコメントと server/README.md の数字だけ直した。
- [x] 確かめ: ハーネスの guide（38 項目すべて PASS。画面: out/ui/guide-menu・guide-drag・guide-dialog・guide-need）。

## メモ
- 保存形式は、使っているときだけ書く（以前の版でも開ける）。clipToJson / clipFromJson（projectio.cpp）。
- 新しい .cpp/.h は native/CMakeLists.txt の SOURCES に足す。アイコンは resources/icons と app.qrc。
