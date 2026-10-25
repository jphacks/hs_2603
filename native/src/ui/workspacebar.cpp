#include "ui/workspacebar.h"

#include <QAction>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QSignalBlocker>
#include <QTabBar>
#include <QToolButton>
#include <algorithm>

#include "core/editor.h"
#include "core/textstyles.h"
#include "ui/objectmenu.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

namespace {

/**
 * QTabBar は、スクロールの矢印ぶんの幅（約 140px）より小さくならないことがあり、タブが 1 つだけのとき
 * 実際のタブより広い領域を取って「＋」が離れてしまう。タブが収まるあいだは、タブの幅ぴったりまで縮められるようにする。
 * 収まらなくなったら、これまでどおり矢印つきでスクロールする
 */
class FittedTabBar : public QTabBar {
 public:
  using QTabBar::QTabBar;
  QSize minimumSizeHint() const override {
    const QSize base = QTabBar::minimumSizeHint();
    return QSize(std::min(base.width(), sizeHint().width()), base.height());
  }
};

}  // namespace

WorkspaceBar::WorkspaceBar(QWidget* parent) : QWidget(parent) {
  setObjectName(QStringLiteral("WorkspaceBar"));
  setStyleSheet(QStringLiteral("#WorkspaceBar{background:#1b1b1b;border-bottom:1px solid #2a2a2a}"));
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(8, 4, 8, 0);
  // タブ同士の間（2px）と合わせて、最後のタブと「＋」の間は 6px ほどになる
  h->setSpacing(4);

  tabs_ = new FittedTabBar(this);
  tabs_->setExpanding(false);
  tabs_->setDrawBase(false);
  tabs_->setMovable(false);
  tabs_->setDocumentMode(true);
  tabs_->setUsesScrollButtons(true);  // タブが増えたら横にスクロールする
  tabs_->setElideMode(Qt::ElideRight);  // 名前が長いときは省略する
  tabs_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);  // 必要な幅まで。足りなければ縮んでスクロール
  tabs_->setAccessibleName(QStringLiteral("編集場所のタブ"));
  tabs_->setContextMenuPolicy(Qt::CustomContextMenu);
  // 選んでいるタブは、文字を明るくして背景と青い下線で強調する。幅は狭すぎず広すぎないようにする
  tabs_->setStyleSheet(QStringLiteral(
      "QTabBar::tab{background:transparent;color:#9b9b9b;padding:5px 6px 4px 12px;margin-right:2px;"
      "border:none;border-bottom:2px solid transparent;border-top-left-radius:5px;border-top-right-radius:5px;"
      "min-width:72px;max-width:190px}"
      "QTabBar::tab:hover{color:#e6e6e6;background:#222222}"
      "QTabBar::tab:selected{color:#ffffff;background:#2c2c2c;border-bottom:2px solid #2680eb}"));
  // タブと「＋」を同じ横並びのレイアウトに入れる。タブの領域は引き伸ばさず、タブの幅ぶんだけ取るので、
  // 「＋」はいつも最後のタブのすぐ右隣に来る。余った幅は右側の空きが受け取る。
  // タブが画面幅に収まらなくなったら、タブの領域だけが縮んでスクロールし、「＋」は隠れない
  h->addWidget(tabs_, 0);

  add_ = makeToolButton(QStringLiteral("plus"), QStringLiteral("新しい編集場所を追加 (Ctrl+N)"), this);
  add_->setFixedSize(28, 28);
  add_->setIconSize(QSize(16, 16));
  add_->setAutoRaise(false);
  add_->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;border-radius:5px;padding:0}"
                                     "QToolButton:hover{background:#2c2c2c}"
                                     "QToolButton:pressed{background:#363636}"));
  h->addWidget(add_, 0, Qt::AlignVCenter);
  h->addStretch(1);

  // 名前を変えるための入力欄。タブの上に重ねて出す
  renameEdit_ = new QLineEdit(tabs_);
  renameEdit_->setMaxLength(40);
  renameEdit_->setAccessibleName(QStringLiteral("タブの名前"));
  renameEdit_->setStyleSheet(QStringLiteral("QLineEdit{background:#111111;color:#ffffff;border:1px solid #2680eb;border-radius:3px;padding:1px 4px}"));
  renameEdit_->hide();
  renameEdit_->installEventFilter(this);

  connect(add_, &QToolButton::clicked, this, [] { editor().addWorkspace(); });
  connect(tabs_, &QTabBar::currentChanged, this, [](int index) { editor().switchWorkspace(index); });
  connect(tabs_, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
    if (index >= 0) beginRename(index);
  });
  connect(tabs_, &QWidget::customContextMenuRequested, this, &WorkspaceBar::showTabMenu);
  connect(renameEdit_, &QLineEdit::editingFinished, this, &WorkspaceBar::finishRename);
  connect(&editor(), &Editor::workspacesChanged, this, &WorkspaceBar::sync);
  // ドラッグしたままタブの上で少し止めると、そのタブに切り替える（通りかかっただけでは切り替えない）
  setAcceptDrops(true);
  hoverTimer_.setSingleShot(true);
  hoverTimer_.setInterval(450);
  connect(&hoverTimer_, &QTimer::timeout, this, [this] {
    const int index = hoverIndex_;
    hoverIndex_ = -1;
    if (index >= 0 && index < editor().workspaceCount() && index != editor().workspaceIndex()) editor().switchWorkspace(index);
  });
  sync();
}

