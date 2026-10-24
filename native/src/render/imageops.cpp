#include "render/imageops.h"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vector>

namespace kv {

/** src を srcStride おきに n 個読み、ぼかした結果を dst に詰めて書く */
static void blurLineRgb(const QRgb* src, QRgb* dst, int n, int srcStride, int r) {
  // 端は端の色を繰り返す
  const int w = 2 * r + 1;
  int sr = 0, sg = 0, sb = 0;
  for (int i = -r; i <= r; ++i) {
    const QRgb p = src[std::clamp(i, 0, n - 1) * srcStride];
    sr += qRed(p);
    sg += qGreen(p);
    sb += qBlue(p);
  }
  for (int i = 0; i < n; ++i) {
    dst[i] = qRgb(sr / w, sg / w, sb / w);
    const QRgb out = src[std::clamp(i - r, 0, n - 1) * srcStride];
    const QRgb in = src[std::clamp(i + r + 1, 0, n - 1) * srcStride];
    sr += qRed(in) - qRed(out);
    sg += qGreen(in) - qGreen(out);
    sb += qBlue(in) - qBlue(out);
  }
}

void boxBlurRgb(QImage& img, int radius, int passes) {
  if (img.isNull() || radius <= 0) return;
  if (img.format() != QImage::Format_RGB32) img = img.convertToFormat(QImage::Format_RGB32);
  const int w = img.width();
  const int h = img.height();
  std::vector<QRgb> tmp(size_t(w) * h);
  for (int pass = 0; pass < passes; ++pass) {
    QRgb* data = reinterpret_cast<QRgb*>(img.bits());
    const int stride = int(img.bytesPerLine() / 4);
    for (int y = 0; y < h; ++y) blurLineRgb(data + y * stride, tmp.data() + size_t(y) * w, w, 1, radius);
    std::vector<QRgb> col(static_cast<size_t>(h));
    for (int x = 0; x < w; ++x) {
      blurLineRgb(tmp.data() + x, col.data(), h, w, radius);
      for (int y = 0; y < h; ++y) data[y * stride + x] = col[size_t(y)];
    }
  }
}

static void boxBlurAlphaPass(QImage& img, int r) {
  const int w = img.width();
  const int h = img.height();
  if (r <= 0) return;
  const int win = 2 * r + 1;
  std::vector<uint8_t> line(size_t(std::max(w, h)));
  for (int y = 0; y < h; ++y) {
    uint8_t* row = img.scanLine(y);
    int sum = 0;
    for (int i = -r; i <= r; ++i) sum += row[std::clamp(i, 0, w - 1)];
    for (int x = 0; x < w; ++x) {
      line[size_t(x)] = uint8_t(sum / win);
      sum += row[std::clamp(x + r + 1, 0, w - 1)] - row[std::clamp(x - r, 0, w - 1)];
    }
    std::copy(line.begin(), line.begin() + w, row);
  }
  for (int x = 0; x < w; ++x) {
    int sum = 0;
    for (int i = -r; i <= r; ++i) sum += img.scanLine(std::clamp(i, 0, h - 1))[x];
    for (int y = 0; y < h; ++y) {
      line[size_t(y)] = uint8_t(sum / win);
      sum += img.scanLine(std::clamp(y + r + 1, 0, h - 1))[x] - img.scanLine(std::clamp(y - r, 0, h - 1))[x];
    }
    for (int y = 0; y < h; ++y) img.scanLine(y)[x] = line[size_t(y)];
  }
}

void gaussianBlurAlpha(QImage& img, double sigma) {
  if (img.isNull() || sigma < 0.5) return;
  // 3 回の箱型ぼかしでガウスぼかしに近づける
  const int r = std::max(1, int(std::lround(std::sqrt(12.0 * sigma * sigma / 3 + 1) / 2)));
  for (int i = 0; i < 3; ++i) boxBlurAlphaPass(img, r);
}

QRectF containRect(QSizeF src, QSizeF dst) {
  if (src.isEmpty()) return {};
  const double s = std::min(dst.width() / src.width(), dst.height() / src.height());
  const double w = src.width() * s;
  const double h = src.height() * s;
  return {(dst.width() - w) / 2, (dst.height() - h) / 2, w, h};
}

QRectF coverRect(QSizeF src, QSizeF dst) {
  if (src.isEmpty()) return {};
  const double s = std::max(dst.width() / src.width(), dst.height() / src.height());
  const double w = src.width() * s;
  const double h = src.height() * s;
  return {(dst.width() - w) / 2, (dst.height() - h) / 2, w, h};
}

static int evenAtLeast2(double v) {
  return std::max(2, int(std::lround(v / 2)) * 2);
}

QImage composeClipLayer(const QImage& frame, FitMode fit, QSize canvas, const QRectF& keep) {
  QImage out(canvas, QImage::Format_RGB32);
  out.fill(Qt::black);
  if (frame.isNull() || canvas.isEmpty()) return out;
  QPainter p(&out);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  const QSizeF fs = frame.size();
  // 切り抜き: 残す部分（keep は素材に対する割合）だけを、切り抜く前と同じ場所・大きさに描く（ほかは黒）
  const bool cropped = keep.x() > 1e-6 || keep.y() > 1e-6 || keep.width() < 1 - 1e-6 || keep.height() < 1 - 1e-6;
  auto drawKept = [&](const QRectF& fitted) {
    if (!cropped) {
      p.drawImage(fitted, frame);
      return;
    }
    const QRectF src(keep.x() * fs.width(), keep.y() * fs.height(), keep.width() * fs.width(), keep.height() * fs.height());
    const QRectF dst(fitted.x() + keep.x() * fitted.width(), fitted.y() + keep.y() * fitted.height(), keep.width() * fitted.width(),
                     keep.height() * fitted.height());
    p.drawImage(dst, frame, src);
  };
  if (fit == FitMode::Cover) {
    drawKept(coverRect(fs, canvas));
    return out;
  }
  if (fit == FitMode::Blur) {
    // 書き出しと同じ: 画面いっぱいに拡大 → 1/8 に縮小 → ぼかし(半径 8 を 2 回) → 元の大きさへ
    const QRectF cover = coverRect(fs, canvas);
    const double s = cover.width() / fs.width();
    const QRectF crop(-cover.x() / s, -cover.y() / s, canvas.width() / s, canvas.height() / s);
    QImage small = frame.copy(crop.toAlignedRect().intersected(frame.rect()))
                       .scaled(evenAtLeast2(canvas.width() / 8.0), evenAtLeast2(canvas.height() / 8.0), Qt::IgnoreAspectRatio,
                               Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_RGB32);
    boxBlurRgb(small, 8, 2);
    p.drawImage(QRectF(QPointF(0, 0), QSizeF(canvas)), small);
  }
  drawKept(containRect(fs, canvas));
  return out;
}

void drawClipMedia(QPainter& p, const QImage& frame, const QRectF& keep, const LayerState& s, QSize canvas) {
  if (frame.isNull() || canvas.isEmpty()) return;
  const QSizeF fs = frame.size();
  const QRectF fitted = containRect(fs, canvas);
  const QRectF src(keep.x() * fs.width(), keep.y() * fs.height(), keep.width() * fs.width(), keep.height() * fs.height());
  const QRectF dst(fitted.x() + keep.x() * fitted.width(), fitted.y() + keep.y() * fitted.height(), keep.width() * fitted.width(),
                   keep.height() * fitted.height());
  if (dst.isEmpty() || src.isEmpty()) return;
  p.save();
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.setRenderHint(QPainter::Antialiasing);
  p.setTransform(layerMatrix(s, canvas), true);
  p.drawImage(dst, frame, src);
  p.restore();
}

QPolygonF clipMediaQuad(QSizeF frame, const QRectF& keep, const LayerState& s, QSizeF canvas) {
  if (frame.isEmpty()) return {};
  const QRectF fitted = containRect(frame, canvas);
  const QRectF dst(fitted.x() + keep.x() * fitted.width(), fitted.y() + keep.y() * fitted.height(), keep.width() * fitted.width(),
                   keep.height() * fitted.height());
  QPolygonF poly;
  poly << dst.topLeft() << dst.topRight() << dst.bottomRight() << dst.bottomLeft();
  return layerMatrix(s, canvas).map(poly);
}

// ---- クロマキー ---------------------------------------------------------------------------
// ffmpeg の chromakey と同じ考え方: 色み（BT.601 の U・V。明るさは使わない）の距離を 0〜1 で測り、
// 周りの 3×3 の平均をとって、similarity までは透明、そこから blend の幅でなめらかに不透明へ変える。
// 色かぶりの抑え（despill）は、ffmpeg の despill の既定の値（mix 0.5・expand 0）と同じ: 緑なら G を (R+B)/2 まで下げる

static inline double chromaU(double r, double g, double b) {
  return 128 + (-0.168736 * r - 0.331264 * g + 0.5 * b) * 224.0 / 255.0;
}
static inline double chromaV(double r, double g, double b) {
  return 128 + (0.5 * r - 0.418688 * g - 0.081312 * b) * 224.0 / 255.0;
}

int despillType(const QColor& key) {
  const int r = key.red(), g = key.green(), b = key.blue();
  if (g > r && g >= b) return 1;  // 緑
  if (b > r && b > g) return 2;   // 青
  return 0;
}

QImage chromaKeyImage(const QImage& src, const ChromaKey& k) {
  QImage img = src.convertToFormat(QImage::Format_ARGB32);
  if (img.isNull()) return img;
  const int w = img.width();
  const int h = img.height();
  // 透明にする色の U・V は、書き出し（ffmpeg の chromakey に yuv=1 で渡す値）と同じく整数に丸める
  const double ku = std::round(chromaU(k.color.red(), k.color.green(), k.color.blue()));
  const double kv = std::round(chromaV(k.color.red(), k.color.green(), k.color.blue()));
  // 1 画素ごとの色の違い
  std::vector<float> diff(size_t(w) * h);
  for (int y = 0; y < h; ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    for (int x = 0; x < w; ++x) {
      const QRgb px = line[x];
      const double du = chromaU(qRed(px), qGreen(px), qBlue(px)) - ku;
      const double dv = chromaV(qRed(px), qGreen(px), qBlue(px)) - kv;
      diff[size_t(y) * w + x] = float(std::sqrt((du * du + dv * dv) / (255.0 * 255.0 * 2)));
    }
  }
  // 周りの 3×3 の平均（横 → 縦の順に 3 画素の平均。端は端の画素を繰り返す）
  std::vector<float> tmp(diff.size());
  for (int y = 0; y < h; ++y) {
    const float* row = diff.data() + size_t(y) * w;
    float* out = tmp.data() + size_t(y) * w;
    for (int x = 0; x < w; ++x) out[x] = (row[std::max(0, x - 1)] + row[x] + row[std::min(w - 1, x + 1)]) / 3.0f;
  }
  const double sim = std::max(1e-5, k.similarity);
  const int spill = k.despill ? despillType(k.color) : 0;
  for (int y = 0; y < h; ++y) {
    const float* up = tmp.data() + size_t(std::max(0, y - 1)) * w;
    const float* mid = tmp.data() + size_t(y) * w;
    const float* down = tmp.data() + size_t(std::min(h - 1, y + 1)) * w;
    QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
    for (int x = 0; x < w; ++x) {
      const double d = (up[x] + mid[x] + down[x]) / 3.0;
      double a = k.blend > 0.0001 ? std::clamp((d - sim) / k.blend, 0.0, 1.0) : (d > sim ? 1.0 : 0.0);
      const QRgb px = line[x];
      int r = qRed(px), g = qGreen(px), b = qBlue(px);
      if (spill == 1) g = std::min(g, (r + b) / 2);
      else if (spill == 2) b = std::min(b, (r + g) / 2);
      line[x] = qRgba(r, g, b, int(std::lround(a * qAlpha(px))));
    }
  }
  return img;
}

QTransform layerMatrix(const LayerState& s, QSizeF canvas, QPointF extra) {
  QTransform t;
  t.translate(s.x * canvas.width() + extra.x(), s.y * canvas.height() + extra.y());
  t.rotate(s.rotation);
  // 伸縮（幅・高さだけの倍率）は、回転の前（素材の向きのまま）に掛ける
  t.scale(s.scale * s.stretchX, s.scale * s.stretchY);
  t.translate(-canvas.width() / 2, -canvas.height() / 2);
  return t;
}

QImage transformLayer(const QImage& layer, const LayerState& s) {
  QImage out(layer.size(), QImage::Format_RGB32);
  out.fill(Qt::black);
  QPainter p(&out);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.setRenderHint(QPainter::Antialiasing);
  p.setOpacity(std::clamp(s.opacity, 0.0, 1.0));
  p.setTransform(layerMatrix(s, layer.size()));
  p.drawImage(QPointF(0, 0), layer);
  return out;
}

void drawLayer(QPainter& p, const QImage& image, QPointF pos, const LayerState& s, QPointF extra, QSize canvas) {
  p.save();
  const bool moveOnly = std::abs(s.rotation) < 1e-9 && std::abs(s.scale - 1) < 1e-9 && std::abs(s.stretchX - 1) < 1e-9 && std::abs(s.stretchY - 1) < 1e-9;
  if (moveOnly) {
    // 動かすだけのときは、画素にそろえて描く（ぼやけない）
    const QPointF d(std::round(s.x * canvas.width() - canvas.width() / 2.0 + extra.x()),
                    std::round(s.y * canvas.height() - canvas.height() / 2.0 + extra.y()));
    p.drawImage(pos + d, image);
  } else {
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    p.setTransform(layerMatrix(s, canvas, extra), true);
    p.drawImage(pos, image);
  }
  p.restore();
}

}  // namespace kv
