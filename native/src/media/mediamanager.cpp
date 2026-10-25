#include "media/mediamanager.h"

#include <QCollator>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

#include "core/editor.h"
#include "media/ffutil.h"

namespace kv {

MediaManager& MediaManager::instance() {
  static MediaManager* m = new MediaManager();
  return *m;
}

MediaManager::MediaManager() {
  pool_.setMaxThreadCount(2);
  // 閉じたタブのメディアのプレビュー画像は、メモリに残さない
  connect(&editor(), &Editor::workspaceClosed, this, [this](const QStringList& ids) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const QString& id : ids) images_.erase(id);
  });
}

QString MediaManager::cacheDir(const QString& sub) {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1Char('/') + sub;
  QDir().mkpath(dir);
  return dir;
}

QString MediaManager::fileKey(const QString& path) {
  const QFileInfo fi(path);
  const QByteArray src = QStringLiteral("%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch()).toUtf8();
  return QString::fromLatin1(QCryptographicHash::hash(src, QCryptographicHash::Sha1).toHex().left(20));
}

bool MediaManager::busy() const {
  return pending_.load() > 0;
}

void MediaManager::waitForIdle(int msecs) {
  QElapsedTimer t;
  t.start();
  while (busy() && t.elapsed() < msecs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(10);
  }
}

static void onUi(std::function<void()> fn) {
  QMetaObject::invokeMethod(&media(), std::move(fn), Qt::QueuedConnection);
}

/** 一覧にあるメディアのファイル（同じファイルは使い回す） */
static std::map<QString, QString> existingAssets() {
  std::map<QString, QString> existing;
  for (const auto& [id, a] : editor().assets()) {
    if (a.status != AssetStatus::Error) existing[QFileInfo(a.path).absoluteFilePath()] = id;
  }
  return existing;
}

void MediaManager::importPaths(const QStringList& paths, bool addToTimeline, std::optional<int> index, std::optional<double> at, const QString& note) {
  for (const auto& p : paths) {
    if (p.endsWith(QStringLiteral(".kvproj"), Qt::CaseInsensitive)) {
      openProject(p);
      return;
    }
  }
  // 同じファイルがすでに一覧にあれば使い回す
  const std::map<QString, QString> existing = existingAssets();
  // 読み込みの途中でタブを切り替えたり閉じたりしても、取り込んだタブにメディアを入れる
  const int workspace = editor().workspaceUid(editor().workspaceIndex());
  ++pending_;
  pool_.start([this, paths, existing, addToTimeline, index, at, workspace, note] {
    runImport(paths, existing, addToTimeline, index, at, workspace, note, nullptr);
  });
}

bool MediaManager::importFolder(const QString& dir, bool recursive, const QStringList& exts) {
  if (batchRunning_) {
    editor().toast(QStringLiteral("フォルダから読み込んでいる途中です。終わってから、もう一度選んでください"));
    return false;
  }
  batchRunning_ = true;
  cancel_ = false;
  const std::map<QString, QString> existing = existingAssets();
  const int workspace = editor().workspaceUid(editor().workspaceIndex());
  ++pending_;
  emit importProgress(0, 0);  // ファイルを探している途中
  pool_.start([this, dir, recursive, exts, existing, workspace] {
    // ファイルを探す（画面を止めないよう、裏で）。隠しファイル・システムのファイルは入れない。ショートカット（リンク）はたどらない
    BatchReport report;
    QStringList files;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext()) {
      if (cancel_) {
        report.canceled = true;
        break;
      }
      const QString f = it.next();
      if (!exts.contains(QFileInfo(f).suffix().toLower())) {
        ++report.unsupported;  // 対応していない形式は飛ばす
        continue;
      }
      if (files.size() >= kMaxFolderFiles) {
        report.truncated = true;
        break;
      }
      files << f;
    }
    // 名前の順（「2」は「10」より前）。サブフォルダのファイルは、フォルダごとにまとまる
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(files.begin(), files.end(), [&](const QString& a, const QString& b) { return collator.compare(a, b) < 0; });
    report.found = int(files.size());
    runImport(report.canceled ? QStringList() : files, existing, false, std::nullopt, std::nullopt, workspace, QString(), &report);
  });
  return true;
}

void MediaManager::cancelImport() {
  cancel_ = true;
}

