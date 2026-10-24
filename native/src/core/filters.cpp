#include "core/filters.h"

#include <algorithm>
#include <cmath>

namespace kv {

const std::vector<FilterPresetInfo>& filterPresets() {
  static const std::vector<FilterPresetInfo> presets = {
      {FilterPreset::None, QStringLiteral("ノーマル"), 0, 1, 1, 1, 0},
      {FilterPreset::Vivid, QStringLiteral("ビビッド"), 0, 1.02, 1.12, 1.45, 0},
      {FilterPreset::Warm, QStringLiteral("ウォーム"), 0, 1.03, 1.02, 1.1, 0.6},
      {FilterPreset::Cool, QStringLiteral("クール"), 0, 1, 1.05, 0.95, -0.6},
      {FilterPreset::Soft, QStringLiteral("ソフト"), 0, 1.1, 0.85, 0.9, 0.15},
      {FilterPreset::Drama, QStringLiteral("ドラマ"), 0, 0.95, 1.3, 0.8, -0.1},
      {FilterPreset::Retro, QStringLiteral("レトロ"), 0.35, 1.05, 0.9, 0.85, 0.3},
      {FilterPreset::Sepia, QStringLiteral("セピア"), 0.9, 1.02, 1, 1, 0},
      {FilterPreset::Mono, QStringLiteral("モノクロ"), 0, 1, 1.1, 0, 0},
  };
  return presets;
}

const FilterPresetInfo& presetInfo(FilterPreset id) {
  for (const auto& p : filterPresets()) {
    if (p.id == id) return p;
  }
  return filterPresets().front();
}

ColorAdjust colorFromPreset(FilterPreset id) {
  const auto& p = presetInfo(id);
  return {p.id, p.brightness, p.contrast, p.saturation, p.temperature};
}

static const Mat3 kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1};

static Mat3 mul(const Mat3& a, const Mat3& b) {
  Mat3 r{};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += a[i * 3 + k] * b[k * 3 + j];
      r[i * 3 + j] = s;
    }
  }
  return r;
}

// SVG feColorMatrix type="saturate" と同じ行列
static Mat3 saturateMatrix(double s) {
  return {0.213 + 0.787 * s, 0.715 - 0.715 * s, 0.072 - 0.072 * s,
          0.213 - 0.213 * s, 0.715 + 0.285 * s, 0.072 - 0.072 * s,
          0.213 - 0.213 * s, 0.715 - 0.715 * s, 0.072 + 0.928 * s};
}

// CSS sepia() と同じ行列
static Mat3 sepiaMatrix(double amount) {
  const double a = 1 - std::clamp(amount, 0.0, 1.0);
  return {0.393 + 0.607 * a, 0.769 - 0.769 * a, 0.189 - 0.189 * a,
          0.349 - 0.349 * a, 0.686 + 0.314 * a, 0.168 - 0.168 * a,
          0.272 - 0.272 * a, 0.534 - 0.534 * a, 0.131 + 0.869 * a};
}

static Mat3 temperatureMatrix(double t) {
  return {1 + 0.14 * t, 0, 0, 0, 1 + 0.02 * t, 0, 0, 0, 1 - 0.14 * t};
}

ColorPipeline colorPipeline(const ColorAdjust& adj) {
  const auto& preset = presetInfo(adj.preset);
  const Mat3 tone = preset.sepia > 0 ? sepiaMatrix(preset.sepia) : kIdentity;
  Mat3 m = mul(temperatureMatrix(adj.temperature), mul(tone, saturateMatrix(adj.saturation)));
  for (double& v : m) v = std::clamp(v, -2.0, 2.0);
  return {adj.brightness, adj.contrast, m};
}

static bool near(double a, double b) {
  return std::abs(a - b) < 1e-4;
}

static bool isIdentityMatrix(const Mat3& m) {
  for (int i = 0; i < 9; ++i) {
    if (!near(m[i], kIdentity[i])) return false;
  }
  return true;
}

bool isNeutralColor(const ColorAdjust& adj) {
  const ColorPipeline p = colorPipeline(adj);
  return near(p.brightness, 1) && near(p.contrast, 1) && isIdentityMatrix(p.matrix);
}

// JS の v.toFixed(5).replace(/\.?0+$/, '') と同じ文字列にする
static QString num5(double v) {
  QString s = QString::number(v, 'f', 5);
  if (s.contains(u'.')) {
    while (s.endsWith(u'0')) s.chop(1);
    if (s.endsWith(u'.')) s.chop(1);
  }
  if (s == u"-0" || s.isEmpty()) return QStringLiteral("0");
  return s;
}

QString ffmpegColorFilter(const ColorAdjust& adj) {
  if (isNeutralColor(adj)) return {};
  const ColorPipeline p = colorPipeline(adj);
  QStringList parts;
  if (!near(p.brightness, 1) || !near(p.contrast, 1)) {
    const QString e = QStringLiteral("clip((clip(val*%1,0,255)-127.5)*%2+127.5,0,255)").arg(num5(p.brightness), num5(p.contrast));
    parts << QStringLiteral("lutrgb=r='%1':g='%1':b='%1'").arg(e);
  }
  if (!isIdentityMatrix(p.matrix)) {
    const Mat3& m = p.matrix;
    parts << QStringLiteral("colorchannelmixer=rr=%1:rg=%2:rb=%3:gr=%4:gg=%5:gb=%6:br=%7:bg=%8:bb=%9")
                 .arg(num5(m[0]), num5(m[1]), num5(m[2]), num5(m[3]), num5(m[4]), num5(m[5]), num5(m[6]), num5(m[7]), num5(m[8]));
  }
  return parts.join(u',');
}

void applyColor(QImage& img, const ColorAdjust& adj) {
  if (img.isNull() || isNeutralColor(adj)) return;
  if (img.format() != QImage::Format_RGB32 && img.format() != QImage::Format_ARGB32 &&
      img.format() != QImage::Format_ARGB32_Premultiplied) {
    img = img.convertToFormat(QImage::Format_RGB32);
  }
  const ColorPipeline p = colorPipeline(adj);
  // ffmpeg の lutrgb と同じく 8bit の値ごとに表を作る（結果は切り捨て）
  int lut[256];
  for (int v = 0; v < 256; ++v) {
    const double b = std::clamp(v * p.brightness, 0.0, 255.0);
    const double c = std::clamp((b - 127.5) * p.contrast + 127.5, 0.0, 255.0);
    lut[v] = int(c);
  }
  const bool identity = isIdentityMatrix(p.matrix);
  // colorchannelmixer と同じく項ごとに丸めた表を足し合わせる
  int mix[9][256];
  for (int k = 0; k < 9; ++k) {
    for (int v = 0; v < 256; ++v) mix[k][v] = int(std::lrint(v * p.matrix[k]));
  }
  const int w = img.width();
  const int h = img.height();
  for (int y = 0; y < h; ++y) {
    QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
    for (int x = 0; x < w; ++x) {
      const QRgb px = line[x];
      const int r = lut[qRed(px)];
      const int g = lut[qGreen(px)];
      const int b = lut[qBlue(px)];
      if (identity) {
        line[x] = qRgba(r, g, b, qAlpha(px));
        continue;
      }
      const int nr = std::clamp(mix[0][r] + mix[1][g] + mix[2][b], 0, 255);
      const int ng = std::clamp(mix[3][r] + mix[4][g] + mix[5][b], 0, 255);
      const int nb = std::clamp(mix[6][r] + mix[7][g] + mix[8][b], 0, 255);
      line[x] = qRgba(nr, ng, nb, qAlpha(px));
    }
  }
}

}  // namespace kv
