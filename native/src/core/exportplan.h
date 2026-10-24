#pragma once
// 書き出し用の ffmpeg コマンド（入力・フィルターグラフ・出力オプション）を組み立てる。

#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

#include "core/model.h"

namespace kv {

enum class ExportQuality { Fast, Standard, High };

/**
 * 映像・文字の、位置・拡大率・回転・不透明度。transformed が false なら従来どおりの描き方
 * （位置は x, y。拡大・回転・不透明度は無し）で、ほかの値は使わない
 */
struct LayerSpec {
  bool transformed = false;
  double x = 0.5;
  double y = 0.5;
  double scale = 1;
  double rotation = 0;
  double opacity = 1;
  Animation anim;
};

struct ExportClip {
  bool isImage = false;
  QString path;
  bool hasAudio = false;
  double in = 0;
  double duration = 0;
  double speed = 1;
  double volume = 1;
  /** 音量キーフレーム（クリップの先頭からのタイムライン上の秒数）。あるときは volume ではなくこちらで音量を変える */
  std::vector<VolumePoint> points;
  LayerSpec layer;
  double fadeIn = 0;
  double fadeOut = 0;
  FitMode fit = FitMode::Contain;
  std::optional<ColorAdjust> color;
  /** 色の情報が付いていない動画に付ける setparams の値（例 "colorspace=bt709:range=tv"）。付いていれば空 */
  QString colorParams;
};

struct PlannedText {
  QString path;
  double start = 0;
  double duration = 0;
  TextAnimation animation = TextAnimation::None;
  /** transformed のときは、path は文字を画面の中央に置いた画像で、位置は layer.x, layer.y（とキーフレーム）で決める */
  LayerSpec layer;
};

struct ExportAudio {
  QString path;
  double start = 0;
  double in = 0;
  double duration = 0;
  double volume = 1;
  /** 音量キーフレーム（クリップの先頭からの秒数）。あるときは volume ではなくこちらで音量を変える */
  std::vector<VolumePoint> points;
  double fadeIn = 0;
  double fadeOut = 0;
};

struct ExportRequest {
  QString outputPath;
  int width = 1920;
  int height = 1080;
  int fps = 30;
  ExportQuality quality = ExportQuality::Standard;
  double duration = 0;
  std::vector<ExportClip> clips;
  std::vector<ExportAudio> audios;
};

struct ExportPlan {
  QStringList inputArgs;
  QString filterGraph;
  QStringList outputArgs;
  int totalFrames = 0;
  double duration = 0;
};

/** 指数表記を使わない数値の文字列化（JS の toFixed(6) と同じ） */
QString num6(double v);
/**
 * 音量キーフレームを ffmpeg の式（変数 t = クリップの先頭からの秒数）にする。
 * volumeAt() と同じ変わり方（端は保ち、間は直線）なので、プレビューと書き出しの音量が一致する
 */
QString volumeExpression(const std::vector<VolumePoint>& points);
/**
 * 1 つの項目のキーフレームを ffmpeg の式にする。T は「オブジェクトの先頭からの秒数」を表す式（例 "t"、"(t-3.5)"）。
 * animValueAt() と同じ変わり方で、範囲は propLimits に収める
 */
QString keyframeExpression(const std::vector<Keyframe>& keys, double base, const QString& T, AnimProp prop);
ExportPlan buildExportPlan(const ExportRequest& req, const std::vector<PlannedText>& texts);

}  // namespace kv
