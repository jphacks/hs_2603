#include "core/timeline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace kv {

QSize designSize(Aspect a) {
  switch (a) {
    case Aspect::R9_16: return {1080, 1920};
    case Aspect::R1_1: return {1080, 1080};
    case Aspect::R4_5: return {1080, 1350};
    default: return {1920, 1080};
  }
}

QSize exportSize(Aspect a, int shortSide) {
  const QSize d = designSize(a);
  const double scale = double(shortSide) / std::min(d.width(), d.height());
  auto even = [scale](int v) { return std::max(2, int(std::floor(v * scale / 2.0 + 0.5)) * 2); };
  return {even(d.width()), even(d.height())};
}

double aspectValue(Aspect a) {
  const QSize d = designSize(a);
  return double(d.width()) / d.height();
}

double frameRound(double t, int fps) {
  return std::floor(t * fps + 0.5) / fps;
}

double clipDuration(const VideoClip& c, int fps) {
  const double raw = c.isImage ? c.out - c.in : (c.out - c.in) / c.speed;
  return std::max(1.0, std::floor(raw * fps + 0.5)) / fps;
}

int clipFrames(const VideoClip& c, int fps) {
  return int(std::floor(clipDuration(c, fps) * fps + 0.5));
}

TimelineLayout layoutClips(const std::vector<VideoClip>& clips, int fps) {
  // 誤差がたまらないようにフレーム数で積み上げる
  TimelineLayout layout;
  layout.items.reserve(clips.size());
  long long frames = 0;
  for (size_t i = 0; i < clips.size(); ++i) {
    const int f = clipFrames(clips[i], fps);
    ClipLayout l;
    l.clip = clips[i];
    l.index = int(i);
    l.start = double(frames) / fps;
    frames += f;
    l.end = double(frames) / fps;
    l.duration = double(f) / fps;
    layout.items.push_back(l);
  }
  layout.total = double(frames) / fps;
  return layout;
}

const ClipLayout* findClipAt(const TimelineLayout& layout, double t) {
  const auto& items = layout.items;
  if (items.empty()) return nullptr;
  if (t >= layout.total) return &items.back();
  size_t lo = 0;
  size_t hi = items.size() - 1;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (items[mid].end <= t) lo = mid + 1;
    else hi = mid;
  }
  return &items[lo];
}

double fitFade(double value, double duration, double other) {
  return std::clamp(value, 0.0, std::max(0.0, std::min(kMaxFade, duration - std::max(0.0, other))));
}

double fadeFactor(double local, double duration, double fadeIn, double fadeOut) {
  double f = 1;
  if (fadeIn > 0) f = std::min(f, std::max(0.0, local / fadeIn));
  if (fadeOut > 0) f = std::min(f, std::max(0.0, (duration - local) / fadeOut));
  return std::clamp(f, 0.0, 1.0);
}

double audioDuration(const AudioItem& a) {
  return std::max(0.0, a.out - a.in);
}

AnimTimes textAnimTimes(double duration, TextAnimation anim) {
  if (anim == TextAnimation::None) return {};
  AnimTimes t;
  t.fadeIn = std::min(anim == TextAnimation::SlideUp ? 0.45 : 0.35, duration / 3);
  t.fadeOut = std::min(0.3, duration / 3);
  return t;
}

AnimState textAnimState(const TextItem& item, double t) {
  const double local = t - item.start;
  const AnimTimes times = textAnimTimes(item.duration, item.animation);
  AnimState s;
  if (times.fadeIn > 0) s.alpha = std::min(s.alpha, std::max(0.0, local / times.fadeIn));
  if (times.fadeOut > 0) s.alpha = std::min(s.alpha, std::max(0.0, (item.duration - local) / times.fadeOut));
  if (item.animation == TextAnimation::SlideUp && times.fadeIn > 0) {
    const double p = std::clamp(local / times.fadeIn, 0.0, 1.0);
    s.offsetY = kTextSlideDistance * std::pow(1 - p, 3);
  }
  s.alpha = std::clamp(s.alpha, 0.0, 1.0);
  return s;
}

double volumeAt(const std::vector<VolumePoint>& points, double fallback, double local) {
  if (points.empty()) return fallback;
  if (local <= points.front().t) return points.front().volume;
  if (local >= points.back().t) return points.back().volume;
  // local を挟む 2 つのキーフレームの間を、直線でつなぐ
  auto next = std::upper_bound(points.begin(), points.end(), local, [](double v, const VolumePoint& p) { return v < p.t; });
  const VolumePoint& b = *next;
  const VolumePoint& a = *(next - 1);
  const double span = b.t - a.t;
  if (span <= 1e-9) return b.volume;
  return a.volume + (b.volume - a.volume) * (local - a.t) / span;
}

double audioGainAt(const AudioItem& a, double local) {
  return volumeAt(a.volumePoints, a.volume, local);
}

std::vector<VolumePoint> shiftVolumePoints(const std::vector<VolumePoint>& points, double delta) {
  std::vector<VolumePoint> out = points;
  for (auto& p : out) p.t += delta;
  return out;
}