int WorkspaceBar::tabAtGlobal(const QPoint& global) const {
  if (!isVisible()) return -1;
  return tabs_->tabAt(tabs_->mapFromGlobal(global));
}

/**
 * タブへ持っていけるドラッグか（タイムラインで選んだもの・テキストのスタイル）。
 * 素材の一覧のものは、その一覧のタブのメディアなので、ほかのタブへは持っていかない
 */
static bool tabDrag(const QMimeData* m) {
  return m && (m->hasFormat(QString::fromLatin1(kTimelineItemsMime)) || m->hasFormat(QString::fromLatin1(kTextPresetMime)));
}

void WorkspaceBar::hoverTab(const QPoint& pos) {
  const int index = tabs_->tabAt(tabs_->mapFrom(this, pos));
  if (index < 0 || index == editor().workspaceIndex()) {
    hoverIndex_ = -1;
    hoverTimer_.stop();
    return;
  }
  if (index == hoverIndex_) return;  // 同じタブの上で待っている
  hoverIndex_ = index;
  hoverTimer_.start();
}

void WorkspaceBar::dragEnterEvent(QDragEnterEvent* e) {
  if (!tabDrag(e->mimeData())) {
    e->ignore();
    return;
  }
  e->acceptProposedAction();
  hoverTab(e->position().toPoint());
}

void WorkspaceBar::dragMoveEvent(QDragMoveEvent* e) {
  if (!tabDrag(e->mimeData())) {
    e->ignore();
    return;
  }
  hoverTab(e->position().toPoint());
  // タイムラインのものは、タブの上で離しても貼り付けられる（そのタブの再生位置に）。素材などは、切り替えるだけ
  const bool items = e->mimeData()->hasFormat(QString::fromLatin1(kTimelineItemsMime));
  if (items && tabs_->tabAt(tabs_->mapFrom(this, e->position().toPoint())) >= 0) e->acceptProposedAction();
  else e->accept(QRect());  // 動かし続けられるように受け付けるが、ここで離しても何もしない
}

void WorkspaceBar::dragLeaveEvent(QDragLeaveEvent*) {
  hoverIndex_ = -1;
  hoverTimer_.stop();
}

void WorkspaceBar::dropEvent(QDropEvent* e) {
  hoverIndex_ = -1;
  hoverTimer_.stop();
  if (!e->mimeData()->hasFormat(QString::fromLatin1(kTimelineItemsMime))) {
    e->ignore();
    return;
  }
  const int index = tabs_->tabAt(tabs_->mapFrom(this, e->position().toPoint()));
  if (index < 0) {
    e->ignore();
    return;
  }
  // タブの上で離した: そのタブに切り替えて、そのタブの再生位置に貼り付ける
  if (index != editor().workspaceIndex()) editor().switchWorkspace(index);
  pasteItems();
  e->acceptProposedAction();
}