void MediaManager::runImport(const QStringList& paths, const std::map<QString, QString>& existing, bool addToTimeline, std::optional<int> index,
                             std::optional<double> at, int workspace, const QString& note, BatchReport* batch) {
  {
    std::vector<QString> ids;
    std::vector<QString> reused;  // すでに登録してあった素材（同じファイルは 1 つにまとめる）
    int created = 0;
    const int total = int(paths.size());
    int done = 0;
    QElapsedTimer sinceProgress;
    sinceProgress.start();
    if (batch) onUi([this, total] { emit importProgress(0, total); });
    for (const QString& raw : paths) {
      // まとめて読み込んでいるときは、途中でやめられる。進み具合は、1 秒に 20 回まで知らせる
      if (batch) {
        if (cancel_) {
          batch->canceled = true;
          break;
        }
        if (sinceProgress.elapsed() >= 50) {
          sinceProgress.restart();
          onUi([this, done, total] { emit importProgress(done, total); });
        }
      }
      ++done;
      const QString path = QFileInfo(raw).absoluteFilePath();
      auto it = existing.find(path);
      if (it != existing.end()) {
        ids.push_back(it->second);
        reused.push_back(it->second);
        continue;
      }
      ProbeInfo info;
      QString error;
      if (!probeMedia(path, info, error)) {
        // まとめて読み込むときは、1 件ずつ知らせず、最後にまとめて数を知らせる
        if (batch) {
          ++batch->failed;
          continue;
        }
        const QString name = QFileInfo(path).fileName();
        onUi([name, error] { editor().toast(QStringLiteral("%1: %2").arg(name, error), ToastKind::Error); });
        continue;
      }
      Asset a;
      a.id = newId();
      a.path = path;
      a.name = QFileInfo(path).fileName();
      a.kind = info.isImage ? AssetKind::Image : info.hasVideo ? AssetKind::Video : AssetKind::Audio;
      a.duration = a.kind == AssetKind::Image ? 0 : info.duration;
      a.width = info.width;
      a.height = info.height;
      a.hasAudio = a.kind == AssetKind::Video ? info.hasAudio : a.kind == AssetKind::Audio;
      a.status = AssetStatus::Loading;
      ids.push_back(a.id);
      ++created;
      if (batch) ++(a.kind == AssetKind::Audio ? batch->audio : batch->visual);
      onUi([this, a, workspace] {
        editor().addAsset(a, workspace);
        // 読み込み中にそのタブが閉じられていたら、あとの処理はしない
        if (editor().hasAsset(a.id)) prepare(a);
      });
    }
    const std::optional<BatchReport> report = batch ? std::optional<BatchReport>(*batch) : std::nullopt;
    onUi([this, ids, reused, created, addToTimeline, index, at, workspace, note, report] {
      const int target = editor().indexOfWorkspaceUid(workspace);
      // 一覧から外してあった素材（元に戻す用に情報だけ残っていたもの）を選び直したときは、一覧に戻す
      if (target >= 0) {
        for (const QString& id : reused) {
          if (const Asset* known = editor().asset(id); known && target == editor().workspaceIndex()) editor().addAsset(*known, workspace);
        }
        if (report) {
          editor().toast(batchMessage(*report, int(reused.size())), created > 0 ? ToastKind::Success : ToastKind::Info);
        } else if (!addToTimeline && !ids.empty()) {
          // 一覧に登録するだけのとき（オーディオ一覧の「追加」）は、結果を知らせる
          QString message = created == 0 ? QStringLiteral("すでに登録されています") : QStringLiteral("%1 件を登録しました").arg(created);
          if (created > 0 && !note.isEmpty()) message += QStringLiteral("。") + note;
          editor().toast(message, created == 0 ? ToastKind::Info : ToastKind::Success);
        }
      }
      if (addToTimeline && !ids.empty() && target >= 0) {
        if (target == editor().workspaceIndex()) {
          // 画像は表示サイズの確認が終わってから並べる必要はないので、そのまま追加する
          editor().addToTimeline(ids, index, at);
        } else {
          // 切り替えたあとのタブのタイムラインは勝手に変えない
          editor().toast(QStringLiteral("メディアを「%1」の一覧に追加しました").arg(editor().workspaceName(target)));
        }
      }
      if (report) {
        batchRunning_ = false;
        emit importFinished();
      }
      --pending_;
    });
  }
}