void normalizeVolumePoints(std::vector<VolumePoint>& points) {
  for (auto& p : points) p.volume = std::clamp(p.volume, 0.0, kMaxVolume);
  std::stable_sort(points.begin(), points.end(), [](const VolumePoint& a, const VolumePoint& b) { return a.t < b.t; });
  // 近すぎるキーフレームは後ろのものを省く
  std::vector<VolumePoint> kept;
  for (const auto& p : points) {
    if (!kept.empty() && p.t - kept.back().t < kMinVolumePointGap - 1e-9) continue;
    kept.push_back(p);
  }
  points = std::move(kept);
}

void splitVolumePointList(const std::vector<VolumePoint>& points, double fallback, double cut, std::vector<VolumePoint>& first,
                          std::vector<VolumePoint>& second) {
  first.clear();
  second.clear();
  if (points.empty()) return;
  const double g = volumeAt(points, fallback, cut);
  for (const auto& p : points) {
    if (p.t < cut - kMinVolumePointGap) first.push_back(p);
    else if (p.t > cut + kMinVolumePointGap) second.push_back({p.t - cut, p.volume});
  }
  // 分け目の前後が同じ音量でつながるように、それぞれの端にキーフレームを置く
  first.push_back({cut, g});
  second.insert(second.begin(), {0.0, g});
}

void splitVolumePoints(const AudioItem& a, double cut, std::vector<VolumePoint>& first, std::vector<VolumePoint>& second) {
  splitVolumePointList(a.volumePoints, a.volume, cut, first, second);
}

double clipGainAt(const VideoClip& c, double local) {
  return volumeAt(c.volumePoints, c.volume, local);
}

// ---- キーフレームアニメーション -----------------------------------------------------

PropLimits propLimits(AnimProp p) {
  switch (p) {
    case AnimProp::X:
    case AnimProp::Y: return {-2.0, 3.0, 0.5};
    case AnimProp::Scale: return {0.05, 4.0, 1.0};
    case AnimProp::Rotation: return {-3600.0, 3600.0, 0.0};
    case AnimProp::Opacity: return {0.0, 1.0, 1.0};
    case AnimProp::LetterSpacing: return {kMinLetterSpacing, kMaxLetterSpacing, 0.0};
  }
  return {0, 1, 0};
}

double clampProp(AnimProp p, double v) {
  const PropLimits l = propLimits(p);
  if (!std::isfinite(v)) return l.def;
  return std::clamp(v, l.min, l.max);
}

QString propLabel(AnimProp p) {
  switch (p) {
    case AnimProp::X: return QStringLiteral("位置 X");
    case AnimProp::Y: return QStringLiteral("位置 Y");
    case AnimProp::Scale: return QStringLiteral("拡大率");
    case AnimProp::Rotation: return QStringLiteral("回転");
    case AnimProp::Opacity: return QStringLiteral("不透明度");
    case AnimProp::LetterSpacing: return QStringLiteral("字間");
  }
  return {};
}

double easeCurve(Easing e, double u) {
  u = std::clamp(u, 0.0, 1.0);
  switch (e) {
    case Easing::EaseIn: return u * u * u;
    case Easing::EaseOut: return 1 - std::pow(1 - u, 3);
    case Easing::EaseInOut: return u * u * (3 - 2 * u);
    case Easing::Hold: return 0;
    default: return u;
  }
}

QString easingLabel(Easing e) {
  switch (e) {
    case Easing::EaseIn: return QStringLiteral("イーズイン（ゆっくり始まる）");
    case Easing::EaseOut: return QStringLiteral("イーズアウト（ゆっくり終わる）");
    case Easing::EaseInOut: return QStringLiteral("イーズインアウト");
    case Easing::Hold: return QStringLiteral("ホールド（次まで維持）");
    default: return QStringLiteral("リニア（一定の速さ）");
  }
}

double animValueAt(const std::vector<Keyframe>& keys, double base, double local) {
  if (keys.empty()) return base;
  if (local <= keys.front().t) return keys.front().value;
  if (local >= keys.back().t) return keys.back().value;
  // local を挟む 2 つのキーフレームの間を、変わり方に合わせてつなぐ
  auto next = std::upper_bound(keys.begin(), keys.end(), local, [](double v, const Keyframe& k) { return v < k.t; });
  const Keyframe& b = *next;
  const Keyframe& a = *(next - 1);
  const double span = b.t - a.t;
  if (a.easing == Easing::Hold || span <= 1e-9) return a.easing == Easing::Hold ? a.value : b.value;
  return a.value + (b.value - a.value) * easeCurve(a.easing, (local - a.t) / span);
}

double baseProp(const VideoClip& c, AnimProp p) {
  switch (p) {
    case AnimProp::X: return c.x;
    case AnimProp::Y: return c.y;
    case AnimProp::Scale: return c.scale;
    case AnimProp::Rotation: return c.rotation;
    case AnimProp::Opacity: return c.opacity;
    case AnimProp::LetterSpacing: return 0;  // 字間は文字だけの項目
  }
  return 0;
}

