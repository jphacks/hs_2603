#include "render/compositor.h"

#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <list>
#include <set>

#include "core/effects.h"
#include "core/filters.h"
#include "core/textstyles.h"
#include "core/transitions.h"
#include "mask/masktrack.h"
#include "render/imageops.h"

namespace kv {

namespace {

/** 先読みを始める時間（秒）。この時間より先に始まる動画は、まだ読まない */
constexpr double kPreloadSeconds = 3;

/** 重ねるトラックを描く順（下の段から。同じ段は始まりの順。後ろのものほど前に重なる） */
std::vector<const OverlayItem*> overlayOrder(const ProjectData& p) {
  std::vector<const OverlayItem*> order;
  for (const auto& o : p.overlays) order.push_back(&o);
  std::stable_sort(order.begin(), order.end(), [](const OverlayItem* a, const OverlayItem* b) {
    if (a->track != b->track) return a->track < b->track;
    return a->start < b->start;
  });
  return order;
}

}  // namespace

/**
 * メインのビデオのクリップ l の、タイムラインの時刻 t のフレーム（トランジションのときは、クリップの範囲の外も）。
 * 素材の外は端のフレームにする（書き出しの tpad の clone と同じ）
 */
static bool mainNeed(const ClipLayout& l, const AssetLookup& asset, double t, FrameNeed* out) {
  const VideoClip& c = l.clip;
  const Asset* a = asset(c.assetId);
  if (!a || a->status == AssetStatus::Missing) return false;
  const double local = t - l.start;
  FrameNeed n;
  n.key = QStringLiteral("clip:") + c.id;
  n.assetId = c.assetId;
  n.path = a->path;
  n.isImage = c.isImage;
  n.srcT = c.in + local * c.speed;
  // クリップの範囲の外（トランジションで前後を使う所）だけ、素材の中に収める（素材の最初より前は最初のフレーム。
  // VideoSource は 0 を求められたら、映像が少し遅れて始まる素材でも最初のフレームを出す）
  if (!c.isImage && (local < -1e-9 || local > l.duration + 1e-9)) n.srcT = std::clamp(n.srcT, 0.0, std::max(0.0, a->duration - 0.02));
  n.rate = c.speed;
  *out = n;
  return true;
}

FramePlan planFrame(const ProjectData& p, const TimelineLayout& layout, const AssetLookup& asset, double t) {
  FramePlan plan;
  const int fps = p.settings.fps;
  // メインのビデオを非表示にしているときは、メインのフレームは使わない（黒）
  const ClipLayout* cur = p.mainVideoHidden ? nullptr : findClipAt(layout, t);
  ActiveTransition tr;
  if (cur && transitionAt(layout, t, fps, &tr)) {
    // トランジションの途中: 前のクリップ（範囲の後ろの続き）と次のクリップ（範囲の前の手前）の両方
    for (const int index : {tr.index - 1, tr.index}) {
      FrameNeed n;
      if (mainNeed(layout.items[size_t(index)], asset, t, &n)) plan.needs.push_back(n);
    }
  } else if (cur) {
    const VideoClip& c = cur->clip;
    const Asset* a = asset(c.assetId);
    if (a && a->status != AssetStatus::Missing) {
      const double local = std::clamp(t - cur->start, 0.0, cur->duration);
      FrameNeed n;
      n.key = QStringLiteral("clip:") + c.id;
      n.assetId = c.assetId;
      n.path = a->path;
      n.isImage = c.isImage;
      n.srcT = c.in + local * c.speed;
      n.rate = c.speed;
      plan.needs.push_back(n);
    }
  }
  if (cur) {
    // 次のクリップを先読みしておく（トランジションがあるときは、そのトランジションの始まりのフレーム）
    const ClipLayout* next = cur->index + 1 < int(layout.items.size()) ? &layout.items[size_t(cur->index + 1)] : nullptr;
    const bool needed = next && std::any_of(plan.needs.begin(), plan.needs.end(), [&](const FrameNeed& n) { return n.key == QStringLiteral("clip:") + next->clip.id; });
    if (next && !needed && !next->clip.isImage) {
      // トランジションがあるときは、その始まり（切れ目より before フレーム前）から数えて先読みする
      const TransitionSpan span = transitionSpanAt(layout, next->index, fps);
      const double from = next->start - double(span.before) / std::max(1, fps);
      FrameNeed n;
      if (from - t < kPreloadSeconds && mainNeed(*next, asset, from, &n)) plan.preload.push_back(n);
    }
  }
  // 重ねるトラック: 今の時間に出ているもの。もうすぐ始まる動画は先読みしておく
  for (const OverlayItem* o : overlayOrder(p)) {
    if (isOverlayHidden(p, *o)) continue;  // 非表示の段は重ねない
    const Asset* a = asset(o->assetId);
    if (!a || a->status == AssetStatus::Missing || a->status == AssetStatus::Error) continue;
    const double dur = overlayDuration(*o, fps);
    const bool visible = t >= o->start - 1e-9 && t < o->start + dur - 1e-9;
    FrameNeed n;
    n.key = QStringLiteral("overlay:") + o->id;
    n.assetId = o->assetId;
    n.path = a->path;
    n.isImage = o->isImage;
    n.rate = o->speed;
    if (!visible) {
      if (!o->isImage && o->start > t && o->start - t < kPreloadSeconds) {
        n.srcT = o->in;
        plan.preload.push_back(n);
      }
      continue;
    }
    const double local = std::clamp(t - o->start, 0.0, dur);
    n.srcT = o->in + local * o->speed;
    plan.needs.push_back(n);
  }
  return plan;
}

long long effectFrameIndex(const VideoClip& c, double t, int fps) {
  // 写真のクリップは、書き出しでは 1 枚の画像を伸ばして使うので、ノイズの模様も変えない（プレビューと書き出しで同じ見え方）
  if (c.isImage) return 0;
  return std::llround(t * std::max(1, fps));
}

QImage prepareSourceFrame(const QImage& src, const VideoClip& c, long long frame, const QImage* fxMask) {
  const bool flip = c.flipX || c.flipY;
  const bool fx = anyEffectActive(c.effects);
  if (src.isNull() || (!flip && !fx)) return src;
  QImage work = flip ? src.mirrored(c.flipX, c.flipY) : src;
  if (!fx) return work;
  if (!fxMask || fxMask->size() != work.size()) {
    applyEffects(work, c.effects, frame);
    return work;
  }
  // エフェクトを、マスクの中だけに掛ける: 掛けた画像と元の画像を、マスクの値で混ぜる（エフェクトレイヤーと同じ式。書き出しの overlay と同じ）
  const QImage base = work.convertToFormat(QImage::Format_ARGB32);
  QImage fxd = base;
  applyEffects(fxd, c.effects, frame);
  if (fxd.format() != QImage::Format_ARGB32) fxd = fxd.convertToFormat(QImage::Format_ARGB32);
  QImage out = base;
  for (int y = 0; y < out.height(); ++y) {
    const uchar* m = fxMask->constScanLine(y);
    const QRgb* f = reinterpret_cast<const QRgb*>(fxd.constScanLine(y));
    QRgb* b = reinterpret_cast<QRgb*>(out.scanLine(y));
    for (int x = 0; x < out.width(); ++x) {
      const int a = m[x];
      if (a == 0) continue;
      if (a == 255) {
        b[x] = (b[x] & 0xff000000u) | (f[x] & 0x00ffffffu);
        continue;
      }
      const QRgb bv = b[x], fv = f[x];
      auto mix = [a](int bc, int fc) { return (bc * (255 - a) + fc * a + 127) / 255; };
      b[x] = qRgba(mix(qRed(bv), qRed(fv)), mix(qGreen(bv), qGreen(fv)), mix(qBlue(bv), qBlue(fv)), qAlpha(bv));
    }
  }
  return out;
}

QImage frameMaskImage(const FrameMask& m, QSize size, const VideoClip& c, bool mirror) {
  if (!m.logits) {
    QImage none(size.expandedTo(QSize(1, 1)), QImage::Format_Grayscale8);
    none.fill(m.invert ? 255 : 0);
    return none;
  }
  // 広げる・ぼかしは素材のピクセルで決めてあるので、描く大きさに合わせる（プレビューは素材より小さく描く）
  const double k = m.sourceWidth > 0 ? double(size.width()) / m.sourceWidth : 1.0;
  mask::MaskLook look;
  look.expand = m.expand * k;
  look.feather = m.feather * k;
  look.invert = m.invert;
  look.flipX = mirror && c.flipX;
  look.flipY = mirror && c.flipY;
  return mask::renderMask(*m.logits, size, look);
}

QImage processSourceFrame(const QString& key, const QImage& src, const VideoClip& c, bool withColor, bool onBlack, const QString& chromaPick,
                          ComposeCache* cache, long long frame, const FrameMask* mask) {
  const bool color = withColor && !isNeutralColor(c.color);
  // スポイトで色を選んでいる映像は、クロマキーを外して元の色で出す（透明にした所も、元の背景の色を見て選べるように）
  const bool keyOn = c.chroma.enabled && c.id != chromaPick;
  const bool flip = c.flipX || c.flipY;
  const bool fx = anyEffectActive(c.effects);
  // オブジェクトマスク: 背景を消す（透明度に掛ける）か、エフェクトをマスクの中だけに掛ける（エフェクトが無ければ何もしない）
  const bool maskCut = mask && mask->cutout;
  const bool maskFx = mask && !mask->cutout && fx;
  if (src.isNull() || (!keyOn && !color && !flip && !fx && !maskCut)) return src;
  const ColorAdjust& a = c.color;
  const ChromaKey& k = c.chroma;
  QString params = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                       .arg(keyOn)
                       .arg(k.color.name())
                       .arg(k.similarity)
                       .arg(k.blend)
                       .arg(k.despill)
                       .arg(color)
                       .arg(a.brightness)
                       .arg(a.contrast)
                       .arg(a.saturation) +
                   QStringLiteral("|%1|%2|%3").arg(a.temperature).arg(int(a.preset)).arg(onBlack);
  // 反転・エフェクト（ノイズはフレームごとに模様が変わる）
  params += QStringLiteral("|%1%2|%3").arg(c.flipX).arg(c.flipY).arg(effectsSignature(c.effects));
  if (effectsVaryPerFrame(c.effects)) params += QStringLiteral("|f%1").arg(frame);
  if (maskCut || maskFx) {
    params += QStringLiteral("|m%1|%2|%3|%4|%5|%6").arg(mask->key).arg(maskCut).arg(mask->invert).arg(mask->expand).arg(mask->feather).arg(mask->sourceWidth);
  }
  if (cache) {
    auto it = cache->processed.find(key);
    if (it != cache->processed.end() && it->second.source == src.cacheKey() && it->second.params == params) return it->second.image;
  }
  // マスクは、このフレームの大きさ・反転したあとの向きで描く
  const QImage maskImage = maskCut || maskFx ? frameMaskImage(*mask, src.size(), c) : QImage();
  // 反転 → エフェクト（マスクの中だけ）→ クロマキー → マスクで背景を消す → 色の調整（書き出しと同じ順）
  const QImage prepared = prepareSourceFrame(src, c, frame, maskFx ? &maskImage : nullptr);
  QImage out = keyOn ? chromaKeyImage(prepared, k) : prepared.convertToFormat(QImage::Format_ARGB32);
  if (maskCut && maskImage.size() == out.size()) {
    // 透明度にマスクを掛ける（クロマキーで透明にした所は、そのまま透明）
    for (int y = 0; y < out.height(); ++y) {
      const uchar* m = maskImage.constScanLine(y);
      QRgb* p = reinterpret_cast<QRgb*>(out.scanLine(y));
      for (int x = 0; x < out.width(); ++x) {
        const int alpha = (qAlpha(p[x]) * m[x] + 127) / 255;
        p[x] = (p[x] & 0x00ffffffu) | (uint(alpha) << 24);
      }
    }
  }
  if (color) applyColor(out, a);
  if ((keyOn || maskCut) && onBlack) {
    // 透明にした所を黒で埋める（ぼかし背景も、黒にしたこの映像から作るようにする。書き出しと同じ）
    QImage flat(out.size(), QImage::Format_RGB32);
    flat.fill(Qt::black);
    QPainter fp(&flat);
    fp.drawImage(0, 0, out);
    fp.end();
    out = flat;
  }
  if (cache) cache->processed[key] = {src.cacheKey(), params, out};
  return out;
}

namespace {

double smoothstep(double e0, double e1, double x) {
  const double t = std::clamp((x - e0) / std::max(1e-9, e1 - e0), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

/**
 * 範囲の形の画像（size の大きさの白で、透明度が形。境目のぼかしつき）。範囲は、この画面いっぱいの画像を、位置・拡大率・回転で動かしたもの。
 * 同じ大きさ・同じ形のあいだ使い回す（別のスレッドからも呼ばれる）
 */
QImage regionShapeImage(QSize size, RegionShape shape, double feather) {
  static QMutex mutex;
  static std::list<std::pair<QString, QImage>> cache;
  const QString key = QStringLiteral("%1x%2|%3|%4").arg(size.width()).arg(size.height()).arg(int(shape)).arg(feather);
  {
    QMutexLocker lock(&mutex);
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (it->first == key) {
        cache.splice(cache.begin(), cache, it);
        return cache.front().second;
      }
    }
  }
  const int w = std::max(1, size.width());
  const int h = std::max(1, size.height());
  const double shortSide = std::min(w, h);
  // 境目のぼかしの幅（画素）。範囲の短い辺に対する割合
  const double F = std::clamp(feather, 0.0, kMaxRegionFeather) * shortSide;
  QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < h; ++y) {
    QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
    const double v = (y + 0.5) / h * 2 - 1;
    for (int x = 0; x < w; ++x) {
      // 形の端からの、内側への距離（画素）
      double d;
      if (shape == RegionShape::Ellipse) {
        const double u = (x + 0.5) / w * 2 - 1;
        d = (1 - std::sqrt(u * u + v * v)) * shortSide / 2;
      } else {
        d = std::min({x + 0.5, w - x - 0.5, y + 0.5, h - y - 0.5});
      }
      const double a = F < 0.5 ? std::clamp(d + 0.5, 0.0, 1.0) : smoothstep(0, F, d);
      const int alpha = std::clamp(int(std::lround(a * 255)), 0, 255);
      line[x] = qRgba(alpha, alpha, alpha, alpha);
    }
  }
  QMutexLocker lock(&mutex);
  cache.emplace_front(key, img);
  while (cache.size() > 4) cache.pop_back();
  return img;
}

}  // namespace

bool effectLayerMaskStatic(const OverlayItem& o) {
  return o.anim.empty() && o.fadeIn <= 1e-6 && o.fadeOut <= 1e-6;
}

QImage effectLayerMask(const OverlayItem& o, double local, double duration, QSize canvas) {
  QImage mask(canvas.expandedTo(QSize(1, 1)), QImage::Format_Grayscale8);
  const LayerState ls = layerStateAt(o, local);
  const double alpha = std::clamp(ls.opacity, 0.0, 1.0) * fadeFactor(local, duration, o.fadeIn, o.fadeOut);
  if (o.region.shape == RegionShape::Full) {
    mask.fill(uint(std::clamp(int(std::lround(alpha * 255)), 0, 255)));
    return mask;
  }
  // 画面いっぱいの形を、位置・拡大率・伸縮・回転で動かす（重ねるトラックの映像と同じ動かし方）
  QImage layer(mask.size(), QImage::Format_ARGB32_Premultiplied);
  layer.fill(Qt::transparent);
  {
    QPainter p(&layer);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    p.setTransform(layerMatrix(ls, mask.size()));
    p.drawImage(QPointF(0, 0), regionShapeImage(mask.size(), o.region.shape, o.region.feather));
  }
  const int a255 = std::clamp(int(std::lround(alpha * 255)), 0, 255);
  for (int y = 0; y < mask.height(); ++y) {
    const QRgb* src = reinterpret_cast<const QRgb*>(layer.constScanLine(y));
    uchar* dst = mask.scanLine(y);
    for (int x = 0; x < mask.width(); ++x) {
      int m = qAlpha(src[x]);
      if (o.region.invert) m = 255 - m;
      dst[x] = uchar((m * a255 + 127) / 255);
    }
  }
  return mask;
}

namespace {

/** 効果を掛ける範囲（マスク）と、マスクが 0 でない所を囲む四角 */
struct LayerMask {
  QImage mask;
  QRect bounds;  // 空なら、どこにも掛けない
};

LayerMask makeLayerMask(const OverlayItem& o, double local, double duration, QSize canvas) {
  LayerMask r;
  r.mask = effectLayerMask(o, local, duration, canvas);
  int top = r.mask.height(), bottom = -1, left = r.mask.width(), right = -1;
  for (int y = 0; y < r.mask.height(); ++y) {
    const uchar* m = r.mask.constScanLine(y);
    const uchar* first = std::find_if(m, m + r.mask.width(), [](uchar v) { return v != 0; });
    if (first == m + r.mask.width()) continue;
    const uchar* last = m + r.mask.width() - 1;
    while (last > first && *last == 0) --last;
    top = std::min(top, y);
    bottom = y;
    left = std::min(left, int(first - m));
    right = std::max(right, int(last - m));
  }
  if (bottom >= 0) r.bounds = QRect(QPoint(left, top), QPoint(right, bottom));
  return r;
}

/**
 * 動かない範囲（キーフレーム・フェードなし）のマスクは、同じ設定・同じ大きさなら作り直さない
 * （再生中に毎フレーム、画面全体のマスクを作らないように）
 */
LayerMask layerMaskFor(const OverlayItem& o, double local, double duration, QSize canvas) {
  if (!effectLayerMaskStatic(o)) return makeLayerMask(o, local, duration, canvas);
  static QMutex mutex;
  static std::list<std::pair<QString, LayerMask>> cache;
  auto n = [](double v) { return QString::number(v, 'g', 17); };
  const QString key = QStringList{QString::number(canvas.width()), QString::number(canvas.height()), QString::number(int(o.region.shape)),
                                  n(o.region.feather), QString::number(int(o.region.invert)), n(o.x), n(o.y), n(o.scale), n(o.rotation),
                                  n(o.opacity), n(o.stretchX), n(o.stretchY)}
                          .join(QLatin1Char('|'));
  {
    QMutexLocker lock(&mutex);
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (it->first == key) {
        cache.splice(cache.begin(), cache, it);
        return cache.front().second;
      }
    }
  }
  LayerMask m = makeLayerMask(o, local, duration, canvas);
  QMutexLocker lock(&mutex);
  cache.emplace_front(key, m);
  while (cache.size() > 6) cache.pop_back();
  return m;
}

}  // namespace

