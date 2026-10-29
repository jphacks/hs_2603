#include "ui/shortcutsdialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <map>

#include "ui/keymap.h"
#include "ui/theme.h"

namespace kv {

namespace {

constexpr int kIdRole = Qt::UserRole + 1;

/** 変えられない操作（マウスと組み合わせるもの・数値欄の中のもの） */
struct FixedRow {
  const char* label;
  const char* keys;
};
const FixedRow kFixedRows[] = {
    {"数値欄の調整（選んでいるとき）", "↑ → 増 / ↓ ← 減"},
    {"数値欄の調整（カーソルを合わせて）", "ホイール"},
    {"数値の調整量を10倍 / 0.1倍", "Shift / Alt"},
    {"タイムラインの拡大・縮小", "Ctrl+ホイール"},
    {"吸い付かせずに動かす", "Alt+ドラッグ"},
    {"プレビューの全画面表示を終える（全画面表示中）", "Esc / F / F11"},
    {"プレビューで文字を直接編集", "文字をダブルクリック"},
    {"文字の直接編集を確定 / 取り消す（編集中）", "Ctrl+Enter（外側をクリックも）/ Esc"},
    {"フェードの長さを変える", "バーの上の角の ● をドラッグ"},
    {"書き出す範囲を決める", "目盛りを Shift+ドラッグ"},
    {"テキストの段を変える", "テキストを上下にドラッグ"},
    {"重ねる映像の段（前・後ろ）を変える", "重ねるトラックの映像を上下にドラッグ"},
    {"いくつか一緒に選ぶ（テキスト・オーディオ・重ねる映像）", "Ctrl+クリック"},
    {"映像の幅・高さだけを変える", "プレビューの枠の辺の ● をドラッグ"},
    {"タイムラインの段の高さを変える", "左の見出しの段の境目をドラッグ"},
    {"トランジション・エフェクトを付ける", "左の「エフェクト」タブからドラッグ"},
    {"トランジションの長さを変える", "切れ目の帯の端をドラッグ"},
    {"マーカーを動かす / 名前・色・メモを編集", "目盛りの印をドラッグ / ダブルクリック"},
};

/** ほかの操作で使っているキーなら、その操作の id（無ければ空） */
QString ownerOf(const QKeySequence& key, const QString& except) {
  for (const keymap::Command& c : keymap::commands()) {
    if (c.id != except && keymap::keys(c.id).contains(key)) return c.id;
  }
  return {};
}

/** 1つの操作のキーを変える画面。OK で保存したら true */
bool editKeys(QWidget* parent, const QString& id) {
  const keymap::Command* cmd = keymap::find(id);
  if (!cmd) return false;
  QDialog dlg(parent);
  dlg.setWindowTitle(QStringLiteral("キーを変更"));
  dlg.setMinimumWidth(380);
  auto* v = new QVBoxLayout(&dlg);
  auto* title = new QLabel(cmd->label, &dlg);
  QFont f = title->font();
  f.setBold(true);
  title->setFont(f);
  v->addWidget(title);

  auto* list = new QListWidget(&dlg);
  list->setMinimumHeight(110);
  v->addWidget(list);
  QList<QKeySequence> keys = keymap::keys(id);
  // ほかの操作から外して、こちらに付けたキー（OK のときに、ほかの操作から外す）
  std::map<QString, QList<QKeySequence>> taken;
  auto refreshList = [&] {
    list->clear();
    for (const QKeySequence& k : keys) list->addItem(keymap::text({k}));
    if (keys.isEmpty()) {
      auto* none = new QListWidgetItem(QStringLiteral("（キーを割り当てていません）"), list);
      none->setFlags(Qt::NoItemFlags);
      none->setForeground(theme::dim);
    }
  };
  refreshList();

  auto* addRow = new QHBoxLayout();
  addRow->addWidget(new QLabel(QStringLiteral("追加するキー:"), &dlg));
  auto* recorder = new QKeySequenceEdit(&dlg);
  recorder->setMaximumSequenceLength(1);
  recorder->setClearButtonEnabled(true);
  recorder->setToolTip(QStringLiteral("ここを選んで、割り当てたいキーを押してください"));
  addRow->addWidget(recorder, 1);
  v->addLayout(addRow);
  auto* hint = new QLabel(QStringLiteral("Ctrl・Alt を使わないキー（S や Space など）は、文字を入力している間は効きません"), &dlg);
  hint->setWordWrap(true);
  hint->setStyleSheet(QStringLiteral("color:%1").arg(theme::muted.name()));
  v->addWidget(hint);

  auto* row = new QHBoxLayout();
  auto* removeBtn = new QPushButton(QStringLiteral("選んだキーを外す"), &dlg);
  auto* defaultBtn = new QPushButton(QStringLiteral("既定に戻す"), &dlg);
  row->addWidget(removeBtn);
  row->addWidget(defaultBtn);
  row->addStretch(1);
  v->addLayout(row);
  removeBtn->setEnabled(false);
  QObject::connect(list, &QListWidget::currentRowChanged, &dlg, [&](int r) { removeBtn->setEnabled(r >= 0 && r < keys.size()); });

  QObject::connect(recorder, &QKeySequenceEdit::editingFinished, &dlg, [&] {
    const QKeySequence k = keymap::normalize(recorder->keySequence());
    recorder->clear();
    if (k.isEmpty()) return;
    if (keys.contains(k)) {
      list->setCurrentRow(int(keys.indexOf(k)));
      list->setFocus();
      return;
    }
    const QString owner = ownerOf(k, id);
    if (!owner.isEmpty()) {
      const QString ownerLabel = keymap::find(owner)->label;
      const auto r = QMessageBox::question(&dlg, QStringLiteral("キーを変更"),
                                           QStringLiteral("%1 は「%2」に割り当てられています。\n「%2」から外して、「%3」に割り当てますか？")
                                               .arg(keymap::text({k}), ownerLabel, cmd->label));
      if (r != QMessageBox::Yes) {
        recorder->setFocus();
        return;
      }
      taken[owner] << k;
    }
    keys << k;
    refreshList();
    list->setCurrentRow(int(keys.size()) - 1);
    list->setFocus();
  });
  QObject::connect(removeBtn, &QPushButton::clicked, &dlg, [&] {
    const int r = list->currentRow();
    if (r < 0 || r >= keys.size()) return;
    keys.removeAt(r);
    refreshList();
  });
  QObject::connect(defaultBtn, &QPushButton::clicked, &dlg, [&] {
    // 既定のキーをほかの操作で使っていたら、尋ねてからそちらを外す
    QList<QKeySequence> next;
    for (const QKeySequence& k : cmd->defaults) {
      const QString owner = ownerOf(k, id);
      if (!owner.isEmpty() && !taken[owner].contains(k)) {
        const auto r = QMessageBox::question(&dlg, QStringLiteral("既定に戻す"),
                                             QStringLiteral("%1 は「%2」に割り当てられています。\n「%2」から外して、既定に戻しますか？")
                                                 .arg(keymap::text({k}), keymap::find(owner)->label));
        if (r != QMessageBox::Yes) return;
        taken[owner] << k;
      }
      next << k;
    }
    keys = next;
    refreshList();
  });

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("OK"));
  buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("キャンセル"));
  // Enter・Esc は、キーの記録に使えるように、ボタンには付けない
  buttons->button(QDialogButtonBox::Ok)->setAutoDefault(false);
  buttons->button(QDialogButtonBox::Cancel)->setAutoDefault(false);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  v->addWidget(buttons);
  recorder->setFocus();

