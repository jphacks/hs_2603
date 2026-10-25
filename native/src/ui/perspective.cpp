#include "ui/perspective.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QSplitter>
#include <QStandardPaths>
#include <QEnterEvent>
#include <QIcon>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>

#include <algorithm>
#include <functional>

#include "ui/theme.h"

namespace kv {

using Side = PerspectiveManager::Side;

static const QString kBuiltinName = QStringLiteral("標準");

// ---- 欄のタブ ---------------------------------------------------------------------

/**
 * 欄のタブ。どのタブも同じ幅にする。タブをドラッグすると、左右の欄へ動かしたり並べ替えたりできる。
 * タブに × は付けない。閉じるのは、右クリックの「閉じる」か「表示 → ビュー」（どちらからでも開き直せる）
 */
class ViewTabBar : public QTabBar {
 public:
  ViewTabBar(PerspectiveManager* manager, ViewArea* area) : QTabBar(), manager_(manager), area_(area) {
    // 欄が狭いときは、タブの名前を省いて全部のタブを並べる。
    // （setUsesScrollButtons(false) にはしない: 最小の幅がタブの幅の合計 = 今の幅になり、欄を狭くできなくなる）
    setElideMode(Qt::ElideRight);
  }
  /** ドロップしたときに入る位置の印（-1 で消す） */
  void setDropIndex(int index) {
    if (dropIndex_ == index) return;
    dropIndex_ = index;
    update();
  }
 protected:
  QSize tabSizeHint(int index) const override {
    QSize s = QTabBar::tabSizeHint(index);
    const int n = count();
    if (n == 0) return s;
    // どのタブも同じ幅にする。ただし、同じ幅では名前が入りきらないタブがあり、名前の長さに合わせれば全部入りきるときは、
    // 名前の長さに合わせて分ける（欄が少し狭くても、名前を省かずに済むように）
    const int avail = std::max(0, width());
    const int equal = avail / n;
    int need = 0;
    int widest = 0;
    for (int i = 0; i < n; ++i) {
      const int w = QTabBar::tabSizeHint(i).width();
      need += w;
      widest = std::max(widest, w);
    }
    if (equal >= widest || need > avail) s.setWidth(equal);
    else s.setWidth(s.width() + (avail - need) / n);
    return s;
  }
  void resizeEvent(QResizeEvent* e) override {
    QTabBar::resizeEvent(e);
    // 幅が変わったら、タブの並びを引き直す
    if (count() > 0) setTabText(0, tabText(0));
  }
  void tabInserted(int index) override {
    QTabBar::tabInserted(index);
    if (count() > 0) setTabText(0, tabText(0));  // タブの幅を引き直す
  }
  void tabRemoved(int index) override {
    QTabBar::tabRemoved(index);
    if (count() > 0) setTabText(0, tabText(0));
  }
  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) {
      pressIndex_ = tabAt(e->position().toPoint());
      pressPos_ = e->position().toPoint();
    }
    QTabBar::mousePressEvent(e);
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (dragging_) {
      // ほかのアプリに切り替えたときなど、離したのを受け取れなかったら、やめる
      if (!(e->buttons() & Qt::LeftButton)) finish(e->globalPosition().toPoint(), false);
      else manager_->dragMove(e->globalPosition().toPoint());
      return;
    }
    if (pressIndex_ >= 0 && (e->buttons() & Qt::LeftButton) &&
        (e->position().toPoint() - pressPos_).manhattanLength() >= QApplication::startDragDistance()) {
      dragging_ = true;
      grabKeyboard();  // Esc でやめられるように
      manager_->beginDrag(area_, pressIndex_);
      manager_->dragMove(e->globalPosition().toPoint());
      return;
    }
    QTabBar::mouseMoveEvent(e);
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (dragging_ && e->button() == Qt::LeftButton) finish(e->globalPosition().toPoint(), true);
    pressIndex_ = -1;
    // 押したタブの状態は、QTabBar にも戻させる（Windows ではタブは押したときに選ばれるので、離したときには選び直さない）
    QTabBar::mouseReleaseEvent(e);
  }
  void keyPressEvent(QKeyEvent* e) override {
    if (dragging_ && e->key() == Qt::Key_Escape) {
      finish(QCursor::pos(), false);
      return;
    }
    QTabBar::keyPressEvent(e);
  }
  void contextMenuEvent(QContextMenuEvent* e) override {
    const int index = tabAt(e->pos());
    if (index >= 0) manager_->execTabMenu(area_, index, e->globalPos());
  }
  void paintEvent(QPaintEvent* e) override {
    QTabBar::paintEvent(e);
    if (dropIndex_ < 0 || count() == 0) return;
    const int x = dropIndex_ < count() ? tabRect(dropIndex_).left() : tabRect(count() - 1).right();
    QPainter p(this);
    p.fillRect(QRect(std::clamp(x - 1, 0, width() - 3), 4, 3, height() - 8), theme::accent);
  }

 private:
  void finish(const QPoint& globalPos, bool drop) {
    dragging_ = false;
    pressIndex_ = -1;
    releaseKeyboard();
    manager_->endDrag(globalPos, drop);
  }

  PerspectiveManager* manager_;
  ViewArea* area_;
  int pressIndex_ = -1;
  QPoint pressPos_;
  bool dragging_ = false;
  int dropIndex_ = -1;
};

