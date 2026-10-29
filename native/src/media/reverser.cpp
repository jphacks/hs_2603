#include "media/reverser.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <cmath>

#include "export/exporter.h"
#include "media/ffutil.h"

namespace kv {

namespace {

/** 1 つの区切りで覚えておくフレームの大きさの上限（バイト。デコードした映像は 1 画素 1.5 バイトとして数える） */
constexpr double kChunkMemory = 320.0 * 1024 * 1024;
/** 音の標本化周波数（書き出しと同じ） */
constexpr int kRate = 48000;

QString generatedDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("generated"));
}

/** concat の一覧に書くファイル名（' はエスケープする） */
QString concatLine(const QString& path) {
  QString p = QDir::fromNativeSeparators(path);
  p.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
  return QStringLiteral("file '%1'").arg(p);
}

QString num(double v) {
  return QString::number(v, 'f', 6);
}

}  // namespace

QString reversedVideoPath(const QString& source) {
  const QFileInfo fi(source);
  const QByteArray key = (fi.absoluteFilePath().toLower() + QLatin1Char('|') + QString::number(fi.size()) + QLatin1Char('|') +
                          QString::number(fi.lastModified().toMSecsSinceEpoch()))
                             .toUtf8();
  const QString hash = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
  return QDir(generatedDir()).filePath(QStringLiteral("reversed-%1.mov").arg(hash));
}

ReverseJob::ReverseJob(const QString& source, double duration, double fps, int width, int height, bool hasAudio, QObject* parent)
    : QObject(parent), source_(source), duration_(duration), width_(width), height_(height), hasAudio_(hasAudio) {
  output_ = reversedVideoPath(source);
  fps_ = fps > 1 && fps < 241 ? fps : 30;
}

ReverseJob::~ReverseJob() {
  canceled_ = true;
  if (proc_) {
    proc_->disconnect(this);
    proc_->kill();
    proc_->waitForFinished(3000);
    delete proc_;
    proc_ = nullptr;
  }
  removeWork();
}

void ReverseJob::start() {
  if (QFileInfo(output_).isFile() && QFileInfo(output_).size() > 0) {
    emit progress(1);
    emit finished(true, QString());
    return;
  }
  if (duration_ <= 0.01) {
    fail(QStringLiteral("動画の長さが分かりません"));
    return;
  }
  QDir().mkpath(generatedDir());
  work_ = output_ + QStringLiteral(".work");
  QDir(work_).removeRecursively();
  if (!QDir().mkpath(work_)) {
    fail(QStringLiteral("作業用のフォルダを作れません"));
    return;
  }
  // 区切り: 1 度に覚えるフレームが kChunkMemory に収まる長さ（0.5〜4 秒）。最後の区切りが短すぎるときは、1 つ前に含める
  const double frameBytes = double(std::max(16, width_ > 0 ? width_ : 1920)) * std::max(16, height_ > 0 ? height_ : 1080) * 1.5;
  const int perChunk = std::clamp(int(kChunkMemory / frameBytes), std::max(1, int(fps_ * 0.5)), std::max(1, int(fps_ * 4)));
  const int total = std::max(1, int(std::ceil(duration_ * fps_ - 1e-6)));
  std::vector<std::pair<int, int>> spans;  // 先頭のフレームと、フレーム数（前から）
  for (int f0 = 0; f0 < total; f0 += perChunk) spans.push_back({f0, std::min(perChunk, total - f0)});
  if (spans.size() >= 2 && spans.back().second < std::max(1, int(fps_ / 4))) {
    spans[spans.size() - 2].second += spans.back().second;
    spans.pop_back();
  }
  // できあがりの順（素材の後ろの区切りから）に並べる
  chunks_.clear();
  for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
    Chunk c;
    c.start = double(it->first) / fps_;
    c.frames = it->second;
    const int index = int(chunks_.size());
    c.video = QDir(work_).filePath(QStringLiteral("v%1.mov").arg(index, 5, 10, QLatin1Char('0')));
    if (hasAudio_) c.audio = QDir(work_).filePath(QStringLiteral("a%1.wav").arg(index, 5, 10, QLatin1Char('0')));
    chunks_.push_back(c);
  }
  next_ = 0;
  nextAudio_ = false;
  stepsDone_ = 0;
  stepsTotal_ = int(chunks_.size()) * (hasAudio_ ? 2 : 1) + 1;
  emit progress(0);
  runNext();
}

void ReverseJob::cancel() {
  canceled_ = true;
  if (proc_) {
    proc_->disconnect(this);
    proc_->kill();
    proc_->waitForFinished(3000);
    proc_->deleteLater();
    proc_ = nullptr;
  }
  removeWork();
}