double baseProp(const TextItem& t, AnimProp p) {
  switch (p) {
    case AnimProp::X: return t.x;
    case AnimProp::Y: return t.y;
    case AnimProp::Scale: return t.scale;
    case AnimProp::Rotation: return t.rotation;
    case AnimProp::Opacity: return t.opacity;
    case AnimProp::LetterSpacing: return t.letterSpacing;
  }
  return 0;
}

void setBaseProp(VideoClip& c, AnimProp p, double v) {
  v = clampProp(p, v);
  switch (p) {
    case AnimProp::X: c.x = v; break;
    case AnimProp::Y: c.y = v; break;
    case AnimProp::Scale: c.scale = v; break;
    case AnimProp::Rotation: c.rotation = v; break;
    case AnimProp::Opacity: c.opacity = v; break;
    case AnimProp::LetterSpacing: break;  // 映像には字間が無い
  }
}

void setBaseProp(TextItem& t, AnimProp p, double v) {
  v = clampProp(p, v);
  switch (p) {
    case AnimProp::X: t.x = v; break;
    case AnimProp::Y: t.y = v; break;
    case AnimProp::Scale: t.scale = v; break;
    case AnimProp::Rotation: t.rotation = v; break;
    case AnimProp::Opacity: t.opacity = v; break;
    case AnimProp::LetterSpacing: t.letterSpacing = v; break;
  }
}

template <class T>
static LayerState layerStateOf(const T& o, double local) {
  LayerState s;
  s.x = clampProp(AnimProp::X, animValueAt(o.anim.keys(AnimProp::X), o.x, local));
  s.y = clampProp(AnimProp::Y, animValueAt(o.anim.keys(AnimProp::Y), o.y, local));
  s.scale = clampProp(AnimProp::Scale, animValueAt(o.anim.keys(AnimProp::Scale), o.scale, local));
  s.rotation = clampProp(AnimProp::Rotation, animValueAt(o.anim.keys(AnimProp::Rotation), o.rotation, local));
  s.opacity = clampProp(AnimProp::Opacity, animValueAt(o.anim.keys(AnimProp::Opacity), o.opacity, local));
  return s;
}

LayerState layerStateAt(const VideoClip& c, double local) {
  LayerState s = layerStateOf(c, local);
  s.stretchX = std::clamp(c.stretchX, kMinStretch, kMaxStretch);
  s.stretchY = std::clamp(c.stretchY, kMinStretch, kMaxStretch);
  return s;
}

// ---- 切り抜き・伸縮 -------------------------------------------------------------------------

bool hasCrop(const VideoClip& c) {
  return c.cropL > 1e-6 || c.cropT > 1e-6 || c.cropR > 1e-6 || c.cropB > 1e-6;
}

bool hasStretch(const VideoClip& c) {
  return std::abs(c.stretchX - 1) > 1e-9 || std::abs(c.stretchY - 1) > 1e-9;
}

void clampCropStretch(VideoClip& c) {
  auto fix = [](double& a, double& b) {
    if (!std::isfinite(a)) a = 0;
    if (!std::isfinite(b)) b = 0;
    a = std::clamp(a, 0.0, kMaxCrop);
    b = std::clamp(b, 0.0, kMaxCrop);
    // 両側を合わせて kMaxCrop まで（残る幅・高さが無くならないように）
    if (a + b > kMaxCrop) {
      const double k = kMaxCrop / (a + b);
      a *= k;
      b *= k;
    }
  };
  fix(c.cropL, c.cropR);
  fix(c.cropT, c.cropB);
  if (!std::isfinite(c.stretchX)) c.stretchX = 1;
  if (!std::isfinite(c.stretchY)) c.stretchY = 1;
  c.stretchX = std::clamp(c.stretchX, kMinStretch, kMaxStretch);
  c.stretchY = std::clamp(c.stretchY, kMinStretch, kMaxStretch);
  c.chroma.similarity = std::clamp(c.chroma.similarity, 0.0, kMaxChromaSimilarity);
  c.chroma.blend = std::clamp(c.chroma.blend, 0.0, kMaxChromaBlend);
}

QRectF cropSourceRect(const VideoClip& c, QSizeF src) {
  return {src.width() * c.cropL, src.height() * c.cropT, src.width() * std::max(0.0, 1 - c.cropL - c.cropR),
          src.height() * std::max(0.0, 1 - c.cropT - c.cropB)};
}

QRectF cropTargetRect(const VideoClip& c, const QRectF& fitted) {
  return {fitted.x() + fitted.width() * c.cropL, fitted.y() + fitted.height() * c.cropT, fitted.width() * std::max(0.0, 1 - c.cropL - c.cropR),
          fitted.height() * std::max(0.0, 1 - c.cropT - c.cropB)};
}

LayerState layerStateAt(const TextItem& t, double local) {
  LayerState s = layerStateOf(t, local);
  s.letterSpacing = clampProp(AnimProp::LetterSpacing, animValueAt(t.anim.keys(AnimProp::LetterSpacing), t.letterSpacing, local));
  return s;
}

