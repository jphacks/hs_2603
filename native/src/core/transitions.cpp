#include "core/transitions.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace kv {

const std::vector<TransitionInfo>& transitionInfos() {
  using T = TransitionType;
  const QString dissolve = QStringLiteral("ディゾルブ");
  const QString wipe = QStringLiteral("ワイプ");
  const QString slide = QStringLiteral("スライド");
  const QString other = QStringLiteral("ズーム・その他");
  static const std::vector<TransitionInfo> list = {
      {T::Dissolve, QStringLiteral("dissolve"), QStringLiteral("クロスディゾルブ"), QStringLiteral("前の映像が、だんだん次の映像に変わります"),
       QStringLiteral("fade"), dissolve},
      {T::DipBlack, QStringLiteral("dipBlack"), QStringLiteral("暗転"), QStringLiteral("いったん黒くなってから、次の映像が出てきます"),
       QStringLiteral("fadeblack"), dissolve},
      {T::DipWhite, QStringLiteral("dipWhite"), QStringLiteral("ホワイトアウト"), QStringLiteral("いったん白くなってから、次の映像が出てきます"),
       QStringLiteral("fadewhite"), dissolve},
      {T::Grain, QStringLiteral("grain"), QStringLiteral("ノイズディゾルブ"), QStringLiteral("細かい点がばらばらに入れ替わって、次の映像になります"),
       QStringLiteral("dissolve"), dissolve},
      {T::WipeLeft, QStringLiteral("wipeLeft"), QStringLiteral("ワイプ ←"), QStringLiteral("右から左へ、線で区切って入れ替わります"),
       QStringLiteral("wipeleft"), wipe},
      {T::WipeRight, QStringLiteral("wipeRight"), QStringLiteral("ワイプ →"), QStringLiteral("左から右へ、線で区切って入れ替わります"),
       QStringLiteral("wiperight"), wipe},
      {T::WipeUp, QStringLiteral("wipeUp"), QStringLiteral("ワイプ ↑"), QStringLiteral("下から上へ、線で区切って入れ替わります"), QStringLiteral("wipeup"),
       wipe},
      {T::WipeDown, QStringLiteral("wipeDown"), QStringLiteral("ワイプ ↓"), QStringLiteral("上から下へ、線で区切って入れ替わります"),
       QStringLiteral("wipedown"), wipe},
      {T::SoftLeft, QStringLiteral("softLeft"), QStringLiteral("ソフトワイプ ←"), QStringLiteral("境目をぼかして、右から左へ入れ替わります"),
       QStringLiteral("smoothleft"), wipe},
      {T::SoftRight, QStringLiteral("softRight"), QStringLiteral("ソフトワイプ →"), QStringLiteral("境目をぼかして、左から右へ入れ替わります"),
       QStringLiteral("smoothright"), wipe},
      {T::Clock, QStringLiteral("clock"), QStringLiteral("時計回りワイプ"), QStringLiteral("時計の針のように回って入れ替わります"), QStringLiteral("radial"),
       wipe},
      {T::DoorsOpen, QStringLiteral("doorsOpen"), QStringLiteral("扉（左右に開く）"), QStringLiteral("真ん中から左右へ開くように、次の映像が出てきます"),
       QStringLiteral("vertopen"), wipe},
      {T::BlindsOpen, QStringLiteral("blindsOpen"), QStringLiteral("扉（上下に開く）"), QStringLiteral("真ん中から上下へ開くように、次の映像が出てきます"),
       QStringLiteral("horzopen"), wipe},
      {T::IrisOpen, QStringLiteral("irisOpen"), QStringLiteral("アイリス（開く）"), QStringLiteral("真ん中の円が広がって、次の映像が出てきます"),
       QStringLiteral("circleopen"), wipe},
      {T::IrisClose, QStringLiteral("irisClose"), QStringLiteral("アイリス（閉じる）"), QStringLiteral("周りから円が縮まって、次の映像に変わります"),
       QStringLiteral("circleclose"), wipe},
      {T::PushLeft, QStringLiteral("pushLeft"), QStringLiteral("プッシュ ←"), QStringLiteral("次の映像が、前の映像を左へ押し出します"),
       QStringLiteral("slideleft"), slide},
      {T::PushRight, QStringLiteral("pushRight"), QStringLiteral("プッシュ →"), QStringLiteral("次の映像が、前の映像を右へ押し出します"),
       QStringLiteral("slideright"), slide},
      {T::PushUp, QStringLiteral("pushUp"), QStringLiteral("プッシュ ↑"), QStringLiteral("次の映像が、前の映像を上へ押し出します"), QStringLiteral("slideup"),
       slide},
      {T::PushDown, QStringLiteral("pushDown"), QStringLiteral("プッシュ ↓"), QStringLiteral("次の映像が、前の映像を下へ押し出します"),
       QStringLiteral("slidedown"), slide},
      {T::SlideLeft, QStringLiteral("slideLeft"), QStringLiteral("スライド ←"), QStringLiteral("次の映像が右から滑り込んで、前の映像の上に重なります"),
       QStringLiteral("coverleft"), slide},
      {T::SlideRight, QStringLiteral("slideRight"), QStringLiteral("スライド →"), QStringLiteral("次の映像が左から滑り込んで、前の映像の上に重なります"),
       QStringLiteral("coverright"), slide},
      {T::Zoom, QStringLiteral("zoom"), QStringLiteral("ズーム"), QStringLiteral("前の映像に寄っていきながら、次の映像に変わります"), QStringLiteral("zoomin"),
       other},
      {T::Pixelize, QStringLiteral("pixelize"), QStringLiteral("ピクセル化"), QStringLiteral("モザイクのように粗くなってから、次の映像に変わります"),
       QStringLiteral("pixelize"), other},
  };
  return list;
}

