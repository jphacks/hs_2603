#pragma once
// プレビュー用の動画デコード（別スレッド）。表示したい時刻を伝えると、その周辺のフレームを用意する。

#include <QImage>
#include <QString>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace kv {

class VideoSource {
 public:
  VideoSource(const QString& path, int maxW, int maxH, std::function<void()> onFrame);
  ~VideoSource();
  VideoSource(const VideoSource&) = delete;
  VideoSource& operator=(const VideoSource&) = delete;

  const QString& path() const { return path_; }
  /** 素材の時刻 t を表示したい。playing のときは先読みする */
  void setTarget(double t, bool playing, double rate);
  /** t に表示するフレーム（無ければ直前に表示したもの）。exact は t のフレームが用意できているか */
  QImage frameAt(double t, bool* exact = nullptr);
  bool failed() const;

 private:
  struct Frame {
    double pts;
    QImage image;
  };
  void run();

  QString path_;
  int maxW_, maxH_;
  std::function<void()> onFrame_;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  double target_ = 0;
  bool playing_ = false;
  double rate_ = 1;
  unsigned long long serial_ = 0;
  std::deque<Frame> frames_;
  double frameDur_ = 1.0 / 30;
  bool eof_ = false;
  bool failed_ = false;
  QImage last_;
};

}  // namespace kv