bool layerIsPlain(const VideoClip& c) {
  return c.anim.empty() && std::abs(c.x - 0.5) < 1e-9 && std::abs(c.y - 0.5) < 1e-9 && std::abs(c.scale - 1) < 1e-9 &&
         std::abs(c.rotation) < 1e-9 && std::abs(c.opacity - 1) < 1e-9 && !hasStretch(c);
}

bool layerIsPlain(const TextItem& t) {
  return t.anim.empty() && std::abs(t.scale - 1) < 1e-9 && std::abs(t.rotation) < 1e-9 && std::abs(t.opacity - 1) < 1e-9;
}

int findKey(const std::vector<Keyframe>& keys, double t, double tol) {
  int best = -1;
  double bestD = tol;
  for (size_t i = 0; i < keys.size(); ++i) {
    const double d = std::abs(keys[i].t - t);
    if (d <= bestD) {
      bestD = d;
      best = int(i);
    }
  }
  return best;
}

int insertKey(std::vector<Keyframe>& keys, const Keyframe& k) {
  for (size_t i = 0; i < keys.size(); ++i) {
    if (std::abs(keys[i].t - k.t) < kMinKeyGap - 1e-9) return int(i);
  }
  auto it = std::lower_bound(keys.begin(), keys.end(), k.t, [](const Keyframe& a, double t) { return a.t < t; });
  const int idx = int(it - keys.begin());
  keys.insert(it, k);
  return idx;
}

void normalizeKeys(AnimProp p, std::vector<Keyframe>& keys) {
  for (auto& k : keys) {
    k.value = clampProp(p, k.value);
    if (!std::isfinite(k.t)) k.t = 0;
  }
  std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
  // 近すぎるキーフレームは後ろのものを省く
  std::vector<Keyframe> kept;
  for (const auto& k : keys) {
    if (!kept.empty() && k.t - kept.back().t < kMinKeyGap - 1e-9) continue;
    kept.push_back(k);
  }
  keys = std::move(kept);
}

Animation shiftAnimation(const Animation& a, double delta) {
  Animation out = a;
  for (auto& track : out.tracks) {
    for (auto& k : track) k.t += delta;
  }
  return out;
}

Animation scaleAnimationTime(const Animation& a, double factor) {
  Animation out = a;
  for (auto& track : out.tracks) {
    for (auto& k : track) k.t *= factor;
  }
  return out;
}

void splitAnimation(const Animation& a, double cut, Animation& first, Animation& second) {
  first = Animation();
  second = Animation();
  for (size_t i = 0; i < a.tracks.size(); ++i) {
    const auto& keys = a.tracks[i];
    if (keys.empty()) continue;
    const double g = animValueAt(keys, 0, cut);
    // 分け目をまたぐ区間の変わり方は、後半の先頭のキーフレームに引き継ぐ
    Easing carry = Easing::Linear;
    for (const auto& k : keys) {
      if (k.t <= cut) carry = k.easing;
    }
    for (const auto& k : keys) {
      if (k.t < cut - kMinKeyGap) first.tracks[i].push_back(k);
      else if (k.t > cut + kMinKeyGap) {
        Keyframe s = k;
        s.t -= cut;
        second.tracks[i].push_back(s);
      }
    }
    first.tracks[i].push_back({cut, g, Easing::Linear});
    second.tracks[i].insert(second.tracks[i].begin(), {0.0, g, carry});
  }
}

Animation cropAnimationStart(const Animation& a, double from) {
  Animation first, second;
  splitAnimation(a, from, first, second);
  return second;
}

Animation trimAnimationStart(const Animation& a, double shift) {
  if (std::abs(shift) < 1e-9) return a;
  return shift > 0 ? cropAnimationStart(a, shift) : shiftAnimation(a, -shift);
}

std::vector<VolumePoint> trimVolumePointsStart(const std::vector<VolumePoint>& points, double fallback, double shift) {
  if (points.empty() || std::abs(shift) < 1e-9) return points;
  if (shift < 0) return shiftVolumePoints(points, -shift);
  std::vector<VolumePoint> first, second;
  splitVolumePointList(points, fallback, shift, first, second);
  return second;
}

bool textTrackFree(const std::vector<TextItem>& texts, int track, double start, double duration, const QString& excludeId) {
  for (const auto& t : texts) {
    if (t.track != track || (!excludeId.isEmpty() && t.id == excludeId)) continue;
    if (t.start < start + duration - 1e-6 && start < t.start + t.duration - 1e-6) return false;
  }
  return true;
}

int freeTextTrack(const std::vector<TextItem>& texts, double start, double duration, const QString& excludeId) {
  int maxTrack = -1;
  for (const auto& t : texts) {
    if (!excludeId.isEmpty() && t.id == excludeId) continue;
    maxTrack = std::max(maxTrack, t.track);
  }
  for (int track = 0; track <= maxTrack; ++track) {
    if (textTrackFree(texts, track, start, duration, excludeId)) return track;
  }
  return maxTrack + 1;
}

