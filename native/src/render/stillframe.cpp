#include "render/stillframe.h"

#include <QCoreApplication>
#include <QImageReader>
#include <QPointer>
#include <QThreadPool>
#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include "core/editor.h"
#include "core/timeline.h"
#include "media/ffutil.h"
#include "media/mediamanager.h"
#include "render/compositor.h"
#include "render/maskframes.h"

namespace kv {

namespace {

/** 静止画に使う動画のフレームの、大きさの上限（8K まではそのまま） */
constexpr int kMaxSourceSide = 8192;

/**
 * 素材の時刻 t のフレーム（読めなければ空）。t より前のキーフレームへ移動して、t のフレームまで読み進める。
 * 選び方はプレビュー（VideoSource::frameAt）と同じ: 時刻が t + 1/4 フレーム以下のうち一番後ろのフレーム
 * （t が素材の最初のフレームより前なら最初のフレーム、終わりより後ろなら最後のフレーム）
 */
QImage readFrame(VideoDecoder& dec, double t, double* framePts = nullptr) {
  dec.seek(t);
  const double fd = dec.frameDuration();
  const double limit = t + fd * 0.25;
  QImage best;
  double bestPts = t;
  bool decoded = false;
  bool lastConverted = true;  // 最後にデコードしたフレームを、画像にしたか
  for (int i = 0; i < 4000 && dec.decodeNext(); ++i) {
    decoded = true;
    const double pts = dec.framePts();
    if (pts > limit + 1e-9) {
      if (best.isNull()) {
        best = dec.frameImage(kMaxSourceSide, kMaxSourceSide);  // 素材の最初のフレームより前
        bestPts = pts;
      }
      if (framePts) *framePts = bestPts;
      return best;
    }
    // 次のフレームが t を越えそうなら、これが答えになる（画像にするのは、最後の数フレームだけ）
    if (pts >= limit - fd * 1.5) {
      best = dec.frameImage(kMaxSourceSide, kMaxSourceSide);
      bestPts = pts;
      lastConverted = true;
    } else {
      lastConverted = false;
    }
  }
  // 終わりまで来た: 最後のフレーム
  if (decoded && !lastConverted) {
    best = dec.frameImage(kMaxSourceSide, kMaxSourceSide);
    bestPts = dec.framePts();
  }
  if (framePts) *framePts = bestPts;
  return best;
}

/** 結果を UI のスレッドへ渡す（context が残っているときだけ done を呼ぶ） */
void deliver(const QPointer<QObject>& guard, const std::function<void(QImage)>& done, QImage img) {
  QCoreApplication* app = QCoreApplication::instance();
  if (!app) return;
  QMetaObject::invokeMethod(
      app,
      [guard, done, img = std::move(img)]() mutable {
        if (guard) done(std::move(img));
      },
      Qt::QueuedConnection);
}

}  // namespace

void renderStillFrame(double t, QSize size, QObject* context, std::function<void(QImage)> done) {
  const Editor& ed = editor();
  const ProjectData project = ed.project();
  const TimelineLayout layout = ed.layout();
  const AssetLookup lookup = [&ed](const QString& id) { return ed.asset(id); };
  const FramePlan plan = planFrame(project, layout, lookup, t);
  // 写真は、読み込み用に整えた画像（大きいまま）。まだ無い・読めないときはプレビュー用の画像
  std::map<QString, QString> stillPaths;
  std::map<QString, QImage> stills;
  std::vector<FrameNeed> videos;
  for (const FrameNeed& n : plan.needs) {
    if (!n.isImage) {
      videos.push_back(n);
      continue;
    }
    const Asset* a = ed.asset(n.assetId);
    if (a && !a->imagePath.isEmpty()) stillPaths[n.key] = a->imagePath;
    if (const QImage img = media().previewImage(n.assetId); !img.isNull()) stills[n.key] = img;
  }
  // オブジェクトマスクを使う映像（マスクは、読んだフレームの時刻で選ぶ。素材の情報はここで写しておく）
  std::map<QString, Asset> assets;
  for (const FrameNeed& n : plan.needs) {
    if (const Asset* a = ed.asset(n.assetId)) assets[n.assetId] = *a;
  }
  QPointer<QObject> guard(context);
  QThreadPool::globalInstance()->start([project, layout, plan, assets, t, size, videos, stillPaths, stills, guard, done = std::move(done)]() mutable {
    std::map<QString, QImage> frames = stills;
    std::map<QString, double> framePts;
    for (const auto& [key, path] : stillPaths) {
      QImageReader reader(path);
      QImage img = reader.read();
      // 写真の透明な所は、読み込んだときに黒にしてある（プレビュー・書き出しと同じ）
      if (!img.isNull()) frames[key] = img.convertToFormat(QImage::Format_RGB32);
    }
    // 動画: 映像ごとにデコーダーを開いて、その時刻のフレームを読む（トランジションの間は、前後のクリップの 2 つ）
    for (const FrameNeed& n : videos) {
      VideoDecoder dec;
      if (!dec.open(n.path)) continue;
      double pts = n.srcT;
      const QImage f = readFrame(dec, n.srcT, &pts);
      if (!f.isNull()) {
        frames[n.key] = f;
        framePts[n.key] = pts;
      }
    }
    ComposeOptions opt;
    opt.canvas = size;
    opt.masks = resolveFrameMasks(project, plan, [&assets](const QString& id) -> const Asset* {
      const auto it = assets.find(id);
      return it == assets.end() ? nullptr : &it->second;
    }, framePts);
    QImage img = composeFrame(project, layout, t, frames, opt, nullptr);
    deliver(guard, done, std::move(img));
  });
}

void decodeSourceFrame(const QString& path, double srcT, QObject* context, std::function<void(QImage)> done) {
  decodeSourceFrameAt(path, srcT, context, [done = std::move(done)](QImage img, double) { done(std::move(img)); });
}

void decodeSourceFrameAt(const QString& path, double srcT, QObject* context, std::function<void(QImage, double)> done) {
  QPointer<QObject> guard(context);
  QThreadPool::globalInstance()->start([path, srcT, guard, done = std::move(done)]() mutable {
    VideoDecoder dec;
    QImage img;
    double pts = srcT;
    if (dec.open(path)) img = readFrame(dec, std::max(0.0, srcT), &pts);
    if (!img.isNull()) img = img.convertToFormat(QImage::Format_RGB32);
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return;
    QMetaObject::invokeMethod(
        app,
        [guard, done, img = std::move(img), pts]() mutable {
          if (guard) done(std::move(img), pts);
        },
        Qt::QueuedConnection);
  });
}

}  // namespace kv
