#pragma once
// 編集データの型。プロジェクトファイル (.kvproj) は JSON 形式で読み書きする。

#include <QColor>
#include <QString>
#include <array>
#include <vector>

namespace kv {

enum class AssetKind { Video, Audio, Image };
enum class AssetStatus { Loading, Ready, Error, Missing };

/** 読み込んだメディア */
struct Asset {
  QString id;
  QString path;
  QString name;
  AssetKind kind = AssetKind::Video;
  double duration = 0;
  int width = 0;
  int height = 0;
  bool hasAudio = false;
  AssetStatus status = AssetStatus::Loading;
  /** 画像: 向き(EXIF)とサイズを整えたファイル。書き出しで使う */
  QString imagePath;
  QString error;
};

/** 音量の上限（1.0 = 100% = 元の音量）。クリップの音量もオーディオの音量も、0〜この値の範囲で扱う */
constexpr double kMaxVolume = 1.0;

/** フェードイン・アウトの長さの上限（秒）。設定欄のスライダーとタイムラインのつまみで、この範囲で自由に変えられる */
constexpr double kMaxFade = 10.0;

enum class FitMode { Contain, Cover, Blur };
enum class FilterPreset { None, Vivid, Warm, Cool, Soft, Drama, Retro, Sepia, Mono };

struct ColorAdjust {
  FilterPreset preset = FilterPreset::None;
  double brightness = 1;
  double contrast = 1;
  double saturation = 1;
  double temperature = 0;
  bool operator==(const ColorAdjust&) const = default;
};

/**
 * 音量キーフレーム 1 つ。t はオーディオ(クリップ)の先頭からの秒数（動かしても変わらない）、
 * volume は 0〜kMaxVolume。隣のキーフレームとの間は直線でつなぐ
 */
struct VolumePoint {
  double t = 0;
  double volume = kMaxVolume;
  bool operator==(const VolumePoint&) const = default;
};

/** パン（左右の振り分け）の範囲。-1 が左だけ、0 が真ん中（元のまま）、+1 が右だけ */
constexpr double kMaxPan = 1.0;

/** パン p（-1〜+1）のときの、左・右の音量の倍率（バランス: 反対側を弱める。真ん中は元のまま） */
inline double panGainLeft(double p) { return p > 0 ? 1 - p : 1; }
inline double panGainRight(double p) { return p < 0 ? 1 + p : 1; }

// ---- キーフレームアニメーション（位置・拡大率・回転・不透明度） --------------------------
// 値は「オブジェクトの先頭からの秒数 t」のキーフレームで持つ。キーフレームが 1 つも無い項目は、
// 静的な値（x, y, scale, rotation, opacity）のまま変わらない。あるときはキーフレームの値が優先される。

/** キーフレームから次のキーフレームまでの変わり方 */
enum class Easing { Linear, EaseIn, EaseOut, EaseInOut, Hold };

/**
 * アニメーションできる項目。X, Y は画面に対する割合（中心の位置。0.5 が画面の中央）。
 * LetterSpacing（字間）は文字だけの項目で、基準解像度の px（映像のアニメーションには使わない）
 */
enum class AnimProp { X, Y, Scale, Rotation, Opacity, LetterSpacing };
constexpr int kAnimPropCount = 6;

/** 字間の範囲（基準解像度の px）。0 はフォント本来の文字間隔 */
constexpr double kMinLetterSpacing = -50;
constexpr double kMaxLetterSpacing = 100;

struct Keyframe {
  double t = 0;
  double value = 0;
  /** このキーフレームから次のキーフレームまでの変わり方 */
  Easing easing = Easing::Linear;
  bool operator==(const Keyframe&) const = default;
};

/** 項目ごとに独立したキーフレーム列（t の順）。空の項目はアニメーションしない */
struct Animation {
  std::array<std::vector<Keyframe>, kAnimPropCount> tracks;
  std::vector<Keyframe>& keys(AnimProp p) { return tracks[size_t(p)]; }
  const std::vector<Keyframe>& keys(AnimProp p) const { return tracks[size_t(p)]; }
  bool empty() const {
    for (const auto& t : tracks) {
      if (!t.empty()) return false;
    }
    return true;
  }
  bool operator==(const Animation&) const = default;
};

/** 切り抜き（素材の上下左右の端から切り落とす割合）の上限。左右・上下それぞれ、合わせてもこの範囲に収める */
constexpr double kMaxCrop = 0.9;
/** 伸縮（幅・高さだけの倍率）の範囲 */
constexpr double kMinStretch = 0.1;
constexpr double kMaxStretch = 4.0;
/** クロマキーの「透明にする色の近さ」と「境目のなめらかさ」の上限 */
constexpr double kMaxChromaSimilarity = 0.6;
constexpr double kMaxChromaBlend = 0.5;

/**
 * クロマキー: 指定した色（グリーンバックなど）に近い部分を透明にする。書き出しの ffmpeg chromakey と同じ考え方で、
 * 色の違いは明るさを除いた色み（BT.601 の U・V）の距離で測る（0〜1）
 */
struct ChromaKey {
  bool enabled = false;
  QColor color{0, 255, 0};
  /** この近さまでの色を、完全に透明にする */
  double similarity = 0.2;
  /** 透明から不透明へ、なめらかに変える幅（0 なら境目がくっきり） */
  double blend = 0.1;
  /** 残った部分に映り込んだ緑・青の色かぶりを抑える */
  bool despill = true;
  bool operator==(const ChromaKey&) const = default;
};

// ---- エフェクト（モザイク・ぼかしなど） ----------------------------------------------------------------
// クリップ（メイン・重ねるトラック）に掛けるものと、エフェクトレイヤー（重ねるトラックの段に置き、下にあるものに掛ける）で使うもの。
// 値の意味と範囲は種類ごとに決まる（core/effects の effectInfo）。大きさの値は、掛ける画像の高さに対する割合（解像度が違っても同じ見た目）。

enum class EffectType {
  Mosaic,     // モザイク（amount: ブロックの大きさ）
  Blur,       // ぼかし（amount: ぼかしの大きさ）
  Sharpen,    // シャープ（amount: 強さ、size: 半径）
  Vignette,   // 周辺減光（amount: 暗さ、size: 明るく残す範囲、softness: 境目のなめらかさ）
  Invert,     // 色の反転（ネガ）
  Posterize,  // ポスタリゼーション（amount: 色の段階の数）
  Noise,      // ノイズ・フィルムの粒子（amount: 強さ）
  RgbShift,   // 色ずれ（amount: ずらす幅）
  Fill,       // 塗りつぶし（color。エフェクトレイヤーで、範囲を隠すとき）
};
constexpr int kEffectTypeCount = 9;

struct Effect {
  /** クリップの中で、エフェクトを見分ける目印（設定欄で並べ替えるときなど） */
  QString id;
  EffectType type = EffectType::Mosaic;
  bool enabled = true;
  double amount = 0;
  double size = 0;
  double softness = 0;
  QColor color{0, 0, 0};
  bool operator==(const Effect&) const = default;
};

// ---- トランジション（メインのビデオの切れ目） -------------------------------------------------------
// 値の意味は core/transitions の transitionInfos()。並びは保存に使わない（保存は名前で書く）

enum class TransitionType {
  None,
  Dissolve,    // クロスディゾルブ（xfade fade）
  DipBlack,    // 暗転（fadeblack）
  DipWhite,    // ホワイトアウト（fadewhite）
  Grain,       // ノイズディゾルブ（dissolve）
  WipeLeft,    // ワイプ ←（wipeleft）
  WipeRight,   // ワイプ →（wiperight）
  WipeUp,      // ワイプ ↑（wipeup）
  WipeDown,    // ワイプ ↓（wipedown）
  SoftLeft,    // ソフトワイプ ←（smoothleft）
  SoftRight,   // ソフトワイプ →（smoothright）
  Clock,       // 時計回りワイプ（radial）
  DoorsOpen,   // 扉（左右に開く。vertopen）
  BlindsOpen,  // 扉（上下に開く。horzopen）
  IrisOpen,    // アイリス（開く。circleopen）
  IrisClose,   // アイリス（閉じる。circleclose）
  PushLeft,    // プッシュ ←（slideleft）
  PushRight,   // プッシュ →（slideright）
  PushUp,      // プッシュ ↑（slideup）
  PushDown,    // プッシュ ↓（slidedown）
  SlideLeft,   // スライド ←（coverleft）
  SlideRight,  // スライド →（coverright）
  Zoom,        // ズーム（zoomin）
  Pixelize,    // ピクセル化（pixelize）
};

/** クリップに入るときのトランジション（前のクリップとの切れ目）。type が None なら無し */
struct Transition {
  TransitionType type = TransitionType::None;
  /** 長さ（秒）。切れ目を真ん中にして、前後のクリップの長さの半分までに収めて使う */
  double duration = 1.0;
  /**
   * 音もクロスフェードする（トランジションの間、前のクリップの音を小さくしながら、次のクリップの音を大きくする）。
   * オフなら音は切れ目でそのまま切り替わる。新しく付けるトランジションはオン（この項目が無い古いファイルはオフとして読む）
   */
  bool audioCrossfade = true;
  bool operator==(const Transition&) const = default;
};

// ---- オブジェクトマスク（AI で被写体を囲み、動きを追いかけるマスク） ----------------------------------------
// プレビューで被写体をクリックすると、AI（EdgeTAM）がそのフレームのマスクを作り、素材のフレームを順に追いかける（トラック）。
// マスクの中身（素材のフレームごと）は generated フォルダーのファイル（file）。クリックした点（prompts）も覚えておくので、
// ファイルが無いパソコンでも「トラック」で作り直せる。

/** クリックした点。x・y は素材のフレーム（反転の前）に対する割合（0〜1）。positive が false なら「ここは含めない」 */
struct MaskPoint {
  double x = 0.5;
  double y = 0.5;
  bool positive = true;
  bool operator==(const MaskPoint&) const = default;
};

/** 1 つのフレームでクリックした点（t はそのフレームの素材の時刻。写真は 0） */
struct MaskPrompt {
  double t = 0;
  std::vector<MaskPoint> points;
  bool operator==(const MaskPrompt&) const = default;
};

/** マスクの使い方 */
enum class MaskMode {
  Cutout,   // 被写体だけを残す（ほかは透明。メインのトラックでは黒）
  Effects,  // エフェクトを、被写体の中だけに掛ける（反転すると外だけ）
};

/** 広げる・狭める、境目のぼかしの上限（素材のピクセル） */
constexpr double kMaxMaskExpand = 60;
constexpr double kMaxMaskFeather = 60;

struct ObjectMask {
  /**
   * マスクの目印（空ならマスクなし）。分割・複製した映像は同じ目印のまま。トラックの結果は、同じ目印の映像すべてに入る
   * （クリップや素材の ID ではないので、開き直して ID が変わっても使える）
   */
  QString id;
  /** 素材のフレームごとのマスク（generated の objmask-*.kvmask のファイル名）。空なら、まだ作っていない */
  QString file;
  /** クリックした点（1 つ目が最初にクリックしたフレーム） */
  std::vector<MaskPrompt> prompts;
  bool enabled = true;
  MaskMode mode = MaskMode::Cutout;
  /** 被写体の外側をマスクにする（背景を消す: 被写体を消して背景を残す / エフェクト: 外側だけに掛ける） */
  bool invert = false;
  /** 広げる（+）・狭める（-）幅と、境目のぼかし（素材のピクセル） */
  double expand = 0;
  double feather = 0;
  bool has() const { return !id.isEmpty() && !prompts.empty(); }
  bool operator==(const ObjectMask&) const = default;
};

/** ビデオトラックのクリップ。前から順に隙間なく並ぶ */
struct VideoClip {
  QString id;
  QString assetId;
  bool isImage = false;
  double in = 0;
  /** 素材の終了位置(秒)。画像は in からの表示時間 */
  double out = 0;
  double speed = 1;
  double volume = 1;
  /** 音量キーフレーム（クリップの先頭からの、タイムライン上の秒数）。空なら volume のまま */
  std::vector<VolumePoint> volumePoints;
  bool muted = false;
  /** パン（-1 が左だけ、0 が真ん中、+1 が右だけ） */
  double pan = 0;
  double fadeIn = 0;
  double fadeOut = 0;
  FitMode fit = FitMode::Contain;
  ColorAdjust color;
  // 映像の位置（画面の中心が 0.5, 0.5）・拡大率・回転(度。時計回りが正)・不透明度
  double x = 0.5;
  double y = 0.5;
  double scale = 1;
  double rotation = 0;
  double opacity = 1;
  Animation anim;
  /**
   * 切り抜き（素材の左・上・右・下の端から切り落とす割合。0〜kMaxCrop）。残った部分は、切り抜く前と同じ場所・大きさのまま出す
   * （切り落としたところは、メインのトラックでは黒、重ねるトラックでは透明）
   */
  double cropL = 0;
  double cropT = 0;
  double cropR = 0;
  double cropB = 0;
  /** 伸縮（幅・高さだけの倍率。拡大率に掛ける。1 が素材の比率のまま） */
  double stretchX = 1;
  double stretchY = 1;
  ChromaKey chroma;
  /** 左右・上下の反転（素材の向きそのものを変える。切り抜きの左右・上下は、反転したあとの見た目の端） */
  bool flipX = false;
  bool flipY = false;
  /** エフェクト（並べた順に掛ける）。素材のフレームに、クロマキー・色の調整の前に掛ける */
  std::vector<Effect> effects;
  /** 前のクリップからこのクリップへ切り替えるトランジション（メインのビデオだけ。先頭のクリップ・重ねるトラックでは使わない） */
  Transition transIn;
  /**
   * 逆再生: 空でなければ、このクリップは逆再生していて、assetId は逆向きに作り直した動画、reversedFrom が元の動画。
   * in・out は逆向きの動画の時間（元の動画の [長さ - out, 長さ - in] にあたる）。逆再生をやめると元の動画に戻す
   */
  QString reversedFrom;
  /** オブジェクトマスク（AI で被写体を囲んで追いかけるマスク）。id が空なら無し */
  ObjectMask mask;
  bool operator==(const VideoClip&) const = default;
};

/** 重ねるトラックに置くものの種類 */
enum class LayerKind {
  Media,   // 動画・写真
  Effect,  // エフェクトレイヤー（素材を使わず、下にあるものに効果を掛ける）
};

/** エフェクトレイヤーの範囲の形。Full は画面全体（調整レイヤー） */
enum class RegionShape { Full, Rect, Ellipse };

/** エフェクトレイヤーが効果を掛ける範囲 */
struct EffectRegion {
  RegionShape shape = RegionShape::Ellipse;
  /** 境目のぼかし（範囲の短い辺に対する割合。0 ならくっきり） */
  double feather = 0.1;
  /** 範囲の外側に掛ける（範囲の中は、そのまま見せる） */
  bool invert = false;
  bool operator==(const EffectRegion&) const = default;
};

/** エフェクトレイヤーの範囲の境目のぼかしの上限 */
constexpr double kMaxRegionFeather = 0.5;

/**
 * 重ねるトラックの映像・画像。メインの映像の上に、好きな時間（start）から重ねる。中身はビデオクリップと同じ
 * （素材・切り出し・速度・音量・フェード・位置・拡大・回転・不透明度・アニメーション・色・切り抜き・伸縮・クロマキー）。
 * 表示方法はいつも「全体表示」で、余白は透明。段（track）は 0 がメインのすぐ上で、数字が大きいほど前に重なる。文字はいつも一番前。
 *
 * kind が Effect のもの（エフェクトレイヤー）は素材を使わない（assetId は空、isImage で長さは in〜out の自由な長さ）。
 * 下にあるもの（メインの映像と、下の段のもの）の、範囲（region）の中だけに effects と color を掛ける。範囲は画面いっぱいの四角を、
 * 位置・拡大率・伸縮・回転（とキーフレーム）で動かしたもの。不透明度とフェードは、効き目の強さになる
 */
struct OverlayItem : VideoClip {
  double start = 0;
  int track = 0;
  /** グループ（同じ値のもの同士が、一緒に動く・消える・複製される）。空ならグループなし */
  QString group;
  LayerKind kind = LayerKind::Media;
  EffectRegion region;
  bool isEffectLayer() const { return kind == LayerKind::Effect; }
  bool operator==(const OverlayItem&) const = default;
};

enum class TextAnimation { None, Fade, SlideUp };
enum class FontId { Gothic, Maru, Mincho, Ud, Kyokasho, Impact, Meiryo };
enum class TextAlign { Left, Center, Right };

/**
 * 文字の一部だけ別のフォントにする範囲。start・length は item.text の位置（UTF-16）。
 * family が空でなければそのファミリー（読み込んだフォント・同梱のフォント）、空なら font（組み込みの種類）を使う
 */
struct FontSpan {
  int start = 0;
  int length = 0;
  FontId font = FontId::Gothic;
  QString family;
  bool operator==(const FontSpan&) const = default;
};

struct TextItem {
  QString id;
  QString text;
  double start = 0;
  double duration = 3;
  /** 中心位置（画面に対する割合） */
  double x = 0.5;
  double y = 0.82;
  FontId font = FontId::Gothic;
  /**
   * 読み込んだフォント・同梱のフォントのファミリー名。空でなければ font より優先する
   * （そのフォントが無いパソコンでは font のフォントで描く）
   */
  QString fontFamily;
  /** 文字の一部だけ別のフォントにする範囲（start の順・重なりなし）。文字を書き換えると、位置を合わせ直す */
  std::vector<FontSpan> fontSpans;
  /** 文字の大きさ（画面の高さに対する割合） */
  double size = 0.075;
  bool bold = true;
  QColor color{255, 255, 255};
  QColor strokeColor{0, 0, 0};
  /** 縁取りの太さ（文字サイズに対する割合） */
  double strokeWidth = 0.12;
  QColor boxColor{0, 0, 0};
  double boxOpacity = 0;
  bool shadow = true;
  TextAlign align = TextAlign::Center;
  /** 字間（基準解像度の px。プラスで広がり、マイナスで狭まる。0 はフォント本来の間隔）。キーフレームでアニメーションできる */
  double letterSpacing = 0;
  TextAnimation animation = TextAnimation::Fade;
  /**
   * タイムラインの段の並び順（0 が一番上。上の段ほどプレビュー・書き出しで手前に重なる）。段そのものの ID は
   * ProjectData::textTrackIds[track]。追加するときに一度だけ決め、開始時間や長さを変えても、
   * ほかのテキストが増えたり減ったりしても変わらない。変えるのは、段を移す操作（Alt+↑↓・右クリック）と、
   * タイムラインでテキストを上下にドラッグしたとき（段の境目に落とすと、そこに新しい段を差し込む）だけ。
   * 同じ段の中では、時間が重ならない（重なりそうなときは、時間をずらすか、空いている段へ入れる）
   */
  int track = 0;
  /** 拡大率・回転(度。時計回りが正)・不透明度。位置は x, y。文字の中心を基準に動かす */
  double scale = 1;
  double rotation = 0;
  double opacity = 1;
  Animation anim;
  /** グループ（同じ値のもの同士が、一緒に動く・消える・複製される）。空ならグループなし */
  QString group;
  bool operator==(const TextItem&) const = default;
};

struct AudioItem {
  QString id;
  QString assetId;
  double start = 0;
  double in = 0;
  double out = 0;
  /**
   * タイムラインのオーディオの段（0 が一番上）。テキストと同じく、追加するときに一度だけ決め、時間を変えても段は変わらない。
   * 同じ段の中では時間が重ならない（重なりそうなときは、空いている段へ入れる）。段を変えるのは、見出しの列のドラッグ（段の入れ替え）と、
   * タイムラインでオーディオを上下にドラッグしたとき（段の境目に落とすと、そこに新しい段を差し込む）だけ
   */
  int track = 0;
  double volume = kMaxVolume;
  /** 音量キーフレーム（t の順）。空ならクリップ全体を volume で鳴らす。あるときは、キーフレームの音量が優先される */
  std::vector<VolumePoint> volumePoints;
  /** パン（-1 が左だけ、0 が真ん中、+1 が右だけ） */
  double pan = 0;
  double fadeIn = 0;
  double fadeOut = 0;
  /** グループ（同じ値のもの同士が、一緒に動く・消える・複製される）。空ならグループなし */
  QString group;
  bool operator==(const AudioItem&) const = default;
};

enum class Aspect { R16_9, R9_16, R1_1, R4_5 };

/** 動画全体の再生速度の範囲（1 が元の速さ） */
constexpr double kMinProjectSpeed = 0.25;
constexpr double kMaxProjectSpeed = 4.0;

struct ProjectSettings {
  Aspect aspect = Aspect::R16_9;
  int fps = 30;
  /**
   * 動画全体の再生速度（1 が元の速さ）。タイムラインの時間はそのままで、プレビューの再生と書き出しを、
   * 映像・文字・音のすべてまとめてこの速さにする（音は音程を変えずに速さだけ変える）
   */
  double speed = 1;
  bool operator==(const ProjectSettings&) const = default;
};

/** タイムラインのマーカー（目盛りの上の印。M キーで再生位置に置く。名前・色・メモを付けられる） */
struct Marker {
  QString id;
  /** タイムライン上の時刻（秒。フレームにそろえる） */
  double t = 0;
  QString name;
  QString comment;
  QColor color{0x3f, 0xb6, 0x7b};
  bool operator==(const Marker&) const = default;
};

struct ProjectData {
  QString name;
  ProjectSettings settings;
  /** マーカー（時間の順） */
  std::vector<Marker> markers;
  std::vector<VideoClip> clips;
  /** 重ねるトラックの映像・画像（並びは追加した順。重なりの順は track で決める） */
  std::vector<OverlayItem> overlays;
  std::vector<TextItem> texts;
  /**
   * テキストの段の ID（上の段から順。添字が TextItem::track）。並び順と ID を分けて持つので、段を差し込んだり
   * 並べ替えたりしても、それぞれの段の ID は変わらない（保存するときは、テキストごとに段の ID も書く）。
   * 段の数（textTrackCount）にいつもそろえる（syncTextTrackIds）
   */
  std::vector<QString> textTrackIds;
  /**
   * 段の番号（タイムラインの見出しの「テキスト 1」「オーディオ 2」の数字。上の段から順。添字が track）。
   * 段を入れ替えると、番号も中身と一緒に動く（「オーディオ 1」を下へ動かすと、見出しも「オーディオ 1」が下になる）。
   * 段の数にいつもそろえる（syncTextTrackIds・syncAudioTrackNos）。段が 1 つだけになったら 1 に戻す
   */
  std::vector<int> textTrackNos;
  std::vector<AudioItem> audios;
  std::vector<int> audioTrackNos;