void autoAssignTextTracks(std::vector<TextItem>& texts) {
  std::vector<size_t> order(texts.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return texts[a].start < texts[b].start; });
  std::vector<double> ends;  // 各段の、いちばん最後のテキストの終わり
  for (size_t idx : order) {
    TextItem& t = texts[idx];
    size_t k = 0;
    while (k < ends.size() && ends[k] > t.start + 1e-6) ++k;
    if (k == ends.size()) ends.push_back(0);
    ends[k] = t.start + t.duration;
    t.track = int(k);
  }
}

int textTrackCount(const std::vector<TextItem>& texts) {
  int count = 1;
  for (const auto& t : texts) count = std::max(count, t.track + 1);
  return count;
}

std::vector<size_t> textPaintOrder(const std::vector<TextItem>& texts) {
  std::vector<size_t> order(texts.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  // 番号の大きい段（下の段）から先に描く。選んでいるかどうかでは変えない
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return texts[a].track > texts[b].track; });
  return order;
}

void syncTextTrackIds(ProjectData& p) {
  auto& ids = p.textTrackIds;
  const size_t count = size_t(textTrackCount(p.texts));
  std::set<QString> seen;
  for (auto& id : ids) {
    if (id.isEmpty() || seen.count(id)) id = newId();
    seen.insert(id);
  }
  while (ids.size() < count) ids.push_back(newId());
  if (ids.size() > count) ids.resize(count);
  syncTrackNos(p.textTrackNos, count);
}

void syncTrackNos(std::vector<int>& nos, size_t count) {
  if (count <= 1) {
    nos.assign(count, 1);  // 段が 1 つだけなら、番号は付け直す（見出しは「テキスト」「オーディオ」だけ）
    return;
  }
  if (nos.size() > count) nos.resize(count);
  std::set<int> used;
  std::vector<size_t> fix;
  for (size_t i = 0; i < nos.size(); ++i) {
    if (nos[i] < 1 || used.count(nos[i])) fix.push_back(i);
    else used.insert(nos[i]);
  }
  auto next = [&used] {
    int n = 1;
    while (used.count(n)) ++n;
    used.insert(n);
    return n;
  };
  for (size_t i : fix) nos[i] = next();
  while (nos.size() < count) nos.push_back(next());
}

bool moveTextToTrack(ProjectData& p, const QString& id, const TextTrackTarget& target) {
  syncTextTrackIds(p);
  auto it = std::find_if(p.texts.begin(), p.texts.end(), [&](const TextItem& t) { return t.id == id; });
  if (it == p.texts.end() || target.kind == TextTrackTarget::Kind::Stay) return false;
  std::vector<int> before;
  for (const auto& t : p.texts) before.push_back(t.track);
  const std::vector<QString> idsBefore = p.textTrackIds;
  const std::vector<int> nosBefore = p.textTrackNos;
  const int from = it->track;
  if (target.kind == TextTrackTarget::Kind::Lane) {
    if (target.index < 0 || target.index == from) return false;
    it->track = target.index;
  } else {
    auto& ids = p.textTrackIds;
    auto& nos = p.textTrackNos;  // syncTextTrackIds で ids と同じ数にそろっている
    int at = std::clamp(target.index, 0, int(ids.size()));
    bool alone = true;
    for (const auto& t : p.texts) {
      if (t.id != id && t.track == from) alone = false;
    }
    // 元の段に自分だけなら、その段を詰めて、段（の ID・番号）ごと差し込む位置へ動かす。空の段は残さない
    QString laneId;
    int laneNo = 0;  // 0 は「新しい段」（番号は syncTextTrackIds で、空いている一番小さい番号を付ける）
    if (alone && from < int(ids.size())) {
      laneId = ids[size_t(from)];
      ids.erase(ids.begin() + from);
      if (from < int(nos.size())) {
        laneNo = nos[size_t(from)];
        nos.erase(nos.begin() + from);
      }
      for (auto& t : p.texts) {
        if (t.id != id && t.track > from) --t.track;
      }
      if (at > from) --at;
    }
    // at 以上の段のテキストを 1 段ずつ下へ（ID・番号も一緒にずらす）
    for (auto& t : p.texts) {
      if (t.id != id && t.track >= at) ++t.track;
    }
    ids.insert(ids.begin() + std::clamp(at, 0, int(ids.size())), laneId.isEmpty() ? newId() : laneId);
    nos.insert(nos.begin() + std::clamp(at, 0, int(nos.size())), laneNo);
    it->track = at;
  }
  syncTextTrackIds(p);
  bool changed = false;
  for (size_t i = 0; i < p.texts.size(); ++i) changed = changed || p.texts[i].track != before[i];
  if (!changed) {
    // 並びが同じなら、段の ID・番号も元のまま
    p.textTrackIds = idsBefore;
    p.textTrackNos = nosBefore;
  }
  return changed;
}

