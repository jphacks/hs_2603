#include "mask/maskjob.h"

#include <QDateTime>
#include <QDir>
#include <QImageReader>
#include <algorithm>
#include <cmath>

#include "mask/masktrack.h"
#include "media/ffutil.h"

namespace kv::mask {

namespace {

std::vector<PromptPoint> pointsOf(const MaskPrompt& p) {
  std::vector<PromptPoint> out;
  for (const MaskPoint& q : p.points) out.push_back({q.x, q.y, q.positive});
  return out;
}

}  // namespace

TrackJob::TrackJob(TrackRequest request, QObject* parent) : QObject(parent), req_(std::move(request)) {}

TrackJob::~TrackJob() {
  abandon_ = true;
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

void TrackJob::start() {
  if (thread_.joinable()) return;
  thread_ = std::thread([this] { run(); });
}

void TrackJob::stop() {
  stop_ = true;
}

void TrackJob::publish(bool force) {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  std::shared_ptr<MaskTrack> snap;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!force && now - lastPublish_ < 350) return;
    lastPublish_ = now;
    snap = std::make_shared<MaskTrack>();
    snap->width = req_.width;
    snap->height = req_.height;
    snap->frameDuration = frameDur_;
    const double fd = frameDur_;
    // 片方だけを追うときは、もう片方（前に作った結果）を残す
    if (req_.base && !(req_.forward && req_.backward) && !req_.prompts.empty()) {
      const double t0 = req_.prompts.front().t;
      for (const MaskFrame& f : req_.base->frames) {
        const bool keep = req_.forward ? f.pts < t0 - fd * 0.5 : f.pts > t0 + fd * 0.5;
        if (keep) snap->frames.push_back(f);
      }
    }
    for (const auto& [pts, r] : results_) snap->frames.push_back({pts, r.first, r.second});
  }
  std::stable_sort(snap->frames.begin(), snap->frames.end(), [](const MaskFrame& a, const MaskFrame& b) { return a.pts < b.pts; });
  // 同じ時刻のフレームは、新しく作ったもの（後ろに入れたもの）を使う
  std::vector<MaskFrame> unique;
  for (const MaskFrame& f : snap->frames) {
    if (!unique.empty() && std::abs(unique.back().pts - f.pts) < 1e-6) unique.back() = f;
    else unique.push_back(f);
  }
  snap->frames = std::move(unique);
  MaskStore::instance().setLive(req_.maskId, snap);
  QMetaObject::invokeMethod(this, [this] { emit updated(); }, Qt::QueuedConnection);
}

