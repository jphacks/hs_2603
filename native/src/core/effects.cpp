#include "core/effects.h"

#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <list>

#include "core/exportplan.h"
#include "core/filters.h"
#include "core/timeline.h"

namespace kv {

// ---- 種類ごとの決まり ------------------------------------------------------------------------

const std::vector<EffectInfo>& effectInfos() {
  using U = EffectParam::Unit;
  static const std::vector<EffectInfo> list = {
      {EffectType::Mosaic, QStringLiteral("mosaic"), QStringLiteral("モザイク"), QStringLiteral("ブロック状にして、細かい所を見えなくします"), true, true,
       {QStringLiteral("ブロックの大きさ"), 0.005, 0.2, 0.04, 0.001, U::Percent}, {}, {}},
      {EffectType::Blur, QStringLiteral("blur"), QStringLiteral("ぼかし"), QStringLiteral("全体をぼかします"), true, true,
       {QStringLiteral("ぼかしの強さ"), 0, 0.1, 0.02, 0.001, U::Percent}, {}, {}},
      {EffectType::Sharpen, QStringLiteral("sharpen"), QStringLiteral("シャープ"), QStringLiteral("輪郭をくっきりさせます"), true, true,
       {QStringLiteral("強さ"), 0, 3, 1, 0.01, U::Percent}, {QStringLiteral("半径"), 1, 6, 2, 1, U::Integer}, {}},
      {EffectType::Vignette, QStringLiteral("vignette"), QStringLiteral("周辺減光"), QStringLiteral("画面の周りを暗くして、中心に目を向けさせます"), true, true,
       {QStringLiteral("暗さ"), 0, 1, 0.5, 0.01, U::Percent}, {QStringLiteral("明るく残す範囲"), 0, 1, 0.5, 0.01, U::Percent},
       {QStringLiteral("境目のなめらかさ"), 0, 1, 0.5, 0.01, U::Percent}},
      {EffectType::Invert, QStringLiteral("invert"), QStringLiteral("色の反転"), QStringLiteral("色を反対にします（ネガ）"), true, true, {}, {}, {}},
      {EffectType::Posterize, QStringLiteral("posterize"), QStringLiteral("ポスタリゼーション"), QStringLiteral("色の段階を減らして、イラストのようにします"), true,
       true, {QStringLiteral("色の段階"), 2, 16, 5, 1, U::Integer}, {}, {}},
      {EffectType::Noise, QStringLiteral("noise"), QStringLiteral("ノイズ（粒子）"), QStringLiteral("フィルムのような細かい粒子を足します"), true, true,
       {QStringLiteral("強さ"), 0, 1, 0.3, 0.01, U::Percent}, {}, {}},
      {EffectType::RgbShift, QStringLiteral("rgbShift"), QStringLiteral("色ずれ"), QStringLiteral("赤と青を左右にずらします（グリッチ風）"), true, true,
       {QStringLiteral("ずらす幅"), 0, 0.05, 0.008, 0.0005, U::Percent}, {}, {}},
      {EffectType::Fill, QStringLiteral("fill"), QStringLiteral("塗りつぶし"), QStringLiteral("範囲を 1 色で塗りつぶして隠します"), false, true, {}, {}, {},
       true},
  };
  return list;
}

const EffectInfo& effectInfo(EffectType t) {
  for (const auto& i : effectInfos()) {
    if (i.type == t) return i;
  }
  return effectInfos().front();
}

QString effectTypeToString(EffectType t) {
  return effectInfo(t).id;
}

bool effectTypeFromString(const QString& s, EffectType* out) {
  for (const auto& i : effectInfos()) {
    if (i.id == s) {
      *out = i.type;
      return true;
    }
  }
  return false;
}

Effect makeEffect(EffectType t) {
  const EffectInfo& info = effectInfo(t);
  Effect e;
  e.id = newId();
  e.type = t;
  e.amount = info.amount.def;
  e.size = info.size.def;
  e.softness = info.softness.def;
  e.color = QColor(0, 0, 0);
  return e;
}

static double clampParam(const EffectParam& p, double v) {
  if (p.label.isEmpty()) return p.def;
  if (!std::isfinite(v)) return p.def;
  v = std::clamp(v, p.min, p.max);
  if (p.unit == EffectParam::Unit::Integer) v = std::round(v);
  return v;
}

void clampEffect(Effect& e) {
  const EffectInfo& info = effectInfo(e.type);
  e.amount = clampParam(info.amount, e.amount);
  e.size = clampParam(info.size, e.size);
  e.softness = clampParam(info.softness, e.softness);
  if (!e.color.isValid()) e.color = QColor(0, 0, 0);
  e.color.setAlpha(255);
  if (e.id.isEmpty()) e.id = newId();
}

bool effectActive(const Effect& e) {
  if (!e.enabled) return false;
  switch (e.type) {
    case EffectType::Blur:
    case EffectType::Sharpen:
    case EffectType::Vignette:
    case EffectType::Noise: return e.amount > 0.0005;
    case EffectType::RgbShift: return e.amount > 0.00005;
    default: return true;
  }
}

bool anyEffectActive(const std::vector<Effect>& effects) {
  return std::any_of(effects.begin(), effects.end(), [](const Effect& e) { return effectActive(e); });
}

QString effectsSignature(const std::vector<Effect>& effects) {
  QString s;
  for (const Effect& e : effects) {
    if (!effectActive(e)) continue;
    s += QStringLiteral("%1:%2:%3:%4:%5;").arg(int(e.type)).arg(e.amount).arg(e.size).arg(e.softness).arg(e.color.name());
  }
  return s;
}

bool effectsVaryPerFrame(const std::vector<Effect>& effects) {
  return std::any_of(effects.begin(), effects.end(), [](const Effect& e) { return effectActive(e) && e.type == EffectType::Noise; });
}

// ---- プレビューの計算（書き出しの ffmpeg と同じ考え方の計算） -----------------------------------------

namespace {

/** 画像を色ごとの面（R, G, B, A）に分けたもの */
struct Planes {
  int w = 0;
  int h = 0;
  std::vector<uint8_t> c[4];
};

Planes toPlanes(const QImage& img) {
  Planes p;
  p.w = img.width();
  p.h = img.height();
  for (auto& c : p.c) c.resize(size_t(p.w) * p.h);
  for (int y = 0; y < p.h; ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    const size_t o = size_t(y) * p.w;
    for (int x = 0; x < p.w; ++x) {
      const QRgb v = line[x];
      p.c[0][o + x] = uint8_t(qRed(v));
      p.c[1][o + x] = uint8_t(qGreen(v));
      p.c[2][o + x] = uint8_t(qBlue(v));
      p.c[3][o + x] = uint8_t(qAlpha(v));
    }
  }
  return p;
}

void fromPlanes(const Planes& p, QImage& img) {
  if (img.width() != p.w || img.height() != p.h || img.format() != QImage::Format_ARGB32) img = QImage(p.w, p.h, QImage::Format_ARGB32);
  for (int y = 0; y < p.h; ++y) {
    QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
    const size_t o = size_t(y) * p.w;
    for (int x = 0; x < p.w; ++x) line[x] = qRgba(p.c[0][o + x], p.c[1][o + x], p.c[2][o + x], p.c[3][o + x]);
  }
}

struct Tap {
  int index;
  float weight;
};

/** 面積で平均する縮小の重み（ffmpeg の scale flags=area と同じ考え方: 出力の 1 画素が覆う範囲の、入力の画素の重なり） */
std::vector<std::vector<Tap>> areaTaps(int src, int dst) {
  std::vector<std::vector<Tap>> taps(static_cast<size_t>(dst));
  const double scale = double(src) / dst;
  for (int i = 0; i < dst; ++i) {
    const double a = i * scale;
    const double b = (i + 1) * scale;
    const int j0 = std::max(0, int(std::floor(a)));
    const int j1 = std::min(src, int(std::ceil(b)));
    for (int j = j0; j < j1; ++j) {
      const double overlap = std::min(b, double(j + 1)) - std::max(a, double(j));
      if (overlap > 1e-9) taps[size_t(i)].push_back({j, float(overlap / scale)});
    }
  }
  return taps;
}

Planes areaScale(const Planes& s, int bw, int bh) {
  Planes out;
  out.w = bw;
  out.h = bh;
  const auto tx = areaTaps(s.w, bw);
  const auto ty = areaTaps(s.h, bh);
  std::vector<float> tmp(size_t(bw) * s.h);
  for (int c = 0; c < 4; ++c) {
    const std::vector<uint8_t>& src = s.c[c];
    for (int y = 0; y < s.h; ++y) {
      const uint8_t* row = src.data() + size_t(y) * s.w;
      float* trow = tmp.data() + size_t(y) * bw;
      for (int i = 0; i < bw; ++i) {
        float sum = 0;
        for (const Tap& t : tx[size_t(i)]) sum += row[t.index] * t.weight;
        trow[i] = sum;
      }
    }
    out.c[c].resize(size_t(bw) * bh);
    for (int k = 0; k < bh; ++k) {
      for (int i = 0; i < bw; ++i) {
        float sum = 0;
        for (const Tap& t : ty[size_t(k)]) sum += tmp[size_t(t.index) * bw + i] * t.weight;
        out.c[c][size_t(k) * bw + i] = uint8_t(std::clamp(int(std::lround(sum)), 0, 255));
      }
    }
  }
  return out;
}

/** 最近傍の拡大（ffmpeg の scale flags=neighbor と同じ: 出力の画素の中心に一番近い入力の画素） */
Planes nearestScale(const Planes& s, int w, int h) {
  Planes out;
  out.w = w;
  out.h = h;
  std::vector<int> xs(static_cast<size_t>(w));
  for (int x = 0; x < w; ++x) xs[size_t(x)] = std::min(s.w - 1, int((x + 0.5) * s.w / w));
  for (int c = 0; c < 4; ++c) {
    out.c[c].resize(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
      const int sy = std::min(s.h - 1, int((y + 0.5) * s.h / h));
      const uint8_t* row = s.c[c].data() + size_t(sy) * s.w;
      uint8_t* dst = out.c[c].data() + size_t(y) * w;
      for (int x = 0; x < w; ++x) dst[x] = row[xs[size_t(x)]];
    }
  }
  return out;
}

/** 中心をそろえた双線形補間の拡大（ffmpeg の scale flags=bilinear と同じ考え方） */
Planes bilinearScale(const Planes& s, int w, int h) {
  Planes out;
  out.w = w;
  out.h = h;
  struct Pos {
    int i0, i1;
    float f;
  };
  auto positions = [](int src, int dst) {
    std::vector<Pos> p(static_cast<size_t>(dst));
    for (int x = 0; x < dst; ++x) {
      const double sx = std::clamp((x + 0.5) * src / dst - 0.5, 0.0, double(src - 1));
      const int i0 = int(std::floor(sx));
      p[size_t(x)] = {i0, std::min(i0 + 1, src - 1), float(sx - i0)};
    }
    return p;
  };
  const auto px = positions(s.w, w);
  const auto py = positions(s.h, h);
  for (int c = 0; c < 4; ++c) {
    out.c[c].resize(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
      const Pos& yp = py[size_t(y)];
      const uint8_t* r0 = s.c[c].data() + size_t(yp.i0) * s.w;
      const uint8_t* r1 = s.c[c].data() + size_t(yp.i1) * s.w;
      uint8_t* dst = out.c[c].data() + size_t(y) * w;
      for (int x = 0; x < w; ++x) {
        const Pos& xp = px[size_t(x)];
        const float top = r0[xp.i0] + (r0[xp.i1] - r0[xp.i0]) * xp.f;
        const float bottom = r1[xp.i0] + (r1[xp.i1] - r1[xp.i0]) * xp.f;
        dst[x] = uint8_t(std::clamp(int(std::lround(top + (bottom - top) * yp.f)), 0, 255));
      }
    }
  }
  return out;
}

/** 1 行の箱型ぼかし。ffmpeg の boxblur（blur8）と同じ式（端は折り返し、16 ビットの固定小数点で丸める）。len >= 2 * radius + 1 */
void blurLine(uint8_t* dst, const uint8_t* src, int len, int radius) {
  const int length = radius * 2 + 1;
  const int inv = ((1 << 16) + length / 2) / length;
  int x, sum = src[radius];
  for (x = 0; x < radius; x++) sum += src[x] << 1;
  sum = sum * inv + (1 << 15);
  for (x = 0; x <= radius; x++) {
    sum += (src[radius + x] - src[radius - x]) * inv;
    dst[x] = uint8_t(sum >> 16);
  }
  for (; x < len - radius; x++) {
    sum += (src[radius + x] - src[x - radius - 1]) * inv;
    dst[x] = uint8_t(sum >> 16);
  }
  for (; x < len; x++) {
    sum += (src[2 * len - radius - x - 1] - src[x - radius - 1]) * inv;
    dst[x] = uint8_t(sum >> 16);
  }
}

/** power 回くり返す（ffmpeg の blur_power と同じ） */
void blurPower(uint8_t* line, int len, int radius, int power, std::vector<uint8_t>& a, std::vector<uint8_t>& b) {
  if (!radius || !power) return;
  blurLine(a.data(), line, len, radius);
  for (; power > 2; power--) {
    blurLine(b.data(), a.data(), len, radius);
    std::swap(a, b);
  }
  if (power > 1) blurLine(line, a.data(), len, radius);
  else std::copy(a.begin(), a.begin() + len, line);
}

/** 面の箱型ぼかし（ffmpeg の boxblur と同じく、横に power 回、そのあと縦に power 回） */
void boxBlurPlane(std::vector<uint8_t>& plane, int w, int h, int radius, int power) {
  if (radius <= 0 || power <= 0) return;
  const size_t n = static_cast<size_t>(std::max(w, h));
  std::vector<uint8_t> a(n), b(n), col(n);
  if (radius <= (w - 1) / 2) {
    for (int y = 0; y < h; ++y) blurPower(plane.data() + size_t(y) * w, w, radius, power, a, b);
  }
  if (radius <= (h - 1) / 2) {
    for (int x = 0; x < w; ++x) {
      for (int y = 0; y < h; ++y) col[size_t(y)] = plane[size_t(y) * w + x];
      blurPower(col.data(), h, radius, power, a, b);
      for (int y = 0; y < h; ++y) plane[size_t(y) * w + x] = col[size_t(y)];
    }
  }
}

uint8_t clampByte(int v) {
  return uint8_t(std::clamp(v, 0, 255));
}

// モザイク: 高さ方向のブロックの数を割合から決め（解像度によらず同じ並び）、面積で平均して縮め、最近傍で元の大きさに戻す
struct MosaicGrid {
  int bx = 1;
  int by = 1;
};
MosaicGrid mosaicGrid(int w, int h, double frac) {
  MosaicGrid g;
  const int n = std::max(1, int(std::lround(1.0 / std::max(1e-3, frac))));
  g.by = std::clamp(n, 1, h);
  g.bx = std::clamp(int(std::lround(double(g.by) * w / h)), 1, w);
  return g;
}

// ぼかし: 元の大きさでの半径 R = 割合 × 高さ。R が 4 画素を超えるときは、半径が約 4 画素になる大きさに縮めてからぼかし、
// 双線形補間で戻す（縮める大きさは割合だけで決まるので、解像度によらず同じぼけ方。軽い）。箱型ぼかしを 3 回（ガウスぼかしに近い）
struct BlurPlan {
  bool scaled = false;
  int wb = 0;
  int hb = 0;
  int radius = 0;
};
BlurPlan blurPlan(int w, int h, double frac) {
  BlurPlan p;
  const double R = frac * h;
  p.wb = w;
  p.hb = h;
  if (R < 0.5) return p;
  if (R <= 4) {
    p.radius = std::max(1, int(std::lround(R)));
  } else {
    p.scaled = true;
    p.wb = std::min(w, std::max(8, int(std::lround(w * 4.0 / R))));
    p.hb = std::min(h, std::max(8, int(std::lround(h * 4.0 / R))));
    p.radius = std::max(1, int(std::lround(R * p.hb / h)));
  }
  p.radius = std::min(p.radius, (std::min(p.wb, p.hb) - 1) / 2);
  return p;
}

/** シャープ（ffmpeg の unsharp の明るさの計算: 二項係数の重みのぼかしとの差を、強さの割合だけ足す）。半径は 1080 の高さでの値 */
int sharpenSteps(int h, double size) {
  return std::clamp(int(std::lround(size * h / 1080.0)), 1, 6);
}

void sharpen(Planes& p, double amount, int steps) {
  const int w = p.w, h = p.h;
  if (w < 2 || h < 2) return;
  // 明るさ（BT.601 フルレンジ）
  std::vector<int> Y(size_t(w) * h);
  for (size_t i = 0; i < Y.size(); ++i) Y[i] = std::clamp(int(std::lround(0.299 * p.c[0][i] + 0.587 * p.c[1][i] + 0.114 * p.c[2][i])), 0, 255);
  // 二項係数（2*steps 次）
  const int order = 2 * steps;
  std::vector<int64_t> coef(size_t(order + 1));
  coef[0] = 1;
  for (int k = 1; k <= order; ++k) coef[size_t(k)] = coef[size_t(k - 1)] * (order - k + 1) / k;
  std::vector<int64_t> hsum(size_t(w) * h);
  for (int y = 0; y < h; ++y) {
    const int* row = Y.data() + size_t(y) * w;
    for (int x = 0; x < w; ++x) {
      int64_t s = 0;
      for (int k = 0; k <= order; ++k) s += coef[size_t(k)] * row[std::clamp(x - steps + k, 0, w - 1)];
      hsum[size_t(y) * w + x] = s;
    }
  }
  const int scalebits = order * 2;
  const int64_t halfscale = int64_t(1) << (scalebits - 1);
  const int64_t fixedAmount = int64_t(amount * 65536.0);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int64_t s = 0;
      for (int k = 0; k <= order; ++k) s += coef[size_t(k)] * hsum[size_t(std::clamp(y - steps + k, 0, h - 1)) * w + x];
      const int64_t blur = (s + halfscale) >> scalebits;
      const size_t i = size_t(y) * w + x;
      const int64_t src = Y[i];
      const int res = std::clamp(int(src + (((src - blur) * fixedAmount) >> 16)), 0, 255);
      const int d = res - Y[i];
      if (d == 0) continue;
      for (int c = 0; c < 3; ++c) p.c[c][i] = clampByte(p.c[c][i] + d);
    }
  }
}

uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

/** ノイズの強さ（ffmpeg の noise の c0s と同じ数え方。0〜40） */
int noiseStrength(double amount) {
  return std::clamp(int(std::lround(amount * 40)), 0, 100);
}

void addNoise(Planes& p, double amount, long long frame) {
  const int s = noiseStrength(amount);
  if (s <= 0) return;
  // ffmpeg の noise（ガウス分布、標準偏差 = 強さ/√3）に近い分布。明るさだけに足す
  const double scale = s / std::sqrt(3.0);
  const uint32_t seed = hash32(uint32_t(frame) * 0x9e3779b9U + 0x1234567U);
  for (size_t i = 0; i < p.c[0].size(); ++i) {
    const uint32_t h1 = hash32(uint32_t(i) ^ seed);
    const uint32_t h2 = hash32(h1 + 0x68e31da4U);
    const uint32_t h3 = hash32(h2 + 0xb5297a4dU);
    // 一様分布 3 つの和（平均 0、分散 1 にそろえる）
    const double u = ((h1 >> 8) + (h2 >> 8) + (h3 >> 8)) / double(1 << 24) - 1.5;
    const int n = int(std::lround(u * 2.0 * scale));
    if (n == 0) continue;
    for (int c = 0; c < 3; ++c) p.c[c][i] = clampByte(p.c[c][i] + n);
  }
}

