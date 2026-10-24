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
};

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
};

enum class TextAnimation { None, Fade, SlideUp };
enum class FontId { Gothic, Maru, Mincho, Ud, Kyokasho, Impact, Meiryo };
enum class TextAlign { Left, Center, Right };

struct TextItem {
  QString id;
  QString text;
  double start = 0;
  double duration = 3;
  /** 中心位置（画面に対する割合） */
  double x = 0.5;
  double y = 0.82;
  FontId font = FontId::Gothic;
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
   * タイムラインの段（0 が一番上）。追加するときに一度だけ決め、開始時間や長さを変えても、
   * ほかのテキストが増えたり減ったりしても変わらない。変えるのは、段を移す明示的な操作のときだけ
   */
  int track = 0;
  /** 拡大率・回転(度。時計回りが正)・不透明度。位置は x, y。文字の中心を基準に動かす */
  double scale = 1;
  double rotation = 0;
  double opacity = 1;
  Animation anim;
};

struct AudioItem {
  QString id;
  QString assetId;
  double start = 0;
  double in = 0;
  double out = 0;
  double volume = kMaxVolume;
  /** 音量キーフレーム（t の順）。空ならクリップ全体を volume で鳴らす。あるときは、キーフレームの音量が優先される */
  std::vector<VolumePoint> volumePoints;
  double fadeIn = 0;
  double fadeOut = 0;
};

enum class Aspect { R16_9, R9_16, R1_1, R4_5 };

struct ProjectSettings {
  Aspect aspect = Aspect::R16_9;
  int fps = 30;
};

struct ProjectData {
  QString name;
  ProjectSettings settings;
  std::vector<VideoClip> clips;
  std::vector<TextItem> texts;
  std::vector<AudioItem> audios;
};

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
