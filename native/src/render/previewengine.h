#pragma once
// プレビューの再生エンジン。再生位置に合わせてフレームを用意し、色・フェード・テキストを重ねた画像を作る。

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <deque>
#include <map>
#include <memory>

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

 signals:
  void frameReady();

 private:
  void tick();
  void compose(double t, bool playing);
  void updateAudioSnapshot();
  VideoSource* source(const QString& key, const QString& path);

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
  QElapsedTimer wall_;
};

}  // namespace kv