void applyEffectLayer(QImage& canvas, const OverlayItem& o, double local, double duration, long long frame) {
  if (canvas.isNull()) return;
  const bool color = !isNeutralColor(o.color);
  if (!anyEffectActive(o.effects) && !color) return;
  const LayerMask lm = layerMaskFor(o, local, duration, canvas.size());
  const QImage& mask = lm.mask;
  // 範囲が画面の外・強さが 0 なら、何もしない
  if (lm.bounds.isEmpty()) return;
  const int top = lm.bounds.top(), bottom = lm.bounds.bottom();
  const int left = lm.bounds.left(), right = lm.bounds.right();
  if (canvas.format() != QImage::Format_RGB32 && canvas.format() != QImage::Format_ARGB32) canvas = canvas.convertToFormat(QImage::Format_RGB32);
  // 効果を掛けた画面（モザイクのブロックの並びは、画面全体でそろえる。書き出しと同じ）
  QImage fx = canvas.convertToFormat(QImage::Format_ARGB32);
  applyEffects(fx, o.effects, frame);
  if (color) applyColor(fx, o.color);
  for (int y = top; y <= bottom; ++y) {
    const uchar* m = mask.constScanLine(y);
    const QRgb* f = reinterpret_cast<const QRgb*>(fx.constScanLine(y));
    QRgb* b = reinterpret_cast<QRgb*>(canvas.scanLine(y));
    for (int x = left; x <= right; ++x) {
      const int a = m[x];
      if (a == 0) continue;
      if (a == 255) {
        b[x] = 0xff000000u | (f[x] & 0x00ffffffu);
        continue;
      }
      const QRgb bv = b[x], fv = f[x];
      auto mix = [a](int bc, int fc) { return (bc * (255 - a) + fc * a + 127) / 255; };
      b[x] = qRgb(mix(qRed(bv), qRed(fv)), mix(qGreen(bv), qGreen(fv)), mix(qBlue(bv), qBlue(fv)));
    }
  }
}