double freeTextShift(const std::vector<TextItem>& texts, const std::vector<TextShiftItem>& movers, const std::vector<QString>& exclude, double want,
                     double minDelta, double prefer) {
  constexpr double kEps = 1e-6;
  const double inf = std::numeric_limits<double>::infinity();
  // 置けない delta の範囲（両端ちょうどは、端が接するだけなので置ける）
  std::vector<std::pair<double, double>> bad;
  for (const TextShiftItem& m : movers) {
    for (const auto& o : texts) {
      if (o.track != m.track || std::find(exclude.begin(), exclude.end(), o.id) != exclude.end()) continue;
      const double lo = o.start - (m.start + m.duration);
      const double hi = o.start + o.duration - m.start;
      if (hi - lo > 2 * kEps) bad.push_back({lo, hi});
    }
  }
  std::sort(bad.begin(), bad.end());
  // 置ける範囲（minDelta から先で、置けない範囲を除いたところ）
  std::vector<std::pair<double, double>> ok;
  double cur = minDelta;
  for (const auto& [lo, hi] : bad) {
    if (hi <= cur + kEps) continue;
    if (lo >= cur - kEps) ok.push_back({cur, std::max(cur, lo)});
    cur = std::max(cur, hi);
  }
  ok.push_back({cur, inf});
  double best = cur;
  double bestD = inf;
  for (const auto& [a, b] : ok) {
    const double c = std::clamp(want, a, b);
    const double d = std::abs(c - want);
    if (d < bestD - 1e-9 || (std::abs(d - bestD) <= 1e-9 && std::abs(c - prefer) < std::abs(best - prefer))) {
      best = c;
      bestD = d;
    }
  }
  return best;
}

void resolveTextOverlaps(ProjectData& next, const ProjectData& before) {
  for (auto& t : next.texts) {
    const auto b = std::find_if(before.texts.begin(), before.texts.end(), [&](const TextItem& x) { return x.id == t.id; });
    const bool changed = b == before.texts.end() || std::abs(b->start - t.start) > 1e-9 || std::abs(b->duration - t.duration) > 1e-9 ||
                         b->track != t.track;
    if (!changed || textTrackFree(next.texts, t.track, t.start, t.duration, t.id)) continue;
    // 同じ段のほかのテキストに重なった: 重ならない近くの段へ（下の段から先に探す。一番下の段のさらに下は、いつも空いている）
    int lastOther = -1;
    for (const auto& x : next.texts) {
      if (x.id != t.id) lastOther = std::max(lastOther, x.track);
    }
    const int from = std::max(0, t.track);
    int to = lastOther + 1;
    for (int d = 1; d <= lastOther + 1; ++d) {
      if (from + d <= lastOther + 1 && textTrackFree(next.texts, from + d, t.start, t.duration, t.id)) {
        to = from + d;
        break;
      }
      if (from - d >= 0 && textTrackFree(next.texts, from - d, t.start, t.duration, t.id)) {
        to = from - d;
        break;
      }
    }
    t.track = to;
  }
}

bool audioTrackFree(const std::vector<AudioItem>& items, int track, double start, double duration, const QString& excludeId) {
  for (const auto& a : items) {
    if (a.track != track || (!excludeId.isEmpty() && a.id == excludeId)) continue;
    if (a.start < start + duration - 1e-6 && start < a.start + audioDuration(a) - 1e-6) return false;
  }
  return true;
}

int freeAudioTrack(const std::vector<AudioItem>& items, double start, double duration, const QString& excludeId) {
  int maxTrack = -1;
  for (const auto& a : items) {
    if (!excludeId.isEmpty() && a.id == excludeId) continue;
    maxTrack = std::max(maxTrack, a.track);
  }
  for (int track = 0; track <= maxTrack; ++track) {
    if (audioTrackFree(items, track, start, duration, excludeId)) return track;
  }
  return maxTrack + 1;
}

void autoAssignAudioTracks(std::vector<AudioItem>& items) {
  std::vector<size_t> order(items.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return items[a].start < items[b].start; });
  std::vector<double> ends;  // 各段の、いちばん最後のオーディオの終わり
  for (size_t idx : order) {
    AudioItem& a = items[idx];
    size_t k = 0;
    while (k < ends.size() && ends[k] > a.start + 1e-6) ++k;
    if (k == ends.size()) ends.push_back(0);
    ends[k] = a.start + audioDuration(a);
    a.track = int(k);
  }
}

int audioTrackCount(const std::vector<AudioItem>& items) {
  int count = 1;
  for (const auto& a : items) count = std::max(count, a.track + 1);
  return count;
}

void resolveAudioOverlaps(ProjectData& next, const ProjectData& before) {
  for (auto& a : next.audios) {
    a.track = std::max(0, a.track);
    const auto b = std::find_if(before.audios.begin(), before.audios.end(), [&](const AudioItem& x) { return x.id == a.id; });
    const bool changed = b == before.audios.end() || std::abs(b->start - a.start) > 1e-9 || std::abs(audioDuration(*b) - audioDuration(a)) > 1e-9 ||
                         b->track != a.track;
    const double dur = audioDuration(a);
    if (!changed || audioTrackFree(next.audios, a.track, a.start, dur, a.id)) continue;
    // 同じ段のほかのオーディオに重なった: 重ならない近くの段へ（下の段から先に探す。一番下の段のさらに下は、いつも空いている）
    int lastOther = -1;
    for (const auto& x : next.audios) {
      if (x.id != a.id) lastOther = std::max(lastOther, x.track);
    }
    const int from = a.track;
    int to = lastOther + 1;
    for (int d = 1; d <= lastOther + 1; ++d) {
      if (from + d <= lastOther + 1 && audioTrackFree(next.audios, from + d, a.start, dur, a.id)) {
        to = from + d;
        break;
      }
      if (from - d >= 0 && audioTrackFree(next.audios, from - d, a.start, dur, a.id)) {
        to = from - d;
        break;
      }
    }
    a.track = to;
  }
}

