#include "ui/widgets.h"

#include <QApplication>
#include <QButtonGroup>
#include <QColorDialog>
#include <QDateTime>
#include <QDrag>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTimer>
#include <QWheelEvent>
#include <QMimeData>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <algorithm>
#include <cmath>

#include "core/textstyles.h"
#include "render/textrender.h"
#include "ui/theme.h"

namespace kv {

QLabel* makeLabel(const QString& text, const char* role, QWidget* parent) {
  auto* l = new QLabel(text, parent);
  if (role) l->setProperty("role", QString::fromLatin1(role));
  return l;
}

QFrame* makeSeparator(QWidget* parent) {
  auto* f = new QFrame(parent);
  f->setProperty("role", QStringLiteral("separator"));
  f->setFixedHeight(1);
  return f;
}

QToolButton* makeToolButton(const QString& iconName, const QString& tip, QWidget* parent, const QString& text) {
  auto* b = new QToolButton(parent);
  b->setIcon(theme::icon(iconName));
  b->setIconSize(QSize(16, 16));
  b->setToolTip(tip);
  b->setAccessibleName(tip);
  b->setFocusPolicy(Qt::NoFocus);
  b->setAutoRaise(true);
  if (!text.isEmpty()) {
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  }
  return b;
}

QToolButton* makeTextPresetButton(const TextPreset& preset, QSize cardSize, QWidget* parent) {
  auto* b = new QToolButton(parent);
  b->setIcon(QIcon(renderTextPresetCard(preset, cardSize, b->devicePixelRatioF())));
  b->setIconSize(cardSize);
  b->setText(preset.label);
  b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  b->setToolTip(preset.label);
  b->setAccessibleName(preset.label);
  b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  b->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:1px solid transparent;border-radius:6px;padding:3px;color:#9b9b9b}"
                                  "QToolButton:hover{border-color:#9b9b9b;color:#e6e6e6}"));
  return b;
}

namespace {
/** カードのドラッグ開始を見張る（クリックの動きは変えない） */
class PresetDragFilter : public QObject {
 public:
  PresetDragFilter(QToolButton* button, QString presetId) : QObject(button), button_(button), presetId_(std::move(presetId)) {}

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override {
    if (obj != button_) return false;
    if (e->type() == QEvent::MouseButtonPress) {
      auto* m = static_cast<QMouseEvent*>(e);
      pressed_ = m->button() == Qt::LeftButton;
      pressPos_ = m->position().toPoint();
    } else if (e->type() == QEvent::MouseButtonRelease) {
      pressed_ = false;
    } else if (e->type() == QEvent::MouseMove && pressed_) {
      auto* m = static_cast<QMouseEvent*>(e);
      if (!(m->buttons() & Qt::LeftButton)) {
        pressed_ = false;
      } else if ((m->position().toPoint() - pressPos_).manhattanLength() >= QApplication::startDragDistance()) {
        pressed_ = false;
        startDrag();
        return true;
      }
    }
    return false;
  }

 private:
  void startDrag() {
    const TextPreset& preset = textPreset(presetId_);
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kTextPresetMime), presetId_.toUtf8());
    // カーソルに付ける見本: カードを半透明にしたもの。カーソルは見本の左端あたり（=置いたときの開始位置）にする
    const qreal dpr = button_->devicePixelRatioF();
    const QSize card(112, 56);
    const QPixmap src = renderTextPresetCard(preset, card, dpr);
    QPixmap ghost(src.size());
    ghost.setDevicePixelRatio(dpr);
    ghost.fill(Qt::transparent);
    {
      QPainter p(&ghost);
      p.setOpacity(0.85);
      p.drawPixmap(0, 0, src);
    }
    if (button_->isCheckable()) button_->setChecked(true);  // ドラッグ中のスタイルが分かるようにする
    auto* drag = new QDrag(button_);
    drag->setMimeData(mime);
    drag->setPixmap(ghost);
    drag->setHotSpot(QPoint(14, card.height() / 2));
    drag->exec(Qt::CopyAction);  // Esc で中止できる。落とされなかったときは何も追加されない
    // ボタンが押されたままの見た目・状態にならないようにする（離したときのクリック扱いも防ぐ）
    button_->setDown(false);
  }

  QToolButton* button_;
  QString presetId_;
  QPoint pressPos_;
  bool pressed_ = false;
};
}  // namespace