QImage composeFrame(const ProjectData& p, const TimelineLayout& layout, double t, const std::map<QString, QImage>& frames, const ComposeOptions& opt,
                    ComposeCache* cache) {
  const int fps = p.settings.fps;
  const QSize canvasSize = opt.canvas;
  auto frameOf = [&](const QString& key) -> const QImage* {
    auto it = frames.find(key);
    return it == frames.end() || it->second.isNull() ? nullptr : &it->second;
  };
  std::set<QString> usedProcessed;

  // ---- メインのビデオ ----
  // クロマキーで透明にした所は黒（書き出しと同じく、黒い背景の上に置いてから色を調整する）。切り抜いた所も黒。
  // local はクリップの先頭からの秒数（トランジションのときは、クリップの範囲の外もそのまま使う。フェード・キーフレームは端の値になる）
  auto maskOf = [&](const QString& key) -> const FrameMask* {
    auto it = opt.masks.find(key);
    return it == opt.masks.end() ? nullptr : &it->second;
  };
  auto mainLayer = [&](const ClipLayout& l, const QImage& frame, double local) {
    const QString key = QStringLiteral("clip:") + l.clip.id;
    usedProcessed.insert(key);
    const QImage keyed =
        processSourceFrame(key, frame, l.clip, false, true, opt.chromaPick, cache, effectFrameIndex(l.clip, t, fps), maskOf(key));
    QImage layer = composeClipLayer(keyed, l.clip.fit, canvasSize, cropSourceRect(l.clip, QSizeF(1, 1)));
    applyColor(layer, l.clip.color);
    // 位置・拡大率・回転・伸縮・不透明度（キーフレームがあれば、この時間の値）。動かしていなければ何もしない
    if (!layerIsPlain(l.clip)) layer = transformLayer(layer, layerStateAt(l.clip, local));
    const double fade = 1 - fadeFactor(local, l.duration, l.clip.fadeIn, l.clip.fadeOut);
    if (fade > 0.001) {
      QPainter fp(&layer);
      fp.fillRect(layer.rect(), QColor(0, 0, 0, int(std::lround(fade * 255))));
    }
    return layer;
  };
  auto black = [&] {
    QImage img(canvasSize, QImage::Format_RGB32);
    img.fill(Qt::black);
    return img;
  };
  QImage canvas;
  // メインのビデオを非表示にしているときは、黒（書き出しと同じ）
  const ClipLayout* cur = p.mainVideoHidden ? nullptr : findClipAt(layout, t);
  ActiveTransition tr;
  if (cur && transitionAt(layout, t, fps, &tr)) {
    // トランジション: 前のクリップと次のクリップをそれぞれ作ってから、書き出しの xfade と同じ式で混ぜる
    const ClipLayout& a = layout.items[size_t(tr.index - 1)];
    const ClipLayout& b = layout.items[size_t(tr.index)];
    const QImage* fa = frameOf(QStringLiteral("clip:") + a.clip.id);
    const QImage* fb = frameOf(QStringLiteral("clip:") + b.clip.id);
    const QImage la = fa ? mainLayer(a, *fa, t - a.start) : black();
    const QImage lb = fb ? mainLayer(b, *fb, t - b.start) : black();
    canvas = blendTransition(la, lb, b.clip.transIn.type, tr.progress);
  } else if (const QImage* clipFrame = cur ? frameOf(QStringLiteral("clip:") + cur->clip.id) : nullptr) {
    canvas = mainLayer(*cur, *clipFrame, std::clamp(t - cur->start, 0.0, cur->duration));
  } else {
    canvas = black();
  }

  // ---- 重ねるトラック（クロマキー・色の調整は素材だけに掛け、余白は透明。フェードは透明度で） ----
  {
    QPainter op;
    for (const OverlayItem* item : overlayOrder(p)) {
      const OverlayItem& o = *item;
      if (!isOverlayVisible(o, t, fps) || isOverlayHidden(p, o)) continue;  // 非表示の段は重ねない
      const double dur = overlayDuration(o, fps);
      const double local = std::clamp(t - o.start, 0.0, dur);
      // エフェクトレイヤー: ここまでに重ねた画面（メインと下の段）の、範囲の中に効果を掛ける
      if (o.isEffectLayer()) {
        if (op.isActive()) op.end();
        applyEffectLayer(canvas, o, local, dur, std::llround(t * fps));
        continue;
      }
      const QString key = QStringLiteral("overlay:") + o.id;
      const QImage* f = frameOf(key);
      if (!f) continue;
      usedProcessed.insert(key);
      const LayerState ls = layerStateAt(o, local);
      const double alpha = std::clamp(ls.opacity, 0.0, 1.0) * fadeFactor(local, dur, o.fadeIn, o.fadeOut);
      if (alpha <= 0.001) continue;
      const QImage media = processSourceFrame(key, *f, o, true, false, opt.chromaPick, cache, effectFrameIndex(o, t, fps), maskOf(key));
      if (!op.isActive()) op.begin(&canvas);
      op.setOpacity(alpha);
      drawClipMedia(op, media, cropSourceRect(o, QSizeF(1, 1)), ls, canvasSize);
    }
    if (op.isActive()) op.end();
  }
  // 使わなくなった加工済みのフレームは手放す
  if (cache) {
    for (auto it = cache->processed.begin(); it != cache->processed.end();) {
      if (usedProcessed.count(it->first)) ++it;
      else it = cache->processed.erase(it);
    }
  }

  // ---- テキスト ----
  const QSize design = designSize(p.settings.aspect);
  QPainter tp(&canvas);
  std::set<QString> live;
  // 下の段の文字から描く（タイムラインの上の段の文字ほど手前に重なる。書き出しと同じ順。選んでいるかどうかでは変えない）
  for (const size_t index : textPaintOrder(p.texts)) {
    const TextItem& item = p.texts[index];
    // 編集中の文字は、確定前の内容（変換中の文字を含む）で描く
    const QString& shownText = !opt.textOverrideId.isEmpty() && item.id == opt.textOverrideId ? opt.textOverride : item.text;
    if (!isTextVisible(item, t) || shownText.isEmpty()) continue;
    // 非表示の段の文字は描かない（プレビューで書き換えている最中の文字だけは、見えるように描く）
    if (isTextHidden(p, item) && item.id != opt.textOverrideId && item.id != opt.editingId) continue;
    live.insert(item.id);
    // 動かしていない文字は、位置まで描いた画像をそのまま重ねる。動かす文字は、画面の中央に置いた画像を
    // 位置・拡大率・回転・不透明度で動かす（動かしても描き直さない）
    const bool plain = layerIsPlain(item);
    const LayerState ls = layerStateAt(item, t - item.start);
    TextItem drawItem = item;
    drawItem.text = shownText;
    // 変換中の文字を差し込んだときも、一部だけ別のフォントにした範囲が、元の文字の位置からずれないようにする
    if (shownText != item.text) adjustFontSpans(item.text, shownText, drawItem.fontSpans);
    drawItem.letterSpacing = ls.letterSpacing;  // 字間のキーフレームも含めた、この時間の字間で描く
    if (!plain) {
      drawItem.x = 0.5;
      drawItem.y = 0.5;
    }
    TextSprite local;
    const TextSprite* sprite = nullptr;
    if (cache) {
      const QString spriteKey = textSpriteKey(drawItem, canvasSize);
      auto it = cache->sprites.find(item.id);
      if (it == cache->sprites.end() || it->second.key != spriteKey) {
        cache->sprites[item.id] = renderTextSprite(drawItem, design, canvasSize);
        it = cache->sprites.find(item.id);
      }
      sprite = &it->second;
    } else {
      local = renderTextSprite(drawItem, design, canvasSize);
      sprite = &local;
    }
    const AnimState st = previewTextState(item, t, opt.editingId);
    if (st.alpha <= 0.001 || sprite->image.isNull()) continue;
    if (plain) {
      tp.setOpacity(st.alpha);
      tp.drawImage(sprite->pos + QPointF(0, st.offsetY * canvasSize.height()), sprite->image);
      continue;
    }
    if (st.alpha * ls.opacity <= 0.001) continue;
    tp.save();
    tp.setOpacity(st.alpha * ls.opacity);
    drawLayer(tp, sprite->image, sprite->pos, ls, QPointF(0, st.offsetY * canvasSize.height()), canvasSize);
    tp.restore();
  }
  tp.end();
  if (cache && cache->sprites.size() > live.size() + 16) {
    for (auto it = cache->sprites.begin(); it != cache->sprites.end();) {
      if (live.count(it->first)) ++it;
      else it = cache->sprites.erase(it);
    }
  }
  return canvas;
}

}  // namespace kv
