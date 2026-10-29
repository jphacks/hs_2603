#pragma once
// 逆再生のための、逆向きの動画を作る。素材を短く区切って ffmpeg の reverse・areverse で 1 つずつ逆向きにし、
// 後ろの区切りから順につなぐ（reverse は区切りの中のフレームをすべて覚えておくので、1 度に覚えるフレームを少なくする）。

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

class QProcess;

namespace kv {

/**
 * source を逆向きにした動画の置き場所（アプリのデータのフォルダの generated。キャッシュとは別で、消さない）。
 * 同じファイル（場所・大きさ・更新日時が同じ）なら、同じ名前になる（作ってあれば使い回す）
 */
QString reversedVideoPath(const QString& source);

class ReverseJob : public QObject {
  Q_OBJECT
 public:
  /**
   * duration は素材の長さ（秒）、fps は映像のフレームレート（分からなければ 0）。width・height は、
   * 1 度に覚えておくフレームの数（区切りの長さ）を決めるのに使う
   */
  ReverseJob(const QString& source, double duration, double fps, int width, int height, bool hasAudio, QObject* parent = nullptr);
  ~ReverseJob() override;
  const QString& source() const { return source_; }
  /** できあがる動画（generated の中） */
  const QString& output() const { return output_; }
  /** 始める（できあがった動画がすでにあれば、すぐに finished(true)） */
  void start();
  /** やめる（作りかけのファイルは消す。finished(false) は出さない） */
  void cancel();

 signals:
  /** 進み具合（0〜1） */
  void progress(double fraction);
  void finished(bool ok, const QString& error);

 private:
  struct Chunk {
    double start = 0;  // 素材の時刻（秒）
    int frames = 0;    // 映像のフレーム数
    QString video;     // 逆向きにした区切りの映像
    QString audio;     // 逆向きにした区切りの音（音が無ければ空）
  };
  void runNext();
  void runConcat();
  void runProcess(const QStringList& args, const std::function<void()>& next);
  void fail(const QString& error);
  void removeWork();

  QString source_;
  QString output_;
  QString work_;
  double duration_ = 0;
  double fps_ = 30;
  int width_ = 0;
  int height_ = 0;
  bool hasAudio_ = false;
  std::vector<Chunk> chunks_;
  /** 次に作る区切り（chunks_ の番号）と、映像・音のどちらを作るか */
  size_t next_ = 0;
  bool nextAudio_ = false;
  int stepsDone_ = 0;
  int stepsTotal_ = 0;
  QProcess* proc_ = nullptr;
  QByteArray stderr_;
  bool canceled_ = false;
};

}  // namespace kv