void makeTextPresetDraggable(QToolButton* button, const TextPreset& preset) {
  button->installEventFilter(new PresetDragFilter(button, preset.id));
  button->setCursor(Qt::OpenHandCursor);
}

namespace {
// ホイールを受け取らずに親（スクロールエリアなど）へ渡す
class WheelBlocker : public QObject {
 public:
  using QObject::QObject;

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override {
    if (e->type() != QEvent::Wheel) return QObject::eventFilter(obj, e);
    e->ignore();
    return true;
  }
};
}  // namespace

void disableWheel(QWidget* w) {
  w->installEventFilter(new WheelBlocker(w));
  // ホイールではフォーカスも移さない（クリックと Tab ではこれまでどおり移る）
  if (w->focusPolicy() == Qt::WheelFocus) w->setFocusPolicy(Qt::StrongFocus);
}

// ---- Segmented --------------------------------------------------------------

Segmented::Segmented(const std::vector<Option>& options, int columns, QWidget* parent) : QFrame(parent), options_(options) {
  setObjectName(QStringLiteral("Segmented"));
  group_ = new QButtonGroup(this);
  group_->setExclusive(false);
  auto* grid = new QGridLayout(this);
  grid->setContentsMargins(2, 2, 2, 2);
  grid->setSpacing(2);
  const int cols = columns > 0 ? columns : int(options.size());
  for (size_t i = 0; i < options.size(); ++i) {
    auto* b = new QToolButton(this);
    b->setText(options[i].label);
    b->setCheckable(true);
    b->setFocusPolicy(Qt::TabFocus);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setMinimumHeight(26);
    b->setAccessibleName(options[i].label);
    if (!options[i].icon.isEmpty()) {
      b->setIcon(theme::icon(options[i].icon, theme::muted));
      b->setIconSize(QSize(14, 14));
      b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    }
    grid->addWidget(b, int(i) / cols, int(i) % cols);
    group_->addButton(b, int(i));
    buttons_.push_back(b);
  }
  connect(group_, &QButtonGroup::idClicked, this, [this](int id) {
    for (size_t i = 0; i < buttons_.size(); ++i) buttons_[i]->setChecked(int(i) == id);
    emit changed(options_[size_t(id)].value);
  });
}

void Segmented::setValue(const QVariant& v) {
  for (size_t i = 0; i < options_.size(); ++i) {
    const bool on = options_[i].value == v;
    buttons_[i]->setChecked(on);
  }
}

QVariant Segmented::value() const {
  for (size_t i = 0; i < buttons_.size(); ++i) {
    if (buttons_[i]->isChecked()) return options_[i].value;
  }
  return {};
}

// ---- 数値の増減（ホイール・矢印キー）の共通の決め方 -------------------------------------------

double steppedValue(double cur, int direction, Qt::KeyboardModifiers mods, const NumberStep& rule) {
  const bool wantFine = (mods & Qt::AltModifier) && !(mods & Qt::ShiftModifier) && rule.fine > 0;
  const double step = (mods & Qt::ShiftModifier) ? rule.unit * 10 : (wantFine ? rule.fine : rule.unit);
  // ちょうど単位の刻みの上にある値は、動かしたあとも刻みの上に丸める（0.30000000000000004 のような誤差を残さない）。
  // 刻みの上にない値（ドラッグで合わせた値など）は、単位ぶんだけ動かす（急に刻みへ飛ばない）。
  // 細かい単位で合わせた値（60.1%）から基本単位ぶん動かしても、その細かさ（61.1%）を保つ
  const double grid = rule.fine > 0 ? rule.fine : rule.unit;
  double v = cur + direction * step;
  if (grid > 0 && std::abs(cur - std::round(cur / grid) * grid) < grid * 1e-3) v = std::round(v / grid) * grid;
  v = std::round(v * 1e9) / 1e9;
  if (rule.max > rule.min) v = std::clamp(v, rule.min, rule.max);
  return v;
}

