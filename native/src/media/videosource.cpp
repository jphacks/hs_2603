#include "media/videosource.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "media/ffutil.h"

namespace kv {

static constexpr size_t kMaxFrames = 10;

VideoSource::VideoSource(const QString& path, int maxW, int maxH, std::function<void()> onFrame)
    : path_(path), maxW_(maxW), maxH_(maxH), onFrame_(std::move(onFrame)) {
  thread_ = std::thread([this] { run(); });
}

VideoSource::~VideoSource() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void VideoSource::setTarget(double t, bool playing, double rate) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::abs(t - target_) < 1e-6 && playing == playing_ && rate == rate_) return;
    target_ = t;
    playing_ = playing;
    rate_ = rate;
    ++serial_;
  }
  cv_.notify_all();
}

bool VideoSource::failed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

QImage VideoSource::frameAt(double t, bool* exact) {
  std::lock_guard<std::mutex> lock(mutex_);
  const double eps = frameDur_ * 0.25;
  int best = -1;
  for (int i = 0; i < int(frames_.size()); ++i) {
    if (frames_[size_t(i)].pts <= t + eps) best = i;
    else break;
  }
  bool ok = false;
  if (best >= 0) {
    const Frame& f = frames_[size_t(best)];
    // 最後のフレームより先を求められていて、まだデコードが追いついていない
    ok = !(best == int(frames_.size()) - 1 && !eof_ && t - f.pts > frameDur_ * 1.5);
    last_ = f.image;
  } else if (!frames_.empty() && frames_.front().pts - t < frameDur_ * 1.5) {
    // 素材の最初のフレームより少し前
    ok = true;
    last_ = frames_.front().image;
  }
  if (exact) *exact = ok;
  return last_;
}

void VideoSource::run() {
  VideoDecoder dec;
  if (!dec.open(path_)) {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frameDur_ = dec.frameDuration();
  }
  double lastPts = -1e9;      // シーク後に最後にデコードしたフレームの時刻
  bool lastConverted = true;  // そのフレームを画像にしたか
  double seekPos = -1e9;
  bool needSeek = true;
  while (true) {
    double target;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stop_) return;
      target = target_;
      const bool playing = playing_;
      const double rate = rate_;
      const double fd = frameDur_;
      if (!needSeek) {
        const double known = std::max(lastPts, seekPos);
        if (!frames_.empty() && target < frames_.front().pts - fd * 0.5) needSeek = true;               // 戻った
        else if (frames_.empty() && lastPts > -1e8 && target < lastPts - fd * 0.5) needSeek = true;  // 戻った
        else if (target > known + std::max(1.0, 2.0 * rate)) needSeek = true;                         // 遠くへ進んだ
      }
      if (needSeek) {
        frames_.clear();
        eof_ = false;
      } else {
        const double ahead = playing ? std::max(0.4, 0.4 * rate) : 0.0;
        const bool reached = lastPts >= target - fd * 0.75;
        if (eof_ || (reached && (lastPts >= target + ahead || frames_.size() >= kMaxFrames))) {
          const auto serial = serial_;
          cv_.wait_for(lock, std::chrono::milliseconds(playing ? 8 : 100), [&] { return stop_ || serial_ != serial; });
          continue;
        }
      }
    }
    if (needSeek) {
      dec.seek(target);
      seekPos = target;
      lastPts = -1e9;
      lastConverted = true;
      needSeek = false;
    }
    if (!dec.decodeNext()) {
      // 終わりまで来た: 最後のフレームを表示できるようにしておく
      QImage img;
      if (!lastConverted && lastPts > -1e8) img = dec.frameImage(maxW_, maxH_);
      lastConverted = true;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!img.isNull()) {
          while (!frames_.empty() && frames_.back().pts >= lastPts) frames_.pop_back();
          frames_.push_back({lastPts, std::move(img)});
        }
        eof_ = true;
      }
      if (onFrame_) onFrame_();
      continue;
    }
    const double pts = dec.framePts();
    lastPts = pts;
    double tgt;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tgt = target_;
    }
    // シーク直後の、目標より前のフレームは画像にしない
    if (pts + dec.frameDuration() * 1.5 < tgt) {
      lastConverted = false;
      continue;
    }
    QImage img = dec.frameImage(maxW_, maxH_);
    lastConverted = true;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      while (!frames_.empty() && frames_.back().pts >= pts) frames_.pop_back();
      frames_.push_back({pts, std::move(img)});
      // 表示し終わったフレームを捨てる（表示中の 1 枚は残す）
      while (frames_.size() > 1 && frames_[1].pts <= target_ + frameDur_ * 0.25) frames_.pop_front();
      while (frames_.size() > kMaxFrames) frames_.pop_back();
    }
    if (onFrame_) onFrame_();
  }
}

}  // namespace kv