void WorkspaceBar::sync() {
  const Editor& ed = editor();
  const int n = ed.workspaceCount();
  {
    // タブの数や文字を合わせている間は、切り替えの通知を出さない
    const QSignalBlocker block(tabs_);
    while (tabs_->count() < n) {
      const int i = tabs_->addTab(QString());
      auto* close = new QToolButton(tabs_);
      close->setIcon(theme::icon(QStringLiteral("x"), theme::muted));
      close->setIconSize(QSize(12, 12));
      close->setFixedSize(18, 18);
      close->setFocusPolicy(Qt::NoFocus);
      close->setCursor(Qt::ArrowCursor);
      close->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;border-radius:3px;padding:0}"
                                          "QToolButton:hover{background:#4a4a4a}"));
      tabs_->setTabButton(i, QTabBar::RightSide, close);
      connect(close, &QToolButton::clicked, this, [this, close] {
        for (int k = 0; k < tabs_->count(); ++k) {
          if (tabs_->tabButton(k, QTabBar::RightSide) == close) {
            emit closeRequested(k);
            return;
          }
        }
      });
    }
    while (tabs_->count() > n) tabs_->removeTab(tabs_->count() - 1);
    for (int i = 0; i < n; ++i) {
      const QString name = ed.workspaceName(i);
      // 保存していない変更があるタブには * を付ける
      tabs_->setTabText(i, ed.workspaceDirty(i) ? name + QStringLiteral(" *") : name);
      const QString path = ed.workspacePath(i);
      tabs_->setTabToolTip(i, path.isEmpty() ? QStringLiteral("%1\nダブルクリックで名前を変更").arg(name)
                                             : QStringLiteral("%1\n%2\nダブルクリックで名前を変更").arg(name, path));
      tabs_->setTabData(i, ed.workspaceUid(i));
      if (auto* close = tabs_->tabButton(i, QTabBar::RightSide)) {
        close->setToolTip(QStringLiteral("「%1」を閉じる").arg(name));
        close->setAccessibleName(QStringLiteral("「%1」を閉じる").arg(name));
      }
    }
    tabs_->setCurrentIndex(ed.workspaceIndex());
  }
  // 名前を変えているタブが閉じられていたらやめる
  if (renamingUid_ >= 0 && ed.indexOfWorkspaceUid(renamingUid_) < 0) cancelRename();
}

void WorkspaceBar::beginRename(int index) {
  const Editor& ed = editor();
  if (index < 0 || index >= ed.workspaceCount()) return;
  renamingUid_ = ed.workspaceUid(index);
  // 文字の部分に重ねる（右端の「×」は隠さない）
  QRect r = tabs_->tabRect(index).adjusted(4, 3, -26, -3);
  renameEdit_->setGeometry(r);
  renameEdit_->setText(ed.workspaceName(index));
  renameEdit_->show();
  renameEdit_->raise();
  renameEdit_->setFocus();
  renameEdit_->selectAll();
}

void WorkspaceBar::finishRename() {
  if (renamingUid_ < 0) return;
  const int index = editor().indexOfWorkspaceUid(renamingUid_);
  const QString text = renameEdit_->text();
  renamingUid_ = -1;
  renameEdit_->hide();
  if (index >= 0) editor().renameWorkspace(index, text);
}

void WorkspaceBar::cancelRename() {
  renamingUid_ = -1;
  renameEdit_->hide();
}

bool WorkspaceBar::eventFilter(QObject* obj, QEvent* e) {
  // Esc で名前の変更をやめる
  if (obj == renameEdit_ && e->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
    cancelRename();
    return true;
  }
  return QWidget::eventFilter(obj, e);
}

void WorkspaceBar::showTabMenu(const QPoint& pos) {
  const int index = tabs_->tabAt(pos);
  if (index < 0) return;
  QMenu menu(this);
  QAction* rename = menu.addAction(QStringLiteral("名前を変更"));
  QAction* close = menu.addAction(QStringLiteral("閉じる"));
  QAction* chosen = menu.exec(tabs_->mapToGlobal(pos));
  if (chosen == rename) beginRename(index);
  else if (chosen == close) emit closeRequested(index);
}

}  // namespace kv