static constexpr int kStripWidth = 28;  // 左の欄を折りたたんだときに残す帯の幅

/**
 * 左の欄を折りたたんだときに残る、細い帯。いちばん上に「開く」ボタン、その下に欄のタブの名前（縦書き）。
 * 帯のどこを押しても開く（タブの名前を押すと、そのタブを前にして開く）
 */
class CollapsedStrip : public QWidget {
 public:
  /** expand: 開く（引数は前にするタブ。-1 なら、折りたたむ前のタブのまま） */
  CollapsedStrip(QTabWidget* tabs, std::function<void(int)> expand, QWidget* parent)
      : QWidget(parent), tabs_(tabs), expand_(std::move(expand)) {
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("左の欄を開く"));
    button_ = new QToolButton(this);
    button_->setIcon(theme::icon(QStringLiteral("panel-left-open"), theme::muted));
    button_->setIconSize(QSize(16, 16));
    button_->setFixedSize(24, 24);
    button_->setStyleSheet(QStringLiteral("QToolButton{padding:0}"));
    button_->setToolTip(QStringLiteral("左の欄を開く"));
    button_->setAccessibleName(QStringLiteral("左の欄を開く"));
    connect(button_, &QToolButton::clicked, this, [this] { expand_(-1); });
    hide();
  }
  QToolButton* button() const { return button_; }

 protected:
  void resizeEvent(QResizeEvent*) override { button_->move((width() - 1 - button_->width()) / 2, 5); }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.fillRect(rect(), theme::panel);
    p.fillRect(QRect(width() - 1, 0, 1, height()), theme::border);
    const QFont f = stripFont();
    p.setFont(f);
    const QFontMetrics fm(f);
    for (const auto& [r, i] : titleRects()) {
      const bool current = i == tabs_->currentIndex();
      const bool hover = i == hover_;
      if (hover) {
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::hover);
        p.drawRoundedRect(QRectF(r).adjusted(3, 0, -3, 0), 4, 4);
      }
      if (current) p.fillRect(QRect(0, r.top() + 4, 2, r.height() - 8), theme::accent);  // 開いたときに前に出るタブ
      const QColor fg = current || hover ? theme::text : theme::muted;
      p.setPen(fg);
      // 縦書き: 1 文字ずつ上から並べる。伸ばす音の「ー」は、縦の線にする
      int y = r.top() + 6;
      const int cx = (width() - 1) / 2;
      for (const QChar c : tabs_->tabText(i)) {
        if (c == QChar(0x30FC)) {
          p.setRenderHint(QPainter::Antialiasing, false);
          p.fillRect(QRect(cx, y + 2, 1, fm.height() - 4), fg);
        } else {
          p.drawText(QRect(0, y, width() - 1, fm.height()), Qt::AlignCenter, QString(c));
        }
        y += fm.height() - 1;
      }
    }
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    const int h = indexAt(e->position().toPoint());
    if (h != hover_) {
      hover_ = h;
      update();
    }
  }
  void leaveEvent(QEvent*) override {
    hover_ = -1;
    update();
  }
  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton) expand_(indexAt(e->position().toPoint()));
  }

 private:
  QFont stripFont() const {
    QFont f = font();
    f.setPointSizeF(8.5);
    return f;
  }
  /** タブの名前を書く所と、そのタブの番号 */
  std::vector<std::pair<QRect, int>> titleRects() const {
    const QFontMetrics fm(stripFont());
    std::vector<std::pair<QRect, int>> out;
    int y = 5 + button_->height() + 8;
    for (int i = 0; i < tabs_->count(); ++i) {
      const int h = 12 + int(tabs_->tabText(i).size()) * (fm.height() - 1);
      out.push_back({QRect(0, y, width() - 1, h), i});
      y += h + 4;
    }
    return out;
  }
  int indexAt(const QPoint& pos) const {
    for (const auto& [r, i] : titleRects()) {
      if (r.contains(pos)) return i;
    }
    return -1;
  }

  QTabWidget* tabs_;
  std::function<void(int)> expand_;
  QToolButton* button_;
  int hover_ = -1;
};