QString MediaManager::batchMessage(const BatchReport& r, int reused) {
  QStringList parts;
  const int created = r.visual + r.audio;
  if (created > 0) {
    QStringList kinds;
    if (r.visual > 0) kinds << QStringLiteral("動画・写真 %1 件").arg(r.visual);
    if (r.audio > 0) kinds << QStringLiteral("オーディオ %1 件").arg(r.audio);
    parts << QStringLiteral("フォルダから %1 件を登録しました（%2）").arg(created).arg(kinds.join(QStringLiteral("・")));
  } else if (reused > 0) {
    parts << QStringLiteral("フォルダのファイルは、すでに登録されています");
  } else if (!r.canceled) {
    parts << QStringLiteral("このフォルダには、追加できる動画・写真・オーディオがありません");
  }
  if (created > 0 && reused > 0) parts << QStringLiteral("登録済みの %1 件はそのままです").arg(reused);
  if (r.unsupported > 0) parts << QStringLiteral("対応していない形式の %1 件は飛ばしました").arg(r.unsupported);
  if (r.failed > 0) parts << QStringLiteral("読み込めなかったファイルが %1 件あります").arg(r.failed);
  if (r.truncated) parts << QStringLiteral("ファイルが多いため、最初の %1 件だけを読み込みました").arg(kMaxFolderFiles);
  if (r.canceled) parts << QStringLiteral("途中でやめました");
  return parts.join(QStringLiteral("。"));
}

void MediaManager::prepare(const Asset& a) {
  ++pending_;
  pool_.start([this, a] {
    if (a.kind == AssetKind::Image) prepareImage(a);
    else if (a.kind == AssetKind::Video) prepareVideo(a);
    else prepareAudio(a);
    onUi([this] { --pending_; });
  });
}

void MediaManager::prepareImage(const Asset& a) {
  const QString key = fileKey(a.path);
  const QString dir = cacheDir(QStringLiteral("images"));
  const bool png = QStringList{QStringLiteral("png"), QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("bmp")}.contains(
      QFileInfo(a.path).suffix().toLower());
  const QString normalized = QStringLiteral("%1/%2.%3").arg(dir, key, png ? QStringLiteral("png") : QStringLiteral("jpg"));
  QImage img;
  if (QFileInfo::exists(normalized)) img.load(normalized);
  if (img.isNull()) {
    QImageReader reader(a.path);
    reader.setAutoTransform(true);
    img = reader.read();
    if (img.isNull()) img = decodeFirstFrame(a.path, 3840);
    if (img.isNull()) {
      const QString id = a.id;
      onUi([id] {
        editor().updateAsset(id, [](Asset& x) {
          x.status = AssetStatus::Error;
          x.error = QStringLiteral("画像を読み込めません");
        });
      });
      return;
    }
    if (std::max(img.width(), img.height()) > 3840) img = img.scaled(3840, 3840, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // 透明部分は黒にする（書き出しと同じ見た目にする）
    QImage flat(img.size(), QImage::Format_RGB32);
    flat.fill(Qt::black);
    {
      QPainter p(&flat);
      p.drawImage(0, 0, img);
    }
    img = flat;
    img.save(normalized, png ? "PNG" : "JPG", png ? -1 : 93);
  }
  QImage previewImg = std::max(img.width(), img.height()) > 2048 ? img.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::SmoothTransformation) : img;
  previewImg = previewImg.convertToFormat(QImage::Format_RGB32);
  QImage thumb = img.scaledToHeight(96, Qt::SmoothTransformation);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    images_[a.id] = previewImg;
  }
  const QString id = a.id;
  const QSize size = img.size();
  onUi([this, id, normalized, size, thumb] {
    if (!editor().hasAsset(id)) {
      // 準備している間にそのタブが閉じられた
      std::lock_guard<std::mutex> lock(mutex_);
      images_.erase(id);
      return;
    }
    editor().setThumbs(id, {thumb});
    editor().updateAsset(id, [&](Asset& x) {
      x.imagePath = normalized;
      x.width = size.width();
      x.height = size.height();
      x.status = AssetStatus::Ready;
    });
  });
}

void MediaManager::prepareVideo(const Asset& a) {
  const QString id = a.id;
  onUi([id] { editor().updateAsset(id, [](Asset& x) { x.status = AssetStatus::Ready; }); });
  const int count = std::clamp(int(std::lround(a.duration / 3)), 6, 30);
  const QString dir = cacheDir(QStringLiteral("thumbs/") + fileKey(a.path));
  std::vector<QImage> thumbs;
  for (int i = 0; i < count; ++i) {
    QImage img(QStringLiteral("%1/%2-%3.jpg").arg(dir).arg(count).arg(i));
    if (img.isNull()) {
      thumbs.clear();
      break;
    }
    thumbs.push_back(img);
  }
  if (thumbs.empty()) {
    thumbs = videoThumbnails(a.path, a.duration, count);
    for (size_t i = 0; i < thumbs.size(); ++i) thumbs[i].save(QStringLiteral("%1/%2-%3.jpg").arg(dir).arg(count).arg(i), "JPG", 80);
  }
  onUi([id, thumbs] { editor().setThumbs(id, thumbs); });
}

