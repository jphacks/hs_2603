#pragma once
// オブジェクトマスクの重い仕事（別のスレッド）:
//   TrackJob       … 素材のフレームを順に読み、クリックしたフレームから前・後ろへ被写体を追いかけて、マスクのファイルを作る
//   HoverSegmenter … プレビューでカーソルを合わせた所の被写体のマスクを、すぐに作る（同じフレームの画像の特徴は使い回す）
// どちらも、結果は UI のスレッドへシグナルで渡す。

#include <QImage>
#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "core/model.h"
#include "mask/edgetam.h"

namespace kv::mask {

class MaskTrack;

struct TrackRequest {
  /** マスクの目印（ObjectMask::id） */
  QString maskId;
  /** 動画（写真なら、読み込み用に整えた画像）のファイル */
  QString path;
  bool isImage = false;
  /** 素材の大きさ（向きを直したあと） */
  int width = 0;
  int height = 0;
  /** クリックした点（1 つ目が最初にクリックしたフレーム） */
  std::vector<MaskPrompt> prompts;
  /** 追いかける素材の時刻の範囲 */
  double rangeStart = 0;
  double rangeEnd = 0;
  bool forward = true;
  bool backward = true;
  /** 片方だけを追うときに、もう片方の結果を残すマスク（同じクリックで作ったもの） */
  std::shared_ptr<const MaskTrack> base;
};

class TrackJob : public QObject {
  Q_OBJECT
 public:
  TrackJob(TrackRequest request, QObject* parent = nullptr);
  /** 止めて、スレッドが終わるのを待つ（結果は渡さない） */
  ~TrackJob() override;
  void start();
  /** 途中でやめる（そこまでの結果でファイルを作って finished を出す） */
  void stop();
  const TrackRequest& request() const { return req_; }
  /** 今の進み具合（0〜1） */
  double fraction() const { return fraction_.load(); }

 signals:
  void progress(double fraction);
  /** できた所までの結果を MaskStore の live に入れた（プレビューを描き直す） */
  void updated();
  /** 終わった。ok なら fileName（maskDir の中）に書いた。stopped は途中でやめたか */
  void finished(bool ok, const QString& error, const QString& fileName, bool stopped);

 private:
  void run();
  void publish(bool force);

  TrackRequest req_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> abandon_{false};
  std::atomic<double> fraction_{0};
  /** できたフレーム（素材の時刻 → マスク）。publish が live にする */
  std::mutex mutex_;
  std::vector<std::pair<double, std::pair<float, QByteArray>>> results_;
  qint64 lastPublish_ = 0;
  double frameDur_ = 1.0 / 30;
};

/**
 * カーソルを合わせた所の被写体（プレビューで被写体を選ぶとき）。新しい頼みが来たら、古い頼みは捨てる。
 * frameKey（素材のファイルとフレームの時刻）が同じなら、画像の特徴を作り直さない
 */
class HoverSegmenter : public QObject {
  Q_OBJECT
 public:
  explicit HoverSegmenter(QObject* parent = nullptr);
  ~HoverSegmenter() override;
  /** frame の、points の所の被写体のマスクを作る。終わったら ready（token は頼んだときの番号） */
  void request(int token, const QString& frameKey, const QImage& frame, const std::vector<PromptPoint>& points);
  /** このフレームの画像の特徴を、先に作っておく（選ぶモードに入ったとき） */
  void prepare(const QString& frameKey, const QImage& frame);

 signals:
  void ready(int token, const std::vector<float>& lowRes, float score);
  void failed(const QString& error);
  /** モデルの読み込み・フレームの準備を始めた / 終わった */
  void busyChanged(bool busy);

 private:
  void run();
  struct Job {
    int token = 0;
    QString frameKey;
    QImage frame;
    std::vector<PromptPoint> points;
    bool decode = true;
  };
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::optional<Job> next_;
};

}  // namespace kv::mask