bool swapTextTracks(ProjectData& p, int a, int b) {
  syncTextTrackIds(p);
  const int count = textTrackCount(p.texts);
  if (a == b || a < 0 || b < 0 || a >= count || b >= count) return false;
  bool any = false;
  for (auto& t : p.texts) {
    if (t.track == a) {
      t.track = b;
      any = true;
    } else if (t.track == b) {
      t.track = a;
      any = true;
    }
  }
  if (!any) return false;  // どちらの段も空
  // 段の ID・番号も、中身と一緒に動く
  std::swap(p.textTrackIds[size_t(a)], p.textTrackIds[size_t(b)]);
  std::swap(p.textTrackNos[size_t(a)], p.textTrackNos[size_t(b)]);
  syncTextTrackIds(p);  // 一番下の段が空になったときは、その段を除く
  return true;
}

void syncAudioTrackNos(ProjectData& p) {
  const size_t count = size_t(audioTrackCount(p.audios));
  // 段が 1 つだけになると番号を 1 に付け直すので、段のミュートも、残った段の新しい番号に付け直す
  if (count <= 1 && !p.mutedAudioTracks.empty() && !p.audioTrackNos.empty()) {
    const bool muted = std::find(p.mutedAudioTracks.begin(), p.mutedAudioTracks.end(), p.audioTrackNos.front()) != p.mutedAudioTracks.end();
    p.mutedAudioTracks.clear();
    if (muted) p.mutedAudioTracks.push_back(1);
  }
  syncTrackNos(p.audioTrackNos, count);
}

bool moveAudioToTrack(ProjectData& p, const QString& id, const TextTrackTarget& target) {
  syncAudioTrackNos(p);
  auto it = std::find_if(p.audios.begin(), p.audios.end(), [&](const AudioItem& a) { return a.id == id; });
  if (it == p.audios.end() || target.kind == TextTrackTarget::Kind::Stay) return false;
  std::vector<int> before;
  for (const auto& a : p.audios) before.push_back(a.track);
  const std::vector<int> nosBefore = p.audioTrackNos;
  const int from = it->track;
  if (target.kind == TextTrackTarget::Kind::Lane) {
    if (target.index < 0 || target.index == from) return false;
    it->track = target.index;
  } else {
    auto& nos = p.audioTrackNos;  // syncAudioTrackNos で段の数にそろっている
    int at = std::clamp(target.index, 0, int(nos.size()));
    bool alone = true;
    for (const auto& a : p.audios) {
      if (a.id != id && a.track == from) alone = false;
    }
    // 元の段に自分だけなら、その段を詰めて、段（の番号）ごと差し込む位置へ動かす。空の段は残さない
    int laneNo = 0;  // 0 は「新しい段」（番号は syncAudioTrackNos で、空いている一番小さい番号を付ける）
    if (alone && from < int(nos.size())) {
      laneNo = nos[size_t(from)];
      nos.erase(nos.begin() + from);
      for (auto& a : p.audios) {
        if (a.id != id && a.track > from) --a.track;
      }
      if (at > from) --at;
    }
    // at 以上の段のオーディオを 1 段ずつ下へ（番号も一緒にずらす）
    for (auto& a : p.audios) {
      if (a.id != id && a.track >= at) ++a.track;
    }
    nos.insert(nos.begin() + std::clamp(at, 0, int(nos.size())), laneNo);
    it->track = at;
  }
  syncAudioTrackNos(p);
  bool changed = false;
  for (size_t i = 0; i < p.audios.size(); ++i) changed = changed || p.audios[i].track != before[i];
  if (!changed) p.audioTrackNos = nosBefore;  // 並びが同じなら、段の番号も元のまま
  return changed;
}

bool swapAudioTracks(ProjectData& p, int a, int b) {
  syncAudioTrackNos(p);
  const int count = audioTrackCount(p.audios);
  if (a == b || a < 0 || b < 0 || a >= count || b >= count) return false;
  bool any = false;
  for (auto& x : p.audios) {
    if (x.track == a) {
      x.track = b;
      any = true;
    } else if (x.track == b) {
      x.track = a;
      any = true;
    }
  }
  if (!any) return false;
  std::swap(p.audioTrackNos[size_t(a)], p.audioTrackNos[size_t(b)]);  // 段の番号も、中身と一緒に動く
  syncAudioTrackNos(p);  // 一番下の段が空になったときは、その段を除く
  return true;
}