void TrackJob::run() {
  auto fail = [this](const QString& error) {
    if (abandon_) return;
    QMetaObject::invokeMethod(this, [this, error] { emit finished(false, error, QString(), false); }, Qt::QueuedConnection);
  };
  if (req_.prompts.empty() || req_.prompts.front().points.empty()) return fail(QStringLiteral("被写体をクリックしてください"));
  QString error;
  EdgeTam* m = EdgeTam::shared(&error);
  if (!m) return fail(error);
  if (abandon_) return;
  auto add = [this](double pts, const Tracker::Result& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back({pts, {r.score, MaskTrack::pack(r.lowRes)}});
  };
  auto report = [this](double f) {
    fraction_ = std::clamp(f, 0.0, 1.0);
    const double v = fraction_.load();
    QMetaObject::invokeMethod(this, [this, v] { emit progress(v); }, Qt::QueuedConnection);
  };
  const MaskPrompt& first = req_.prompts.front();
  try {
    Tracker tracker(*m);
    if (req_.isImage) {
      // 写真: 1 枚だけ（追いかけない）
      QImageReader reader(req_.path);
      reader.setAutoTransform(true);
      const QImage img = reader.read();
      if (img.isNull()) return fail(QStringLiteral("写真を読み込めません"));
      add(0.0, tracker.prompt(0, m->encode(EdgeTam::pixelsFrom(img)), pointsOf(first), true, false));
      report(1);
    } else {
      VideoDecoder dec;
      if (!dec.open(req_.path, &error)) return fail(QStringLiteral("動画を読み込めません: %1").arg(error));
      const double fd = dec.frameDuration();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        frameDur_ = fd;
      }
      auto promptAt = [&](double pts) -> const MaskPrompt* {
        for (size_t i = 1; i < req_.prompts.size(); ++i) {
          if (std::abs(req_.prompts[i].t - pts) < fd * 0.5) return &req_.prompts[i];
        }
        return nullptr;
      };
      auto encode = [&](const QImage& img) { return m->encode(EdgeTam::pixelsFrom(img)); };
      const double t0 = first.t;
      const double lo = req_.backward ? std::min(req_.rangeStart, t0) : t0;
      const double hi = req_.forward ? std::max(req_.rangeEnd, t0) : t0;
      const double total = std::max(1.0, (hi - lo) / fd + 1);
      int done = 0;
      // 1. 最初にクリックしたフレーム
      dec.seek(t0);
      bool got = false;
      for (int i = 0; i < 20000 && dec.decodeNext(); ++i) {
        if (dec.framePts() >= t0 - fd * 0.5) {
          got = true;
          break;
        }
      }
      if (!got) return fail(QStringLiteral("クリックしたフレームを読めません"));
      const double startPts = dec.framePts();
      add(startPts, tracker.prompt(0, encode(dec.frameImageStretched(kModelSize, kModelSize)), pointsOf(first), true, false));
      report(++done / total);
      publish(true);
      // 2. 後ろへ（素材の時刻の順に読む）
      if (req_.forward) {
        int index = 1;
        while (!stop_ && dec.decodeNext()) {
          const double pts = dec.framePts();
          if (pts <= startPts + 1e-9) continue;
          if (pts > hi + fd * 0.5) break;
          const ImageFeatures f = encode(dec.frameImageStretched(kModelSize, kModelSize));
          const MaskPrompt* p = promptAt(pts);
          add(pts, p ? tracker.prompt(index, f, pointsOf(*p), false, false) : tracker.track(index, f, false));
          tracker.forget(index, false);
          ++index;
          report(++done / total);
          publish(false);
        }
      }
      // 3. 前へ（少しずつ読んで、後ろのフレームから順に）
      if (req_.backward && !stop_) {
        double upper = startPts;  // ここより前のフレームを読む
        int index = -1;
        double chunk = std::max(0.5, fd * 15);
        while (!stop_ && upper > lo + fd * 0.5) {
          const double from = std::max(0.0, upper - chunk);
          dec.seek(from);
          std::vector<std::pair<double, QImage>> frames;
          while (!stop_ && dec.decodeNext()) {
            const double pts = dec.framePts();
            if (pts >= upper - fd * 0.25) break;
            if (from > 0 && pts < from - fd * 0.5) continue;  // 次に読む所
            if (pts < lo - fd * 0.5) continue;
            frames.push_back({pts, dec.frameImageStretched(kModelSize, kModelSize)});
          }
          if (frames.empty()) {
            if (from <= 0 || from <= lo - fd) break;  // もう前のフレームは無い
            chunk *= 2;
            continue;
          }
          for (auto it = frames.rbegin(); it != frames.rend() && !stop_; ++it) {
            const ImageFeatures f = encode(it->second);
            const MaskPrompt* p = promptAt(it->first);
            add(it->first, p ? tracker.prompt(index, f, pointsOf(*p), false, true) : tracker.track(index, f, true));
            tracker.forget(index, true);
            --index;
            upper = it->first;
            report(++done / total);
            publish(false);
          }
          chunk = std::max(0.5, fd * 15);
        }
      }
    }
  } catch (const std::exception& e) {
    return fail(QStringLiteral("AI が失敗しました: %1").arg(QString::fromUtf8(e.what())));
  }
  if (abandon_) return;
  const bool stopped = stop_;
  publish(true);
  std::shared_ptr<const MaskTrack> track = MaskStore::instance().live(req_.maskId);
  if (!track || track->empty()) return fail(QStringLiteral("マスクを作れませんでした"));
  const QString name = newMaskFileName();
  if (!track->save(QDir(maskDir()).filePath(name), &error)) return fail(error);
  MaskStore::instance().put(name, track);
  QMetaObject::invokeMethod(this, [this, name, stopped] { emit finished(true, QString(), name, stopped); }, Qt::QueuedConnection);
}

// ---- HoverSegmenter ----

HoverSegmenter::HoverSegmenter(QObject* parent) : QObject(parent) {
  thread_ = std::thread([this] { run(); });
}

HoverSegmenter::~HoverSegmenter() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void HoverSegmenter::request(int token, const QString& frameKey, const QImage& frame, const std::vector<PromptPoint>& points) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    next_ = Job{token, frameKey, frame, points, true};
  }
  cv_.notify_all();
}

void HoverSegmenter::prepare(const QString& frameKey, const QImage& frame) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_ && next_->decode) return;  // マスクを作る頼みが先
    next_ = Job{0, frameKey, frame, {}, false};
  }
  cv_.notify_all();
}

void HoverSegmenter::run() {
  EdgeTam* m = nullptr;
  QString featuresKey;
  ImageFeatures features;
  std::vector<float> noMemory;
  auto busy = [this](bool on) { QMetaObject::invokeMethod(this, [this, on] { emit busyChanged(on); }, Qt::QueuedConnection); };
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || next_.has_value(); });
      if (stop_) return;
      job = std::move(*next_);
      next_.reset();
    }
    try {
      if (!m) {
        busy(true);
        QString error;
        m = EdgeTam::shared(&error);
        if (!m) {
          busy(false);
          QMetaObject::invokeMethod(this, [this, error] { emit failed(error); }, Qt::QueuedConnection);
          continue;
        }
      }
      if (job.frameKey != featuresKey && !job.frame.isNull()) {
        busy(true);
        features = m->encode(EdgeTam::pixelsFrom(job.frame));
        noMemory = m->withoutMemory(features);
        featuresKey = job.frameKey;
        busy(false);
      }
      if (!job.decode || job.points.empty() || features.empty() || job.frameKey != featuresKey) {
        busy(false);
        continue;
      }
      const Decoded d = m->decode(features, noMemory, job.points);
      const int token = job.token;
      QMetaObject::invokeMethod(this, [this, token, low = d.lowRes, score = d.score] { emit ready(token, low, score); }, Qt::QueuedConnection);
    } catch (const std::exception& e) {
      busy(false);
      const QString error = QStringLiteral("AI が失敗しました: %1").arg(QString::fromUtf8(e.what()));
      QMetaObject::invokeMethod(this, [this, error] { emit failed(error); }, Qt::QueuedConnection);
    }
  }
}

}  // namespace kv::mask
