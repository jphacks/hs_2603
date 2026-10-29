#include "mask/maskcontroller.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>

#include "core/editor.h"
#include "mask/edgetam.h"
#include "mask/maskjob.h"
#include "mask/masktrack.h"

namespace kv::mask {

namespace {

/** クリックしたフレームが同じか（同じ素材のフレームの時刻。デコーダーの値なので、ほぼぴったり同じになる） */
bool samePts(double a, double b) {
  return std::abs(a - b) < 1e-3;
}

/** 映像の素材のファイル（写真は、読み込み用に整えた画像） */
QString sourcePath(const VideoClip& c, const Asset& a) {
  return c.isImage && !a.imagePath.isEmpty() ? a.imagePath : a.path;
}

}  // namespace

MaskController& MaskController::instance() {
  // アプリ本体の子にする（終わるときに、トラックのスレッドを止めてから片付ける）
  static MaskController* c = new MaskController(QCoreApplication::instance());
  return *c;
}

MaskController::MaskController(QObject* parent) : QObject(parent) {
  segmenter_ = new HoverSegmenter(this);
  connect(segmenter_, &HoverSegmenter::ready, this, [this](int token, const std::vector<float>& lowRes, float score) {
    if (token != hoverToken_) return;  // もっと新しい頼みがある
    hoverClip_ = requestedClip_;
    hoverPts_ = requestedPts_;
    hoverLogits_.assign(lowRes.size(), 0);
    for (size_t i = 0; i < lowRes.size(); ++i) {
      hoverLogits_[i] = int8_t(std::clamp(int(std::lround((score > 0 ? lowRes[i] : -1024.0f) * 8.0f)), -127, 127));
    }
    ++hoverSerial_;
    emit hoverChanged();
  });
  connect(segmenter_, &HoverSegmenter::busyChanged, this, [this](bool on) {
    if (preparing_ == on) return;
    preparing_ = on;
    emit preparingChanged();
  });
  connect(segmenter_, &HoverSegmenter::failed, this, [this](const QString& error) {
    if (preparing_) {
      preparing_ = false;
      emit preparingChanged();
    }
    editor().toast(error, ToastKind::Error);
    editor().setMaskPick({});
  });
  // 別のタブで終わったトラックの結果は、そのタブに戻ったときに入れる
  connect(&editor(), &Editor::workspaceChanged, this, [this] { applyPending(); });
}

bool MaskController::available(QString* why, bool load) const {
  return EdgeTam::filesPresent(why, load);
}

void MaskController::prepare(const QString& clipId, const QImage& frame, double pts) {
  const VideoClip* c = editor().clip(clipId);
  const Asset* a = c ? editor().asset(c->assetId) : nullptr;
  if (!a || frame.isNull()) return;
  segmenter_->prepare(QStringLiteral("%1|%2|%3").arg(sourcePath(*c, *a)).arg(pts, 0, 'f', 6).arg(frame.cacheKey()), frame);
}

void MaskController::hover(const QString& clipId, const QImage& frame, double pts, QPointF src) {
  const VideoClip* c = editor().clip(clipId);
  const Asset* a = c ? editor().asset(c->assetId) : nullptr;
  if (!a || frame.isNull()) return;
  requestedClip_ = clipId;
  requestedPts_ = pts;
  const QString key = QStringLiteral("%1|%2|%3").arg(sourcePath(*c, *a)).arg(pts, 0, 'f', 6).arg(frame.cacheKey());
  segmenter_->request(++hoverToken_, key, frame, {PromptPoint{std::clamp(src.x(), 0.0, 1.0), std::clamp(src.y(), 0.0, 1.0), true}});
}

void MaskController::clearHover() {
  ++hoverToken_;  // 作っている途中の結果も使わない
  if (hoverClip_.isEmpty() && hoverLogits_.empty()) return;
  hoverClip_.clear();
  hoverLogits_.clear();
  emit hoverChanged();
}

void MaskController::click(const QString& clipId, double pts, QPointF src, PickClick kind) {
  Editor& ed = editor();
  const VideoClip* c = ed.clip(clipId);
  if (!c) return;
  const MaskPoint point{std::clamp(src.x(), 0.0, 1.0), std::clamp(src.y(), 0.0, 1.0), kind != PickClick::Remove};
  const double t = c->isImage ? 0.0 : pts;
  ObjectMask m = c->mask;
  if (kind == PickClick::Replace || !m.has()) {
    if (kind == PickClick::Remove) {
      ed.toast(QStringLiteral("先に、被写体をクリックして選んでください（Alt+クリックは、選んだ範囲から外すときに使います）"));
      return;
    }
    // 新しく選ぶ: この映像だけの新しいマスク（使い方の設定は、前のマスクのまま）
    if (!m.has()) m = ObjectMask{};
    m.id = newId();
    m.file.clear();
    m.prompts = {MaskPrompt{t, {point}}};
    ed.updateClip(clipId, [&](VideoClip& x) { x.mask = m; });
  } else {
    // 点を足す・外す: このフレームの点に加える（無ければ、このフレームを新しく足す）。同じマスクの映像すべてを直す
    bool found = false;
    for (MaskPrompt& p : m.prompts) {
      if (samePts(p.t, t)) {
        p.points.push_back(point);
        found = true;
        break;
      }
    }
    if (!found) m.prompts.push_back(MaskPrompt{t, {point}});
    const std::vector<MaskPrompt> prompts = m.prompts;
    ed.updateMasks(m.id, [&](ObjectMask& x) {
      x.prompts = prompts;
      x.file.clear();
    });
  }
  clearHover();
  track(clipId, TrackDirection::Both);
}

void MaskController::dropJobs(const QString& maskId) {
  // 同じマスクのトラックを、結果を使わずにやめる（クリックし直したなど）
  for (auto it = queue_.begin(); it != queue_.end();) {
    if ((*it)->maskId == maskId) {
      it = queue_.erase(it);  // TrackJob を消すと、スレッドを止めて待つ
    } else {
      ++it;
    }
  }
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.maskId == maskId; }), pending_.end());
  MaskStore::instance().clearLive(maskId);
}