/** 左・右の欄。ビューをタブで重ねて置く */
class ViewArea : public QTabWidget {
 public:
  ViewArea(PerspectiveManager* manager, Side side, QWidget* parent) : QTabWidget(parent), side_(side) {
    bar_ = new ViewTabBar(manager, this);
    setTabBar(bar_);
    setDocumentMode(true);
    setMovable(false);  // 並べ替えも、ドラッグ（ViewTabBar）でまとめて扱う
    bar_->setExpanding(true);
    setAutoFillBackground(true);
    if (side == Side::Left) {
      // 左の欄を折りたたんだときに残る細い帯（押すと開く）。折りたたむ・開くボタンは、画面の一番上（メニューの「ファイル」の左隣）
      strip_ = new CollapsedStrip(this, [this, manager](int index) {
        manager->setLeftCollapsed(false);
        if (index >= 0 && index < count()) setCurrentIndex(index);
      }, this);
    }
    // ドラッグしている間に、置ける欄を囲む枠
    overlay_ = new QWidget(this);
    overlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlay_->setStyleSheet(QStringLiteral("background: rgba(38,128,235,24); border: 2px solid #2680eb;"));
    overlay_->hide();
    connect(this, &QTabWidget::currentChanged, manager, [manager] { manager->scheduleSave(); });
  }
  Side side() const { return side_; }
  ViewTabBar* bar() const { return bar_; }
  void setDropTarget(bool on) {
    if (on == overlay_->isVisible()) return;
    overlay_->setGeometry(rect());
    overlay_->setVisible(on);
    if (on) overlay_->raise();
  }
  QSize minimumSizeHint() const override {
    if (collapsed_) return QSize(kStripWidth, 0);
    // ビューが無い欄は、ドラッグしている間だけ、置き場所として出す
    if (count() == 0) return QSize(180, 0);
    return QTabWidget::minimumSizeHint();
  }
  bool collapsed() const { return collapsed_; }
  CollapsedStrip* strip() const { return strip_; }
  /** 折りたたむ（細い帯だけにする）/ 開く。中のタブとビューは、そのままの状態で隠しておく（開いたときに、同じ状態で戻る） */
  void setCollapsed(bool on) {
    if (!strip_ || on == collapsed_) return;
    collapsed_ = on;
    if (auto* stack = findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly)) stack->setVisible(!on);
    bar_->setVisible(!on);
    if (on) {
      setMinimumWidth(kStripWidth);
      setMaximumWidth(kStripWidth);
      strip_->setGeometry(rect());
      strip_->show();
      strip_->raise();
    } else {
      strip_->hide();
      setMinimumWidth(0);
      setMaximumWidth(QWIDGETSIZE_MAX);
    }
    if (overlay_->isVisible()) overlay_->raise();
    QCoreApplication::postEvent(this, new QEvent(QEvent::LayoutRequest));  // タブと中身の置き場所を引き直す
    updateGeometry();
  }

 protected:
  void resizeEvent(QResizeEvent* e) override {
    QTabWidget::resizeEvent(e);
    overlay_->setGeometry(rect());
    if (strip_) strip_->setGeometry(rect());
  }
  void paintEvent(QPaintEvent* e) override {
    QTabWidget::paintEvent(e);
    if (count() > 0) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect r = rect().adjusted(8, 8, -9, -9);
    p.setPen(QPen(theme::border2, 1, Qt::DashLine));
    p.drawRoundedRect(r, 6, 6);
    p.setPen(theme::dim);
    p.drawText(r.adjusted(12, 0, -12, 0), Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("ここにタブをドロップすると、この欄に置けます"));
  }

 private:
  Side side_;
  ViewTabBar* bar_;
  QWidget* overlay_;
  CollapsedStrip* strip_ = nullptr;
  bool collapsed_ = false;
};

// ---- 保存の形 ---------------------------------------------------------------------

