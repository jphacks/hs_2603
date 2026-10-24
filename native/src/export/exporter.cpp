#include "export/exporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

#include "core/editor.h"
#include "core/filters.h"
#include "core/timeline.h"
#include "media/ffutil.h"
#include "render/textrender.h"

namespace kv {

ExportCheck checkExportable() {
  const Editor& ed = editor();
  const ProjectData& p = ed.project();
  if (p.clips.empty()) return {false, QStringLiteral("クリップがありません")};
  std::vector<QString> used;
  for (const auto& c : p.clips) used.push_back(c.assetId);
  for (const auto& a : p.audios) used.push_back(a.assetId);
  for (const auto& id : used) {
    const Asset* a = ed.asset(id);
    if (!a) return {false, QStringLiteral("見つからないファイルがあります")};
    if (a->status == AssetStatus::Missing) return {false, QStringLiteral("ファイルが見つかりません: %1").arg(a->name)};
    if (a->status == AssetStatus::Error) return {false, QStringLiteral("読み込めないファイルがあります: %1").arg(a->name)};
    if (a->kind == AssetKind::Image && a->imagePath.isEmpty()) return {false, QStringLiteral("画像を準備しています")};
  }
  return {};
}

QString ffmpegPath() {
  const QString local = QCoreApplication::applicationDirPath() + QStringLiteral("/ffmpeg.exe");
  if (QFileInfo::exists(local)) return local;
  const QString env = qEnvironmentVariable("KANTAN_FFMPEG");
  if (!env.isEmpty() && QFileInfo::exists(env)) return env;
  const QString found = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  return found;
}

static int avutilMajor(const QString& ff) {
  static QString cachedFor;
  static int cached = 0;
  if (cachedFor == ff) return cached;
  QProcess p;
  p.start(ff, {QStringLiteral("-hide_banner"), QStringLiteral("-version")});
  p.waitForFinished(15000);
  const QString out = QString::fromUtf8(p.readAllStandardOutput());
  const auto m = QRegularExpression(QStringLiteral("libavutil\\s+(\\d+)\\.")).match(out);
  cachedFor = ff;
  cached = m.hasMatch() ? m.captured(1).toInt() : 58;
  return cached;
}

static QString summarizeError(const QString& stderrText) {
  QStringList lines;
  for (const QString& l : stderrText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) lines << l.trimmed();
  QStringList important;
  static const QRegularExpression re(QStringLiteral("error|invalid|failed|no such|cannot|unable|not found"), QRegularExpression::CaseInsensitiveOption);
  for (const QString& l : lines) {
    if (re.match(l).hasMatch()) important << l;
  }
  const QStringList& pick = important.isEmpty() ? lines : important;
  return pick.mid(std::max<qsizetype>(0, pick.size() - 4)).join(QLatin1Char('\n'));
}

/**
 * 字間が時間で変わる文字の、1 フレームごとの画像（連番）を書く。pattern は text-N-%05d.png の形で、0 番から並べる。
 * 各フレームの字間は、プレビューと同じ layerStateAt() で求め、同じ描き方（textrender）で描く。
 * 字間が前のフレームと同じなら、描き直さずに前の画像を写す。書けたら true
 */
static bool writeSpacingSequence(const ExportRequest& req, const TextItem& t, TextItem centered, QSize design, QSize out, const QString& pattern) {
  const double duration = std::min(t.duration, req.duration - t.start);
  const int frames = exportTextFrames(req, t.start, duration);
  if (frames <= 0) return false;
  const int fps = exportFps(req);
  QString prevFile;
  long long prevKey = 0;
  QElapsedTimer pump;
  pump.start();
  for (int f = 0; f < frames; ++f) {
    const double spacing = layerStateAt(t, double(f) / fps).letterSpacing;
    const long long key = std::llround(spacing * 1000);
    QString file = pattern;
    file.replace(QStringLiteral("%05d"), QStringLiteral("%1").arg(f, 5, 10, QLatin1Char('0')));
    if (f > 0 && key == prevKey) {
      if (!QFile::copy(prevFile, file)) return false;
    } else {
      centered.letterSpacing = spacing;
      if (!renderTextFrame(centered, design, out).save(file, "PNG", 90)) return false;
    }
    prevFile = file;
    prevKey = key;
    // 枚数が多いと時間がかかるので、ときどき画面を描き直して、固まっているように見せない（操作は受け付けない）
    if (pump.elapsed() > 100) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      pump.restart();
    }
  }
  return true;
}

Exporter::Exporter(QObject* parent) : QObject(parent) {}

Exporter::~Exporter() {
  if (proc_) {
    canceled_ = true;
    proc_->kill();
    proc_->waitForFinished(3000);
  }
}