const TransitionInfo& transitionInfo(TransitionType t) {
  for (const auto& i : transitionInfos()) {
    if (i.type == t) return i;
  }
  return transitionInfos().front();
}

QString transitionToString(TransitionType t) {
  if (t == TransitionType::None) return QStringLiteral("none");
  return transitionInfo(t).id;
}

bool transitionFromString(const QString& s, TransitionType* out) {
  for (const auto& i : transitionInfos()) {
    if (i.id == s) {
      *out = i.type;
      return true;
    }
  }
  return false;
}

QString transitionIconName(TransitionType t) {
  using T = TransitionType;
  switch (t) {
    case T::Dissolve: return QStringLiteral("blend");
    case T::DipBlack: return QStringLiteral("circle");
    case T::DipWhite: return QStringLiteral("sun");
    case T::Grain: return QStringLiteral("sparkles");
    case T::WipeLeft:
    case T::WipeRight:
    case T::WipeUp:
    case T::WipeDown:
    case T::SoftLeft:
    case T::SoftRight: return QStringLiteral("square-dashed");
    case T::Clock: return QStringLiteral("timer");
    case T::DoorsOpen:
    case T::BlindsOpen: return QStringLiteral("chevrons-left-right");
    case T::IrisOpen:
    case T::IrisClose: return QStringLiteral("circle-dot");
    case T::PushLeft:
    case T::PushRight:
    case T::PushUp:
    case T::PushDown:
    case T::SlideLeft:
    case T::SlideRight: return QStringLiteral("arrow-right");
    case T::Zoom: return QStringLiteral("zoom-in");
    case T::Pixelize: return QStringLiteral("grid-3x3");
    case T::None: break;
  }
  return QStringLiteral("blend");
}

TransitionSpan transitionSpanFrames(int wantFrames, int prevFrames, int nextFrames) {
  TransitionSpan s;
  const int want = std::max(0, wantFrames);
  s.before = std::min(want / 2, std::max(0, prevFrames) / 2);
  s.after = std::min(want - want / 2, std::max(0, nextFrames) / 2);
  if (!s.active()) return {};
  return s;
}

static int framesOf(double seconds, int fps) {
  return int(std::floor(seconds * fps + 0.5));
}

TransitionSpan transitionSpanFor(const Transition& tr, int prevFrames, int nextFrames, int fps) {
  if (tr.type == TransitionType::None) return {};
  const int want = framesOf(std::clamp(tr.duration, kMinTransitionDuration, kMaxTransitionDuration), std::max(1, fps));
  return transitionSpanFrames(std::max(2, want), prevFrames, nextFrames);
}

TransitionSpan transitionSpanAt(const TimelineLayout& layout, int index, int fps) {
  if (index <= 0 || index >= int(layout.items.size())) return {};
  return transitionSpanFor(layout.items[size_t(index)].clip.transIn, framesOf(layout.items[size_t(index - 1)].duration, fps),
                           framesOf(layout.items[size_t(index)].duration, fps), fps);
}

double transitionEffectiveDuration(const TimelineLayout& layout, int index, int fps) {
  return double(transitionSpanAt(layout, index, fps).frames()) / std::max(1, fps);
}

long long frameIndexAt(double t, int fps) {
  return (long long)std::floor(t * fps + 1e-6);
}