double freeItemShift(const ProjectData& p, const std::vector<ShiftItem>& movers, const std::vector<QString>& exclude, double want, double minDelta,
                     double prefer) {
  // テキストとオーディオを、同じ並べ方の材料（段・始まり・長さ）にそろえて、freeTextShift で調べる。
  // オーディオの段は、テキストの段と混ざらないよう、番号をずらしておく
  constexpr int kAudioOffset = 1 << 20;
  std::vector<TextItem> obstacles;
  obstacles.reserve(p.texts.size() + p.audios.size());
  for (const auto& t : p.texts) {
    TextItem o;
    o.id = t.id;
    o.track = t.track;
    o.start = t.start;
    o.duration = t.duration;
    obstacles.push_back(o);
  }
  for (const auto& a : p.audios) {
    TextItem o;
    o.id = a.id;
    o.track = kAudioOffset + a.track;
    o.start = a.start;
    o.duration = audioDuration(a);
    obstacles.push_back(o);
  }
  std::vector<TextShiftItem> ms;
  ms.reserve(movers.size());
  for (const ShiftItem& m : movers) ms.push_back({m.audio ? kAudioOffset + m.track : m.track, m.start, m.duration});
  return freeTextShift(obstacles, ms, exclude, want, minDelta, prefer);
}

double overlayDuration(const OverlayItem& o, int fps) {
  return clipDuration(o, fps);
}

bool isOverlayVisible(const OverlayItem& o, double t, int fps) {
  return t >= o.start - 1e-9 && t < o.start + overlayDuration(o, fps) - 1e-9;
}

bool overlayTrackFree(const std::vector<OverlayItem>& items, int track, double start, double duration, int fps, const QString& excludeId) {
  for (const auto& o : items) {
    if (o.track != track || (!excludeId.isEmpty() && o.id == excludeId)) continue;
    if (o.start < start + duration - 1e-6 && start < o.start + overlayDuration(o, fps) - 1e-6) return false;
  }
  return true;
}

void normalizeTrackFlags(ProjectData& p) {
  auto dedupe = [](auto& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
  };
  std::erase_if(p.hiddenTextTracks, [&](const QString& id) { return std::find(p.textTrackIds.begin(), p.textTrackIds.end(), id) == p.textTrackIds.end(); });
  dedupe(p.hiddenTextTracks);
  const int lanes = overlayTrackCount(p.overlays);
  for (auto* v : {&p.hiddenOverlayTracks, &p.mutedOverlayTracks}) {
    std::erase_if(*v, [&](int k) { return k < 0 || k >= lanes; });
    dedupe(*v);
  }
  std::erase_if(p.mutedAudioTracks, [&](int n) { return std::find(p.audioTrackNos.begin(), p.audioTrackNos.end(), n) == p.audioTrackNos.end(); });
  dedupe(p.mutedAudioTracks);
}

bool liftOverlayToFreeLane(std::vector<OverlayItem>& items, const QString& id, int fps) {
  auto it = std::find_if(items.begin(), items.end(), [&](const OverlayItem& o) { return o.id == id; });
  if (it == items.end()) return false;
  const double dur = overlayDuration(*it, fps);
  int lane = std::max(0, it->track);
  while (!overlayTrackFree(items, lane, it->start, dur, fps, id)) ++lane;
  if (lane == it->track) return false;
  it->track = lane;
  return true;
}

int freeOverlayTrack(const std::vector<OverlayItem>& items, double start, double duration, int fps, const QString& excludeId) {
  int maxTrack = -1;
  for (const auto& o : items) {
    if (!excludeId.isEmpty() && o.id == excludeId) continue;
    maxTrack = std::max(maxTrack, o.track);
  }
  for (int track = 0; track <= maxTrack; ++track) {
    if (overlayTrackFree(items, track, start, duration, fps, excludeId)) return track;
  }
  return maxTrack + 1;
}

int overlayTrackCount(const std::vector<OverlayItem>& items) {
  int count = 1;
  for (const auto& o : items) count = std::max(count, o.track + 1);
  return count;
}

bool isTextVisible(const TextItem& item, double t) {
  return t >= item.start && t < item.start + item.duration;
}

std::vector<double> atempoFactors(double speed) {
  std::vector<double> out;
  double r = speed;
  while (r > 2 + 1e-9) {
    out.push_back(2);
    r /= 2;
  }
  while (r < 0.5 - 1e-9) {
    out.push_back(0.5);
    r /= 0.5;
  }
  if (std::abs(r - 1) > 1e-6) out.push_back(r);
  return out;
}

QString formatTime(double t, bool withFraction) {
  // 2.9999… のような誤差で表示がずれないよう 0.1 秒単位の整数にしてから分解する
  const long long tenths = (long long)std::floor(std::max(0.0, t) * 10 + 1e-6);
  const long long totalSec = tenths / 10;
  const QString base = QStringLiteral("%1:%2").arg(totalSec / 60).arg(totalSec % 60, 2, 10, QLatin1Char('0'));
  return withFraction ? QStringLiteral("%1.%2").arg(base).arg(tenths % 10) : base;
}

QString formatDuration(double t) {
  if (t < 60) return QString::number(t, 'f', 1) + QStringLiteral("秒");
  const long long total = (long long)std::floor(t + 0.5);
  return QStringLiteral("%1分%2秒").arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace kv