  // ---- トラックの非表示・ミュート（タイムラインの見出しの目・スピーカーの印。プレビューと書き出しの両方で効く） ----
  /** メインのビデオの映像を出さない（その間は黒。音は mainAudioMuted で別に決める） */
  bool mainVideoHidden = false;
  /** メインのビデオの音を出さない */
  bool mainAudioMuted = false;
  /** 出さないテキストの段（textTrackIds の ID。段を並べ替えても、その段についていく） */
  std::vector<QString> hiddenTextTracks;
  /** 映像を出さない重ねるトラックの段（OverlayItem::track の番号） */
  std::vector<int> hiddenOverlayTracks;
  /** 音を出さない重ねるトラックの段（OverlayItem::track の番号） */
  std::vector<int> mutedOverlayTracks;
  /** 音を出さないオーディオの段（audioTrackNos の番号。段を並べ替えても、その段についていく） */
  std::vector<int> mutedAudioTracks;

  bool operator==(const ProjectData&) const = default;
};

/** トラックの非表示・ミュートで、出さないもの（プレビュー・書き出し・音の組み立てで共通に使う） */
bool isTextHidden(const ProjectData& p, const TextItem& t);
bool isOverlayHidden(const ProjectData& p, const OverlayItem& o);
bool isOverlayMuted(const ProjectData& p, const OverlayItem& o);
bool isAudioMuted(const ProjectData& p, const AudioItem& a);

/** 1 つの編集場所（タブ）の保存・読み込み用データ */
struct WorkspaceData {
  ProjectData project;
  std::vector<Asset> assets;
  /** 利用者が付けたタブ名。空なら「編集N」 */
  QString name;
  /** 自動で付ける「編集N」の N（0 なら未定） */
  int number = 0;
  /** 保存先のファイル（まだ保存していなければ空） */
  QString path;
  /** 保存していない変更があるか（自動保存からの復元で使う） */
  bool dirty = false;
};

/** 読み込んだプロジェクトファイル。編集場所は 1 つ以上で、開いていた編集場所の番号を持つ */
struct ProjectFile {
  std::vector<WorkspaceData> workspaces;
  int active = 0;
};

/** 波形（0〜1 のピーク値） */
struct Waveform {
  int peaksPerSecond = 0;
  std::vector<float> peaks;
};

QString newId();

QString assetKindToString(AssetKind k);
AssetKind assetKindFromString(const QString& s);
QString fitToString(FitMode f);
FitMode fitFromString(const QString& s);
QString presetToString(FilterPreset p);
FilterPreset presetFromString(const QString& s);
QString animationToString(TextAnimation a);
TextAnimation animationFromString(const QString& s);
QString fontToString(FontId f);
FontId fontFromString(const QString& s);
QString alignToString(TextAlign a);
TextAlign alignFromString(const QString& s);
QString aspectToString(Aspect a);
Aspect aspectFromString(const QString& s);
QString easingToString(Easing e);
Easing easingFromString(const QString& s);
QString propToString(AnimProp p);
/** 知らない名前なら false */
bool propFromString(const QString& s, AnimProp* out);

}  // namespace kv
