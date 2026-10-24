#pragma once
// 編集画面の上に置く、編集場所（タブ）の切り替えバー。
// 「＋」で増やし、「×」で閉じ、ダブルクリックで名前を変える。タブが増えたら横にスクロールできる。

#include <QWidget>

class QLineEdit;
class QTabBar;
class QToolButton;

namespace kv {

class WorkspaceBar : public QWidget {
  Q_OBJECT
 public:
  explicit WorkspaceBar(QWidget* parent = nullptr);

 signals:
  /** 閉じたいタブがある。未保存の確認などは受け取る側で行う */
  void closeRequested(int index);

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override;

 private:
  /** Editor の編集場所の一覧に、タブの表示を合わせる */
  void sync();
  void beginRename(int index);
  void finishRename();
  void cancelRename();
  void showTabMenu(const QPoint& pos);

  QTabBar* tabs_;
  QToolButton* add_;
  QLineEdit* renameEdit_;
  /** 名前を変えている最中のタブ（閉じても変わらない ID）。変えていなければ -1 */
  int renamingUid_ = -1;
};

}  // namespace kv
