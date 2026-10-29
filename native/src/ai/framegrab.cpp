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
#include "core/timeline.h"
#include "media/ffutil.h"
#include "media/mediamanager.h"
#include "render/compositor.h"
#include "render/maskframes.h"

namespace kv::ai {

namespace {

/** 1 枚分の、写し取った状態（別スレッドで使うので、編集データは参照せずコピーで持つ） */
struct Job {
  double t = 0;
  /** この画面に要るフレーム（オブジェクトマスクを選ぶのに使う） */
  FramePlan plan;
  /** デコードして用意する動画のフレーム */
  std::vector<FrameNeed> videos;
  /** 写真のフレーム（UI のスレッドで、プレビュー用の画像を写しておく） */
  std::map<QString, QImage> stills;
};

QSize canvasFor(Aspect aspect, int longSide) {
  const QSize d = designSize(aspect);
  const double k = double(longSide) / std::max(d.width(), d.height());
  auto even = [](double v) { return std::max(2, int(std::lround(v / 2)) * 2); };
  return {even(d.width() * k), even(d.height() * k)};
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
  // 何がどこにあるか（要るフレームの一覧）は、プレビューと同じ決め方。写真は、ここでプレビュー用の画像を写しておく
  const AssetLookup lookup = [&ed](const QString& id) { return ed.asset(id); };
  std::vector<Job> jobs;
  for (double t : times) {
    Job job;
    job.t = std::clamp(t, 0.0, std::max(0.0, end));
    job.plan = planFrame(project, layout, lookup, job.t);
    const FramePlan& plan = job.plan;
    for (const FrameNeed& n : plan.needs) {
      if (!n.isImage) {
        job.videos.push_back(n);
        continue;
      }
      const QImage img = media().previewImage(n.assetId);
      if (!img.isNull()) job.stills[n.key] = img;
    }
    jobs.push_back(std::move(job));
  }

  // オブジェクトマスクを使う映像（マスクは、読んだコマの時刻で選ぶ。素材の情報はここで写しておく）
  std::map<QString, Asset> assets;
  for (const Job& job : jobs) {
    for (const FrameNeed& n : job.plan.needs) {
      if (const Asset* a = ed.asset(n.assetId)) assets[n.assetId] = *a;
    }
  }
  QPointer<QObject> guard(context);
  QThreadPool::globalInstance()->start([project, layout, jobs = std::move(jobs), assets, size, quality, guard, done = std::move(done)] {
    std::vector<FrameShot> shots;
    std::map<QString, std::unique_ptr<Reader>> readers;
    const AssetLookup lookup = [&assets](const QString& id) -> const Asset* {
      const auto it = assets.find(id);
      return it == assets.end() ? nullptr : &it->second;
    };
    for (const Job& job : jobs) {
      // 合成はプレビューと同じ計算（文字の画像などは毎回作る）
      ComposeOptions opt;
      opt.canvas = size;
      std::map<QString, QImage> frames = job.stills;
      std::map<QString, double> framePts;
      for (const FrameNeed& n : job.videos) {
        // デコーダーは映像（クリップ）ごとに持つ（トランジションの間は、同じ素材の前後のクリップを同じ時刻に読むので、
        // 素材ごとに 1 つだと行ったり来たりして遅く、別の位置のコマが混ざりやすい）
        auto& r = readers[n.key + QLatin1Char('|') + n.path];
        if (!r) {
          r = std::make_unique<Reader>();
          r->open = r->dec.open(n.path);
          if (!r->open) r->dec.close();
        }
        const QImage f = videoFrame(*r, n.srcT);
        if (!f.isNull()) {
          frames[n.key] = f;
          framePts[n.key] = r->pts;
        }
      }
      opt.masks = resolveFrameMasks(project, job.plan, lookup, framePts);
      const QImage img = composeFrame(project, layout, job.t, frames, opt, nullptr);
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