QString formatPercent(double v) {
  const double p = v * 100;
  if (std::abs(p - std::round(p)) < 0.05) return QStringLiteral("%1%").arg(std::lround(p));
  return QStringLiteral("%1%").arg(p, 0, 'f', 1);
}

QString formatDegrees(double v) {
  if (std::abs(v - std::round(v)) < 0.05) return QStringLiteral("%1°").arg(std::lround(v));
  return QStringLiteral("%1°").arg(v, 0, 'f', 1);
}

namespace {
constexpr int kSettleMs = 450;  // ホイール・矢印キーの操作が止まってから、履歴を区切るまで
constexpr int kWheelEngagedMs = 600;
constexpr int kWheelScrollGuardMs = 300;

qint64 nowMs() {
  return QDateTime::currentMSecsSinceEpoch();
}

/** 数値の部品（またはその中の部品）か */
bool isNumericWidget(const QWidget* w) {
  for (; w; w = w->parentWidget()) {
    if (w->property("kvNumeric").toBool()) return true;
  }
  return false;
}

qint64 g_lastScrollWheel = 0;  // 数値の部品の外（パネルのスクロールなど）で、ホイールが使われた最後の時刻

/**
 * パネルをホイールでスクロールしているとき、通り道にある数値欄の値が勝手に変わらないようにする。
 * スクロールが止まって少したってから回したとき（または、その欄を回している最中）だけ、値を変える
 */
class WheelScrollWatcher : public QObject {
 protected:
  bool eventFilter(QObject* obj, QEvent* e) override {
    if (e->type() == QEvent::Wheel) {
      const auto* w = qobject_cast<QWidget*>(obj);
      if (w && !isNumericWidget(w)) g_lastScrollWheel = nowMs();
    }
    return false;
  }
};

void ensureWheelWatcher() {
  static bool installed = false;
  if (installed || !qApp) return;
  installed = true;
  qApp->installEventFilter(new WheelScrollWatcher());
}

/** ホイール 1 回ぶん（ノッチ）の数。高精度のホイールは少しずつためる。Alt を押すと縦横が入れ替わるので、横も見る */
int wheelSteps(const QWheelEvent* e, double* accum) {
  int d = e->angleDelta().y();
  if (d == 0) d = e->angleDelta().x();
  *accum += d;
  const int steps = int(*accum / 120);
  *accum -= steps * 120;
  return steps;
}

/** この欄でホイールを使ってよいか（スクロールの途中なら、パネルのスクロールに任せる） */
bool wheelAllowed(qint64 lastOwn) {
  const qint64 now = nowMs();
  return now - lastOwn < kWheelEngagedMs || now - g_lastScrollWheel >= kWheelScrollGuardMs;
}
}  // namespace

// ---- NumberSpin -------------------------------------------------------------

NumberSpin::NumberSpin(QWidget* parent) : QDoubleSpinBox(parent) {
  ensureWheelWatcher();
  setProperty("kvNumeric", true);
  setKeyboardTracking(false);
  setFocusPolicy(Qt::StrongFocus);
  setAccelerated(false);
  setToolTip(QStringLiteral("数値を入力 / ホイールや矢印キーで調整（Shift で10倍、Alt で細かく）"));
  settle_ = new QTimer(this);
  settle_->setSingleShot(true);
  settle_->setInterval(kSettleMs);
  connect(settle_, &QTimer::timeout, this, &NumberSpin::stepFinished);
  // 入力を確定したら「入力の途中」ではなくなる（そのあとの ← → は、値の増減に使う）
  connect(this, &QAbstractSpinBox::editingFinished, this, [this] {
    if (lineEdit()) lineEdit()->setModified(false);
  });
}

void NumberSpin::setUnit(double unit, double fine) {
  setSingleStep(unit);
  fine_ = fine;
}

void NumberSpin::restartSettle() {
  settle_->start();
}

