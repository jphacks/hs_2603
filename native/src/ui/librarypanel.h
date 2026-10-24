#pragma once

#include <QListWidget>
#include <QWidget>

namespace kv {

inline constexpr const char* kAssetMime = "application/x-kantan-asset";

/**
 * 読み込んだ素材の一覧（タイムラインへドラッグできる）。メディア用とオーディオ用で、同じ仕組みを使う:
 * 一覧に載った素材は、タイムラインから消しても残り、何度でも追加できる。
 */
class AssetList : public QListWidget {
  Q_OBJECT
 public:
  /** Media: 動画と写真（サムネイルのカード）  /  Audio: 音声（1 行ずつのカード。試聴ボタン付き） */
  enum class Kind { Media, Audio };
  explicit AssetList(Kind kind = Kind::Media, QWidget* parent = nullptr);
  void rebuild();
  Kind kind() const { return kind_; }

 protected:
  QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
  QStringList mimeTypes() const override;
  void contextMenuEvent(QContextMenuEvent* e) override;
  void mousePressEvent(QMouseEvent* e) override;
  void paintEvent(QPaintEvent* e) override;

 private:
  /** 一覧から外す。タイムラインで使っている素材は外さず、理由を知らせる */
  void removeFromList(const QString& id);
  Kind kind_;
};

class LibraryPanel : public QWidget {
  Q_OBJECT
 public:
  explicit LibraryPanel(QWidget* parent = nullptr);
  /**
   * 素材を選ぶダイアログを開いて読み込む。audioOnly ならオーディオだけを選べる。
   * addToTimeline が true なら、読み込んだものをそのままタイムラインに置く（オーディオは再生位置から）。
   * false なら一覧に登録するだけ
   */
  static void pickAndImport(QWidget* parent, bool audioOnly = false, bool addToTimeline = true);

 private:
  AssetList* list_;
  AssetList* audioList_;
};

}  // namespace kv