void MediaManager::prepareAudio(const Asset& a) {
  const QString id = a.id;
  onUi([id] { editor().updateAsset(id, [](Asset& x) { x.status = AssetStatus::Ready; }); });
  const QString file = cacheDir(QStringLiteral("waves")) + QLatin1Char('/') + fileKey(a.path) + QStringLiteral(".bin");
  Waveform w;
  {
    QFile f(file);
    if (f.open(QIODevice::ReadOnly)) {
      QDataStream ds(&f);
      qint32 pps = 0;
      quint32 n = 0;
      ds >> pps >> n;
      if (pps > 0 && n < 10'000'000) {
        w.peaksPerSecond = pps;
        w.peaks.resize(n);
        for (auto& v : w.peaks) ds >> v;
        if (ds.status() != QDataStream::Ok) w = {};
      }
    }
  }
  if (w.peaks.empty()) {
    w = audioWaveform(a.path, a.duration);
    QFile f(file);
    if (!w.peaks.empty() && f.open(QIODevice::WriteOnly)) {
      QDataStream ds(&f);
      ds << qint32(w.peaksPerSecond) << quint32(w.peaks.size());
      for (float v : w.peaks) ds << v;
    }
  }
  onUi([id, w] { editor().setWaveform(id, w); });
}

QImage MediaManager::previewImage(const QString& assetId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = images_.find(assetId);
  return it == images_.end() ? QImage() : it->second;
}

/** メディアのファイルがあるか調べて状態を決める。見つからない数を返す */
static int markAssetStatus(WorkspaceData& wd) {
  int missing = 0;
  for (Asset& a : wd.assets) {
    const bool exists = QFileInfo(a.path).isFile();
    if (!exists) ++missing;
    a.status = exists ? AssetStatus::Loading : AssetStatus::Missing;
  }
  return missing;
}

bool MediaManager::openProject(const QString& path) {
  const int already = editor().workspaceWithPath(path);
  if (already >= 0) {
    editor().switchWorkspace(already);
    editor().toast(QStringLiteral("「%1」はすでに開いています").arg(editor().workspaceName(already)));
    return true;
  }
  QFile f(path);
  QString error;
  if (!f.open(QIODevice::ReadOnly)) {
    editor().toast(QStringLiteral("開けませんでした: %1").arg(f.errorString()), ToastKind::Error);
    return false;
  }
  auto file = parseProject(f.readAll(), &error);
  if (!file) {
    editor().toast(QStringLiteral("開けませんでした: %1").arg(error), ToastKind::Error);
    return false;
  }
  int missing = 0;
  const bool single = file->workspaces.size() == 1;
  for (WorkspaceData& wd : file->workspaces) {
    missing += markAssetStatus(wd);
    wd.dirty = false;
    // 保存したファイルは 1 つのプロジェクト。タブにはファイル名を出す
    wd.path = single ? path : QString();
    if (single) wd.name = QFileInfo(path).completeBaseName();
  }
  for (WorkspaceData& wd : file->workspaces) {
    editor().openWorkspace(wd);  // ID は付け直されるので、読み込みは付け直したあとのメディアで行う
    for (const auto& a : wd.assets) {
      if (a.status != AssetStatus::Missing) prepare(a);
    }
  }
  if (missing > 0) editor().toast(QStringLiteral("%1 件のファイルが見つかりません").arg(missing), ToastKind::Error);
  return true;
}

void MediaManager::loadSession(const ProjectFile& file) {
  editor().pause();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    images_.clear();
  }
  ProjectFile loaded = file;
  int missing = 0;
  for (auto& ws : loaded.workspaces) missing += markAssetStatus(ws);
  editor().resetSession(loaded);
  for (const auto& ws : loaded.workspaces) {
    for (const auto& a : ws.assets) {
      if (a.status != AssetStatus::Missing) prepare(a);
    }
  }
  if (missing > 0) editor().toast(QStringLiteral("%1 件のファイルが見つかりません").arg(missing), ToastKind::Error);
}

}  // namespace kv