void MaskController::track(const QString& clipId, TrackDirection dir) {
  Editor& ed = editor();
  const VideoClip* c = ed.clip(clipId);
  if (!c || !c->mask.has()) return;
  const Asset* a = ed.asset(c->assetId);
  if (!a || a->status == AssetStatus::Missing || a->status == AssetStatus::Error) {
    ed.toast(QStringLiteral("素材のファイルが見つかりません"), ToastKind::Error);
    return;
  }
  QString why;
  if (!available(&why)) {
    ed.toast(why, ToastKind::Error);
    return;
  }
  const ObjectMask m = c->mask;
  const bool wasFront = !queue_.empty() && queue_.front()->maskId == m.id;
  dropJobs(m.id);
  TrackRequest req;
  req.maskId = m.id;
  req.path = sourcePath(*c, *a);
  req.isImage = c->isImage;
  req.width = a->width;
  req.height = a->height;
  req.prompts = m.prompts;
  // 追いかける範囲: 同じマスクの映像（分割したものなど）が使う、素材の範囲すべて
  double lo = c->in, hi = c->isImage ? c->in : c->out;
  auto widen = [&](const VideoClip& x) {
    if (x.mask.id != m.id || x.assetId != c->assetId) return;
    lo = std::min(lo, x.in);
    hi = std::max(hi, x.isImage ? x.in : x.out);
  };
  for (const auto& x : ed.project().clips) widen(x);
  for (const auto& x : ed.project().overlays) widen(x);
  req.rangeStart = std::max(0.0, lo);
  req.rangeEnd = a->duration > 0 ? std::min(hi, a->duration) : hi;
  req.forward = dir != TrackDirection::Backward;
  req.backward = dir != TrackDirection::Forward;
  if (dir != TrackDirection::Both && !m.file.isEmpty()) req.base = MaskStore::instance().get(m.file);
  auto e = std::make_unique<Entry>();
  e->workspaceUid = ed.workspaceUid(ed.workspaceIndex());
  e->maskId = m.id;
  e->prompts = m.prompts;
  e->job = std::make_unique<TrackJob>(std::move(req));
  TrackJob* job = e->job.get();
  const QString id = m.id;
  connect(job, &TrackJob::progress, this, [this, id](double) { emit trackingChanged(id); });
  connect(job, &TrackJob::updated, this, [this] { emit liveUpdated(); });
  connect(job, &TrackJob::finished, this, [this, job](bool ok, const QString& error, const QString& file, bool stopped) {
    if (queue_.empty() || queue_.front()->job.get() != job) return;
    finishFront(ok, error, file, stopped);
  });
  // やり直すときは、前と同じ順番（先頭）で始める
  if (wasFront) queue_.push_front(std::move(e));
  else queue_.push_back(std::move(e));
  emit trackingChanged(id);
  startNext();
}

