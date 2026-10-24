#pragma once

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QPushButton;
class QTableWidget;

namespace kv {

/**
 * キーボードショートカットの一覧。行を選んで、その操作のキーを変えられる（1つの操作に、いくつでも割り当てられる）。
 * マウスと組み合わせる操作と、数値欄の中の操作は、変えられない（一覧に「固定」として載せる）
 */
class ShortcutsDialog : public QDialog {
  Q_OBJECT
 public:
  /** hiddenIds: 一覧に出さない操作（AI を使えないときの AI アシスタントなど） */
  explicit ShortcutsDialog(const QStringList& hiddenIds, QWidget* parent = nullptr);

 signals:
  /** 割り当てを変えて保存した */
  void changed();

 private:
  void rebuild();
  void applyFilter();
  void editSelected();
  QString selectedId() const;

  QStringList hiddenIds_;
  QLineEdit* search_;
  QTableWidget* table_;
  QPushButton* editButton_;
};

}  // namespace kv