bool Exporter::start(const QString& outputPath, ExportQuality quality, int shortSide, QString* error) {
  if (proc_) {
    if (error) *error = QStringLiteral("別の書き出しを実行中です");
    return false;
  }
  const QString ff = ffmpegPath();
  if (ff.isEmpty()) {
    if (error) *error = QStringLiteral("ffmpeg が見つかりません。アプリを再インストールしてください");
    return false;
  }
  const Editor& ed = editor();
  const ProjectData& p = ed.project();
  const QSize out = exportSize(p.settings.aspect, shortSide);
  const QSize design = designSize(p.settings.aspect);
  const TimelineLayout& layout = ed.layout();

  ExportRequest req;
  req.outputPath = outputPath;
  req.width = out.width();
  req.height = out.height();
  req.fps = p.settings.fps;
  req.quality = quality;
  req.duration = layout.total;
  QHash<QString, QString> colorParams;  // 同じ素材を何度も開かない
  for (const auto& l : layout.items) {
    const VideoClip& c = l.clip;
    const Asset* a = ed.asset(c.assetId);
    if (!a) continue;
    ExportClip ec;
    ec.isImage = c.isImage;
    ec.path = QDir::toNativeSeparators(c.isImage && !a->imagePath.isEmpty() ? a->imagePath : a->path);
    ec.hasAudio = a->hasAudio;
    ec.in = c.in;
    ec.duration = l.duration;
    ec.speed = c.isImage ? 1 : c.speed;
    ec.volume = c.muted ? 0 : c.volume;
    if (!c.muted) ec.points = c.volumePoints;
    if (!layerIsPlain(c)) {
      ec.layer.transformed = true;
      ec.layer.x = c.x;
      ec.layer.y = c.y;
      ec.layer.scale = c.scale;
      ec.layer.rotation = c.rotation;
      ec.layer.opacity = c.opacity;
      ec.layer.anim = c.anim;
    }
    ec.fadeIn = c.fadeIn;
    ec.fadeOut = c.fadeOut;
    ec.fit = c.fit;
    if (!isNeutralColor(c.color)) ec.color = c.color;
    if (!c.isImage) {
      auto it = colorParams.find(a->path);
      if (it == colorParams.end()) it = colorParams.insert(a->path, assumedColorParams(a->path));
      ec.colorParams = *it;
    }
    req.clips.push_back(ec);
  }
  for (const auto& au : p.audios) {
    const Asset* a = ed.asset(au.assetId);
    if (!a || au.start >= layout.total) continue;
    ExportAudio ea;
    ea.path = QDir::toNativeSeparators(a->path);
    ea.start = au.start;
    ea.in = au.in;
    ea.duration = audioDuration(au);
    ea.volume = au.volume;
    ea.points = au.volumePoints;
    ea.fadeIn = au.fadeIn;
    ea.fadeOut = au.fadeOut;
    req.audios.push_back(ea);
  }

  work_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/kantan-export-XXXXXX"));
  if (!work_->isValid()) {
    if (error) *error = QStringLiteral("一時フォルダを作れません");
    return false;
  }
  std::vector<PlannedText> texts;
  int i = 0;
  for (const auto& t : p.texts) {
    if (t.start >= layout.total || t.text.trimmed().isEmpty()) continue;
    const QString png = work_->filePath(QStringLiteral("text-%1.png").arg(i++));
    PlannedText pt;
    const bool plain = layerIsPlain(t);
    if (plain) {
      // 動かさない文字: 位置まで描き込んだ画像をそのまま重ねる（従来どおり）
      if (!renderTextFrame(t, design, out).save(png, "PNG")) continue;
    } else {
      // 動かす文字: 画面の中央に置いた画像を、位置・拡大率・回転・不透明度（とキーフレーム）で動かす
      TextItem centered = t;
      centered.x = 0.5;
      centered.y = 0.5;
      if (!t.anim.keys(AnimProp::LetterSpacing).empty()) {
        // 字間が時間で変わる文字: 字間は文字の形そのものを変えるので、ffmpeg の式では表せない。
        // プレビューと同じ描き方（textrender）で、1 フレームごとの画像（連番）を作る
        const QString pattern = work_->filePath(QStringLiteral("text-%1-%2.png").arg(i - 1).arg(QStringLiteral("%05d")));
        if (!writeSpacingSequence(req, t, centered, design, out, pattern)) continue;
        pt.sequence = true;
        pt.path = QDir::toNativeSeparators(pattern);
      } else if (!renderTextFrame(centered, design, out).save(png, "PNG")) {
        continue;
      }
      pt.layer.transformed = true;
      pt.layer.x = t.x;
      pt.layer.y = t.y;
      pt.layer.scale = t.scale;
      pt.layer.rotation = t.rotation;
      pt.layer.opacity = t.opacity;
      pt.layer.anim = t.anim;
    }
    if (!pt.sequence) pt.path = QDir::toNativeSeparators(png);
    pt.start = t.start;
    pt.duration = std::min(t.duration, layout.total - t.start);
    pt.animation = t.animation;
    texts.push_back(pt);
  }

  ExportPlan plan;
  try {
    plan = buildExportPlan(req, texts);
  } catch (const std::exception& e) {
    if (error) *error = QString::fromUtf8(e.what());
    return false;
  }
  const QString graphFile = work_->filePath(QStringLiteral("graph.txt"));
  {
    QFile f(graphFile);
    if (!f.open(QIODevice::WriteOnly)) {
      if (error) *error = QStringLiteral("一時ファイルを書き込めません");
      return false;
    }
    f.write(plan.filterGraph.toUtf8());
  }
  // FFmpeg 7 以降は -/filter_complex、それより前は -filter_complex_script
  const QStringList graphArgs = avutilMajor(ff) >= 59 ? QStringList{QStringLiteral("-/filter_complex"), QDir::toNativeSeparators(graphFile)}
                                                      : QStringList{QStringLiteral("-filter_complex_script"), QDir::toNativeSeparators(graphFile)};
  outPath_ = QDir::toNativeSeparators(outputPath);
  tmpOut_ = outPath_ + QStringLiteral(".part");
  QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("warning")};
  args << plan.inputArgs << graphArgs << plan.outputArgs;
  args << QStringLiteral("-f") << QStringLiteral("mp4") << QStringLiteral("-progress") << QStringLiteral("pipe:1") << QStringLiteral("-nostats") << tmpOut_;
  if (qEnvironmentVariableIsSet("KANTAN_DEBUG_EXPORT")) {
    QFile dbg(QDir::tempPath() + QStringLiteral("/kantan-last-export.txt"));
    if (dbg.open(QIODevice::WriteOnly)) dbg.write((args.join(QLatin1Char('\n')) + QStringLiteral("\n\n") + plan.filterGraph).toUtf8());
  }

  duration_ = plan.duration;
  canceled_ = false;
  stdoutBuf_.clear();
  stderrBuf_.clear();
  proc_ = new QProcess(this);
  proc_->setProgram(ff);
  proc_->setArguments(args);
  connect(proc_, &QProcess::readyReadStandardOutput, this, [this] {
    stdoutBuf_ += proc_->readAllStandardOutput();
    int nl;
    while ((nl = int(stdoutBuf_.indexOf('\n'))) >= 0) {
      const QByteArray line = stdoutBuf_.left(nl).trimmed();
      stdoutBuf_.remove(0, nl + 1);
      if (!line.startsWith("out_time_us=") && !line.startsWith("out_time_ms=")) continue;
      bool ok = false;
      const double done = line.mid(line.indexOf('=') + 1).toLongLong(&ok) / 1e6;
      if (!ok || duration_ <= 0) continue;
      const double ratio = std::clamp(done / duration_, 0.0, 0.999);
      const double elapsed = timer_.elapsed() / 1000.0;
      const double eta = ratio > 0.02 ? elapsed * (1 - ratio) / ratio : -1;
      emit progress(ratio, elapsed, eta);
    }
  });
  connect(proc_, &QProcess::readyReadStandardError, this, [this] {
    stderrBuf_ += proc_->readAllStandardError();
    if (stderrBuf_.size() > 64000) stderrBuf_ = stderrBuf_.right(32000);
  });
  connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { onFinished(code); });
  connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) onFinished(-1);
  });
  timer_.start();
  proc_->start();
  return true;
}