void MaskController::startNext() {
  if (queue_.empty() || queue_.front()->running) return;
  Entry& e = *queue_.front();
  e.running = true;
  e.job->start();
  emit trackingChanged(e.maskId);
}

void MaskController::finishFront(bool ok, const QString& error, const QString& file, bool stopped) {
  std::unique_ptr<Entry> e = std::move(queue_.front());
  queue_.pop_front();
  Editor& ed = editor();
  if (!ok) {
    MaskStore::instance().clearLive(e->maskId);
    ed.toast(QStringLiteral("トラックできませんでした: %1").arg(error), ToastKind::Error);
  } else {
    pending_.push_back({e->workspaceUid, e->maskId, e->prompts, file});
    applyPending();
    if (!stopped) ed.toast(QStringLiteral("被写体のトラックが終わりました"), ToastKind::Success);
  }
  const QString id = e->maskId;
  // スレッドは終わっているので、そのまま片付ける（finished のシグナルの中なので、あとで消す）
  e->job.release()->deleteLater();
  emit trackingChanged(id);
  emit liveUpdated();
  startNext();
}

void MaskController::applyPending() {
  Editor& ed = editor();
  const int uid = ed.workspaceUid(ed.workspaceIndex());
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->workspaceUid != uid) {
      // 閉じたタブの結果は捨てる
      if (ed.indexOfWorkspaceUid(it->workspaceUid) < 0) {
        MaskStore::instance().clearLive(it->maskId);
        it = pending_.erase(it);
      } else {
        ++it;
      }
      continue;
    }
    const Pending p = *it;
    it = pending_.erase(it);
    // 同じクリックで作ったマスクの映像だけに入れる（トラックしている間に、クリックし直した・元に戻したものには入れない）
    ed.updateMasks(p.maskId, [&](ObjectMask& m) {
      if (m.prompts == p.prompts) m.file = p.file;
    });
    MaskStore::instance().clearLive(p.maskId);
  }
}

void MaskController::stop(const QString& maskId) {
  for (auto& e : queue_) {
    if (e->maskId != maskId) continue;
    if (e->running) {
      e->job->stop();  // そこまでの結果でファイルを作って、finished が来る
    } else {
      const QString id = e->maskId;
      dropJobs(id);
      emit trackingChanged(id);
      startNext();
    }
    return;
  }
}

int MaskController::state(const QString& maskId) const {
  for (const auto& e : queue_) {
    if (e->maskId == maskId) return e->running ? 2 : 1;
  }
  return 0;
}

double MaskController::progress(const QString& maskId) const {
  for (const auto& e : queue_) {
    if (e->maskId == maskId) return e->job->fraction();
  }
  return 0;
}

void MaskController::remove(const QString& clipId) {
  Editor& ed = editor();
  const VideoClip* c = ed.clip(clipId);
  if (!c || !c->mask.has()) return;
  const QString id = c->mask.id;
  // 消すのはこの映像だけ（分割したほかの映像のマスクは残す。トラックは、ほかに同じマスクの映像が無いときだけやめる）
  ed.updateClip(clipId, [](VideoClip& x) { x.mask = ObjectMask{}; });
  bool used = false;
  for (const auto& x : ed.project().clips) used = used || x.mask.id == id;
  for (const auto& x : ed.project().overlays) used = used || x.mask.id == id;
  if (!used) {
    dropJobs(id);
    emit trackingChanged(id);
    emit liveUpdated();
    startNext();
  }
}

}  // namespace kv::mask
