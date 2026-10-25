#include "ai/framegrab.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

#include "core/editor.h"
#include "core/filters.h"
#include "core/timeline.h"
#include "media/ffutil.h"
#include "media/mediamanager.h"
#include "render/imageops.h"
#include "render/textrender.h"

namespace kv::ai {

namespace {

/** 1 枚分の、写し取った状態（別スレッドで使うので、編集データは参照せずコピーで持つ） */
struct Job {
  double t = 0;
  bool hasClip = false;
  bool isImage = false;
  QString path;
  QImage still;  // 写真のクリップ
  double srcT = 0;
  FitMode fit = FitMode::Contain;
  ColorAdjust color;
  bool plain = true;
  LayerState layer;
  double fade = 0;  // 黒を重ねる割合
  std::vector<TextItem> texts;
};

QSize canvasFor(Aspect aspect, int longSide) {
  const QSize d = designSize(aspect);
  const double k = double(longSide) / std::max(d.width(), d.height());
  auto even = [](double v) { return std::max(2, int(std::lround(v / 2)) * 2); };
  return {even(d.width() * k), even(d.height() * k)};
}

QImage compose(const Job& job, const QImage& frame, QSize size, QSize design) {
  QImage canvas;
  if (job.hasClip && !frame.isNull()) {
    canvas = composeClipLayer(frame, job.fit, size);
    applyColor(canvas, job.color);
    if (!job.plain) canvas = transformLayer(canvas, job.layer);
    if (job.fade > 0.001) {
      QPainter p(&canvas);
      p.fillRect(canvas.rect(), QColor(0, 0, 0, int(std::lround(job.fade * 255))));
    }
  } else {
    canvas = QImage(size, QImage::Format_RGB32);
    canvas.fill(Qt::black);
  }
  QPainter p(&canvas);
  for (const TextItem& item : job.texts) {
    const bool plain = layerIsPlain(item);
    const LayerState ls = layerStateAt(item, job.t - item.start);
    TextItem drawItem = item;
    drawItem.letterSpacing = ls.letterSpacing;  // 字間のキーフレームも含めた、この時間の字間
    if (!plain) {
      drawItem.x = 0.5;
      drawItem.y = 0.5;
    }
    const TextSprite s = renderTextSprite(drawItem, design, size);
    const AnimState st = textAnimState(item, job.t);
    if (s.image.isNull() || st.alpha <= 0.001) continue;
    const QPointF offset(0, st.offsetY * size.height());
    if (plain) {
      p.setOpacity(st.alpha);
      p.drawImage(s.pos + offset, s.image);
      continue;
    }
    p.save();
    p.setOpacity(st.alpha * ls.opacity);
    drawLayer(p, s.image, s.pos, ls, offset, size);
    p.restore();
  }
  p.end();
  return canvas;
}

/** 動画 1 つ分のデコーダーと、直前に作ったコマ */
struct Reader {
  VideoDecoder dec;
  bool open = false;
  bool has = false;
  double pts = 0;
  QImage image;
};

/** 素材の時刻 t のコマ（読めなければ空）。少し先の時刻なら、キーフレームへ戻らずに読み進める（動くプレビューのコマを続けて作るとき） */
QImage videoFrame(Reader& r, double t) {
  if (!r.open) return {};
  const double tol = r.dec.frameDuration() * 0.5;
  // 直前のコマのままでよいとき（遅くしたクリップなど）
  if (r.has && t >= r.pts - tol && t < r.pts + tol) return r.image;
  auto decodeTo = [&r, t, tol] {
    bool got = false;
    for (int i = 0; i < 2000 && r.dec.decodeNext(); ++i) {
      got = true;
      if (r.dec.framePts() >= t - tol) break;
    }
    return got;
  };
  const bool forward = r.has && t > r.pts && t - r.pts < 2.0;
  if (!forward) r.dec.seek(t);
  bool got = decodeTo();
  // 読み進めて終わりに着いたときは、移動してから読み直す（終わりの近くのコマ）
  if (!got && forward) {
    r.dec.seek(t);
    got = decodeTo();
  }
  if (!got) return r.has ? r.image : QImage();
  r.has = true;
  r.pts = r.dec.framePts();
  r.image = r.dec.frameImage(1280, 1280);
  return r.image;
}

void grabImpl(const ProjectData& project, const std::vector<double>& times, int longSide, int quality, QObject* context,
              std::function<void(std::vector<FrameShot>)> done);

}  // namespace

void grabFrames(const std::vector<double>& times, int longSide, QObject* context, std::function<void(std::vector<FrameShot>)> done) {
  grabImpl(editor().project(), times, longSide, 80, context, std::move(done));
}

void grabFrames(const ProjectData& project, const std::vector<double>& times, int longSide, QObject* context,
                std::function<void(std::vector<FrameShot>)> done) {
  grabImpl(project, times, longSide, 80, context, std::move(done));
}

void grabMotionFrames(const ProjectData& project, const std::vector<double>& times, int longSide, QObject* context,
                      std::function<void(std::vector<FrameShot>)> done) {
  grabImpl(project, times, longSide, 72, context, std::move(done));
}

namespace {

void grabImpl(const ProjectData& project, const std::vector<double>& times, int longSide, int quality, QObject* context,
              std::function<void(std::vector<FrameShot>)> done) {
  const Editor& ed = editor();
  const TimelineLayout layout = layoutClips(project.clips, project.settings.fps);
  // Editor::contentEnd と同じ（テキスト・オーディオが動画より後ろまであれば、そこまで）
  double end = layout.total;
  for (const auto& t : project.texts) end = std::max(end, t.start + t.duration);
  for (const auto& a : project.audios) end = std::max(end, a.start + audioDuration(a));
  const QSize size = canvasFor(project.settings.aspect, longSide);
  const QSize design = designSize(project.settings.aspect);
  std::vector<Job> jobs;
  for (double t : times) {
    Job job;
    job.t = std::clamp(t, 0.0, std::max(0.0, end));
    if (const ClipLayout* cur = findClipAt(layout, job.t)) {
      const VideoClip& c = cur->clip;
      const Asset* a = ed.asset(c.assetId);
      const double local = std::clamp(job.t - cur->start, 0.0, cur->duration);
      if (a && a->status != AssetStatus::Missing && a->status != AssetStatus::Error) {
        job.hasClip = true;
        job.isImage = c.isImage;
        if (c.isImage) job.still = media().previewImage(c.assetId);
        else job.path = a->path;
        job.srcT = c.in + local * c.speed;
        job.fit = c.fit;
        job.color = c.color;
        job.plain = layerIsPlain(c);
        job.layer = layerStateAt(c, local);
        job.fade = 1 - fadeFactor(local, cur->duration, c.fadeIn, c.fadeOut);
      }
    }
    // 下の段の文字から描く（上の段の文字ほど手前。プレビュー・書き出しと同じ順）
    for (const size_t index : textPaintOrder(project.texts)) {
      const TextItem& item = project.texts[index];
      if (isTextVisible(item, job.t) && !item.text.isEmpty()) job.texts.push_back(item);
    }
    jobs.push_back(std::move(job));
  }

  QPointer<QObject> guard(context);
  QThreadPool::globalInstance()->start([jobs = std::move(jobs), size, design, quality, guard, done = std::move(done)] {
    std::vector<FrameShot> shots;
    std::map<QString, std::unique_ptr<Reader>> readers;
    for (const Job& job : jobs) {
      QImage frame = job.still;
      if (job.hasClip && !job.isImage) {
        auto& r = readers[job.path];
        if (!r) {
          r = std::make_unique<Reader>();
          r->open = r->dec.open(job.path);
          if (!r->open) r->dec.close();
        }
        frame = videoFrame(*r, job.srcT);
      }
      const QImage img = compose(job, frame, size, design);
      FrameShot shot;
      shot.t = job.t;
      shot.size = img.size();
      QBuffer buf(&shot.jpeg);
      buf.open(QIODevice::WriteOnly);
      img.save(&buf, "JPG", quality);
      shots.push_back(std::move(shot));
    }
    // 結果はアプリ本体（最後まで残る）を通して UI スレッドへ渡し、そこで context が残っているかを確かめる
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return;
    QMetaObject::invokeMethod(
        app,
        [guard, done, shots = std::move(shots)]() mutable {
          if (guard) done(std::move(shots));
        },
        Qt::QueuedConnection);
  });
}

}  // namespace

}  // namespace kv::ai