void Exporter::cancel() {
  if (!proc_) return;
  canceled_ = true;
  proc_->kill();
}

void Exporter::onFinished(int code) {
  if (!proc_) return;
  QProcess* p = proc_;
  proc_ = nullptr;
  p->deleteLater();
  const double elapsed = timer_.elapsed() / 1000.0;
  if (canceled_) {
    QFile::remove(tmpOut_);
    work_.reset();
    emit finished(false, true, QString(), QString(), elapsed);
    return;
  }
  if (code != 0 || !QFileInfo::exists(tmpOut_)) {
    QFile::remove(tmpOut_);
    work_.reset();
    QString msg = summarizeError(QString::fromUtf8(stderrBuf_));
    if (msg.isEmpty()) msg = code == -1 ? QStringLiteral("ffmpeg を起動できませんでした") : QStringLiteral("書き出しに失敗しました");
    emit finished(false, false, QString(), msg, elapsed);
    return;
  }
  QFile::remove(outPath_);
  if (!QFile::rename(tmpOut_, outPath_)) {
    work_.reset();
    emit finished(false, false, QString(), QStringLiteral("保存先に書き込めませんでした"), elapsed);
    return;
  }
  work_.reset();
  emit progress(1, elapsed, 0);
  emit finished(true, false, outPath_, QString(), elapsed);
}

}  // namespace kv
