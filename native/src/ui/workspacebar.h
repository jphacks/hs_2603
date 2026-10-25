#pragma once
// 編集画面の上に置く、編集場所（タブ）の切り替えバー。
// 「＋」で増やし、「×」で閉じ、ダブルクリックで名前を変える。タブが増えたら横にスクロールできる。

#include <QTimer>
#include <QWidget>

class QLineEdit;
class QTabBar;
class QToolButton;

namespace kv {

class WorkspaceBar : public QWidget {
  Q_OBJECT
 public:
  explicit WorkspaceBar(QWidget* parent = nullptr);
  /** 画面の位置 global にあるタブ（無ければ -1） */
  int tabAtGlobal(const QPoint& global) const;

 signals:
  /** 閉じたいタブがある。未保存の確認などは受け取る側で行う */
  void closeRequested(int index);

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override;
  // タイムラインのもの・素材をドラッグしてタブの上で少し止めると、そのタブに切り替わる（そのまま下へ動かして、置く所を決める）
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dragMoveEvent(QDragMoveEvent* e) override;
  void dragLeaveEvent(QDragLeaveEvent* e) override;
  void dropEvent(QDropEvent* e) override;

 private:
  /** ドラッグしているものの上にあるタブを見て、切り替えの待ち時間を始める・やめる */
  void hoverTab(const QPoint& pos);
  /** 切り替えを待っているタブ（無ければ -1）と、その待ち時間 */
  int hoverIndex_ = -1;
  QTimer hoverTimer_;
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