void NumberSpin::stepBy(int steps, Qt::KeyboardModifiers mods) {
  const NumberStep rule{singleStep(), fine_, minimum(), maximum()};
  // 小数の桁より細かい単位は使えない（表示と値がずれるため）
  NumberStep r = rule;
  if (r.fine > 0 && std::abs(std::round(r.fine * std::pow(10, decimals())) - r.fine * std::pow(10, decimals())) > 1e-6) r.fine = 0;
  double v = value();
  const int dir = steps > 0 ? 1 : -1;
  for (int i = 0; i < std::abs(steps); ++i) v = steppedValue(v, dir, mods, r);
  setValue(v);
  restartSettle();
}

void NumberSpin::wheelEvent(QWheelEvent* e) {
  if (!wheelAllowed(lastWheelMs_)) {
    e->ignore();
    return;
  }
  const int steps = wheelSteps(e, &wheelAccum_);
  e->accept();
  if (steps == 0) return;
  lastWheelMs_ = nowMs();
  stepBy(steps, e->modifiers());
}

void NumberSpin::keyPressEvent(QKeyEvent* e) {
  const bool editing = lineEdit() && lineEdit()->isModified();
  int dir = 0;
  switch (e->key()) {
    case Qt::Key_Up: dir = 1; break;
    case Qt::Key_Down: dir = -1; break;
    // 左右は、入力の途中でなければ値の増減（入力の途中は、カーソルの移動のまま）
    case Qt::Key_Right: dir = editing ? 0 : 1; break;
    case Qt::Key_Left: dir = editing ? 0 : -1; break;
    default: break;
  }
  if (dir != 0 && !(e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
    stepBy(dir, e->modifiers());
    e->accept();
    return;
  }
  QDoubleSpinBox::keyPressEvent(e);
}

void NumberSpin::focusInEvent(QFocusEvent* e) {
  QDoubleSpinBox::focusInEvent(e);
  // 選んだら全体を選択状態にして、そのまま打ち込み / 矢印キーで調整できるようにする
  QTimer::singleShot(0, this, [this] {
    if (hasFocus() && lineEdit()) lineEdit()->selectAll();
  });
}

void NumberSpin::focusOutEvent(QFocusEvent* e) {
  if (settle_->isActive()) {
    settle_->stop();
    emit stepFinished();
  }
  QDoubleSpinBox::focusOutEvent(e);
}

void NumberSpin::enterEvent(QEnterEvent* e) {
  QDoubleSpinBox::enterEvent(e);
  setStyleSheet(QStringLiteral("QDoubleSpinBox{border:1px solid #2680eb}"));  // ホイールで調整できることを、枠の色で知らせる
}

void NumberSpin::leaveEvent(QEvent* e) {
  QDoubleSpinBox::leaveEvent(e);
  setStyleSheet(QString());
}

// ---- SliderRow --------------------------------------------------------------

/** SliderRow のスライダー。ダブルクリックで基準値、ホイール・矢印キーは SliderRow の決め方で動かす */
class NumberSlider : public QSlider {
 public:
  using QSlider::QSlider;
  std::function<void()> onDoubleClick;
  SliderRow* row = nullptr;
  /** つまみの中心（スライダー内の座標） */
  QPoint handleCenter() const {
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    return style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this).center();
  }

 protected:
  void mouseDoubleClickEvent(QMouseEvent* e) override {
    if (onDoubleClick) onDoubleClick();
    e->accept();
  }
  void wheelEvent(QWheelEvent* e) override {
    if (row) row->handleWheel(e);
    else QSlider::wheelEvent(e);
  }
  void keyPressEvent(QKeyEvent* e) override {
    if (row && !(e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
      switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
          row->handleArrow(e);
          return;
        case Qt::Key_Escape:
        case Qt::Key_Return:
        case Qt::Key_Enter:
          clearFocus();  // 選択をやめる（矢印キーは、タイムラインなどのショートカットに戻る）
          e->accept();
          return;
        default: break;
      }
    }
    QSlider::keyPressEvent(e);
  }
};

