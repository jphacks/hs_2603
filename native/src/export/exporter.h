#pragma once
// 書き出し（ffmpeg を起動して進み具合を通知する）

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <memory>

#include "core/exportplan.h"

namespace kv {

struct ExportCheck {
  bool ok = true;
  QString message;
};

ExportCheck checkExportable();
/** 使う ffmpeg.exe（アプリと同じフォルダ → 環境変数 E_EDITOR_FFMPEG → PATH） */
QString ffmpegPath();

class Exporter : public QObject {
  Q_OBJECT
 public:
  explicit Exporter(QObject* parent = nullptr);
  ~Exporter() override;
  bool running() const { return proc_ != nullptr; }
  /**
   * 書き出しを始める。始められなかったときは error に理由を入れて false。
   * rangeOnly のときは、決めてある書き出す範囲（Editor::exportRange）だけを書き出す。format は書き出す形式（MP4・GIF・MP3 など）
   */
  bool start(const QString& outputPath, ExportQuality quality, int shortSide, QString* error = nullptr, bool rangeOnly = false,
             ExportFormat format = ExportFormat::Mp4H264);
  void cancel();

 signals:
  void progress(double ratio, double elapsed, double eta);
  void finished(bool ok, bool canceled, const QString& outputPath, const QString& error, double elapsed);

 private:
  void onFinished(int code);
  QProcess* proc_ = nullptr;
  std::unique_ptr<QTemporaryDir> work_;
  QString tmpOut_;
  QString outPath_;
  double duration_ = 0;
  bool canceled_ = false;
  QElapsedTimer timer_;
  QByteArray stdoutBuf_;
  QByteArray stderrBuf_;
};

}  // namespace kv
