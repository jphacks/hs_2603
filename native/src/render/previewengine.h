#pragma once
// プレビューの再生エンジン。再生位置に合わせてフレームを用意し、色・フェード・テキストを重ねた画像を作る。

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QTimer>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <optional>

#include "core/model.h"
#include "media/audioengine.h"
#include "render/textrender.h"

namespace kv {

class VideoSource;

class PreviewEngine : public QObject {
  Q_OBJECT
 public:
  explicit PreviewEngine(QObject* parent = nullptr);
  ~PreviewEngine() override;

  /** プレビュー画面の大きさ（実ピクセル） */
  void setCanvasSize(QSize size);
  const QImage& frame() const { return frame_; }
  bool audioOk() const { return audio_.ok(); }
  /** 今のフレームが目的の時刻のものか（読み込み待ちでないか） */
  bool frameExact() const { return exact_; }

  /**
   * プレビュー上で文字を直接編集している間、その文字を text の内容で描く（確定前の、変換中の文字を含む）。
   * 編集データは変えない。id が空なら元に戻す
   */
  void setTextOverride(const QString& id, const QString& text);
  /** 停止中なら、今の再生位置の画面をすぐ作り直す（編集中の文字と、その上に重ねる入力位置の表示がずれないように） */
  void refreshNow();

  /**
   * スポイト: 画面上の点 at（左上 (0,0)〜右下 (1,1) の割合）にある、映像 clipId（メイン・重ねるトラック）の元の色
   * （クロマキー・色の調整の前。周りの数画素の平均）。今の画面にその映像が出ていない・点がその映像の外なら空
   */
  std::optional<QColor> sampleSource(const QString& clipId, QPointF at) const;

 signals:
  void frameReady();

 private:
  void tick();
  void compose(double t, bool playing);
  void updateAudioSnapshot();
  VideoSource* source(const QString& key, const QString& path);
  /**
   * クロマキー（と、withColor なら色の調整）を掛けたフレーム。同じフレーム・同じ設定なら、前に作ったものを使う
   * （一時停止中に何度描き直しても、重い処理をくり返さない）。onBlack なら、透明にした所を黒で埋める（メインのトラック）
   */
  QImage processedFrame(const QString& key, const QImage& src, const VideoClip& c, bool withColor, bool onBlack);
  struct Processed {
    qint64 source = 0;
    QString params;
    QImage image;
  };
  std::map<QString, Processed> processed_;
  /** 今の画面に使った映像の元のフレーム（"clip:<id>" / "overlay:<id>"。スポイト用） */
  std::map<QString, QImage> raw_;

  QTimer timer_;
  AudioEngine audio_;
  QSize canvas_{960, 540};
  QImage frame_;
  bool exact_ = false;
  std::map<QString, std::unique_ptr<VideoSource>> sources_;
  std::deque<QString> idle_;  // 今は使っていないデコーダー（新しい順）
  std::map<QString, TextSprite> sprites_;
  QString textOverrideId_;
  QString textOverride_;
  std::atomic<bool> dirty_{true};
  bool wasPlaying_ = false;
  int seenSeek_ = -1;
  double playStart_ = 0;
  /** 再生を始めたときの、動画全体の再生速度（再生中に変わったら、その位置から始め直す） */
  double playSpeed_ = 1;
  QElapsedTimer wall_;
};

}  // namespace kv
