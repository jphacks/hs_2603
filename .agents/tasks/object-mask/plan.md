# AI オブジェクトマスク（被写体をクリック → マスク → トラック）

依頼（2026-10-27）: Premiere の「オブジェクトマスク」のように、被写体にカーソルを合わせてクリックするだけで AI がマスクしてトラックする機能。
コミットはしない（これまでと同じ）。

## 方針（決めたこと）
- AI はアプリの中（オフライン）。ONNX Runtime（onnxruntime.dll を LoadLibrary で読む。C API。無ければ機能だけ使えない）+ SAM2 系の動画トラッカー（ONNX）。
  モデルとランタイムは setup.ps1 でダウンロード（版・SHA-256 固定）→ build.ps1 が dist に入れる。
- マスクは「素材のフレーム（pts）ごと」のデータ。generated フォルダーの不変のファイル（*.kvmask。作り直すたびに新しいファイル）。
  VideoClip に ObjectMask（file・設定・プロンプトの点と時間）。split・複製・コピーはそのまま共有（不変なので安全）。履歴も軽い。
  プロンプトを保存するので、ファイルが無い PC でも「トラックし直す」で作り直せる。
- 保存は低解像度のロジット（256×256 など）を圧縮。描くときに必要な大きさへ双線形で広げ、境目・広げる・ぼかし・反転を C++ の同じ関数で作る
  → プレビュー・静止画・書き出し（PNG 連番にして alphamerge）が同じ計算。
- モード: 背景を消す（alpha = マスク。メインでは黒）/ エフェクトをマスクの中だけ（反転で外だけ）。
- 書き出し: Exporter::start で、出力フレームごとに「ffmpeg の fps が選ぶ素材のフレーム」（round(((pts-readStart)/speed+lead)*FPS) ≤ n の最後）の
  マスクを srcW×srcH の Grayscale8 PNG にする。フレーム番号でそろえて [cNsrc] と組にする。
- プレビュー: VideoSource::frameAt に pts を返させ、ComposeOptions でマスクを渡す。processSourceFrame / prepareSourceFrame で掛ける。

## 仕組みのメモ（調べた結果。行は 2026-10-27 時点）
- model.h VideoClip(203-253, operator== default)。OverlayItem : VideoClip。ProjectData operator== → 新しい構造体にも == default が要る。
- projectio.cpp clipToJson(121-174)/clipFromJson(367-432)。使っているときだけ書く。overlay も同じ関数。
- editor.cpp commit(308) → clampVolumes(247。fixClip・エフェクトレイヤーで消す所) / insertFrameHold(2298。hold = c のコピー → マスクは消す) /
  setClipReversed(2359。素材が変わる → マスクのデータは使えない) / pasteAttributes(2397。AttrPart に足す。dialog は clipdialogs.cpp 220) /
  remapIds(3496。ID は開くたびに変わる → マスクは ID で持たない)。スポイト chromaPick_（Editor ctor 37-56 で自動でやめる）が「選ぶモード」の手本。
- compositor.cpp prepareSourceFrame(133)/processSourceFrame(142。キャッシュの params に足す)。mainLayer(381)・overlay(441)。
- previewengine.cpp compose(316)・sampleSource(91。画面→素材の座標)。VideoSource::frameAt(videosource.cpp 44)。
- ffutil.cpp VideoDecoder（open/seek/decodeNext/framePts/frameImage。RGB32・回転済み・SAR 済み）。
- exportplan.cpp メイン 439-。head 481（setpts=PTS/speed+lead/TB,fps=FPS:start_time=0）→ clipPreStatements(167) → mediaFit(498)/fit → tail。
  重ねる 653-（-ss in、lead なし）。エフェクトレイヤーのマスク PNG 連番の手本 667-703、exporter.cpp 236-278。
- previewwidget.cpp スポイトのモード（87-98, 250, 452, 552, 669, 1063, 1090, 1103）。TransportBar 1352-（loop_/guides_ の横にボタン）。
- inspector.cpp ClipPanel(195-)。クロマキーの欄 322-364・558-572・686-698 が手本。
- clipdialogs.cpp toggleReverseClip(400) が「重い作業 + 進み具合 + キャンセル + 終わったら確かめて commit」の手本。
- guidecatalog.cpp rows（props.clip.* / preview.* / menu.clip.*）。assistant.cpp systemPrompt の「道具ではできない編集」。

## モデル（決定: EdgeTAM。jax-image-tools/edgetam-video-onnx、fp32、4 グラフ 65MB、Apache-2.0）
- 試した所（C:\Users\yunre\e-editor-dev\tmp\objmask。Python の venv に onnxruntime・torch・transformers）:
  CPU（Core Ultra 9 185H）1 フレーム: EdgeTAM 586ms（venc 154・mdec 39・matt 234・menc 103。spin を切る。threads は既定=16 が速い。
  spin=1 だと 3 倍遅い）/ SAM2.1-tiny 512 fp16 630ms / SAM2.1-tiny 1024 3s。