static QJsonObject toJson(const PerspectiveLayout& l) {
  return QJsonObject{{QStringLiteral("left"), QJsonArray::fromStringList(l.left)},
                     {QStringLiteral("right"), QJsonArray::fromStringList(l.right)},
                     {QStringLiteral("leftCurrent"), l.leftCurrent},
                     {QStringLiteral("rightCurrent"), l.rightCurrent},
                     {QStringLiteral("leftWidth"), l.leftWidth},
                     {QStringLiteral("rightWidth"), l.rightWidth}};
}

static QStringList stringList(const QJsonValue& v) {
  QStringList out;
  for (const QJsonValue& x : v.toArray()) {
    if (x.isString()) out << x.toString();
  }
  return out;
}

static PerspectiveLayout layoutFromJson(const QJsonObject& o) {
  PerspectiveLayout l;
  l.left = stringList(o.value(QStringLiteral("left")));
  l.right = stringList(o.value(QStringLiteral("right")));
  l.leftCurrent = o.value(QStringLiteral("leftCurrent")).toString();
  l.rightCurrent = o.value(QStringLiteral("rightCurrent")).toString();
  l.leftWidth = std::clamp(o.value(QStringLiteral("leftWidth")).toInt(264), 120, 1600);
  l.rightWidth = std::clamp(o.value(QStringLiteral("rightWidth")).toInt(316), 120, 1600);
  return l;
}

static QString storePath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("perspectives.json"));
}

// ---- PerspectiveManager ---------------------------------------------------------

PerspectiveManager::PerspectiveManager(QWidget* window) : QObject(window), window_(window), current_(kBuiltinName) {
  left_ = new ViewArea(this, Side::Left, window);
  right_ = new ViewArea(this, Side::Right, window);
  saveTimer_.setSingleShot(true);
  saveTimer_.setInterval(400);
  connect(&saveTimer_, &QTimer::timeout, this, [this] {
    state_.insert(current_, capture());
    writeStore();
  });
}

QWidget* PerspectiveManager::area(Side side) const {
  return areaOf(side);
}

ViewArea* PerspectiveManager::areaOf(Side side) const {
  return side == Side::Left ? left_ : right_;
}

void PerspectiveManager::attach(QSplitter* splitter, int leftIndex, int centerIndex, int rightIndex) {
  splitter_ = splitter;
  leftIndex_ = leftIndex;
  centerIndex_ = centerIndex;
  rightIndex_ = rightIndex;
  connect(splitter, &QSplitter::splitterMoved, this, [this] {
    captureWidths();
    scheduleSave();
  });
}

void PerspectiveManager::addView(const QString& id, const QString& title, QWidget* widget, Side home) {
  // 閉じたままのビューが、作ったときの親（ウィンドウ）の上にそのまま出ないようにする。
  // 欄に置いたら、タブを選んだときに出る
  widget->hide();
  views_.push_back({id, title, widget, home});
}

