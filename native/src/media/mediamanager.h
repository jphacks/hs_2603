#pragma once
// メディアの読み込み（解析・サムネイル・波形・画像の向きの補正）とプロジェクトを開く処理

#include <QImage>
#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <map>
#include <mutex>
#include <optional>

#include "core/model.h"
#include "core/projectio.h"

namespace kv {

class MediaManager : public QObject {
  Q_OBJECT
 public:
  static MediaManager& instance();

  /** ファイルを読み込む（.kvproj ならプロジェクトとして開く） */
  void importPaths(const QStringList& paths, bool addToTimeline = true, std::optional<int> index = std::nullopt,
                   std::optional<double> at = std::nullopt);
  void prepare(const Asset& a);
  /** プロジェクトを新しいタブで開く。すでに開いているファイルなら、そのタブへ切り替える */
  bool openProject(const QString& path);
  /** 前回の編集内容（全タブ）を復元する */
  void loadSession(const ProjectFile& file);
  /** 画像クリップのプレビュー用画像 */
  QImage previewImage(const QString& assetId) const;
  /** 読み込み中のメディアがあるか */
  bool busy() const;
  void waitForIdle(int msecs);

  static QString cacheDir(const QString& sub);
  static QString fileKey(const QString& path);

 private:
  MediaManager();
  void prepareImage(const Asset& a);
  void prepareVideo(const Asset& a);
  void prepareAudio(const Asset& a);

  QThreadPool pool_;
  mutable std::mutex mutex_;
  std::map<QString, QImage> images_;
  std::atomic<int> pending_{0};
};

inline MediaManager& media() {
  return MediaManager::instance();
}

}  // namespace kv