SliderRow::SliderRow(const QString& label, double min, double max, double step, std::function<QString(double)> format, QWidget* parent)
    : QWidget(parent), min_(min), max_(max), step_(step), unit_(step), value_(min), format_(std::move(format)) {
  ensureWheelWatcher();
  setProperty("kvNumeric", true);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(2);
  auto* head = new QHBoxLayout();
  head->setSpacing(8);
  label_ = new QLabel(label, this);
  valueLabel_ = makeLabel(QString(), "muted", this);
  valueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  valueLabel_->setCursor(Qt::IBeamCursor);
  valueLabel_->setToolTip(QStringLiteral("クリックして選ぶと、矢印キーで調整できます（Shift で10倍、Alt で細かく）。ホイールでも調整できます"));
  valueLabel_->installEventFilter(this);
  refreshValueStyle();
  head->addWidget(label_);
  head->addStretch(1);
  head->addWidget(valueLabel_);
  v->addLayout(head);
  auto* s = new NumberSlider(Qt::Horizontal, this);
  s->row = this;
  slider_ = s;
  slider_->setRange(0, int(std::lround((max - min) / step)));
  // クリックでは選ばない（ドラッグのあとに矢印キーがタイムラインの操作でなくなるのを防ぐ）。数値をクリックするか Tab で選ぶ
  slider_->setFocusPolicy(Qt::TabFocus);
  slider_->setAccessibleName(label);
  slider_->setToolTip(QStringLiteral("ドラッグ / ホイールで調整（Shift で10倍、Alt で細かく）。ダブルクリックで元の値"));
  slider_->installEventFilter(this);
  v->addWidget(slider_);
  settle_ = new QTimer(this);
  settle_->setSingleShot(true);
  settle_->setInterval(kSettleMs);
  connect(settle_, &QTimer::timeout, this, &SliderRow::settle);
  s->onDoubleClick = [this] {
    setValue(default_);
    emit changed(default_);
    emit released();
  };
  // ドラッグ・Page キーなど、スライダー自身の操作
  connect(slider_, &QSlider::valueChanged, this, [this](int pos) {
    value_ = std::clamp(min_ + pos * step_, min_, max_);
    updateText();
    updateBubble();  // ドラッグ中なら、マウスの動きに合わせて数値も更新する
    emit changed(value());
  });
  connect(slider_, &QSlider::sliderPressed, this, [this] { updateBubble(); });
  connect(slider_, &QSlider::sliderReleased, this, [this] {
    if (bubble_) bubble_->hide();  // ドラッグが終わったら消す
    emit released();
  });
}

void SliderRow::setUnit(double unit, double fine) {
  unit_ = unit > 0 ? unit : step_;
  fine_ = fine;
}

void SliderRow::updateBubble(bool force) {
  if (!bubbleOn_ || !(force || slider_->isSliderDown())) return;
  if (!bubble_) {
    // 画面の一番上に出る小さな表示（クリックなどは受け取らない）
    bubble_ = new QLabel(this);
    bubble_->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    bubble_->setAttribute(Qt::WA_ShowWithoutActivating);
    bubble_->setAttribute(Qt::WA_TransparentForMouseEvents);
    bubble_->setAlignment(Qt::AlignCenter);
    bubble_->setStyleSheet(QStringLiteral("QLabel{background:#2680eb;color:#ffffff;border-radius:4px;padding:2px 8px;font-weight:bold}"));
  }
  bubble_->setText(format_ ? format_(value()) : QString::number(value(), 'f', 2));
  bubble_->adjustSize();
  const QPoint handle = static_cast<NumberSlider*>(slider_)->handleCenter();
  const QPoint pos = slider_->mapToGlobal(QPoint(handle.x(), 0));
  bubble_->move(pos.x() - bubble_->width() / 2, pos.y() - bubble_->height() - 4);
  if (!bubble_->isVisible()) bubble_->show();
}

void SliderRow::setValue(double v) {
  const QSignalBlocker block(slider_);
  value_ = std::clamp(v, min_, max_);
  slider_->setValue(int(std::lround((value_ - min_) / step_)));
  updateText();
}

double SliderRow::value() const {
  return value_;
}

void SliderRow::setLabelVisible(bool v) {
  label_->setVisible(v);
}

void SliderRow::updateText() {
  valueLabel_->setText(format_ ? format_(value()) : QString::number(value(), 'f', 2));
}

// ---- SliderRow: ホイール・矢印キー ------------------------------------------------------

