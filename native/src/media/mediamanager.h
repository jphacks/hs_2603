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

  /**
   * ファイルを読み込む（.kvproj ならプロジェクトとして開く）。note は、一覧に登録するだけのときの「登録しました」の知らせに
   * 添える一言（Web の画像の著作権の注意など）
   */
  void importPaths(const QStringList& paths, bool addToTimeline = true, std::optional<int> index = std::nullopt,
                   std::optional<double> at = std::nullopt, const QString& note = QString());
  /**
   * フォルダの中の、拡張子が exts のファイル（小文字）をまとめて一覧に登録する（タイムラインには置かない）。動画・写真はメディアの一覧、
   * 音声はオーディオの一覧に入る。recursive ならサブフォルダの中も。探すのも読み込むのも裏で行い、進み具合は importProgress で知らせる。
   * 対応していない形式・読み込めないファイルは飛ばし、最後にまとめて知らせる。すでにまとめて読み込んでいる途中なら false
   */
  bool importFolder(const QString& dir, bool recursive, const QStringList& exts);
  /** まとめての読み込みをやめる（そこまでに登録したものは残る） */
  void cancelImport();
  /** フォルダからまとめて読み込んでいる途中か */
  bool batchRunning() const { return batchRunning_; }
  /** フォルダから一度に読み込むファイルの数の上限 */
  static constexpr int kMaxFolderFiles = 2000;
  void prepare(const Asset& a);
  /** プロジェクトを新しいタブで開く。すでに開いているファイルなら、そのタブへ切り替える */
  bool openProject(const QString& path);
  /** 前回の編集内容（全タブ）を復元する */
  void loadSession(const ProjectFile& file);
  /** 画像クリップのプレビュー用画像 */
  QImage previewImage(const QString& assetId) const;
  /**
   * アプリが作った画像（フレームの保持・カラーマット）を PNG にして、今の編集場所のメディアの一覧に入れる（タイムラインには置かない）。
   * ファイルは、アプリのデータのフォルダ（generated。キャッシュとは別で、消さない）に置く。足したメディアの ID（書けなければ空）
   */
  QString addGeneratedImage(const QImage& image, const QString& name);
  /**
   * アプリが作った動画（逆再生の動画など）を、今の編集場所のメディアの一覧に入れる（タイムラインには置かない）。
   * 同じファイルがもう一覧にあれば、その ID。足したメディアの ID（読めなければ空）
   */
  QString addGeneratedVideo(const QString& path, const QString& name);
  /** 読み込み中のメディアがあるか */
  bool busy() const;
  void waitForIdle(int msecs);

  static QString cacheDir(const QString& sub);
  static QString fileKey(const QString& path);

 signals:
  /** フォルダからまとめて読み込んでいる途中（done / total 件。total が 0 のときは、ファイルを探している途中） */
  void importProgress(int done, int total);
  /** まとめての読み込みが終わった（やめたときも） */
  void importFinished();

 private:
  MediaManager();
  /** フォルダからまとめて読み込んだ結果（最後にまとめて知らせる） */
  struct BatchReport {
    int found = 0;        // 対応している形式のファイル
    int unsupported = 0;  // 対応していない形式で、飛ばしたファイル
    int failed = 0;       // 読み込めなかったファイル
    int visual = 0;       // 新しく登録した動画・写真
    int audio = 0;        // 新しく登録したオーディオ
    bool truncated = false;
    bool canceled = false;
  };
  /** 裏の処理で、ファイルを 1 つずつ調べて登録する（batch があれば、まとめての読み込み） */
  void runImport(const QStringList& paths, const std::map<QString, QString>& existing, bool addToTimeline, std::optional<int> index,
                 std::optional<double> at, int workspace, const QString& note, BatchReport* batch);
  static QString batchMessage(const BatchReport& r, int reused);
  void prepareImage(const Asset& a);
  void prepareVideo(const Asset& a);
  void prepareAudio(const Asset& a);

  QThreadPool pool_;
  mutable std::mutex mutex_;
  std::map<QString, QImage> images_;
  std::atomic<int> pending_{0};
  std::atomic<bool> cancel_{false};
  bool batchRunning_ = false;
};

inline MediaManager& media() {
  return MediaManager::instance();
}

}  // namespace kv
