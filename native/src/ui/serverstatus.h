#pragma once
// 右下の「AI サーバー」の表示（つながっているか・ログインしている人）。
// 押すと、ログインしていなければログインの画面、ログインしていればアカウントのメニューを出す。
// アドレス（.env の KANTAN_AI_SERVER）は出さない。決まっていなければ、.env の場所を出す。

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QToolButton>

class QMenu;

namespace kv {

/** つながりの丸の色と、短い説明（右下の表示とログインの画面で使う） */
struct LinkLook {
  QColor color;
  QString text;
};
/** 今のつながりとログインの状態 */
LinkLook currentLinkLook();
/** 色の付いた丸 */
QPixmap linkDot(const QColor& color, qreal dpr);

class ServerStatus : public QToolButton {
  Q_OBJECT
 public:
  explicit ServerStatus(QWidget* parent = nullptr);

 private:
  void refresh();
  void onClicked();
  /** 確かめ直した答えを、通知で知らせる */
  void announce();
  void popupMenu();
  QMenu* menu_;
  /** 「確かめ直す」「読み直す」を押して、答えを待っている */
  bool announce_ = false;
};

}  // namespace kv