void SliderRow::wheelEvent(QWheelEvent* e) {
  handleWheel(e);
}

void SliderRow::handleWheel(QWheelEvent* e) {
  // スライダーが無効（キーフレームの音量など）のときは、何も変えずパネルのスクロールに任せる
  if (!slider_->isEnabled() || !wheelAllowed(lastWheelMs_)) {
    e->ignore();
    return;
  }
  const int steps = wheelSteps(e, &wheelAccum_);
  e->accept();
  if (steps == 0) return;
  lastWheelMs_ = nowMs();
  stepBy(steps, e->modifiers());
}

void SliderRow::handleArrow(QKeyEvent* e) {
  const int dir = (e->key() == Qt::Key_Up || e->key() == Qt::Key_Right) ? 1 : -1;
  e->accept();
  stepBy(dir, e->modifiers());
}

void SliderRow::stepBy(int steps, Qt::KeyboardModifiers mods) {
  if (!slider_->isEnabled() || steps == 0) return;
  const NumberStep rule{unit_, fine_, min_, max_};
  double v = value_;
  const int dir = steps > 0 ? 1 : -1;
  for (int i = 0; i < std::abs(steps); ++i) v = steppedValue(v, dir, mods, rule);
  if (std::abs(v - value_) > 1e-12) {
    // スライダーの位置を合わせてから（シグナルは出さない）、変わったことを知らせる
    setValue(v);
    updateBubble(true);
    emit changed(value_);
  }
  restartSettle();
}

void SliderRow::restartSettle() {
  settle_->start();
}

void SliderRow::settle() {
  if (bubble_ && !slider_->isSliderDown()) bubble_->hide();
  emit released();
}

void SliderRow::setHot(bool on) {
  if (hot_ == on) return;
  hot_ = on;
  refreshValueStyle();
}

/** 数値の見た目: カーソルを合わせると青、選んでいる（矢印キーで調整できる）ときは枠も付く。枠の幅は常に同じなので、大きさは変わらない */
void SliderRow::refreshValueStyle() {
  QString css = QStringLiteral("border:1px solid %1;border-radius:3px;padding:0 3px;").arg(focused_ ? QStringLiteral("#2680eb") : QStringLiteral("transparent"));
  if (hot_ || focused_) css += QStringLiteral("color:#4da3ff;");
  valueLabel_->setStyleSheet(css);
}

bool SliderRow::eventFilter(QObject* obj, QEvent* e) {
  if (obj == slider_) {
    switch (e->type()) {
      case QEvent::Enter: setHot(true); break;
      case QEvent::Leave: setHot(false); break;
      case QEvent::FocusIn:
        focused_ = true;
        refreshValueStyle();
        break;
      case QEvent::FocusOut:
        focused_ = false;
        refreshValueStyle();
        if (settle_->isActive()) {
          settle_->stop();
          settle();
        }
        break;
      default: break;
    }
  } else if (obj == valueLabel_) {
    if (e->type() == QEvent::Enter) setHot(true);
    else if (e->type() == QEvent::Leave) setHot(false);
    // 数値をクリックすると選ばれて、矢印キーで調整できる
    else if (e->type() == QEvent::MouseButtonPress && slider_->isEnabled()) slider_->setFocus(Qt::MouseFocusReason);
  }
  return QWidget::eventFilter(obj, e);
}

// ---- ColorSwatches ----------------------------------------------------------