const PerspectiveManager::View* PerspectiveManager::view(const QString& id) const {
  for (const View& v : views_) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

QString PerspectiveManager::idOf(const QWidget* w) const {
  for (const View& v : views_) {
    if (v.widget == w) return v.id;
  }
  return {};
}

ViewArea* PerspectiveManager::areaContaining(const QString& id) const {
  const View* v = view(id);
  if (!v) return nullptr;
  for (ViewArea* a : {left_, right_}) {
    if (a->indexOf(v->widget) >= 0) return a;
  }
  return nullptr;
}

// ---- 並び ---------------------------------------------------------------------------

PerspectiveLayout PerspectiveManager::defaultLayout() const {
  PerspectiveLayout l;
  for (const View& v : views_) (v.home == Side::Left ? l.left : l.right) << v.id;
  l.leftCurrent = l.left.value(0);
  l.rightCurrent = l.right.value(0);
  return l;
}

PerspectiveLayout PerspectiveManager::definition(const QString& name) const {
  for (const auto& [n, l] : saved_) {
    if (n == name) return l;
  }
  return defaultLayout();
}

void PerspectiveManager::captureWidths() {
  if (!splitter_ || !splitter_->isVisible()) return;
  const QList<int> s = splitter_->sizes();
  if (s.size() <= std::max({leftIndex_, centerIndex_, rightIndex_})) return;
  // ビューの無い欄（ドラッグしている間の置き場所）と、折りたたんでいる左の欄（開いたときに戻す幅を残す）の幅は覚えない
  if (!leftCollapsed_ && !left_->isHidden() && left_->count() > 0 && s[leftIndex_] > 0) leftWidth_ = s[leftIndex_];
  if (!right_->isHidden() && right_->count() > 0 && s[rightIndex_] > 0) rightWidth_ = s[rightIndex_];
}

PerspectiveLayout PerspectiveManager::capture() {
  captureWidths();
  PerspectiveLayout l;
  for (int i = 0; i < left_->count(); ++i) l.left << idOf(left_->widget(i));
  for (int i = 0; i < right_->count(); ++i) l.right << idOf(right_->widget(i));
  l.leftCurrent = idOf(left_->currentWidget());
  l.rightCurrent = idOf(right_->currentWidget());
  l.leftWidth = leftWidth_;
  l.rightWidth = rightWidth_;
  return l;
}

void PerspectiveManager::apply(const PerspectiveLayout& layout) {
  applying_ = true;
  while (left_->count() > 0) left_->removeTab(0);
  while (right_->count() > 0) right_->removeTab(0);
  QStringList placed;
  auto place = [&](ViewArea* a, const QStringList& ids, const QString& cur) {
    for (const QString& id : ids) {
      const View* v = view(id);
      // 知らないビュー（前の版の名前など）と、2 回目に出てきたビューは置かない
      if (!v || placed.contains(id)) continue;
      a->addTab(v->widget, v->title);
      placed << id;
    }
    if (const View* v = view(cur); v && a->indexOf(v->widget) >= 0) a->setCurrentWidget(v->widget);
  };
  place(left_, layout.left, layout.leftCurrent);
  place(right_, layout.right, layout.rightCurrent);
  leftWidth_ = layout.leftWidth;
  rightWidth_ = layout.rightWidth;
  left_->setCollapsed(leftCollapsed_);  // 折りたたんでいるかは、並びを変えてもそのまま
  applying_ = false;
  updateAreaVisibility();
  applySizes();
}

void PerspectiveManager::updateAreaVisibility() {
  for (ViewArea* a : {left_, right_}) a->setVisible(dragging_ || a->count() > 0);
  emit leftViewsChanged();
}

void PerspectiveManager::applySizes() {
  if (!splitter_) return;
  QList<int> s = splitter_->sizes();
  if (s.size() <= std::max({leftIndex_, centerIndex_, rightIndex_})) return;
  int total = 0;
  for (int x : s) total += x;
  // まだ画面に出す前は、ウィンドウの幅で分ける（出したときに、同じ割合で広げられる）
  if (!splitter_->isVisible() || total <= 0) total = std::max(window_->width(), 800);
  int others = 0;
  for (int i = 0; i < s.size(); ++i) {
    if (i != leftIndex_ && i != centerIndex_ && i != rightIndex_) others += s[i];
  }
  // 折りたたんでいる左の欄は細い帯の幅だけ（空いた幅は、プレビューが使う）
  s[leftIndex_] = left_->isHidden() ? 0 : leftCollapsed_ ? kStripWidth : leftWidth_;
  s[rightIndex_] = right_->isHidden() ? 0 : rightWidth_;
  s[centerIndex_] = std::max(200, total - s[leftIndex_] - s[rightIndex_] - others);
  splitter_->setSizes(s);
}

void PerspectiveManager::moveView(const QString& id, Side side, int index) {
  const View* v = view(id);
  if (!v) return;
  // 折りたたんでいる左の欄へ置くときは、開いてから（置いたビューが見えるように）
  if (side == Side::Left && leftCollapsed_) setLeftCollapsed(false);
  ViewArea* to = areaOf(side);
  ViewArea* from = areaContaining(id);
  if (index < 0 || index > to->count()) index = to->count();
  if (from == to) {
    // 同じ欄の中での並べ替え（取り出すと、後ろのタブが 1 つ前に詰まる）
    const int cur = to->indexOf(v->widget);
    if (index > cur) --index;
    if (index == cur) {
      to->setCurrentWidget(v->widget);
      return;
    }
  }
  const bool visibilityBefore[2] = {left_->isHidden(), right_->isHidden()};
  applying_ = true;
  if (from) from->removeTab(from->indexOf(v->widget));
  index = std::min(index, to->count());
  to->insertTab(index, v->widget, v->title);
  to->setCurrentWidget(v->widget);
  applying_ = false;
  updateAreaVisibility();
  // 欄が出たり隠れたりしたら、幅を覚えている幅に合わせ直す
  if (visibilityBefore[0] != left_->isHidden() || visibilityBefore[1] != right_->isHidden()) applySizes();
  scheduleSave();
}

void PerspectiveManager::closeView(const QString& id) {
  ViewArea* a = areaContaining(id);
  if (!a) return;
  captureWidths();
  applying_ = true;
  a->removeTab(a->indexOf(view(id)->widget));
  applying_ = false;
  const bool wasHidden = a->isHidden();
  updateAreaVisibility();
  if (wasHidden != a->isHidden()) applySizes();
  scheduleSave();
}

void PerspectiveManager::showView(const QString& id) {
  const View* v = view(id);
  if (!v) return;
  if (ViewArea* a = areaContaining(id)) {
    if (a == left_ && leftCollapsed_) setLeftCollapsed(false);
    a->setCurrentWidget(v->widget);
    return;
  }
  moveView(id, v->home);
}

bool PerspectiveManager::leftHasViews() const {
  return left_->count() > 0;
}

void PerspectiveManager::setLeftCollapsed(bool on) {
  if (on == leftCollapsed_) return;
  if (on) captureWidths();  // 開いたときに、今の幅に戻す
  // 折りたたむ欄の中で文字を入力していたら、入力先を帯の「開く」ボタンへ移す（隠れた所に文字が入らないように）。
  // 開くときに帯のボタンにいたら、欄のタブへ移す
  QWidget* focus = QApplication::focusWidget();
  const bool focusInside = focus && left_->isAncestorOf(focus);
  leftCollapsed_ = on;
  left_->setCollapsed(on);
  applySizes();
  if (focusInside) {
    if (on) left_->strip()->button()->setFocus(Qt::OtherFocusReason);
    else left_->setFocus(Qt::OtherFocusReason);
  }
  scheduleSave();  // 次に起動したときも、同じ状態にする
  emit leftCollapsedChanged(on);
}

// ---- ドラッグ -------------------------------------------------------------------------

ViewArea* PerspectiveManager::areaAt(const QPoint& globalPos) const {
  for (ViewArea* a : {left_, right_}) {
    if (a->isVisible() && QRect(a->mapToGlobal(QPoint(0, 0)), a->size()).contains(globalPos)) return a;
  }
  return nullptr;
}

int PerspectiveManager::insertIndexAt(ViewArea* area, const QPoint& globalPos) const {
  QTabBar* bar = area->bar();
  const QPoint p = bar->mapFromGlobal(globalPos);
  // タブの上なら、その位置に。中身の上なら、いちばん後ろに
  if (area->count() == 0 || p.y() < -8 || p.y() > bar->height() + 8) return area->count();
  int index = 0;
  for (int i = 0; i < bar->count(); ++i) {
    if (p.x() > bar->tabRect(i).center().x()) index = i + 1;
  }
  return index;
}

void PerspectiveManager::beginDrag(ViewArea* from, int index) {
  dragId_ = idOf(from->widget(index));
  if (dragId_.isEmpty()) return;
  captureWidths();
  dragging_ = true;
  dragLabel_ = new QLabel(view(dragId_)->title);
  dragLabel_->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
  dragLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
  dragLabel_->setAttribute(Qt::WA_ShowWithoutActivating);
  dragLabel_->setStyleSheet(QStringLiteral("QLabel{background:#2c2c2c;color:#e6e6e6;border:1px solid #2680eb;border-radius:4px;padding:4px 10px}"));
  dragLabel_->adjustSize();
  QApplication::setOverrideCursor(Qt::ClosedHandCursor);
  // ビューの無い欄も、置き場所として出す
  const bool hidden[2] = {left_->isHidden(), right_->isHidden()};
  updateAreaVisibility();
  if (hidden[0] != left_->isHidden() || hidden[1] != right_->isHidden()) applySizes();
}

void PerspectiveManager::dragMove(const QPoint& globalPos) {
  if (!dragging_) return;
  if (dragLabel_) {
    dragLabel_->move(globalPos + QPoint(14, 16));
    if (!dragLabel_->isVisible()) dragLabel_->show();
  }
  ViewArea* target = areaAt(globalPos);
  for (ViewArea* a : {left_, right_}) {
    a->setDropTarget(a == target);
    a->bar()->setDropIndex(a == target && a->count() > 0 ? insertIndexAt(a, globalPos) : -1);
  }
}

void PerspectiveManager::endDrag(const QPoint& globalPos, bool drop) {
  if (!dragging_) return;
  ViewArea* target = drop ? areaAt(globalPos) : nullptr;
  const int index = target ? insertIndexAt(target, globalPos) : -1;
  dragging_ = false;
  QApplication::restoreOverrideCursor();
  if (dragLabel_) {
    dragLabel_->deleteLater();
    dragLabel_ = nullptr;
  }
  for (ViewArea* a : {left_, right_}) {
    a->setDropTarget(false);
    a->bar()->setDropIndex(-1);
  }
  if (target) moveView(dragId_, target->side(), index);
  dragId_.clear();
  // 置き場所として出していた、ビューの無い欄をしまう
  updateAreaVisibility();
  applySizes();
}

void PerspectiveManager::execTabMenu(ViewArea* area, int index, const QPoint& globalPos) {
  const QString id = idOf(area->widget(index));
  if (id.isEmpty()) return;
  QMenu menu(window_);
  QAction* toLeft = menu.addAction(QStringLiteral("左の欄へ移動"));
  QAction* toRight = menu.addAction(QStringLiteral("右の欄へ移動"));
  toLeft->setEnabled(area->side() != Side::Left);
  toRight->setEnabled(area->side() != Side::Right);
  menu.addSeparator();
  QAction* close = menu.addAction(QStringLiteral("閉じる"));
  menu.addSeparator();
  QAction* reset = menu.addAction(QStringLiteral("パースペクティブをリセット"));
  QAction* chosen = menu.exec(globalPos);
  if (chosen == toLeft) moveView(id, Side::Left);
  else if (chosen == toRight) moveView(id, Side::Right);
  else if (chosen == close) closeView(id);
  else if (chosen == reset) resetCurrent();
}

// ---- パースペクティブ -----------------------------------------------------------------

QStringList PerspectiveManager::names() const {
  QStringList out{kBuiltinName};
  for (const auto& [n, l] : saved_) out << n;
  return out;
}

bool PerspectiveManager::isBuiltin(const QString& name) const {
  return name == kBuiltinName;
}

bool PerspectiveManager::isKnown(const QString& name) const {
  return names().contains(name);
}

void PerspectiveManager::switchTo(const QString& name) {
  if (name == current_ || !isKnown(name)) return;
  saveTimer_.stop();
  state_.insert(current_, capture());
  current_ = name;
  apply(state_.contains(name) ? state_.value(name) : definition(name));
  writeStore();
}

void PerspectiveManager::saveAs() {
  bool ok = false;
  const QString name = QInputDialog::getText(window_, QStringLiteral("パースペクティブを保存"),
                                             QStringLiteral("今の並びを、名前を付けて保存します。\n名前:"), QLineEdit::Normal,
                                             isBuiltin(current_) ? QString() : current_, &ok)
                           .trimmed();
  if (!ok || name.isEmpty()) return;
  if (isBuiltin(name)) {
    QMessageBox::information(window_, QStringLiteral("パースペクティブを保存"),
                             QStringLiteral("「%1」は変えられません。別の名前を付けてください。").arg(kBuiltinName));
    return;
  }
  auto it = std::find_if(saved_.begin(), saved_.end(), [&](const auto& p) { return p.first == name; });
  if (it != saved_.end()) {
    const auto r = QMessageBox::question(window_, QStringLiteral("パースペクティブを保存"),
                                         QStringLiteral("「%1」はもうあります。今の並びで上書きしますか？").arg(name));
    if (r != QMessageBox::Yes) return;
  }
  saveTimer_.stop();
  const PerspectiveLayout l = capture();
  if (it != saved_.end()) it->second = l;
  else saved_.push_back({name, l});
  // 変えた並びは新しいパースペクティブに移したので、元のパースペクティブは保存したときの並びに戻す
  if (name != current_) state_.remove(current_);
  state_.remove(name);
  current_ = name;
  writeStore();
}

void PerspectiveManager::resetCurrent() {
  const auto r = QMessageBox::question(window_, QStringLiteral("パースペクティブをリセット"),
                                       QStringLiteral("「%1」を、保存したときの並びに戻しますか？").arg(current_));
  if (r != QMessageBox::Yes) return;
  saveTimer_.stop();
  state_.remove(current_);
  apply(definition(current_));
  writeStore();
}

void PerspectiveManager::remove(const QString& name) {
  if (isBuiltin(name)) return;
  const auto r = QMessageBox::question(window_, QStringLiteral("パースペクティブを削除"), QStringLiteral("「%1」を削除しますか？").arg(name));
  if (r != QMessageBox::Yes) return;
  saveTimer_.stop();
  saved_.erase(std::remove_if(saved_.begin(), saved_.end(), [&](const auto& p) { return p.first == name; }), saved_.end());
  state_.remove(name);
  if (current_ == name) {
    current_ = kBuiltinName;
    apply(state_.contains(current_) ? state_.value(current_) : definition(current_));
  }
  writeStore();
}

/** メニューの中身を消す（前に作った下のメニューと QActionGroup も） */
static void clearMenu(QMenu* menu) {
  menu->clear();
  for (QMenu* sub : menu->findChildren<QMenu*>(Qt::FindDirectChildrenOnly)) delete sub;
  for (QActionGroup* g : menu->findChildren<QActionGroup*>(Qt::FindDirectChildrenOnly)) delete g;
}

void PerspectiveManager::fillPerspectiveMenu(QMenu* menu) {
  clearMenu(menu);
  auto* group = new QActionGroup(menu);
  for (const QString& n : names()) {
    QAction* a = menu->addAction(n, this, [this, n] { switchTo(n); });
    a->setCheckable(true);
    a->setChecked(n == current_);
    group->addAction(a);
  }
  menu->addSeparator();
  menu->addAction(QStringLiteral("名前を付けて保存..."), this, [this] { saveAs(); });
  menu->addAction(QStringLiteral("リセット"), this, [this] { resetCurrent(); });
  QMenu* del = menu->addMenu(QStringLiteral("削除"));
  for (const auto& [n, l] : saved_) {
    const QString name = n;
    del->addAction(name, this, [this, name] { remove(name); });
  }
  del->setEnabled(!saved_.empty());
}

void PerspectiveManager::fillViewMenu(QMenu* menu) {
  clearMenu(menu);
  for (const View& v : views_) {
    const QString id = v.id;
    QAction* a = menu->addAction(v.title);
    a->setCheckable(true);
    a->setChecked(areaContaining(id) != nullptr);
    connect(a, &QAction::triggered, this, [this, id](bool on) {
      if (on) showView(id);
      else closeView(id);
    });
  }
}

// ---- 保存・読み込み ---------------------------------------------------------------------

void PerspectiveManager::scheduleSave() {
  if (applying_ || dragging_) return;
  saveTimer_.start();
}

void PerspectiveManager::storeNow() {
  saveTimer_.stop();
  state_.insert(current_, capture());
  writeStore();
}

void PerspectiveManager::restore() {
  readStore();
  if (!isKnown(current_)) current_ = kBuiltinName;
  apply(state_.contains(current_) ? state_.value(current_) : definition(current_));
}

void PerspectiveManager::readStore() {
  QFile f(storePath());
  if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
  saved_.clear();
  for (const QJsonValue& v : root.value(QStringLiteral("saved")).toArray()) {
    const QJsonObject o = v.toObject();
    const QString name = o.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty() || isBuiltin(name) || isKnown(name)) continue;
    saved_.push_back({name, layoutFromJson(o.value(QStringLiteral("layout")).toObject())});
  }
  state_.clear();
  const QJsonObject st = root.value(QStringLiteral("state")).toObject();
  for (auto it = st.begin(); it != st.end(); ++it) {
    if (isKnown(it.key())) state_.insert(it.key(), layoutFromJson(it.value().toObject()));
  }
  const QString cur = root.value(QStringLiteral("current")).toString();
  if (isKnown(cur)) current_ = cur;
  leftCollapsed_ = root.value(QStringLiteral("leftCollapsed")).toBool(false);
}

void PerspectiveManager::writeStore() {
  QJsonArray saved;
  for (const auto& [n, l] : saved_) saved.append(QJsonObject{{QStringLiteral("name"), n}, {QStringLiteral("layout"), toJson(l)}});
  QJsonObject st;
  for (auto it = state_.cbegin(); it != state_.cend(); ++it) st.insert(it.key(), toJson(it.value()));
  const QJsonObject root{{QStringLiteral("current"), current_},
                         {QStringLiteral("saved"), saved},
                         {QStringLiteral("state"), st},
                         {QStringLiteral("leftCollapsed"), leftCollapsed_}};
  QDir().mkpath(QFileInfo(storePath()).absolutePath());
  QSaveFile f(storePath());
  if (!f.open(QIODevice::WriteOnly)) return;
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  f.commit();
}

}  // namespace kv