int rgbShiftPixels(int h, double amount) {
  return std::clamp(int(std::lround(amount * h)), 0, 255);
}

void rgbShift(Planes& p, int s) {
  if (s <= 0) return;
  const int w = p.w;
  std::vector<uint8_t> row(static_cast<size_t>(w));
  // 赤は右へ、青は左へ（ffmpeg の rgbashift の rh=s:bh=-s:edge=smear と同じ: 出力の x には、入力の x - ずらす量を使う）
  for (int y = 0; y < p.h; ++y) {
    uint8_t* r = p.c[0].data() + size_t(y) * w;
    uint8_t* b = p.c[2].data() + size_t(y) * w;
    std::copy(r, r + w, row.begin());
    for (int x = 0; x < w; ++x) r[x] = row[size_t(std::clamp(x - s, 0, w - 1))];
    std::copy(b, b + w, row.begin());
    for (int x = 0; x < w; ++x) b[x] = row[size_t(std::clamp(x + s, 0, w - 1))];
  }
}

std::array<uint8_t, 256> posterizeLut(int levels) {
  std::array<uint8_t, 256> lut{};
  const int L = std::clamp(levels, 2, 256);
  for (int v = 0; v < 256; ++v) lut[size_t(v)] = clampByte(int(std::floor(v * L / 256.0) * 255.0 / (L - 1) + 0.5));
  return lut;
}