  if (dlg.exec() != QDialog::Accepted) return false;
  for (const auto& [owner, ks] : taken) {
    QList<QKeySequence> rest = keymap::keys(owner);
    bool touched = false;
    for (const QKeySequence& k : ks) {
      // 付けたあとで外したキーは、もとの操作に残す
      if (keys.contains(k)) touched |= rest.removeAll(k) > 0;
    }
    if (touched) keymap::setKeys(owner, rest);
  }
  keymap::setKeys(id, keys);
  return true;
}

}  // namespace

ShortcutsDialog::ShortcutsDialog(const QStringList& hiddenIds, QWidget* parent) : QDialog(parent), hiddenIds_(hiddenIds) {
  setWindowTitle(QStringLiteral("キーボードショートカット"));
  setMinimumWidth(520);
  auto* v = new QVBoxLayout(this);

  search_ = new QLineEdit(this);
  search_->setPlaceholderText(QStringLiteral("操作の名前やキーで探す"));
  search_->setClearButtonEnabled(true);
  v->addWidget(search_);

  table_ = new QTableWidget(this);
  table_->setColumnCount(3);
  table_->horizontalHeader()->hide();
  table_->verticalHeader()->hide();
  table_->setShowGrid(false);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setStyleSheet(QStringLiteral("QTableWidget{background:transparent;border:none}QTableWidget::item{border-bottom:1px solid #2a2a2a;padding:4px}"));
  table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->setMinimumHeight(520);
  v->addWidget(table_, 1);

  auto* hint = new QLabel(QStringLiteral("行をダブルクリックすると、キーを変えられます。1つの操作に、いくつでもキーを割り当てられます"), this);
  hint->setWordWrap(true);
  hint->setStyleSheet(QStringLiteral("color:%1").arg(theme::muted.name()));
  v->addWidget(hint);

  auto* row = new QHBoxLayout();
  editButton_ = new QPushButton(QStringLiteral("キーを変更..."), this);
  auto* resetAll = new QPushButton(QStringLiteral("すべて既定に戻す"), this);
  auto* close = new QPushButton(QStringLiteral("閉じる"), this);
  editButton_->setEnabled(false);
  editButton_->setAutoDefault(false);
  resetAll->setAutoDefault(false);
  row->addWidget(editButton_);
  row->addWidget(resetAll);
  row->addStretch(1);
  row->addWidget(close);
  v->addLayout(row);

  connect(search_, &QLineEdit::textChanged, this, &ShortcutsDialog::applyFilter);
  connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { editButton_->setEnabled(!selectedId().isEmpty()); });
  connect(table_, &QTableWidget::cellDoubleClicked, this, [this] { editSelected(); });
  connect(editButton_, &QPushButton::clicked, this, &ShortcutsDialog::editSelected);
  connect(close, &QPushButton::clicked, this, &QDialog::accept);
  connect(resetAll, &QPushButton::clicked, this, [this] {
    const auto r = QMessageBox::question(this, QStringLiteral("すべて既定に戻す"), QStringLiteral("すべての操作のキーを、既定に戻しますか？"));
    if (r != QMessageBox::Yes) return;
    keymap::resetAll();
    rebuild();
    emit changed();
  });
  rebuild();
}

