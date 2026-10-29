#pragma once
// オブジェクトマスクの操作（UI のスレッド）: 被写体を選ぶ（カーソルを合わせた所のマスク・クリック）と、トラック（追いかける）。
// トラックは順番に 1 つずつ別のスレッドで行い、終わったら同じマスクの目印の映像すべてに、できたファイルを入れる（1 回で元に戻せる）。
// 途中でタブを切り替えても続け、そのタブに戻ったときに入れる。

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QString>
#include <deque>
#include <memory>
#include <vector>

#include "core/model.h"

namespace kv::mask {

class HoverSegmenter;
class TrackJob;

/** トラックする向き */
enum class TrackDirection { Both, Forward, Backward };
/** クリックの意味（Shift: 範囲を足す / Alt: 範囲から外す / どちらでもなければ選び直す） */
enum class PickClick { Replace, Add, Remove };

class MaskController : public QObject {
  Q_OBJECT
 public:
  static MaskController& instance();

  /**
   * オブジェクトマスクを使えるか（AI の部品とモデルがあるか）。使えなければ理由。
   * load が false なら、ファイルがあるかだけを確かめる（画面の表示用。AI の部品は読み込まない）
   */
  bool available(QString* why = nullptr, bool load = true) const;

  // ---- 被写体を選ぶ ----
  /** 選ぶモードの映像の、今のフレーム（プレビューで出しているもの）の特徴を先に作っておく */
  void prepare(const QString& clipId, const QImage& frame, double pts);
  /** カーソルを合わせた所（素材のフレームに対する割合 src）の被写体のマスクを作る（できたら hoverChanged） */
  void hover(const QString& clipId, const QImage& frame, double pts, QPointF src);
  void clearHover();
  /** カーソルを合わせた所のマスク（無ければ空）。clipId と、素材のフレームの時刻も確かめて使う */
  QString hoverClip() const { return hoverClip_; }
  double hoverPts() const { return hoverPts_; }
  const std::vector<int8_t>& hoverLogits() const { return hoverLogits_; }
  /** カーソルを合わせた所のマスクを作り直すたびに増える番号（描いた画像を使い回すかを決める） */
  int hoverSerial() const { return hoverSerial_; }
  /** AI を準備している（モデルの読み込み・フレームの特徴づくり） */
  bool preparing() const { return preparing_; }
  /** クリックした（pts は、そのフレームの素材の時刻）。マスクを作り直して、トラックを始める */
  void click(const QString& clipId, double pts, QPointF src, PickClick kind);

  // ---- トラック ----
  /** 映像 clipId のマスクを、向き dir にトラックする（前のトラックが同じマスクなら、やめてからやり直す） */
  void track(const QString& clipId, TrackDirection dir = TrackDirection::Both);
  /** そのマスクのトラックをやめる（そこまでの結果は残す） */
  void stop(const QString& maskId);
  /** 0: していない / 1: 順番待ち / 2: トラック中 */
  int state(const QString& maskId) const;
  double progress(const QString& maskId) const;
  /** マスク（クリックした点を消す・ファイルを外す）を消す。同じ目印のトラックもやめる */
  void remove(const QString& clipId);

 signals:
  void hoverChanged();
  void preparingChanged();
  /** トラックの状態・進み具合が変わった */
  void trackingChanged(const QString& maskId);
  /** トラックの途中の結果ができた（プレビューを描き直す） */
  void liveUpdated();

 private:
  explicit MaskController(QObject* parent);
  struct Entry {
    int workspaceUid = 0;
    QString maskId;
    std::vector<MaskPrompt> prompts;
    std::unique_ptr<TrackJob> job;
    bool running = false;
  };
  struct Pending {
    int workspaceUid = 0;
    QString maskId;
    std::vector<MaskPrompt> prompts;
    QString file;
  };
  void startNext();
  void finishFront(bool ok, const QString& error, const QString& file, bool stopped);
  void applyPending();
  void dropJobs(const QString& maskId);

  HoverSegmenter* segmenter_ = nullptr;
  int hoverToken_ = 0;
  QString hoverClip_;
  double hoverPts_ = 0;
  QString hoverFrameKey_;
  QString requestedClip_;
  double requestedPts_ = 0;
  std::vector<int8_t> hoverLogits_;
  int hoverSerial_ = 0;
  bool preparing_ = false;
  std::deque<std::unique_ptr<Entry>> queue_;
  std::vector<Pending> pending_;
};

inline MaskController& maskController() {
  return MaskController::instance();
}

}  // namespace kv::mask