double smoothstep(double e0, double e1, double x) {
  const double t = std::clamp((x - e0) / std::max(1e-9, e1 - e0), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

/** 周辺減光の倍率の画像を、同じ大きさ・同じ値のあいだ使い回す（別のスレッドからも呼ばれる） */
QImage cachedVignette(int w, int h, const Effect& e) {
  static QMutex mutex;
  static std::list<std::pair<QString, QImage>> cache;
  const QString key = QStringLiteral("%1x%2|%3|%4|%5").arg(w).arg(h).arg(e.amount).arg(e.size).arg(e.softness);
  {
    QMutexLocker lock(&mutex);
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (it->first == key) {
        cache.splice(cache.begin(), cache, it);
        return cache.front().second;
      }
    }
  }
  QImage m = vignetteMask(w, h, e);
  QMutexLocker lock(&mutex);
  cache.emplace_front(key, m);
  while (cache.size() > 6) cache.pop_back();
  return m;
}

}  // namespace

QImage vignetteMask(int w, int h, const Effect& e) {
  QImage m(std::max(1, w), std::max(1, h), QImage::Format_Grayscale8);
  const double inner = std::clamp(e.size, 0.0, 1.0) * 0.9;
  const double outer = inner + 0.05 + std::clamp(e.softness, 0.0, 1.0) * (1.05 - inner);
  const double amount = std::clamp(e.amount, 0.0, 1.0);
  for (int y = 0; y < m.height(); ++y) {
    uchar* line = m.scanLine(y);
    const double v = (y + 0.5) / m.height() * 2 - 1;
    for (int x = 0; x < m.width(); ++x) {
      const double u = (x + 0.5) / m.width() * 2 - 1;
      // 中心が 0、角が 1 になる距離（画面の縦横の比に合わせた楕円）
      const double d = std::sqrt((u * u + v * v) / 2);
      const double f = 1 - amount * smoothstep(inner, outer, d);
      line[x] = uchar(std::clamp(int(std::lround(f * 255)), 0, 255));
    }
  }
  return m;
}