bool transitionAt(const TimelineLayout& layout, double t, int fps, ActiveTransition* out) {
  const auto& items = layout.items;
  if (items.size() < 2 || t < 0 || t >= layout.total) return false;
  const long long n = frameIndexAt(t, fps);
  const ClipLayout* cur = findClipAt(layout, t);
  if (!cur) return false;
  // 今のクリップに入るトランジション（切れ目の後ろの分）と、次のクリップに入るトランジション（切れ目の前の分）
  for (int index : {cur->index, cur->index + 1}) {
    if (index <= 0 || index >= int(items.size())) continue;
    const TransitionSpan span = transitionSpanAt(layout, index, fps);
    if (!span.active()) continue;
    const long long cut = (long long)std::llround(items[size_t(index)].start * fps);
    const long long start = cut - span.before;
    if (n < start || n >= start + span.frames()) continue;
    if (out) {
      out->index = index;
      out->span = span;
      out->startFrame = start;
      out->k = int(n - start);
      out->progress = std::clamp(1.f - float(n - start) / float(span.frames()), 0.f, 1.f);
    }
    return true;
  }
  return false;
}

// ---- xfade と同じ式（8 ビット。結果は C と同じく、小数を切り捨てる） ----------------------------------------

namespace {

inline float mixf(float a, float b, float m) {
  return a * m + b * (1.f - m);
}

inline float smoothstepf(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

inline uint8_t toByte(float v) {
  // xfade は float を uint8_t に入れる（切り捨て）。範囲の外にはならないが、念のため収める
  return uint8_t(std::clamp(int(v), 0, 255));
}

inline QRgb mixPixel(QRgb a, QRgb b, float m) {
  return qRgb(toByte(mixf(qRed(a), qRed(b), m)), toByte(mixf(qGreen(a), qGreen(b), m)), toByte(mixf(qBlue(a), qBlue(b), m)));
}

float frand(int x, int y) {
  // xfade と同じ式。sinf は、正しく丸めた値（double で求めて float にしたもの）を使う
  const float arg = float(x) * 12.9898f + float(y) * 78.233f;
  const float s = float(std::sin(double(arg)));
  const float r = s * 43758.545f;
  return r - std::floor(r);
}

}  // namespace

QImage blendTransition(const QImage& aIn, const QImage& bIn, TransitionType type, float progress) {
  if (bIn.isNull()) return aIn;
  const QImage b = bIn.convertToFormat(QImage::Format_RGB32);
  QImage a = aIn.isNull() ? QImage() : aIn.convertToFormat(QImage::Format_RGB32);
  if (a.size() != b.size()) {
    if (a.isNull()) {
      a = QImage(b.size(), QImage::Format_RGB32);
      a.fill(Qt::black);
    } else {
      a = a.scaled(b.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
  }
  const int W = b.width();
  const int H = b.height();
  const float p = std::clamp(progress, 0.f, 1.f);
  QImage out(b.size(), QImage::Format_RGB32);
  using T = TransitionType;
  auto rowA = [&](int y) { return reinterpret_cast<const QRgb*>(a.constScanLine(y)); };
  auto rowB = [&](int y) { return reinterpret_cast<const QRgb*>(b.constScanLine(y)); };
  auto rowO = [&](int y) { return reinterpret_cast<QRgb*>(out.scanLine(y)); };
  switch (type) {
    case T::None:
    case T::Dissolve:
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) o[x] = mixPixel(pa[x], pb[x], p);
      }
      break;
    case T::DipBlack:
    case T::DipWhite: {
      const float bg = type == T::DipBlack ? 0.f : 255.f;
      const float phase = 0.2f;
      const float s1 = smoothstepf(1.f - phase, 1.f, p);
      const float s2 = smoothstepf(phase, 1.f, p);
      auto ch = [&](int va, int vb) { return toByte(mixf(mixf(float(va), bg, s1), mixf(bg, float(vb), s2), p)); };
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) o[x] = qRgb(ch(qRed(pa[x]), qRed(pb[x])), ch(qGreen(pa[x]), qGreen(pb[x])), ch(qBlue(pa[x]), qBlue(pb[x])));
      }
      break;
    }
    case T::Grain:
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) {
          const float smooth = frand(x, y) * 2.f + p * 2.f - 1.5f;
          o[x] = smooth >= 0.5f ? pa[x] : pb[x];
        }
      }
      break;
    case T::WipeLeft:
    case T::WipeRight: {
      const int z = type == T::WipeLeft ? int(float(W) * p) : int(float(W) * (1.f - p));
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) o[x] = type == T::WipeLeft ? (x > z ? pb[x] : pa[x]) : (x > z ? pa[x] : pb[x]);
      }
      break;
    }
    case T::WipeUp:
    case T::WipeDown: {
      const int z = type == T::WipeUp ? int(float(H) * p) : int(float(H) * (1.f - p));
      for (int y = 0; y < H; ++y) {
        const bool useB = type == T::WipeUp ? y > z : !(y > z);
        std::copy_n(useB ? rowB(y) : rowA(y), W, rowO(y));
      }
      break;
    }
    case T::SoftLeft:
    case T::SoftRight: {
      const float w = float(W);
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) {
          const float smooth = type == T::SoftLeft ? 1.f + float(x) / w - p * 2.f : 1.f + (w - 1 - float(x)) / w - p * 2.f;
          o[x] = mixPixel(pb[x], pa[x], smoothstepf(0.f, 1.f, smooth));
        }
      }
      break;
    }
    case T::Clock: {
      const float turn = float((double(p) - 0.5) * (3.14159265358979323846 * 2.5));
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) {
          const float smooth = std::atan2(float(x - W / 2), float(y - H / 2)) - turn;
          o[x] = mixPixel(pb[x], pa[x], smoothstepf(0.f, 1.f, smooth));
        }
      }
      break;
    }
    case T::DoorsOpen:
    case T::BlindsOpen: {
      const float w2 = float(W / 2.0);
      const float h2 = float(H / 2.0);
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        const float rowSmooth = 2.f - std::fabs((float(y) - h2) / h2) - p * 2.f;
        for (int x = 0; x < W; ++x) {
          const float smooth = type == T::DoorsOpen ? 2.f - std::fabs((float(x) - w2) / w2) - p * 2.f : rowSmooth;
          o[x] = mixPixel(pb[x], pa[x], smoothstepf(0.f, 1.f, smooth));
        }
      }
      break;
    }
    case T::IrisOpen:
    case T::IrisClose: {
      const float z = std::hypot(float(W / 2), float(H / 2));
      const float pp = type == T::IrisOpen ? (p - 0.5f) * 3.f : (1.f - p - 0.5f) * 3.f;
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) {
          const float smooth = std::hypot(float(x - W / 2), float(y - H / 2)) / z + pp;
          const float s = smoothstepf(0.f, 1.f, smooth);
          o[x] = type == T::IrisOpen ? mixPixel(pa[x], pb[x], s) : mixPixel(pb[x], pa[x], s);
        }
      }
      break;
    }
    case T::PushLeft:
    case T::PushRight:
    case T::SlideLeft:
    case T::SlideRight: {
      const bool left = type == T::PushLeft || type == T::SlideLeft;
      const bool cover = type == T::SlideLeft || type == T::SlideRight;
      const int z = left ? int(-p * float(W)) : int(p * float(W));
      for (int y = 0; y < H; ++y) {
        const QRgb* pa = rowA(y);
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) {
          const int zx = z + x;
          const int zz = std::clamp(zx % W + W * (zx < 0), 0, W - 1);
          const bool inB = zx >= 0 && zx < W;
          o[x] = inB ? pb[zz] : cover ? pa[x] : pa[zz];
        }
      }
      break;
    }
    case T::PushUp:
    case T::PushDown: {
      const int z = type == T::PushUp ? int(-p * float(H)) : int(p * float(H));
      for (int y = 0; y < H; ++y) {
        const int zy = z + y;
        const int zz = std::clamp(zy % H + H * (zy < 0), 0, H - 1);
        const bool inB = zy >= 0 && zy < H;
        std::copy_n(inB ? rowB(zz) : rowA(zz), W, rowO(y));
      }
      break;
    }
    case T::Zoom: {
      const float w = float(W);
      const float h = float(H);
      const float zf = smoothstepf(0.5f, 1.f, p);
      const float m = smoothstepf(0.f, 0.5f, p);
      for (int y = 0; y < H; ++y) {
        const QRgb* pb = rowB(y);
        QRgb* o = rowO(y);
        float v = float(y) / h;
        v = 0.5f + (v - 0.5f) * zf;
        const int iv = std::clamp(int(std::ceil(v * (h - 1))), 0, H - 1);
        const QRgb* pa = rowA(iv);
        for (int x = 0; x < W; ++x) {
          float u = float(x) / w;
          u = 0.5f + (u - 0.5f) * zf;
          const int iu = std::clamp(int(std::ceil(u * (w - 1))), 0, W - 1);
          o[x] = mixPixel(pa[iu], pb[x], m);
        }
      }
      break;
    }
    case T::Pixelize: {
      const float d = std::min(p, 1.f - p);
      const float dist = std::ceil(d * 50.f) / 50.f;
      const float sq = 2.f * dist * float(std::min(W, H)) / 20.f;
      for (int y = 0; y < H; ++y) {
        const int sy = dist > 0.f ? std::clamp(int(std::min((std::floor(float(y) / sq) + .5f) * sq, float(H - 1))), 0, H - 1) : y;
        const QRgb* pa = rowA(sy);
        const QRgb* pb = rowB(sy);
        QRgb* o = rowO(y);
        for (int x = 0; x < W; ++x) {
          const int sx = dist > 0.f ? std::clamp(int(std::min((std::floor(float(x) / sq) + .5f) * sq, float(W - 1))), 0, W - 1) : x;
          o[x] = mixPixel(pa[sx], pb[sx], p);
        }
      }
      break;
    }
  }
  return out;
}

}  // namespace kv
