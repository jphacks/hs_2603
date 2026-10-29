#include "render/previewengine.h"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <set>
#include <thread>

#include "core/editor.h"
#include "core/filters.h"
#include "core/fontlibrary.h"
#include "core/textstyles.h"
#include "core/timeline.h"
#include "core/transitions.h"
#include "media/mediamanager.h"
#include "media/videosource.h"
#include "mask/maskcontroller.h"
#include "render/imageops.h"
#include "render/maskframes.h"

namespace kv {

static constexpr int kSourceMax = 1280;
static constexpr size_t kIdleSources = 2;
/** ループ再生で始めに戻る、この秒数前から、範囲の始めのフレームを先読みする */
static constexpr double kLoopPreloadSeconds = 1.5;

PreviewEngine::PreviewEngine(QObject* parent) : QObject(parent) {
  Editor& ed = editor();
  auto markDirty = [this] { dirty_ = true; };
  connect(&ed, &Editor::projectChanged, this, [this] {
    dirty_ = true;
    updateAudioSnapshot();
  });
  connect(&ed, &Editor::assetsChanged, this, [this] {
    dirty_ = true;
    updateAudioSnapshot();
  });
  connect(&ed, &Editor::timeChanged, this, markDirty);
  connect(&ed, &Editor::selectionChanged, this, markDirty);
  connect(&ed, &Editor::thumbsChanged, this, markDirty);
  connect(&ed, &Editor::playingChanged, this, markDirty);
  // タブを切り替えたら、別のタブの最後の画面を残さず、新しいタブの画面を作り直す
  connect(&ed, &Editor::workspaceChanged, this, [this] {
    frame_ = QImage();
    dirty_ = true;
  });
  // フォントを読み込んだ・外したときは、文字の画像を描き直す（同じファミリー名でも、描くフォントが変わる）
  connect(&fontLibrary(), &FontLibrary::changed, this, [this] {
    cache_.sprites.clear();
    dirty_ = true;
  });
  // スポイトで色を選んでいる間は、その映像のクロマキーを外して出す（被写体を選んでいる間は、マスクを外して出す）
  connect(&ed, &Editor::chromaPickChanged, this, markDirty);
  connect(&ed, &Editor::maskPickChanged, this, markDirty);
  // オブジェクトマスクのトラックの途中の結果ができたら、描き直す
  connect(&mask::maskController(), &mask::MaskController::liveUpdated, this, markDirty);
  timer_.setTimerType(Qt::PreciseTimer);
  timer_.setInterval(15);
  connect(&timer_, &QTimer::timeout, this, &PreviewEngine::tick);
  timer_.start();
  updateAudioSnapshot();
}

PreviewEngine::~PreviewEngine() {
  timer_.stop();
  audio_.stop();
  sources_.clear();
}

void PreviewEngine::setCanvasSize(QSize size) {
  size = size.expandedTo(QSize(16, 16));
  if (size == canvas_) return;
  canvas_ = size;
  cache_.sprites.clear();
  dirty_ = true;
}

void PreviewEngine::setTextOverride(const QString& id, const QString& text) {
  if (textOverrideId_ == id && textOverride_ == text) return;
  textOverrideId_ = id;
  textOverride_ = id.isEmpty() ? QString() : text;
  dirty_ = true;
}

void PreviewEngine::refreshNow() {
  Editor& ed = editor();
  if (ed.playing()) {
    dirty_ = true;
    return;
  }
  dirty_ = false;
  compose(ed.currentTime(), false);
}

std::optional<QColor> PreviewEngine::sampleSource(const QString& clipId, QPointF at) const {
  const Editor& ed = editor();
  const double t = ed.currentTime();
  const int fps = ed.project().settings.fps;
  const QSizeF cs(canvas_);
  const VideoClip* clip = nullptr;
  LayerState ls;
  FitMode fit = FitMode::Contain;  // 重ねるトラックは、いつも全体表示
  bool inLayer = false;            // メインの映像は、画面いっぱいの 1 枚にしてから動かす（その外の部分は描かれない）
  QString key;
  if (const OverlayItem* o = ed.overlay(clipId)) {
    if (!isOverlayVisible(*o, t, fps)) return std::nullopt;
    clip = o;
    ls = layerStateAt(*o, std::clamp(t - o->start, 0.0, overlayDuration(*o, fps)));
    key = QStringLiteral("overlay:") + clipId;
  } else {
    const ClipLayout* cur = findClipAt(ed.layout(), t);
    if (!cur || cur->clip.id != clipId) return std::nullopt;
    clip = &cur->clip;
    ls = layerStateAt(cur->clip, std::clamp(t - cur->start, 0.0, cur->duration));
    fit = cur->clip.fit;
    inLayer = true;
    key = QStringLiteral("clip:") + clipId;
  }
  const auto it = raw_.find(key);
  if (it == raw_.end() || it->second.isNull()) return std::nullopt;
  // クロマキーは、反転とエフェクトを掛けたあとの色で比べるので、色もそこから選ぶ（反転していれば、見えている向きのまま選べる）
  const QImage src = prepareSourceFrame(it->second, *clip, effectFrameIndex(*clip, t, fps));
  // 画面の点 → 動かす前の位置（画面いっぱいに置いたときの座標）→ 素材の中の割合
  bool invertible = false;
  const QTransform inv = layerMatrix(ls, cs).inverted(&invertible);
  if (!invertible) return std::nullopt;
  const QPointF p = inv.map(QPointF(at.x() * cs.width(), at.y() * cs.height()));
  if (inLayer && (p.x() < 0 || p.y() < 0 || p.x() > cs.width() || p.y() > cs.height())) return std::nullopt;
  const QRectF fitted = fit == FitMode::Cover ? coverRect(src.size(), cs) : containRect(src.size(), cs);
  if (fitted.isEmpty()) return std::nullopt;
  const double u = (p.x() - fitted.x()) / fitted.width();
  const double v = (p.y() - fitted.y()) / fitted.height();
  const QRectF keep = cropSourceRect(*clip, QSizeF(1, 1));  // 切り抜いて残した部分
  if (u < keep.left() || u > keep.right() || v < keep.top() || v > keep.bottom()) return std::nullopt;
  // 周りの 5×5 画素の平均（映像のノイズで、1 画素だけ違う色を拾わないように）
  const QImage img = src.format() == QImage::Format_RGB32 || src.format() == QImage::Format_ARGB32 ? src : src.convertToFormat(QImage::Format_ARGB32);
  const int cx = std::clamp(int(u * img.width()), 0, img.width() - 1);
  const int cy = std::clamp(int(v * img.height()), 0, img.height() - 1);
  long long r = 0, g = 0, b = 0, n = 0;
  for (int y = std::max(0, cy - 2); y <= std::min(img.height() - 1, cy + 2); ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    for (int x = std::max(0, cx - 2); x <= std::min(img.width() - 1, cx + 2); ++x) {
      r += qRed(line[x]);
      g += qGreen(line[x]);
      b += qBlue(line[x]);
      ++n;
    }
  }
  if (n == 0) return std::nullopt;
  return QColor(int((r + n / 2) / n), int((g + n / 2) / n), int((b + n / 2) / n));
}

QImage PreviewEngine::rawFrame(const QString& clipId, double* pts) const {
  for (const QString& key : {QStringLiteral("clip:") + clipId, QStringLiteral("overlay:") + clipId}) {
    const auto it = raw_.find(key);
    if (it == raw_.end() || it->second.isNull()) continue;
    if (pts) {
      const auto p = rawPts_.find(key);
      *pts = p != rawPts_.end() ? p->second : 0.0;
    }
    return it->second;
  }
  return {};
}

/**
 * 動画全体の再生速度 S のときの音: 音のミキサーは「聞こえる時間」で動かす。タイムラインの t は、聞こえる時間では t / S。
 * 各音の始まり・長さ・フェード・音量キーフレームの時間を 1/S にし、読み出す速さを S 倍にする（atempo で音程はそのまま）
 */
static void warpSource(MixSource& s, double S) {
  if (std::abs(S - 1) < 1e-9) return;
  s.start /= S;
  s.duration /= S;
  s.fadeIn /= S;
  s.fadeOut /= S;
  s.headExt /= S;
  s.headXf /= S;
  s.tailExt /= S;
  s.tailXf /= S;
  s.speed *= S;
  for (auto& p : s.points) p.t /= S;
}

std::shared_ptr<MixSnapshot> buildMixSnapshot(const ProjectData& p, const TimelineLayout& layout, const std::function<const Asset*(const QString&)>& asset,
                                              double playEnd) {
  auto snap = std::make_shared<MixSnapshot>();
  const int fps = std::max(1, p.settings.fps);
  // メインのビデオの音（トラックのミュートでは鳴らさない）。トランジションで音もクロスフェードする切れ目では、前後に音を足して重ねる
  const auto& items = layout.items;
  for (size_t i = 0; i < items.size() && !p.mainAudioMuted; ++i) {
    const ClipLayout& l = items[i];
    const VideoClip& c = l.clip;
    const Asset* a = asset(c.assetId);
    if (c.isImage || !a || !a->hasAudio || a->status == AssetStatus::Missing || c.muted || (c.volumePoints.empty() && c.volume <= 0.0001)) continue;
    MixSource src{QStringLiteral("clip:") + c.id, a->path, l.start, l.duration, c.in, c.speed, c.volume, c.fadeIn, c.fadeOut};
    src.points = c.volumePoints;  // 音量キーフレーム（タイムライン上の、クリップの先頭からの秒数）
    src.pan = c.pan;
    if (i > 0 && c.transIn.type != TransitionType::None && c.transIn.audioCrossfade) {
      const TransitionSpan sp = transitionSpanAt(layout, int(i), fps);
      if (sp.active()) {
        src.headExt = double(sp.before) / fps;
        src.headXf = double(sp.frames()) / fps;
      }
    }
    if (i + 1 < items.size()) {
      const Transition& next = items[i + 1].clip.transIn;
      if (next.type != TransitionType::None && next.audioCrossfade) {
        const TransitionSpan sp = transitionSpanAt(layout, int(i + 1), fps);
        if (sp.active()) {
          src.tailExt = double(sp.after) / fps;
          src.tailXf = double(sp.frames()) / fps;
        }
      }
    }
    snap->sources.push_back(src);
  }
  // 重ねるトラックの動画の音（音量・ミュート・フェード・音量キーフレームは、ビデオクリップと同じ。段のミュートでは鳴らさない）
  for (const auto& o : p.overlays) {
    const Asset* a = asset(o.assetId);
    if (o.isImage || !a || !a->hasAudio || a->status == AssetStatus::Missing || o.muted || isOverlayMuted(p, o) ||
        (o.volumePoints.empty() && o.volume <= 0.0001)) {
      continue;
    }
    MixSource src{QStringLiteral("overlay:") + o.id, a->path, o.start, overlayDuration(o, p.settings.fps), o.in, o.speed, o.volume,
                  o.fadeIn, o.fadeOut};
    src.points = o.volumePoints;
    src.pan = o.pan;
    snap->sources.push_back(src);
  }
  for (const auto& au : p.audios) {
    const Asset* a = asset(au.assetId);
    if (!a || a->status == AssetStatus::Missing || isAudioMuted(p, au) || (au.volumePoints.empty() && au.volume <= 0.0001)) continue;
    MixSource src{QStringLiteral("audio:") + au.id, a->path, au.start, audioDuration(au), au.in, 1.0, au.volume, au.fadeIn, au.fadeOut};
    src.points = au.volumePoints;
    src.pan = au.pan;
    snap->sources.push_back(src);
  }
  const double S = p.settings.speed;
  for (auto& s : snap->sources) warpSource(s, S);
  snap->end = playEnd / S;
  return snap;
}

void PreviewEngine::updateAudioSnapshot() {
  const Editor& ed = editor();
  audio_.setSnapshot(buildMixSnapshot(ed.project(), ed.layout(), [&ed](const QString& id) { return ed.asset(id); }, ed.playEnd()));
}

VideoSource* PreviewEngine::source(const QString& key, const QString& path) {
  auto it = sources_.find(key);
  if (it != sources_.end() && it->second->path() == path) return it->second.get();
  if (it != sources_.end()) {
    // 終了待ちで画面が止まらないよう、別スレッドで片付ける
    std::thread([p = std::move(it->second)]() mutable { p.reset(); }).detach();
    sources_.erase(it);
  }
  auto src = std::make_unique<VideoSource>(path, kSourceMax, kSourceMax, [this] { dirty_ = true; });
  VideoSource* raw = src.get();
  sources_[key] = std::move(src);
  return raw;
}

void PreviewEngine::tick() {
  Editor& ed = editor();
  double t = ed.currentTime();
  const bool playing = ed.playing();
  if (playing) {
    // 動画全体の再生速度 S: タイムラインの時間は、聞こえる時間（音の時計）の S 倍で進む
    const double S = ed.project().settings.speed;
    if (!wasPlaying_ || ed.seekToken() != seenSeek_ || std::abs(S - playSpeed_) > 1e-9) {
      // 再生中に再生位置を動かした: 新しい時刻のフレームがそろうまで、前の画面のまま（再生を始めたときは、今の画面が同じ時刻）
      if (wasPlaying_ && ed.seekToken() != seenSeek_) {
        awaitExact_ = true;
        awaitTimer_.restart();
      }
      wasPlaying_ = true;
      seenSeek_ = ed.seekToken();
      playStart_ = t;
      playSpeed_ = S;
      audio_.start(t / S);
      wall_.restart();
    }
    if (audio_.ok()) t = audio_.started() ? audio_.clock() * S : playStart_;
    else t = playStart_ + wall_.elapsed() / 1000.0 * S;
    const double end = ed.playEnd();
    // ループ再生: くり返す範囲（書き出す範囲、無ければ全体）の終わりまで来たら、始めに戻って続ける（音も始めから鳴らし直す）。
    // 範囲より後ろへ動かして再生していたときも、動画の終わりまで来たら、範囲の始めに戻る
    const Editor::TimeRange loop = ed.loopRange();
    const bool looping = ed.loopPlayback() && loop.end - loop.start >= kMinLoopLength;
    if (looping && ((playStart_ < loop.end && t >= std::min(loop.end, end)) || t >= end)) {
      t = loop.start;
      // 終わりぎわに先読みしておいた、範囲の始めのデコーダーに入れ替える（すぐに始めのフレームを出せるように）
      std::vector<QString> loopKeys;
      for (const auto& [key, src] : sources_) {
        if (key.startsWith(QLatin1String("loop:"))) loopKeys.push_back(key);
      }
      for (const QString& key : loopKeys) {
        auto lit = sources_.find(key);
        auto rit = sources_.find(key.mid(5));
        if (rit != sources_.end()) {
          std::swap(rit->second, lit->second);
        } else {
          sources_[key.mid(5)] = std::move(lit->second);
          sources_.erase(key);
        }
      }
      ed.seek(t);
      playStart_ = t;
      audio_.start(t / S);
      wall_.restart();
      awaitExact_ = true;
      awaitTimer_.restart();
    } else if (t >= end) {
      ed.setTimeFromEngine(end);
      ed.pause();
      t = end;
    } else if (t != ed.currentTime()) {
      ed.setTimeFromEngine(t);
    }
    seenSeek_ = ed.seekToken();
  } else {
    if (wasPlaying_) {
      audio_.stop();
      wasPlaying_ = false;
    }
    seenSeek_ = ed.seekToken();
  }
  if (!playing && !dirty_.load()) return;
  dirty_ = false;
  compose(t, playing && ed.playing());
}

void PreviewEngine::compose(double t, bool playing) {
  const Editor& ed = editor();
  const ProjectData& project = ed.project();
  const TimelineLayout& layout = ed.layout();
  const double projectSpeed = project.settings.speed;
  const FramePlan plan = planFrame(project, layout, [&ed](const QString& id) { return ed.asset(id); }, t);

  // 要るフレームを用意する（写真はプレビュー用の画像、動画はデコーダーから。再生中は、動画全体の再生速度も掛けた速さで先読みする）
  std::set<QString> keep;
  std::map<QString, QImage> frames;
  std::map<QString, double> framePts;  // 使ったフレームの素材の時刻（オブジェクトマスクを、同じフレームのものにする）
  bool loading = false;  // 動画のフレームをまだ読み込んでいる
  exact_ = true;
  const ClipLayout* cur = findClipAt(layout, t);
  const QString mainKey = cur ? QStringLiteral("clip:") + cur->clip.id : QString();
  for (const FrameNeed& n : plan.needs) {
    if (n.isImage) {
      QImage img = media().previewImage(n.assetId);
      if (img.isNull()) {
        if (n.key == mainKey) exact_ = false;
        continue;
      }
      frames[n.key] = std::move(img);
      framePts[n.key] = 0;
      continue;
    }
    keep.insert(n.key);
    VideoSource* src = source(n.key, n.path);
    src->setTarget(n.srcT, playing, n.rate * projectSpeed);
    bool ex = true;
    double pts = n.srcT;
    QImage f = src->frameAt(n.srcT, &ex, &pts);
    exact_ = exact_ && ex;
    if (f.isNull() && !src->failed()) loading = true;
    if (!f.isNull()) {
      frames[n.key] = std::move(f);
      framePts[n.key] = pts;
    }
  }
  // もうすぐ使う動画は、最初のフレームを読んでおく
  for (const FrameNeed& n : plan.preload) {
    keep.insert(n.key);
    source(n.key, n.path)->setTarget(n.srcT, false, n.rate * projectSpeed);
  }
  // ループ再生で、もうすぐ始めに戻るとき: 範囲の始めのフレームも読んでおく（今使っているデコーダーは動かせないので、
  // そのときは "loop:" の付いた別のデコーダーで読み、戻るときに入れ替える）
  if (playing && ed.loopPlayback()) {
    const Editor::TimeRange loop = ed.loopRange();
    const double wrapAt = t < loop.end ? std::min(loop.end, ed.playEnd()) : ed.playEnd();
    if (loop.end - loop.start >= kMinLoopLength && wrapAt - t < kLoopPreloadSeconds) {
      const FramePlan lp = planFrame(project, layout, [&ed](const QString& id) { return ed.asset(id); }, loop.start);
      for (const FrameNeed& n : lp.needs) {
        if (n.isImage) continue;
        const QString key = keep.count(n.key) ? QStringLiteral("loop:") + n.key : n.key;
        keep.insert(key);
        source(key, n.path)->setTarget(n.srcT, false, n.rate * projectSpeed);
      }
    }
  }
  // 使っていないデコーダーは少しだけ残しておく（前のクリップへ戻ったときにすぐ表示できるように）
  idle_.erase(std::remove_if(idle_.begin(), idle_.end(), [&](const QString& key) { return keep.count(key) || !sources_.count(key); }),
              idle_.end());
  for (const auto& [key, src] : sources_) {
    if (!keep.count(key) && std::find(idle_.begin(), idle_.end(), key) == idle_.end()) idle_.push_front(key);
  }
  while (idle_.size() > kIdleSources) {
    auto it = sources_.find(idle_.back());
    if (it != sources_.end()) {
      // 終了待ちで画面が止まらないよう、別スレッドで片付ける
      std::thread([p = std::move(it->second)]() mutable { p.reset(); }).detach();
      sources_.erase(it);
    }
    idle_.pop_back();
  }

  // 読み込み中は直前の画面を出したままにする（黒い画面がちらつかないように）
  if (loading && !frame_.isNull()) return;
  // 再生中にシークした・ループで戻った直後は、フレームがその時刻のものになるまで待つ（少し待ってもそろわなければ出す）
  if (awaitExact_) {
    if (!playing || exact_ || awaitTimer_.elapsed() > 400 || frame_.isNull()) awaitExact_ = false;
    else return;
  }

  // スポイト・被写体を選ぶ用に、この画面に使う元のフレームと、その素材の時刻を覚えておく
  raw_ = frames;
  rawPts_ = framePts;

  ComposeOptions opt;
  opt.canvas = canvas_;
  opt.chromaPick = ed.chromaPick();
  // オブジェクトマスク（被写体を選んでいる映像は、マスクを外して元の映像を出す）
  opt.masks = resolveFrameMasks(project, plan, [&ed](const QString& id) { return ed.asset(id); }, framePts, ed.maskPick());
  opt.textOverrideId = textOverrideId_;
  opt.textOverride = textOverride_;
  opt.editingId = !playing && ed.selection().type == Selection::Text ? ed.selection().id : QString();
  frame_ = composeFrame(project, layout, t, frames, opt, &cache_);
  emit frameReady();
}

}  // namespace kv