void applyEffects(QImage& img, const std::vector<Effect>& effects, long long frame) {
  if (img.isNull() || !anyEffectActive(effects) || img.width() < 2 || img.height() < 2) return;
  if (img.format() != QImage::Format_ARGB32) img = img.convertToFormat(QImage::Format_ARGB32);
  Planes p = toPlanes(img);
  const int w = p.w, h = p.h;
  for (const Effect& e : effects) {
    if (!effectActive(e)) continue;
    switch (e.type) {
      case EffectType::Mosaic: {
        const MosaicGrid g = mosaicGrid(w, h, e.amount);
        p = nearestScale(areaScale(p, g.bx, g.by), w, h);
        break;
      }
      case EffectType::Blur: {
        const BlurPlan b = blurPlan(w, h, e.amount);
        if (b.radius <= 0) break;
        Planes work = b.scaled ? areaScale(p, b.wb, b.hb) : p;
        for (int c = 0; c < 4; ++c) boxBlurPlane(work.c[c], work.w, work.h, b.radius, 3);
        p = b.scaled ? bilinearScale(work, w, h) : work;
        break;
      }
      case EffectType::Sharpen: sharpen(p, e.amount, sharpenSteps(h, e.size)); break;
      case EffectType::Vignette: {
        const QImage m = cachedVignette(w, h, e);
        for (int y = 0; y < h; ++y) {
          const uchar* line = m.constScanLine(y);
          const size_t o = size_t(y) * w;
          for (int x = 0; x < w; ++x) {
            const int f = line[x];
            // ffmpeg の blend の multiply と同じ（a * b / 255 の切り捨て）
            for (int c = 0; c < 3; ++c) p.c[c][o + x] = uint8_t(p.c[c][o + x] * f / 255);
          }
        }
        break;
      }
      case EffectType::Invert:
        for (int c = 0; c < 3; ++c) {
          for (auto& v : p.c[c]) v = uint8_t(255 - v);
        }
        break;
      case EffectType::Posterize: {
        const auto lut = posterizeLut(int(std::lround(e.amount)));
        for (int c = 0; c < 3; ++c) {
          for (auto& v : p.c[c]) v = lut[v];
        }
        break;
      }
      case EffectType::Noise: addNoise(p, e.amount, frame); break;
      case EffectType::RgbShift: rgbShift(p, rgbShiftPixels(h, e.amount)); break;
      case EffectType::Fill:
        std::fill(p.c[0].begin(), p.c[0].end(), uint8_t(e.color.red()));
        std::fill(p.c[1].begin(), p.c[1].end(), uint8_t(e.color.green()));
        std::fill(p.c[2].begin(), p.c[2].end(), uint8_t(e.color.blue()));
        break;
    }
  }
  fromPlanes(p, img);
}

