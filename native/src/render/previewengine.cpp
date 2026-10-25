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
  // フォントを読み込んだ・外したときは、文字の画像を描き直す（同じファミリー名でも、描くフォントが変わる）
  connect(&fontLibrary(), &FontLibrary::changed, this, [this] {
    sprites_.clear();
    dirty_ = true;
  });
  // スポイトで色を選んでいる間は、その映像のクロマキーを外して出す
  connect(&ed, &Editor::chromaPickChanged, this, markDirty);
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
  const QImage& src = it->second;
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
  s.speed *= S;
  for (auto& p : s.points) p.t /= S;
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
  // 重ねるトラックの動画の音（音量・ミュート・フェード・音量キーフレームは、ビデオクリップと同じ）
  for (const auto& o : ed.project().overlays) {
    const Asset* a = ed.asset(o.assetId);
    if (o.isImage || !a || !a->hasAudio || a->status == AssetStatus::Missing || o.muted || (o.volumePoints.empty() && o.volume <= 0.0001)) continue;
    MixSource src{QStringLiteral("overlay:") + o.id, a->path, o.start, overlayDuration(o, ed.project().settings.fps), o.in, o.speed, o.volume,
                  o.fadeIn, o.fadeOut};
    src.points = o.volumePoints;
    snap->sources.push_back(src);
  }
  for (const auto& au : ed.project().audios) {
    const Asset* a = ed.asset(au.assetId);
    if (!a || a->status == AssetStatus::Missing || (au.volumePoints.empty() && au.volume <= 0.0001)) continue;
    MixSource src{QStringLiteral("audio:") + au.id, a->path, au.start, audioDuration(au), au.in, 1.0, au.volume, au.fadeIn, au.fadeOut};
    src.points = au.volumePoints;
    snap->sources.push_back(src);
  }
  const double S = ed.project().settings.speed;
  for (auto& s : snap->sources) warpSource(s, S);
  snap->end = ed.playEnd() / S;
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
    // 動画全体の再生速度 S: タイムラインの時間は、聞こえる時間（音の時計）の S 倍で進む
    const double S = ed.project().settings.speed;
    if (!wasPlaying_ || ed.seekToken() != seenSeek_ || std::abs(S - playSpeed_) > 1e-9) {
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

QImage PreviewEngine::processedFrame(const QString& key, const QImage& src, const VideoClip& c, bool withColor, bool onBlack) {
  const bool color = withColor && !isNeutralColor(c.color);
  // スポイトで色を選んでいる映像は、クロマキーを外して元の色で出す（透明にした所も、元の背景の色を見て選べるように）
  const bool keyOn = c.chroma.enabled && c.id != editor().chromaPick();
  if (src.isNull() || (!keyOn && !color)) return src;
  const ColorAdjust& a = c.color;
  const ChromaKey& k = c.chroma;
  const QString params = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
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
  auto it = processed_.find(key);
  if (it != processed_.end() && it->second.source == src.cacheKey() && it->second.params == params) return it->second.image;
  QImage out = keyOn ? chromaKeyImage(src, k) : src.convertToFormat(QImage::Format_ARGB32);
  if (color) applyColor(out, a);
  if (keyOn && onBlack) {
    // 透明にした所を黒で埋める（ぼかし背景も、黒にしたこの映像から作るようにする。書き出しと同じ）
    QImage flat(out.size(), QImage::Format_RGB32);
    flat.fill(Qt::black);
    QPainter fp(&flat);
    fp.drawImage(0, 0, out);
    fp.end();
    out = flat;
  }
  processed_[key] = {src.cacheKey(), params, out};
  return out;
}

void PreviewEngine::compose(double t, bool playing) {
  const Editor& ed = editor();
  const TimelineLayout& layout = ed.layout();
  const ClipLayout* cur = findClipAt(layout, t);
  const ClipLayout* next = cur && cur->index + 1 < int(layout.items.size()) ? &layout.items[size_t(cur->index + 1)] : nullptr;
  const int fps = ed.project().settings.fps;
  const double projectSpeed = ed.project().settings.speed;

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
        // 再生中は、動画全体の再生速度も掛けた速さで先読みする
        src->setTarget(srcT, playing, c.speed * ed.project().settings.speed);
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
        source(key, na->path)->setTarget(next->clip.in, false, next->clip.speed * ed.project().settings.speed);
      }
    }
  }
  // 重ねるトラック: 今の時間に出ているもののフレーム（下の段から順に描く）。もうすぐ始まる動画は先読みしておく
  struct OverlayFrame {
    const OverlayItem* item;
    QImage frame;
    double local;
  };
  std::vector<OverlayFrame> overlayFrames;
  {
    std::vector<const OverlayItem*> order;
    for (const auto& o : ed.project().overlays) order.push_back(&o);
    std::stable_sort(order.begin(), order.end(), [](const OverlayItem* a, const OverlayItem* b) {
      if (a->track != b->track) return a->track < b->track;
      return a->start < b->start;
    });
    for (const OverlayItem* o : order) {
      const Asset* a = ed.asset(o->assetId);
      if (!a || a->status == AssetStatus::Missing || a->status == AssetStatus::Error) continue;
      const double dur = overlayDuration(*o, fps);
      const bool visible = t >= o->start - 1e-9 && t < o->start + dur - 1e-9;
      const QString key = QStringLiteral("overlay:") + o->id;
      if (!visible) {
        if (!o->isImage && o->start > t && o->start - t < 3) {
          keep.insert(key);
          source(key, a->path)->setTarget(o->in, false, o->speed * projectSpeed);
        }
        continue;
      }
      const double local = std::clamp(t - o->start, 0.0, dur);
      QImage f;
      if (o->isImage) {
        f = media().previewImage(o->assetId);
      } else {
        keep.insert(key);
        VideoSource* src = source(key, a->path);
        const double srcT = o->in + local * o->speed;
        src->setTarget(srcT, playing, o->speed * projectSpeed);
        bool ex = true;
        f = src->frameAt(srcT, &ex);
        exact_ = exact_ && ex;
        if (f.isNull() && !src->failed()) loading = true;
      }
      if (!f.isNull()) overlayFrames.push_back({o, f, local});
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

  // スポイト用に、この画面に使う元のフレームを覚えておく
  raw_.clear();
  if (cur && !clipFrame.isNull()) raw_[QStringLiteral("clip:") + cur->clip.id] = clipFrame;
  for (const OverlayFrame& of : overlayFrames) raw_[QStringLiteral("overlay:") + of.item->id] = of.frame;

  QImage canvas;
  if (cur && !clipFrame.isNull()) {
    // クロマキーで透明にした所は黒（書き出しと同じく、黒い背景の上に置いてから色を調整する）。切り抜いた所も黒
    const QImage keyed = processedFrame(QStringLiteral("clip:") + cur->clip.id, clipFrame, cur->clip, false, true);
    canvas = composeClipLayer(keyed, cur->clip.fit, canvas_, cropSourceRect(cur->clip, QSizeF(1, 1)));
    applyColor(canvas, cur->clip.color);
    const double local = std::clamp(t - cur->start, 0.0, cur->duration);
    // 位置・拡大率・回転・伸縮・不透明度（キーフレームがあれば、この時間の値）。動かしていなければ何もしない
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

  // 重ねるトラック（クロマキー・色の調整は素材だけに掛け、余白は透明。フェードは透明度で）
  if (!overlayFrames.empty()) {
    QPainter op(&canvas);
    std::set<QString> liveOverlays;
    for (const OverlayFrame& of : overlayFrames) {
      const OverlayItem& o = *of.item;
      const QString key = QStringLiteral("overlay:") + o.id;
      liveOverlays.insert(key);
      const LayerState ls = layerStateAt(o, of.local);
      const double alpha = std::clamp(ls.opacity, 0.0, 1.0) * fadeFactor(of.local, overlayDuration(o, fps), o.fadeIn, o.fadeOut);
      if (alpha <= 0.001) continue;
      const QImage media = processedFrame(key, of.frame, o, true, false);
      op.setOpacity(alpha);
      drawClipMedia(op, media, cropSourceRect(o, QSizeF(1, 1)), ls, canvas_);
    }
    op.end();
  }
  // 使わなくなった加工済みのフレームは手放す
  for (auto it = processed_.begin(); it != processed_.end();) {
    bool used = cur && it->first == QStringLiteral("clip:") + cur->clip.id;
    for (const OverlayFrame& of : overlayFrames) used = used || it->first == QStringLiteral("overlay:") + of.item->id;
    if (used) ++it;
    else it = processed_.erase(it);
  }

  // テキスト
  const QSize design = designSize(ed.project().settings.aspect);
  const QString editingId = !playing && ed.selection().type == Selection::Text ? ed.selection().id : QString();
  QPainter p(&canvas);
  std::set<QString> live;
  // 下の段の文字から描く（タイムラインの上の段の文字ほど手前に重なる。書き出しと同じ順。選んでいるかどうかでは変えない）
  const auto& texts = ed.project().texts;
  for (const size_t index : textPaintOrder(texts)) {
    const TextItem& item = texts[index];
    // 編集中の文字は、確定前の内容（変換中の文字を含む）で描く
    const QString& shownText = item.id == textOverrideId_ ? textOverride_ : item.text;
    if (!isTextVisible(item, t) || shownText.isEmpty()) continue;
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
