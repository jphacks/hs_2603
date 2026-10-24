#pragma once
// 色フィルター。プレビュー（CPU で計算）と書き出し（ffmpeg）で同じ式を使う。
//   1. 明るさ     x' = clamp(x * brightness)
//   2. コントラスト x'' = clamp((x' - 0.5) * contrast + 0.5)
//   3. 色行列     y = clamp(M · x'')   M = 色温度 · トーン(セピア等) · 彩度

#include <QImage>
#include <QString>
#include <array>
#include <optional>
#include <vector>

#include "core/model.h"

namespace kv {

using Mat3 = std::array<double, 9>;

struct FilterPresetInfo {
  FilterPreset id;
  QString label;
  double sepia;
  double brightness, contrast, saturation, temperature;
};

const std::vector<FilterPresetInfo>& filterPresets();
const FilterPresetInfo& presetInfo(FilterPreset id);
ColorAdjust colorFromPreset(FilterPreset id);

struct Range {
  double min, max;
};
namespace colorLimits {
constexpr Range brightness{0.5, 1.5};
constexpr Range contrast{0.5, 1.5};
constexpr Range saturation{0, 1.8};
constexpr Range temperature{-1, 1};
}  // namespace colorLimits

struct ColorPipeline {
  double brightness = 1;
  double contrast = 1;
  Mat3 matrix{};
};

ColorPipeline colorPipeline(const ColorAdjust& adj);
bool isNeutralColor(const ColorAdjust& adj);
/** ffmpeg のフィルター文字列（色を変えない場合は空） */
QString ffmpegColorFilter(const ColorAdjust& adj);

/** 画像（不透明な RGB32 / ARGB32）の色をその場で変える */
void applyColor(QImage& img, const ColorAdjust& adj);

}  // namespace kv