// ---- エフェクトレイヤー ------------------------------------------------------------------------

const std::vector<EffectLayerPreset>& effectLayerPresets() {
  static const std::vector<EffectLayerPreset> list = {
      {QStringLiteral("mosaic"), QStringLiteral("モザイク"), QStringLiteral("顔や文字など、見せたくない所にモザイクを掛けます")},
      {QStringLiteral("blur"), QStringLiteral("ぼかし"), QStringLiteral("見せたくない所をぼかします")},
      {QStringLiteral("fill"), QStringLiteral("黒塗り"), QStringLiteral("見せたくない所を、1 色で塗りつぶして隠します")},
      {QStringLiteral("spotlight"), QStringLiteral("スポットライト"), QStringLiteral("範囲の外を暗くして、範囲の中に目を向けさせます")},
      {QStringLiteral("adjust"), QStringLiteral("調整レイヤー"), QStringLiteral("下にある映像すべての色を、まとめて調整します")},
  };
  return list;
}

OverlayItem makeEffectLayer(const QString& presetId, Aspect aspect) {
  OverlayItem o;
  o.kind = LayerKind::Effect;
  o.assetId.clear();
  o.isImage = true;
  o.in = 0;
  o.out = 3;
  o.fit = FitMode::Contain;
  // 範囲は、画面の短い辺の 3 割の正方形（画面の中央）。大きさはプレビューの枠の角、縦横は辺のつまみで変えられる
  const QSize d = designSize(aspect);
  const double shortSide = std::min(d.width(), d.height());
  o.scale = 0.3;
  o.stretchX = std::clamp(shortSide / d.width(), kMinStretch, kMaxStretch);
  o.stretchY = std::clamp(shortSide / d.height(), kMinStretch, kMaxStretch);
  if (presetId == u"blur") {
    o.region = {RegionShape::Ellipse, 0.15, false};
    Effect e = makeEffect(EffectType::Blur);
    e.amount = 0.03;
    o.effects = {e};
  } else if (presetId == u"fill") {
    // 目の上などに置く、黒い帯（画面の幅の 4 割、高さの 1.2 割）
    o.region = {RegionShape::Rect, 0, false};
    o.effects = {makeEffect(EffectType::Fill)};
    o.scale = 1;
    o.stretchX = 0.4;
    o.stretchY = 0.12;
  } else if (presetId == u"spotlight") {
    // 範囲の外を暗くする（範囲は楕円で、境目は大きくぼかす）
    o.region = {RegionShape::Ellipse, 0.35, true};
    o.scale = 0.6;
    Effect e = makeEffect(EffectType::Fill);
    o.effects = {e};
    o.opacity = 0.6;
  } else if (presetId == u"adjust") {
    o.region = {RegionShape::Full, 0, false};
    o.scale = 1;
    o.stretchX = o.stretchY = 1;
    o.color = colorFromPreset(FilterPreset::Vivid);
  } else {
    o.region = {RegionShape::Ellipse, 0.1, false};
    Effect e = makeEffect(EffectType::Mosaic);
    e.amount = 0.025;
    o.effects = {e};
  }
  return o;
}

