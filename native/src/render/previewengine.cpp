#include "render/previewengine.h"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <set>
#include <thread>

#include "core/editor.h"
#include "core/filters.h"
#include "core/timeline.h"
#include "media/mediamanager.h"
#include "media/videosource.h"
#include "render/imageops.h"

namespace kv {

static constexpr int kSourceMax = 1280;
static constexpr size_t kIdleSources = 2;

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
  sprites_.clear();
  dirty_ = true;
}

void PreviewEngine::updateAudioSnapshot() {
  const Editor& ed = editor();
  auto snap = std::make_shared<MixSnapshot>();
  for (const auto& l : ed.layout().items) {
    const VideoClip& c = l.clip;
    const Asset* a = ed.asset(c.assetId);
    if (c.isImage || !a || !a->hasAudio || a->status == AssetStatus::Missing || c.muted || (c.volumePoints.empty() && c.volume <= 0.0001)) continue;
    MixSource src{QStringLiteral("clip:") + c.id, a->path, l.start, l.duration, c.in, c.speed, c.volume, c.fadeIn, c.fadeOut};
    src.points = c.volumePoints;  // 音量キーフレーム（タイムライン上の、クリップの先頭からの秒数）
    snap->sources.push_back(src);
  }
  for (const auto& au : ed.project().audios) {
    const Asset* a = ed.asset(au.assetId);
    if (!a || a->status == AssetStatus::Missing || (au.volumePoints.empty() && au.volume <= 0.0001)) continue;
    MixSource src{QStringLiteral("audio:") + au.id, a->path, au.start, audioDuration(au), au.in, 1.0, au.volume, au.fadeIn, au.fadeOut};
    src.points = au.volumePoints;
    snap->sources.push_back(src);
  }
  snap->end = ed.playEnd();
  audio_.setSnapshot(snap);
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
    if (!wasPlaying_ || ed.seekToken() != seenSeek_) {
      wasPlaying_ = true;
      seenSeek_ = ed.seekToken();
      playStart_ = t;
      audio_.start(t);
      wall_.restart();
    }
    if (audio_.ok()) t = audio_.started() ? audio_.clock() : playStart_;
    else t = playStart_ + wall_.elapsed() / 1000.0;
    const double end = ed.playEnd();
    if (t >= end) {
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
  const TimelineLayout& layout = ed.layout();
  const ClipLayout* cur = findClipAt(layout, t);
  const ClipLayout* next = cur && cur->index + 1 < int(layout.items.size()) ? &layout.items[size_t(cur->index + 1)] : nullptr;

  std::set<QString> keep;
  QImage clipFrame;
  bool loading = false;  // 動画のフレームをまだ読み込んでいる
  exact_ = true;
  if (cur) {
    const VideoClip& c = cur->clip;
    const Asset* a = ed.asset(c.assetId);
    const double local = std::clamp(t - cur->start, 0.0, cur->duration);
    if (a && a->status != AssetStatus::Missing) {
      if (c.isImage) {
        clipFrame = media().previewImage(c.assetId);
        exact_ = !clipFrame.isNull();
      } else {
        const QString key = QStringLiteral("clip:") + c.id;
        keep.insert(key);
        VideoSource* src = source(key, a->path);
        const double srcT = c.in + local * c.speed;
        src->setTarget(srcT, playing, c.speed);
        clipFrame = src->frameAt(srcT, &exact_);
        loading = clipFrame.isNull() && !src->failed();
      }
    }
    // 次のクリップを先読みしておく
    if (next && !next->clip.isImage && next->start - t < 3) {
      const Asset* na = ed.asset(next->clip.assetId);
      if (na && na->status != AssetStatus::Missing) {
        const QString key = QStringLiteral("clip:") + next->clip.id;
        keep.insert(key);
        source(key, na->path)->setTarget(next->clip.in, false, next->clip.speed);
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

  QImage canvas;
  if (cur && !clipFrame.isNull()) {
    canvas = composeClipLayer(clipFrame, cur->clip.fit, canvas_);
    applyColor(canvas, cur->clip.color);
    const double local = std::clamp(t - cur->start, 0.0, cur->duration);
    // 位置・拡大率・回転・不透明度（キーフレームがあれば、この時間の値）。動かしていなければ何もしない
    if (!layerIsPlain(cur->clip)) canvas = transformLayer(canvas, layerStateAt(cur->clip, local));
    const double fade = 1 - fadeFactor(local, cur->duration, cur->clip.fadeIn, cur->clip.fadeOut);
    if (fade > 0.001) {
      QPainter p(&canvas);
      p.fillRect(canvas.rect(), QColor(0, 0, 0, int(std::lround(fade * 255))));
    }
  } else {
    canvas = QImage(canvas_, QImage::Format_RGB32);
    canvas.fill(Qt::black);
  }

  // テキスト
  const QSize design = designSize(ed.project().settings.aspect);
  const QString editingId = !playing && ed.selection().type == Selection::Text ? ed.selection().id : QString();
  QPainter p(&canvas);
  std::set<QString> live;
  for (const auto& item : ed.project().texts) {
    if (!isTextVisible(item, t) || item.text.isEmpty()) continue;
    live.insert(item.id);
    // 動かしていない文字は、位置まで描いた画像をそのまま重ねる。動かす文字は、画面の中央に置いた画像を
    // 位置・拡大率・回転・不透明度で動かす（動かしても描き直さない）
    const bool plain = layerIsPlain(item);
    TextItem drawItem = item;
    if (!plain) {
      drawItem.x = 0.5;
      drawItem.y = 0.5;
    }
    const QString key = textSpriteKey(drawItem, canvas_);
    auto it = sprites_.find(item.id);
    if (it == sprites_.end() || it->second.key != key) {
      sprites_[item.id] = renderTextSprite(drawItem, design, canvas_);
      it = sprites_.find(item.id);
    }
    const AnimState st = previewTextState(item, t, editingId);
    if (st.alpha <= 0.001 || it->second.image.isNull()) continue;
    if (plain) {
      p.setOpacity(st.alpha);
      p.drawImage(it->second.pos + QPointF(0, st.offsetY * canvas_.height()), it->second.image);
      continue;
    }
    const LayerState ls = layerStateAt(item, t - item.start);
    if (st.alpha * ls.opacity <= 0.001) continue;
    p.save();
    p.setOpacity(st.alpha * ls.opacity);
    drawLayer(p, it->second.image, it->second.pos, ls, QPointF(0, st.offsetY * canvas_.height()), canvas_);
    p.restore();
  }
  p.end();
  if (sprites_.size() > live.size() + 16) {
    for (auto it = sprites_.begin(); it != sprites_.end();) {
      if (live.count(it->first)) ++it;
      else it = sprites_.erase(it);
    }
  }
  frame_ = canvas;
  emit frameReady();
}

}  // namespace kv
