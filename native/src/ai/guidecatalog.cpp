#include "ai/guidecatalog.h"

#include <iterator>
#include <utility>

namespace kv::ai {

namespace {

using K = GuideKind;
using N = GuideNeed;

/** 一覧を作るときの 1 行分（ID の始まりは、グループの prefix と同じ文字で書く） */
struct Row {
  const char* id;
  const char* label;
  const char* place;
  K kind;
  const char* ref;
  N need = N::None;
  const char* view = "";
  const char* drop = "";
  const char* via = "";
};

std::vector<GuideTarget> build() {
  // 画面の言葉と同じ名前で書く（AI はこの名前で手順を説明し、カードにも出る）
  static const Row rows[] = {
      // ---- 画面の上の端 ----
      {"top.export", "「書き出し」ボタン（動画のファイルを作る画面を開く）", "右上の「書き出し」", K::Widget, "TopExport", N::Clips},
      {"top.undo", "元に戻すボタン", "右上の「元に戻す」", K::Widget, "TopUndo"},
      {"top.leftPanel", "左の欄を折りたたむ・開くボタン", "左上の、左の欄のボタン", K::Widget, "TopLeftPanel"},
      // ---- 左右の欄のタブ ----
      {"view.media", "メディア", "左の「メディア」タブ", K::View, "media"},
      {"view.audio", "オーディオ", "左の「オーディオ」タブ", K::View, "audio"},
      {"view.text", "テキスト", "左の「テキスト」タブ", K::View, "text"},
      {"view.effects", "エフェクト（トランジション・エフェクト）", "左の「エフェクト」タブ", K::View, "effects"},
      {"view.properties", "プロパティ（選んだものの設定）", "右の「プロパティ」タブ", K::View, "properties"},
      // ---- 左の欄の中 ----
      {"media.add", "「ファイルを選択」（動画・写真を読み込む）", "メディアの「ファイルを選択」", K::Widget, "MediaAddFiles", N::None, "media"},
      {"media.list", "一覧（ビデオの段へドラッグして入れる）", "メディアの一覧", K::Widget, "MediaList", N::Media, "media", "video"},
      {"media.overlay", "一覧（重ねる段へドラッグして、動画の上に重ねる）", "メディアの一覧 → 重ねる段", K::Widget, "MediaList", N::Clips, "media", "overlay"},
      {"audio.add", "「ファイルを選択」（BGM・効果音を登録する）", "オーディオの「ファイルを選択」", K::Widget, "AudioAddFiles", N::None, "audio"},
      {"audio.list", "一覧（タイムラインへドラッグして入れる）", "オーディオの一覧", K::Widget, "AudioList", N::AudioMedia, "audio", "audio"},
      {"text.presets", "スタイルのカード（タイムラインへドラッグして文字を入れる）", "テキストのスタイル", K::Widget, "TextPresetCard", N::None, "text", "text"},
      {"effects.transitions", "トランジションのカード（ビデオの切れ目へドラッグ）", "エフェクトの「トランジション」", K::Widget, "EffectsTransitionCard", N::TwoClips,
       "effects", "cut"},
      {"effects.effects", "エフェクトのカード（モザイク・ぼかし など。映像へドラッグ）", "エフェクトの「エフェクト」", K::Widget, "EffectsEffectCard", N::Clips,
       "effects", "clip"},
      {"effects.layers", "エフェクトレイヤー（重ねる段へドラッグ。範囲の中だけに効果）", "エフェクトの「エフェクトレイヤー」", K::Widget, "EffectsLayerCard",
       N::Clips, "effects", "overlay"},
      // ---- タイムライン ----
      {"timeline.split", "「分割」ボタン（再生位置で切る）", "タイムラインの「分割」", K::Widget, "TimelineSplit", N::Clips},
      {"timeline.delete", "「削除」ボタン", "タイムラインの「削除」", K::Widget, "TimelineDelete", N::Item},
      {"timeline.addText", "「テキスト」ボタン（再生位置に文字を入れる）", "タイムラインの「テキスト」", K::Widget, "TimelineAddText"},
      {"timeline.addAudio", "「オーディオ」ボタン（音のファイルを選んで入れる）", "タイムラインの「オーディオ」", K::Widget, "TimelineAddAudio"},
      {"timeline.snap", "磁石のボタン（吸着のオン・オフ）", "タイムラインの磁石のボタン", K::Widget, "TimelineSnap"},
      {"timeline.marker", "しおりのボタン（再生位置にマーカーを置く）", "タイムラインのしおりのボタン", K::Widget, "TimelineMarker"},
      {"timeline.zoom", "拡大率のつまみ", "タイムラインの拡大率のつまみ", K::Widget, "TimelineZoom"},
      {"timeline.ruler", "目盛り（押すと再生位置が動く。Shift+ドラッグで書き出す範囲）", "タイムラインの目盛り", K::Timeline, "ruler"},
      {"timeline.playhead", "再生位置の線のつまみ（ドラッグで動かす）", "再生位置のつまみ", K::Timeline, "playhead"},
      {"timeline.video", "ビデオの段（動画・写真が順に並ぶ）", "タイムラインのビデオの段", K::Timeline, "video"},
      {"timeline.overlay", "重ねる段（動画の上に重ねる映像・画像）", "タイムラインの重ねる段", K::Timeline, "overlay"},
      {"timeline.text", "テキストの段", "タイムラインのテキストの段", K::Timeline, "text"},
      {"timeline.audio", "オーディオの段", "タイムラインのオーディオの段", K::Timeline, "audio"},
      {"timeline.clip", "クリップ（押して選ぶ。ドラッグで並べ替え）", "タイムラインのクリップ", K::Timeline, "clip", N::Clips},
      {"timeline.edge", "選んだものの右端（ドラッグで長さを変える）", "選んだものの右端", K::Timeline, "edge", N::Item},
      {"timeline.fade", "選んだものの右上の丸いつまみ（ドラッグでフェード）", "選んだものの右上のつまみ", K::Timeline, "fade", N::Fadable},
      {"timeline.transition", "切れ目のトランジションの印（押して選ぶ）", "切れ目のトランジションの印", K::Timeline, "transition", N::HasTransition},
      {"timeline.videoEye", "ビデオの段の目の印（映像を隠す）", "ビデオの段の目の印", K::Timeline, "videoEye"},
      {"timeline.videoMute", "ビデオの段のスピーカーの印（動画の音を消す）", "ビデオの段のスピーカーの印", K::Timeline, "videoMute"},
      {"timeline.audioMute", "オーディオの段のスピーカーの印", "オーディオの段のスピーカーの印", K::Timeline, "audioMute"},
      // ---- プレビュー ----
      {"preview.stage", "画面（文字・映像をドラッグで動かす。角で大きさ）", "プレビューの画面", K::Widget, "PreviewStage"},
      {"preview.play", "再生ボタン", "プレビューの再生ボタン", K::Widget, "playButton"},
      {"preview.loop", "ループ再生のボタン", "プレビューのループのボタン", K::Widget, "TransportLoop"},
      {"preview.guides", "ガイドのボタン（セーフマージン・グリッド）", "プレビューのガイドのボタン", K::Widget, "TransportGuides"},
      {"preview.objectMask", "オブジェクトマスクのボタン（AI。押してから、プレビューで被写体をクリック）", "プレビューのオブジェクトマスクのボタン", K::Widget,
       "TransportObjectMask", N::Clips},
      {"preview.aspect", "比率（16:9・9:16・1:1・4:5）", "プレビューの比率の切り替え", K::Widget, "TransportAspect"},
      {"preview.fullscreen", "全画面表示のボタン", "プレビューの全画面ボタン", K::Widget, "fullScreenButton"},
      // ---- 右の「プロパティ」 ----
      {"props.project.speed", "「再生速度（動画全体）」", "プロパティの「再生速度（動画全体）」", K::Property, "再生速度（動画全体）", N::Nothing, "properties"},
      {"props.clip.speed", "「速度」", "プロパティの「速度」", K::Property, "速度", N::Video, "properties"},
      {"props.clip.duration", "写真の「表示時間」", "プロパティの「表示時間」", K::Property, "表示時間", N::Photo, "properties"},
      {"props.clip.audio", "「音声」（音量・ミュート）", "プロパティの「音声」", K::Property, "音声", N::Video, "properties"},
      {"props.clip.pan", "音の左右（パン）", "プロパティの「左右」", K::Property, "#ClipPan", N::Video, "properties"},
      {"props.clip.fit", "「表示方法」（全体表示・拡大・ぼかし背景）", "プロパティの「表示方法」", K::Property, "表示方法", N::Visual, "properties"},
      {"props.clip.motion", "位置・拡大率・回転・不透明度（キーフレームも）", "プロパティの位置・拡大率・回転", K::Property, "#ClipMotion", N::Visual, "properties"},
      {"props.clip.size", "「サイズ・切り抜き」", "プロパティの「サイズ・切り抜き」", K::Property, "サイズ・切り抜き", N::Visual, "properties"},
      {"props.clip.chroma", "「クロマキー」（緑の背景を透明に）", "プロパティの「クロマキー」", K::Property, "クロマキー", N::Visual, "properties"},
      {"props.clip.objectMask", "「オブジェクトマスク（AI）」（被写体をクリックしてマスク・トラック。背景を消す・エフェクトを中だけに）",
       "プロパティの「オブジェクトマスク（AI）」", K::Property, "#ClipObjectMask", N::Visual, "properties"},
      {"props.clip.effects", "「エフェクト」（左右・上下反転、付けたエフェクトの調整）", "プロパティの「エフェクト」", K::Property, "エフェクト", N::Visual, "properties"},
      {"props.clip.filter", "「フィルター」", "プロパティの「フィルター」", K::Property, "フィルター", N::Visual, "properties"},
      {"props.clip.adjust", "「調整」（明るさ・コントラスト・彩度・色温度）", "プロパティの「調整」", K::Property, "調整", N::Visual, "properties"},
      {"props.clip.fade", "「フェード」", "プロパティの「フェード」", K::Property, "フェード", N::Visual, "properties"},
      {"props.text.input", "文字の入力欄", "プロパティの文字の入力欄", K::Property, "#TextEdit", N::Text, "properties"},
      {"props.text.style", "「スタイル」", "プロパティの「スタイル」", K::Property, "スタイル", N::Text, "properties"},
      {"props.text.font", "「フォント」", "プロパティの「フォント」", K::Property, "フォント", N::Text, "properties"},
      {"props.text.color", "「色」", "プロパティの「色」", K::Property, "色", N::Text, "properties"},
      {"props.text.position", "「位置」", "プロパティの「位置」", K::Property, "位置", N::Text, "properties"},
      {"props.text.anim", "「登場のアニメーション」", "プロパティの「登場のアニメーション」", K::Property, "登場のアニメーション", N::Text, "properties"},
      {"props.audio.volume", "「音量」", "プロパティの「音量」", K::Property, "音量", N::Audio, "properties"},
      {"props.audio.pan", "音の左右（パン）", "プロパティの「左右」", K::Property, "#AudioPan", N::Audio, "properties"},
      {"props.audio.fade", "「フェード」", "プロパティの「フェード」", K::Property, "フェード", N::Audio, "properties"},
      {"props.transition.type", "「種類」", "プロパティの「種類」", K::Property, "種類", N::Transition, "properties"},
      {"props.transition.duration", "「長さ」", "プロパティの「長さ」", K::Property, "長さ", N::Transition, "properties"},
      {"props.transition.crossfade", "「音もクロスフェードする」", "プロパティの「音もクロスフェードする」", K::Property, "#TransitionCrossfade", N::Transition,
       "properties"},
      // ---- 上のメニュー ----
      {"menu.file.open", "「開く」", "「ファイル」→「開く」", K::Action, "file.open"},
      {"menu.file.save", "「保存」", "「ファイル」→「保存」", K::Action, "file.save"},
      {"menu.file.colorMatte", "「カラーマットを追加」（単色の画像）", "「ファイル」→「カラーマットを追加」", K::Action, "edit.colorMatte"},
      {"menu.file.exportFrame", "「フレームを書き出し」（再生位置の静止画）", "「ファイル」→「フレームを書き出し」", K::Action, "file.exportFrame", N::Clips},
      {"menu.file.importSubtitles", "「字幕を読み込む」（SRT・WebVTT）", "「ファイル」→「字幕を読み込む」", K::Action, "file.importSubtitles"},
      {"menu.file.rangeIn", "「書き出す範囲」→「開始を再生位置にする」", "「ファイル」→「書き出す範囲」→「開始を再生位置にする」", K::Action, "range.in", N::Clips},
      {"menu.file.rangeOut", "「書き出す範囲」→「終了を再生位置にする」", "「ファイル」→「書き出す範囲」→「終了を再生位置にする」", K::Action, "range.out", N::Clips},
      {"menu.edit.rippleDelete", "「リップル削除」（消して、空いた所を詰める）", "「編集」→「リップル削除」", K::Action, "edit.rippleDelete", N::Item},
      {"menu.edit.copyAttributes", "「属性をコピー」（色・エフェクト・動き）", "「編集」→「属性をコピー」", K::Action, "edit.copyAttributes", N::Visual},
      {"menu.edit.pasteAttributes", "「属性をペースト」", "「編集」→「属性をペースト」", K::Action, "edit.pasteAttributes", N::Visual},
      {"menu.edit.group", "「グループ化」（Ctrl+クリックで一緒に選んでから）", "「編集」→「グループ化」", K::Action, "edit.group"},
      {"menu.clip.speed", "「速度・デュレーション」", "「クリップ」→「速度・デュレーション」", K::Action, "edit.speed", N::Visual},
      {"menu.clip.detachAudio", "「音声を切り離す」", "「クリップ」→「音声を切り離す」", K::Action, "edit.detachAudio", N::Video},
      {"menu.clip.frameHold", "「フレームを保持」（再生位置で 2 秒止める）", "「クリップ」→「フレームを保持」", K::Action, "edit.frameHold", N::Clips},
      {"menu.clip.reverse", "「逆再生」", "「クリップ」→「逆再生」", K::Action, "edit.reverse", N::Video},
      {"menu.clip.objectMask", "「オブジェクトマスク（AI）」（押してから、プレビューで被写体をクリック）", "「クリップ」→「オブジェクトマスク（AI）」", K::Action,
       "edit.objectMask", N::Clips},
      {"menu.clip.addEffect", "「エフェクトを追加」", "「クリップ」→「エフェクトを追加」", K::Action, "clip.addEffect", N::Visual},
      {"menu.seq.addTransition", "「既定のトランジションを付ける」（再生位置に近い切れ目）", "「シーケンス」→「既定のトランジションを付ける」", K::Action,
       "edit.addTransition", N::TwoClips},
      {"menu.seq.transitionAll", "「すべての切れ目にトランジションを付ける」", "「シーケンス」→「すべての切れ目にトランジションを付ける」", K::Action,
       "seq.transitionAll", N::TwoClips},
      {"menu.seq.splitAll", "「すべてのトラックを再生位置で分割」", "「シーケンス」→「すべてのトラックを再生位置で分割」", K::Action, "edit.splitAll", N::Clips},
      {"menu.seq.trimStart", "再生位置より前を削除して詰める（リップルトリム）", "「シーケンス」→「リップルトリム: 再生位置より前を…」", K::Action,
       "edit.rippleTrimStart", N::Clips},
      {"menu.seq.trimEnd", "再生位置より後ろを削除して詰める（リップルトリム）", "「シーケンス」→「リップルトリム: 再生位置より後ろを…」", K::Action,
       "edit.rippleTrimEnd", N::Clips},
      {"menu.marker.edit", "「マーカーを編集」（名前・色・メモ）", "「マーカー」→「マーカーを編集」", K::Action, "marker.edit", N::Marker},
      {"menu.view.views", "「ビュー」（閉じた欄を開き直す）", "「表示」→「ビュー」", K::Action, "view.views"},
      {"menu.view.perspective", "「パースペクティブ」（欄の並びを保存・リセット）", "「表示」→「パースペクティブ」", K::Action, "view.perspective"},
      {"menu.workshop.text", "テキストのテンプレートを探す", "「ワークショップ」→「テキストのテンプレートを探す」", K::Action, "workshop.text"},
      {"menu.workshop.audio", "オーディオを探す", "「ワークショップ」→「オーディオを探す」", K::Action, "workshop.audio"},
      {"menu.workshop.image", "写真を探す", "「ワークショップ」→「写真を探す」", K::Action, "workshop.image"},
      {"menu.help.shortcuts", "「キーボードショートカット」（キーの一覧・変更）", "「ヘルプ」→「キーボードショートカット」", K::Action, "help.shortcuts"},
      // ---- ダイアログ ----
      {"dialog.export.format", "「形式」（MP4・GIF・音声だけ など）", "書き出しの画面の「形式」", K::Dialog, "ExportFormat", N::None, "", "", "top.export"},
      {"dialog.export.resolution", "「解像度」", "書き出しの画面の「解像度」", K::Dialog, "ExportResolution", N::None, "", "", "top.export"},
      {"dialog.export.start", "「書き出し」ボタン（書き出しを始める）", "書き出しの画面の「書き出し」", K::Dialog, "ExportStart", N::None, "", "", "top.export"},
      {"dialog.speed.value", "「速度」", "速度・デュレーションの画面の「速度」", K::Dialog, "SpeedValue", N::None, "", "", "menu.clip.speed"},
      {"dialog.speed.reverse", "「逆再生」", "速度・デュレーションの画面の「逆再生」", K::Dialog, "SpeedReverse", N::None, "", "", "menu.clip.speed"},
  };
  std::vector<GuideTarget> out;
  out.reserve(std::size(rows));
  for (const Row& r : rows) {
    GuideTarget t;
    t.id = QString::fromUtf8(r.id);
    t.label = QString::fromUtf8(r.label);
    t.place = QString::fromUtf8(r.place);
    t.kind = r.kind;
    t.ref = QString::fromUtf8(r.ref);
    t.need = r.need;
    t.view = QString::fromUtf8(r.view);
    t.drop = QString::fromUtf8(r.drop);
    t.via = QString::fromUtf8(r.via);
    out.push_back(std::move(t));
  }
  return out;
}

std::function<GuideInfo(const QString&)>& provider() {
  static std::function<GuideInfo(const QString&)> fn;
  return fn;
}

}  // namespace

const std::vector<GuideTarget>& guideTargets() {
  static const std::vector<GuideTarget> list = build();
  return list;
}

const std::vector<GuideGroup>& guideGroups() {
  static const std::vector<GuideGroup> list = {
      {QStringLiteral("top."), QStringLiteral("画面の上の端")},
      {QStringLiteral("view."), QStringLiteral("左右の欄のタブ")},
      {QStringLiteral("media."), QStringLiteral("左の「メディア」タブの中")},
      {QStringLiteral("audio."), QStringLiteral("左の「オーディオ」タブの中")},
      {QStringLiteral("text."), QStringLiteral("左の「テキスト」タブの中")},
      {QStringLiteral("effects."), QStringLiteral("左の「エフェクト」タブの中。タイムラインへドラッグして使う")},
      {QStringLiteral("timeline."), QStringLiteral("下のタイムライン")},
      {QStringLiteral("preview."), QStringLiteral("真ん中のプレビュー")},
      {QStringLiteral("props.project."), QStringLiteral("右の「プロパティ」。何も選んでいないとき")},
      {QStringLiteral("props.clip."), QStringLiteral("右の「プロパティ」。映像（動画・写真）を選んだとき")},
      {QStringLiteral("props.text."), QStringLiteral("右の「プロパティ」。テキストを選んだとき")},
      {QStringLiteral("props.audio."), QStringLiteral("右の「プロパティ」。オーディオを選んだとき")},
      {QStringLiteral("props.transition."), QStringLiteral("右の「プロパティ」。トランジションを選んだとき")},
      {QStringLiteral("menu.file."), QStringLiteral("上のメニュー「ファイル」")},
      {QStringLiteral("menu.edit."), QStringLiteral("上のメニュー「編集」")},
      {QStringLiteral("menu.clip."), QStringLiteral("上のメニュー「クリップ」")},
      {QStringLiteral("menu.seq."), QStringLiteral("上のメニュー「シーケンス」")},
      {QStringLiteral("menu.marker."), QStringLiteral("上のメニュー「マーカー」")},
      {QStringLiteral("menu.view."), QStringLiteral("上のメニュー「表示」")},
      {QStringLiteral("menu.workshop."), QStringLiteral("上のメニュー「ワークショップ」")},
      {QStringLiteral("menu.help."), QStringLiteral("上のメニュー「ヘルプ」")},
      {QStringLiteral("dialog.export."), QStringLiteral("書き出しの画面（top.export で開く）")},
      {QStringLiteral("dialog.speed."), QStringLiteral("速度・デュレーションの画面（menu.clip.speed で開く）")},
  };
  return list;
}

const GuideTarget* guideTarget(const QString& id) {
  for (const GuideTarget& t : guideTargets()) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

void setGuideInfoProvider(std::function<GuideInfo(const QString& id)> fn) {
  provider() = std::move(fn);
}

GuideInfo guideInfo(const QString& id) {
  return provider() ? provider()(id) : GuideInfo{};
}

}  // namespace kv::ai