QString effectLayerLabel(const OverlayItem& o) {
  QStringList names;
  for (const Effect& e : o.effects) {
    if (e.enabled) names << effectInfo(e.type).label;
  }
  if (!isNeutralColor(o.color)) names << QStringLiteral("色の調整");
  if (names.isEmpty()) return QStringLiteral("エフェクトレイヤー");
  if (o.region.shape == RegionShape::Full && names.size() == 1 && names.front() == QStringLiteral("色の調整")) return QStringLiteral("調整レイヤー");
  return names.join(QStringLiteral("・"));
}

// ---- 書き出し（ffmpeg のフィルター） ----------------------------------------------------------

QStringList ffmpegEffectStatements(const std::vector<Effect>& effects, int w, int h, const QString& input, const QString& output, const QString& id,
                                   EffectGraphContext& ctx) {
  if (!anyEffectActive(effects) || w < 2 || h < 2) return {};
  QStringList out;
  QString cur = input;
  QStringList chain{QStringLiteral("format=gbrp")};
  int n = 0;
  // ここまでの続きの文を 1 つにする
  auto flush = [&](const QString& label) {
    out << QStringLiteral("[%1]%2[%3]").arg(cur, chain.join(u','), label);
    cur = label;
    chain = {QStringLiteral("format=gbrp")};
  };
  // 明るさだけを変えるフィルター（BT.601 フルレンジの YUV にして、Y だけに掛けて戻す）
  auto onLuma = [&](const QString& f) {
    chain << QStringLiteral("scale=out_color_matrix=bt601:out_range=pc") << QStringLiteral("format=yuv444p") << f
          << QStringLiteral("scale=in_color_matrix=bt601:in_range=pc") << QStringLiteral("format=gbrp");
  };
  for (const Effect& e : effects) {
    if (!effectActive(e)) continue;
    switch (e.type) {
      case EffectType::Mosaic: {
        const MosaicGrid g = mosaicGrid(w, h, e.amount);
        chain << QStringLiteral("scale=%1:%2:flags=area").arg(g.bx).arg(g.by) << QStringLiteral("format=gbrp")
              << QStringLiteral("scale=%1:%2:flags=neighbor").arg(w).arg(h) << QStringLiteral("format=gbrp");
        break;
      }
      case EffectType::Blur: {
        const BlurPlan b = blurPlan(w, h, e.amount);
        if (b.radius <= 0) break;
        if (b.scaled) chain << QStringLiteral("scale=%1:%2:flags=area").arg(b.wb).arg(b.hb) << QStringLiteral("format=gbrp");
        chain << QStringLiteral("boxblur=lr=%1:lp=3:cr=%1:cp=3").arg(b.radius);
        if (b.scaled) chain << QStringLiteral("scale=%1:%2:flags=bilinear").arg(w).arg(h) << QStringLiteral("format=gbrp");
        break;
      }
      case EffectType::Sharpen: {
        const int m = 2 * sharpenSteps(h, e.size) + 1;
        onLuma(QStringLiteral("unsharp=lx=%1:ly=%1:la=%2:cx=3:cy=3:ca=0").arg(m).arg(num6(e.amount)));
        break;
      }
      case EffectType::Vignette: {
        // 倍率の画像（RGB で同じ値）を掛ける: blend の multiply（a * b / 255）
        const QImage mask = vignetteMask(w, h, e).convertToFormat(QImage::Format_RGB32);
        const int k = ctx.addImage ? ctx.addImage(mask, QStringLiteral("vignette")) : -1;
        if (k < 0) break;
        const QString a = QStringLiteral("%1e%2a").arg(id).arg(n);
        const QString m = QStringLiteral("%1e%2m").arg(id).arg(n);
        const QString b = QStringLiteral("%1e%2b").arg(id).arg(n);
        flush(a);
        out << QStringLiteral("[%1:v]format=gbrp[%2]").arg(k).arg(m);
        out << QStringLiteral("[%1][%2]blend=all_mode=multiply[%3]").arg(a, m, b);
        cur = b;
        break;
      }
      case EffectType::Invert: chain << QStringLiteral("negate"); break;
      case EffectType::Posterize: {
        const int L = std::clamp(int(std::lround(e.amount)), 2, 256);
        const QString expr = QStringLiteral("trunc(floor(val*%1/256)*255/%2+0.5)").arg(L).arg(L - 1);
        chain << QStringLiteral("lutrgb=r='%1':g='%1':b='%1'").arg(expr);
        break;
      }
      case EffectType::Noise: onLuma(QStringLiteral("noise=c0s=%1:c0f=t").arg(noiseStrength(e.amount))); break;
      case EffectType::RgbShift: {
        const int s = rgbShiftPixels(h, e.amount);
        if (s > 0) chain << QStringLiteral("rgbashift=rh=%1:bh=%2:edge=smear").arg(s).arg(-s);
        break;
      }
      case EffectType::Fill:
        chain << QStringLiteral("lutrgb=r=%1:g=%2:b=%3").arg(e.color.red()).arg(e.color.green()).arg(e.color.blue());
        break;
    }
    ++n;
  }
  flush(output);
  return out;
}

}  // namespace kv
