#include "ui/markerdialog.h"

#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/editor.h"
#include "core/timeline.h"
#include "ui/widgets.h"

namespace kv {

const std::vector<MarkerColor>& markerColors() {
  static const std::vector<MarkerColor> list = {
      {QColor(0x3f, 0xb6, 0x7b), QStringLiteral("緑")},     {QColor(0xe5, 0x48, 0x4d), QStringLiteral("赤")},
      {QColor(0xa8, 0x6c, 0xe6), QStringLiteral("紫")},     {QColor(0xe9, 0x8a, 0x2b), QStringLiteral("オレンジ")},
      {QColor(0xf2, 0xd0, 0x3b), QStringLiteral("黄")},     {QColor(0xe8, 0xe8, 0xe8), QStringLiteral("白")},
      {QColor(0x2f, 0x86, 0xeb), QStringLiteral("青")},     {QColor(0x2c, 0xc8, 0xd8), QStringLiteral("シアン")},
  };
  return list;
}

/** 色の丸い見本（メニュー・ボタンのアイコン） */
static QIcon colorDot(const QColor& c, int size = 16) {
  QPixmap pm(size * 2, size * 2);
  pm.setDevicePixelRatio(2);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(0, 0, 0, 140), 1));
  p.setBrush(c);
  p.drawEllipse(QRectF(1.5, 1.5, size - 3, size - 3));
  return QIcon(pm);
}

bool execMarkerDialog(QWidget* parent, const QString& markerId) {
  Editor& ed = editor();
  const Marker* found = ed.marker(markerId);
  if (!found) return false;
  const Marker orig = *found;
  ed.pause();

  QDialog dlg(parent ? parent->window() : nullptr);
  dlg.setWindowTitle(QStringLiteral("マーカー"));
  dlg.setMinimumWidth(380);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(8);
  v->addWidget(makeLabel(QStringLiteral("位置: %1").arg(formatTime(orig.t)), "muted", &dlg));

  v->addWidget(makeLabel(QStringLiteral("名前"), "muted", &dlg));
  auto* name = new QLineEdit(orig.name, &dlg);
  name->setPlaceholderText(QStringLiteral("例: サビの始まり"));
  name->setAccessibleName(QStringLiteral("マーカーの名前"));
  v->addWidget(name);

  v->addWidget(makeLabel(QStringLiteral("色"), "muted", &dlg));
  auto* colors = new QWidget(&dlg);
  auto* ch = new QHBoxLayout(colors);
  ch->setContentsMargins(0, 0, 0, 0);
  ch->setSpacing(4);
  auto* group = new QButtonGroup(&dlg);
  group->setExclusive(true);
  int checked = -1;
  const auto& list = markerColors();
  for (int i = 0; i < int(list.size()); ++i) {
    auto* b = new QToolButton(colors);
    b->setCheckable(true);
    b->setIcon(colorDot(list[size_t(i)].color, 18));
    b->setIconSize(QSize(18, 18));
    b->setToolTip(list[size_t(i)].name);
    b->setAccessibleName(QStringLiteral("色: %1").arg(list[size_t(i)].name));
    group->addButton(b, i);
    ch->addWidget(b);
    if (list[size_t(i)].color == orig.color) checked = i;
  }
  ch->addStretch(1);
  // 選択肢に無い色（以前の版・ほかの所で付けた色）は、どれも選ばずにそのまま残す
  if (checked >= 0) group->button(checked)->setChecked(true);
  v->addWidget(colors);

  v->addWidget(makeLabel(QStringLiteral("メモ"), "muted", &dlg));
  auto* comment = new QPlainTextEdit(orig.comment, &dlg);
  comment->setPlaceholderText(QStringLiteral("あとで見返すときのメモ（書き出す動画には出ません）"));
  comment->setAccessibleName(QStringLiteral("マーカーのメモ"));
  comment->setTabChangesFocus(true);
  comment->setFixedHeight(84);
  v->addWidget(comment);

  auto* buttons = new QDialogButtonBox(&dlg);
  auto* remove = buttons->addButton(QStringLiteral("削除"), QDialogButtonBox::DestructiveRole);
  auto* ok = buttons->addButton(QStringLiteral("OK"), QDialogButtonBox::AcceptRole);
  ok->setProperty("primary", true);
  ok->setDefault(true);
  buttons->addButton(QStringLiteral("キャンセル"), QDialogButtonBox::RejectRole);
  v->addWidget(buttons);
  bool removed = false;
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QObject::connect(remove, &QPushButton::clicked, &dlg, [&] {
    removed = true;
    dlg.reject();
  });
  name->setFocus();
  name->selectAll();

  const int result = dlg.exec();
  if (removed) {
    editor().removeMarker(markerId);
    return true;
  }
  if (result != QDialog::Accepted) return false;
  Marker next = orig;
  next.name = name->text().trimmed();
  next.comment = comment->toPlainText().trimmed();
  if (group->checkedId() >= 0) next.color = list[size_t(group->checkedId())].color;
  // 変えていなければ、履歴を増やさない
  if (next == orig) return false;
  editor().updateMarker(markerId, [&](Marker& m) {
    m.name = next.name;
    m.comment = next.comment;
    m.color = next.color;
  });
  return false;
}

void addMarkerColorMenu(QMenu* menu, const QString& markerId) {
  const Marker* m = editor().marker(markerId);
  if (!m) return;
  QMenu* sub = menu->addMenu(QStringLiteral("色"));
  auto* group = new QActionGroup(sub);
  for (const MarkerColor& c : markerColors()) {
    QAction* a = sub->addAction(colorDot(c.color), c.name);
    a->setCheckable(true);
    a->setChecked(c.color == m->color);
    group->addAction(a);
    const QColor col = c.color;
    QObject::connect(a, &QAction::triggered, sub, [markerId, col] {
      const Marker* cur = editor().marker(markerId);
      if (!cur || cur->color == col) return;
      editor().updateMarker(markerId, [&](Marker& x) { x.color = col; });
    });
  }
}

}  // namespace kv