void ReverseJob::runNext() {
  if (canceled_) return;
  if (next_ >= chunks_.size()) {
    runConcat();
    return;
  }
  const Chunk& c = chunks_[next_];
  const double length = double(c.frames) / fps_;
  QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error")};
  if (c.start > 0.0005) args << QStringLiteral("-ss") << num(c.start);
  args << QStringLiteral("-t") << num(length) << QStringLiteral("-i") << QDir::toNativeSeparators(source_);
  if (!nextAudio_) {
    // 映像: 素材のフレームレートのフレームにそろえ（区切りの始めに足りなければ最初のフレーム、終わりに足りなければ最後のフレームで埋める）、
    // ちょうど c.frames 枚にしてから逆向きにする。どの区切りも同じ長さなので、つないでもずれない
    const QString vf = QStringLiteral("fps=%1:start_time=0,tpad=stop_mode=clone:stop=%2,trim=end_frame=%2,reverse").arg(num(fps_)).arg(c.frames);
    args << QStringLiteral("-map") << QStringLiteral("0:v:0") << QStringLiteral("-an") << QStringLiteral("-sn") << QStringLiteral("-dn")
         << QStringLiteral("-vf") << vf << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-preset") << QStringLiteral("veryfast")
         << QStringLiteral("-crf") << QStringLiteral("16") << QStringLiteral("-g") << QStringLiteral("15") << QStringLiteral("-bf") << QStringLiteral("0")
         << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-f") << QStringLiteral("mov") << QDir::toNativeSeparators(c.video);
  } else {
    // 音: 区切りの長さちょうど（足りなければ無音で埋める）にしてから逆向きにする（映像の区切りと同じ時間）
    const long long s0 = std::llround(c.start * kRate);
    const long long n = std::max(1LL, std::llround((c.start + length) * kRate) - s0);
    const QString af = QStringLiteral("aresample=%1,apad=whole_len=%2,atrim=end_sample=%2,areverse").arg(kRate).arg(n);
    args << QStringLiteral("-map") << QStringLiteral("0:a:0") << QStringLiteral("-vn") << QStringLiteral("-sn") << QStringLiteral("-dn")
         << QStringLiteral("-af") << af << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le") << QStringLiteral("-ar") << QString::number(kRate)
         << QStringLiteral("-ac") << QStringLiteral("2") << QStringLiteral("-f") << QStringLiteral("wav") << QDir::toNativeSeparators(c.audio);
  }
  runProcess(args, [this] {
    if (hasAudio_ && !nextAudio_) {
      nextAudio_ = true;
    } else {
      nextAudio_ = false;
      ++next_;
    }
    runNext();
  });
}

void ReverseJob::runConcat() {
  // 区切りを、素材の後ろから順につなぐ（映像・音とも、作り直さずにそのまま）
  const QString vList = QDir(work_).filePath(QStringLiteral("video.txt"));
  const QString aList = QDir(work_).filePath(QStringLiteral("audio.txt"));
  auto writeList = [&](const QString& file, bool audio) {
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QTextStream out(&f);
    for (const Chunk& c : chunks_) out << concatLine(audio ? c.audio : c.video) << "\n";
    return true;
  };
  if (!writeList(vList, false) || (hasAudio_ && !writeList(aList, true))) {
    fail(QStringLiteral("作業用のファイルを書けません"));
    return;
  }
  const QString part = output_ + QStringLiteral(".part");
  QFile::remove(part);
  QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                   QStringLiteral("-f"), QStringLiteral("concat"), QStringLiteral("-safe"), QStringLiteral("0"), QStringLiteral("-i"),
                   QDir::toNativeSeparators(vList)};
  if (hasAudio_) args << QStringLiteral("-f") << QStringLiteral("concat") << QStringLiteral("-safe") << QStringLiteral("0") << QStringLiteral("-i") << QDir::toNativeSeparators(aList);
  args << QStringLiteral("-map") << QStringLiteral("0:v:0");
  if (hasAudio_) args << QStringLiteral("-map") << QStringLiteral("1:a:0");
  args << QStringLiteral("-c") << QStringLiteral("copy") << QStringLiteral("-movflags") << QStringLiteral("+faststart") << QStringLiteral("-f")
       << QStringLiteral("mov") << QDir::toNativeSeparators(part);
  runProcess(args, [this, part] {
    QFile::remove(output_);
    if (!QFile::rename(part, output_)) {
      fail(QStringLiteral("できあがった動画を置けません"));
      return;
    }
    removeWork();
    emit progress(1);
    emit finished(true, QString());
  });
}

void ReverseJob::runProcess(const QStringList& args, const std::function<void()>& next) {
  const QString ff = ffmpegPath();
  if (ff.isEmpty()) {
    fail(QStringLiteral("ffmpeg が見つかりません"));
    return;
  }
  stderr_.clear();
  proc_ = new QProcess(this);
  proc_->setProgram(ff);
  proc_->setArguments(args);
  connect(proc_, &QProcess::readyReadStandardError, this, [this] {
    stderr_ += proc_->readAllStandardError();
    if (stderr_.size() > 32000) stderr_ = stderr_.right(16000);
  });
  connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) fail(QStringLiteral("ffmpeg を起動できません"));
  });
  connect(proc_, &QProcess::finished, this, [this, next](int code, QProcess::ExitStatus status) {
    QProcess* p = proc_;
    proc_ = nullptr;
    if (p) p->deleteLater();
    if (canceled_) return;
    if (status != QProcess::NormalExit || code != 0) {
      const QString detail = QString::fromUtf8(stderr_).trimmed().section(QLatin1Char('\n'), -1);
      fail(detail.isEmpty() ? QStringLiteral("ffmpeg が失敗しました（%1）").arg(code) : detail);
      return;
    }
    ++stepsDone_;
    emit progress(double(stepsDone_) / std::max(1, stepsTotal_));
    next();
  });
  proc_->start();
}

void ReverseJob::fail(const QString& error) {
  if (canceled_) return;
  canceled_ = true;
  if (proc_) {
    proc_->disconnect(this);
    proc_->kill();
    proc_->deleteLater();
    proc_ = nullptr;
  }
  removeWork();
  QFile::remove(output_ + QStringLiteral(".part"));
  emit finished(false, error);
}

void ReverseJob::removeWork() {
  if (!work_.isEmpty()) QDir(work_).removeRecursively();
}

}  // namespace kv