static QIcon swatchIcon(const QColor& c, bool selected) {
  QPixmap pm(40, 40);
  pm.fill(Qt::transparent);
  pm.setDevicePixelRatio(2);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  if (selected) {
    p.setPen(QPen(theme::text, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(0.75, 0.75, 18.5, 18.5), 4, 4);
  }
  p.setPen(QPen(QColor(255, 255, 255, 50), 1));
  p.setBrush(c);
  p.drawRoundedRect(QRectF(3, 3, 14, 14), 3, 3);
  return QIcon(pm);
}

static QIcon customIcon() {
  QPixmap pm(40, 40);
  pm.fill(Qt::transparent);
  pm.setDevicePixelRatio(2);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  QConicalGradient g(10, 10, 0);
  for (int i = 0; i <= 6; ++i) g.setColorAt(i / 6.0, QColor::fromHsv((i * 60) % 360, 230, 255));
  p.setPen(QPen(QColor(255, 255, 255, 50), 1));
  p.setBrush(g);
  p.drawRoundedRect(QRectF(3, 3, 14, 14), 3, 3);
  return QIcon(pm);
}

ColorSwatches::ColorSwatches(const QString& name, QWidget* parent) : QWidget(parent), name_(name) {
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(2);
  for (const QColor& c : colorSwatches()) {
    auto* b = new QToolButton(this);
    b->setIconSize(QSize(20, 20));
    b->setFixedSize(24, 24);
    b->setFocusPolicy(Qt::TabFocus);
    b->setAccessibleName(QStringLiteral("%1 %2").arg(name, c.name()));
    b->setToolTip(c.name());
    connect(b, &QToolButton::clicked, this, [this, c] {
      setColor(c);
      emit changed(c);
    });
    h->addWidget(b);
    buttons_.push_back(b);
  }
  custom_ = new QToolButton(this);
  custom_->setIconSize(QSize(20, 20));
  custom_->setFixedSize(24, 24);
  custom_->setIcon(customIcon());
  custom_->setToolTip(QStringLiteral("その他の色…"));
  custom_->setAccessibleName(QStringLiteral("%1: その他の色").arg(name));
  connect(custom_, &QToolButton::clicked, this, [this] {
    const QColor c = QColorDialog::getColor(color_, this, name_);
    if (!c.isValid()) return;
    setColor(c);
    emit changed(c);
  });
  h->addWidget(custom_);
  h->addStretch(1);
  refresh();
}

void ColorSwatches::setColor(const QColor& c) {
  color_ = c;
  refresh();
}

void ColorSwatches::refresh() {
  const auto& sw = colorSwatches();
  for (size_t i = 0; i < buttons_.size(); ++i) buttons_[i]->setIcon(swatchIcon(sw[i], sw[i].rgb() == color_.rgb()));
}

// ---- SecondsField -----------------------------------------------------------

SecondsField::SecondsField(const QString& label, double min, double max, QWidget* parent) : QWidget(parent) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(3);
  v->addWidget(makeLabel(label, "muted", this));
  auto* h = new QHBoxLayout();
  h->setSpacing(6);
  spin_ = new NumberSpin(this);
  spin_->setRange(min, max);
  spin_->setDecimals(1);
  spin_->setUnit(0.1);  // 0.1 秒（Shift で 1 秒）。表示は小数 1 桁なので、Alt の細かい単位は無い
  spin_->setButtonSymbols(QAbstractSpinBox::NoButtons);
  spin_->setAccessibleName(label);
  h->addWidget(spin_, 1);
  h->addWidget(makeLabel(QStringLiteral("秒"), "muted", this));
  v->addLayout(h);
  connect(spin_, &QDoubleSpinBox::valueChanged, this, [this](double val) { emit changed(val); });
  // 入力を確定したとき、ホイール・矢印キーが一段落したときに、元に戻す履歴を区切る
  connect(spin_, &QAbstractSpinBox::editingFinished, this, [this] { emit released(); });
  connect(spin_, &NumberSpin::stepFinished, this, [this] { emit released(); });
}

void SecondsField::setValue(double v) {
  if (spin_->hasFocus()) return;
  const QSignalBlocker block(spin_);
  spin_->setValue(v);
}

// ---- Section ----------------------------------------------------------------

Section::Section(const QString& title, QWidget* parent) : QWidget(parent) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 12, 0, 12);
  v->setSpacing(8);
  head_ = new QHBoxLayout();
  head_->setContentsMargins(0, 0, 0, 0);
  head_->addWidget(makeLabel(title, "section", this));
  head_->addStretch(1);
  v->addLayout(head_);
  body_ = new QVBoxLayout();
  body_->setContentsMargins(0, 0, 0, 0);
  body_->setSpacing(10);
  v->addLayout(body_);
}

void Section::setHeaderWidget(QWidget* w) {
  head_->addWidget(w);
}

}  // namespace kv