- ホストの処理は transformers の EdgeTamVideoModel と同じにした（onnx_track.py）。合成した 48 フレームの動画で、PyTorch の
  propagate_in_video_iterator と全フレーム IoU 1.0000・ロジットの差 ≤0.001（前処理を自前の双線形にしても同じ）。GT との IoU 0.78〜0.99。
- グラフに無い定数: no_memory_embedding[256]・memory_temporal_positional_encoding[7][64]（HF yonigozlan/EdgeTAM-hf から取り出した → consts.npz）。
  enable_occlusion_spatial_embedding=false・enable_temporal_pos_encoding_for_object_pointers=false（ポインターの位置は 0）。
- ループ: クリックしたフレーム = feats2 + no_mem → decoder（点は 1024 の座標。ラベル int32）→ memory_encoder(is_mask_from_pts=true)。
  追うフレーム = cond（tpos 行 6）+ 前の 6 フレーム（r=6..1、tpos 行 r-1。逆向きは +r）→ ポインター（cond の t<=i（逆は >=）+ 前の 15 フレーム、
  1 つを 4×64 に割る）→ memory_attention（出力 [1,1,4096,256] → 転置して [1,256,64,64]）→ decoder（点 (0,0)・ラベル -1）→ memory_encoder(false)。
  前処理: 1024×1024 に引き伸ばし（縦横比は無視）、/255、ImageNet の mean/std。後処理: 256×256 のロジットを双線形で広げて > 0。

## 進み具合
- [x] モデルの選定（上）
- [x] setup.ps1 / build.ps1 / CMake（ORT・モデル・EdgeTAM の LICENSE。ONNXRUNTIME_DIR はヘッダーだけ。version.lib でランタイムの版を見る）
- [x] mask/: ORT の読み込み（ortapi）・トラッカー（edgetam）・kvmask と描画（masktrack）・重い仕事（maskjob）・操作（maskcontroller）
- [x] モデル（ObjectMask）・保存・編集操作（分割・複製・属性のペースト・逆再生・フレームの保持・エフェクトレイヤー）
- [x] プレビュー・静止画・AI に見せる画面・書き出し
- [x] UI（プレビューの下のボタン・選ぶモード・ホバー・クリック・設定欄・右クリック・「クリップ」メニュー・キー・案内・AI の説明）
- [x] インストーラー: Visual C++ ランタイム（14.44 以上）が無い・古い PC だけ、Microsoft から vc_redist.x64.exe をダウンロードして入れる
  （vcredist.ps1。署名を確かめる。同梱はしない）。アプリも msvcp140.dll の版を見て、古ければ AI の部品を使わない
- [x] README
- [x] 確かめる（下）

## 確かめたこと（2026-10-27）
一時的な --objmask-test（確かめたあと消した）で、アプリの中の本物の流れ（読み込み → MaskController::click → TrackJob → ファイル →
静止画・書き出し）を動かした:
- トラック: 48 フレームで 23.4 秒（読み込み込みで 1 フレーム 0.49 秒）。可逆の動画（libx264rgb -qp 0。PNG と同じ画素）で、PyTorch との IoU が
  最悪 0.973・ほとんど 0.999 以上、スコアの差 ≤0.1、GT との IoU 平均 0.954（遮る棒の通るフレームも 0.95 前後）。
  - 合成した x264 の mp4（crf 12・yuv420p）だと GT との IoU が 0.4〜0.9 に落ちるが、Python の ONNX でも同じフレームを入れると同じように落ちる
    （縞の細かい合成の被写体が、圧縮で崩れるため。実装の差ではない）。
- プレビュー（renderStillFrame）と書き出し（ProRes）: 同じフレームどうしが一番近い（前後のフレームとの差の 1/10 以下）。
  背景を消す: 平均の差 0.2〜0.3。エフェクトを中だけに（広げる 4・ぼかし 6・モザイク）: マスクの外はプレビューが元のまま（差 0.00）で、
  書き出しとの差（2.8）は書き出しの圧縮の差と同じ。中はどちらも元から 25.5 ほど変わり、互いの差は 2.5。重ねるトラック（半分の大きさ・赤の上）も同じ。
- 片方だけトラックし直す（後ろへ）: フレームの数が変わらない（前の結果を残す）。
- フレームの保持: 写真に 1 枚のマスク（新しい目印・点 1 つ）が付き、静止画が元の動画のそのフレームと同じ。
- 保存して読み直しても ObjectMask が同じ。
- 確かめていないこと: プレビューでのホバー・クリックの座標の変換（widgetToSource）と、設定欄・メニューの見た目は、手で触って確かめる。
