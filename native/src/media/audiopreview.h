#pragma once
// オーディオ一覧での試聴。タイムラインの再生とは別の出力で、1 つの素材だけを頭から鳴らす。
// 試聴を始めるとタイムラインの再生は止まり、タイムラインの再生を始めると試聴は止まる。

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>

#include "media/audioengine.h"

namespace kv {

class AudioPreview : public QObject {
  Q_OBJECT
 public:
  static AudioPreview& instance();

  /** いま試聴している素材の ID（鳴らしていなければ空） */
  const QString& playingAsset() const { return assetId_; }
  /** 試聴中の素材なら止める。そうでなければ、別の素材を試聴中でもそれを止めて、この素材を頭から鳴らす */
  void toggle(const QString& assetId);
  /**
   * 一覧に入っていないファイルを試聴する（ワークショップのオーディオなど）。key で見分ける（playingAsset() に入る）。
   * 同じ key を試聴中なら止める。duration はファイルの長さ（秒）
   */
  void toggleFile(const QString& key, const QString& path, double duration);
  void stop();

 signals:
  /** 試聴の状態（どの素材を鳴らしているか）が変わった */
  void changed();

 private:
  AudioPreview();
  void tick();
  void play(const QString& key, const QString& path, double duration, bool asset);

  std::unique_ptr<AudioEngine> engine_;  // 初めて試聴するときに作る（起動を重くしない）
  QString assetId_;
  /** 鳴らしているのが一覧の素材か（一覧から消えたら止める） */
  bool asset_ = false;
  double duration_ = 0;
  QTimer timer_;
  QElapsedTimer sinceStart_;
};

inline AudioPreview& audioPreview() {
  return AudioPreview::instance();
}

}  // namespace kv