void ShortcutsDialog::rebuild() {
  const QString keep = selectedId();
  table_->setRowCount(0);
  auto addRow = [this](const QString& category, const QString& label, const QString& keys, const QString& id, bool changed) {
    const int r = table_->rowCount();
    table_->insertRow(r);
    auto* cat = new QTableWidgetItem(category);
    cat->setForeground(theme::dim);
    auto* name = new QTableWidgetItem(label);
    auto* k = new QTableWidgetItem(keys.isEmpty() ? QStringLiteral("なし") : keys);
    k->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // 既定から変えたキーは目立たせる
    k->setForeground(changed ? theme::accent : (keys.isEmpty() ? theme::dim : theme::muted));
    for (QTableWidgetItem* it : {cat, name, k}) {
      it->setData(kIdRole, id);
      if (id.isEmpty()) it->setFlags(Qt::ItemIsEnabled);  // 固定の行は選べない
    }
    table_->setItem(r, 0, cat);
    table_->setItem(r, 1, name);
    table_->setItem(r, 2, k);
  };
  for (const keymap::Command& c : keymap::commands()) {
    if (hiddenIds_.contains(c.id)) continue;
    addRow(c.category, c.label, keymap::text(keymap::keys(c.id)), c.id, !keymap::isDefault(c.id));
  }
  for (const FixedRow& f : kFixedRows) {
    addRow(QStringLiteral("固定"), QString::fromUtf8(f.label), QString::fromUtf8(f.keys), {}, false);
  }
  for (int r = 0; r < table_->rowCount(); ++r) {
    if (!keep.isEmpty() && table_->item(r, 0)->data(kIdRole).toString() == keep) table_->selectRow(r);
  }
  applyFilter();
}

void ShortcutsDialog::applyFilter() {
  const QString q = search_->text().trimmed();
  for (int r = 0; r < table_->rowCount(); ++r) {
    bool hit = q.isEmpty();
    for (int c = 0; c < table_->columnCount() && !hit; ++c) hit = table_->item(r, c)->text().contains(q, Qt::CaseInsensitive);
    table_->setRowHidden(r, !hit);
  }
}

QString ShortcutsDialog::selectedId() const {
  const QList<QTableWidgetItem*> items = table_->selectedItems();
  return items.isEmpty() ? QString() : items.first()->data(kIdRole).toString();
}

void ShortcutsDialog::editSelected() {
  const QString id = selectedId();
  if (id.isEmpty()) return;
  if (!editKeys(this, id)) return;
  rebuild();
  emit changed();
}

}  // namespace kv
