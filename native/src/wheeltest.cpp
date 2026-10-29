// 一時的: 設定欄のスライダーの「選んでいるときだけホイール」の確認（確認が終わったら消す）
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "ui/widgets.h"

namespace kv {

namespace {
QStringList g_log;
int g_fail = 0;
QString g_out;
void logLine(const QString& s) {
  g_log << s;
  QFile f(g_out + QStringLiteral("/result.log"));
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(g_log.join(QLatin1Char('\n')).toUtf8());
}
void check(bool ok, const QString& what) {
  if (!ok) ++g_fail;
  logLine(QString(ok ? QStringLiteral("OK   ") : QStringLiteral("FAIL ")) + what);
}
void pump(int ms) {
  QElapsedTimer t;
  t.start();
  while (t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}
/** w の座標 p に、本物のマウスのホイールと同じく（受け取らなければ親へ伝えて）1 回送る */
void wheelOn(QWidget* w, QPoint local, int dy = -120) {
  if (QWidget* c = w->childAt(local)) {
    local = c->mapFrom(w, local);
    w = c;
  }
  while (w) {
    QWheelEvent e(QPointF(local), QPointF(w->mapToGlobal(local)), QPoint(), QPoint(0, dy), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
    if (e.isAccepted() || w->isWindow()) break;
    local += w->pos();
    w = w->parentWidget();
  }
}
/** w の座標 p を押して離す（本物と同じく、受け取らなければ親へ伝える） */
void clickOn(QWidget* w, QPoint local) {
  if (QWidget* c = w->childAt(local)) {
    local = c->mapFrom(w, local);
    w = c;
  }
  QWidget* pressed = w;
  QPoint pl = local;
  while (w) {
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(w->mapToGlobal(local)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    pressed = w;
    pl = local;
    if (press.isAccepted() || w->isWindow()) break;
    local += w->pos();
    w = w->parentWidget();
  }
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(pl), QPointF(pressed->mapToGlobal(pl)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(pressed, &release);
}
/** 選んでいる印（つまみの青い縁）が付いているか */
bool selected(QSlider* s) {
  return !s->styleSheet().isEmpty();
}
}  // namespace

int runWheelTest(const QString& out) {
  g_out = out;
  QScrollArea area;
  area.setAttribute(Qt::WA_DontShowOnScreen);
  area.setWidgetResizable(true);
  auto* content = new QWidget();
  auto* v = new QVBoxLayout(content);
  v->setContentsMargins(12, 12, 12, 12);
  v->setSpacing(14);
  auto* row = new SliderRow(QStringLiteral("音量"), 0, 1, 0.01, [](double x) { return formatPercent(x); }, content);
  row->setUnit(0.01, 0.001);
  auto* row2 = new SliderRow(QStringLiteral("不透明度"), 0, 1, 0.01, [](double x) { return formatPercent(x); }, content);
  auto* hidden = new SliderRow(QStringLiteral("字間"), -50, 100, 1, [](double x) { return QString::number(x); }, content);
  hidden->setLabelVisible(false);
  hidden->setValueVisible(false);
  v->addWidget(row);
  v->addWidget(row2);
  v->addWidget(hidden);
  v->addSpacing(3000);
  area.setWidget(content);
  area.resize(320, 400);
  area.show();
  area.activateWindow();
  pump(500);
  logLine(QStringLiteral("ウィンドウが有効: %1").arg(area.isActiveWindow()));
  auto* slider = row->findChild<QSlider*>();
  auto* slider2 = row2->findChild<QSlider*>();
  auto* hs = hidden->findChild<QSlider*>();
  QLabel* valueLabel = nullptr;
  for (auto* l : row->findChildren<QLabel*>(Qt::FindDirectChildrenOnly)) {
    if (l->text() != QStringLiteral("音量")) valueLabel = l;
  }
  check(slider && slider2 && hs && valueLabel, QStringLiteral("部品が見つかる"));
  if (!slider || !slider2 || !hs || !valueLabel) return 1;
  int top = -1, bottom = -1;
  for (int y = 0; y < slider->height(); ++y) {
    if (static_cast<LineSlider*>(slider)->hitsBody(QPoint(slider->width() / 2, y))) {
      if (top < 0) top = y;
      bottom = y;
    }
  }
  const int midX = slider->width() / 2, cy = (top + bottom) / 2;
  logLine(QStringLiteral("帯: y=%1〜%2").arg(top).arg(bottom));
  auto scroll = [&] { return area.verticalScrollBar()->value(); };
  auto reset = [&](double val) {
    row->setValue(val);
    area.verticalScrollBar()->setValue(0);
    pump(800);  // スクロールの誤操作防止（ガード）が切れるまで
  };
  auto bordered = [&] { return valueLabel->styleSheet().contains(QStringLiteral("#2680eb")); };

  // 0. Qt が自動でフォーカスを移してきても、選んだことにはしない
  pump(300);
  check(!selected(slider) && !bordered() && QApplication::focusWidget() != slider,
        QStringLiteral("ウィンドウが開いたときに自動で来たフォーカスでは選ばない（手放す）: フォーカス %1")
            .arg(QApplication::focusWidget() ? QString::fromLatin1(QApplication::focusWidget()->metaObject()->className()) : QStringLiteral("なし")));
  slider->setFocus(Qt::OtherFocusReason);
  pump(100);
  check(!selected(slider) && !slider->hasFocus(), QStringLiteral("部品が隠れたときなどに移ってきたフォーカスも、選ばずに手放す"));
  slider->setFocus(Qt::TabFocusReason);  // Tab キーを押していないのに Tab の理由で来た（隠れた部品からの自動の移動）
  pump(100);
  check(!selected(slider) && !slider->hasFocus(), QStringLiteral("Tab キーを押していない Tab の理由の移動も、選ばずに手放す"));

  // 1. 選んでいないときのホイールは、値を変えずにスクロール
  reset(0.5);
  wheelOn(slider, QPoint(midX, cy));
  check(std::abs(row->value() - 0.5) < 1e-9 && scroll() > 0, QStringLiteral("選んでいないスライダーの真ん中: 値 %1・スクロール %2").arg(row->value()).arg(scroll()));
  reset(0.5);
  wheelOn(row, valueLabel->geometry().center());
  check(std::abs(row->value() - 0.5) < 1e-9 && scroll() > 0, QStringLiteral("選んでいない数値の上: 値 %1・スクロール %2").arg(row->value()).arg(scroll()));

  // 2. 周りの余白を押しても選ばない
  reset(0.5);
  clickOn(slider, QPoint(midX, top - 1));
  check(!selected(slider) && std::abs(row->value() - 0.5) < 1e-9, QStringLiteral("線の外（帯の 1px 上）を押しても選ばない: 値 %1").arg(row->value()));

  // 3. 線を押すと選ぶ（その位置へ動く）
  clickOn(slider, QPoint(slider->width() / 4, cy));
  pump(100);
  check(selected(slider) && bordered() && row->value() < 0.4, QStringLiteral("線を押すと選ぶ（数値の枠・つまみの縁）: 値 %1").arg(row->value()));
  {
    int t2 = -1, b2 = -1;
    for (int y = 0; y < slider->height(); ++y) {
      if (static_cast<LineSlider*>(slider)->hitsBody(QPoint(midX, y))) {
        if (t2 < 0) t2 = y;
        b2 = y;
      }
    }
    check(t2 == top && b2 == bottom, QStringLiteral("縁を付けても、つまみの大きさは同じ: y=%1〜%2").arg(t2).arg(b2));
  }
  area.grab().save(g_out + QStringLiteral("/selected.png"));

  // 4. 選んでいる間のホイール
  reset(0.5);
  wheelOn(slider, QPoint(midX, cy));
  check(std::abs(row->value() - 0.49) < 1e-9 && scroll() == 0, QStringLiteral("選んでいるスライダーの真ん中: 値 %1・スクロール %2").arg(row->value()).arg(scroll()));
  reset(0.5);
  wheelOn(slider, QPoint(midX, bottom + 1));
  check(std::abs(row->value() - 0.5) < 1e-9 && scroll() > 0, QStringLiteral("選んでいても、帯の 1px 下では変わらない: 値 %1・スクロール %2").arg(row->value()).arg(scroll()));
  reset(0.5);
  wheelOn(row, valueLabel->geometry().center());
  check(std::abs(row->value() - 0.49) < 1e-9 && scroll() == 0, QStringLiteral("選んでいるときの数値の上: 値 %1").arg(row->value()));
  reset(0.5);
  wheelOn(slider2, QPoint(slider2->width() / 2, cy));
  check(std::abs(row2->value()) < 1e-9 && scroll() > 0, QStringLiteral("ほかの（選んでいない）スライダーの上は、スクロール: スクロール %1").arg(scroll()));
  check(selected(slider), QStringLiteral("ホイールでは選択は変わらない"));

  // 5. 矢印キー
  reset(0.5);
  {
    QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(slider, &right);
  }
  check(std::abs(row->value() - 0.51) < 1e-9, QStringLiteral("→ キーで増える: %1").arg(row->value()));

  // 6. 欄の外をクリックすると選択が終わる
  clickOn(content, QPoint(150, content->height() - 50));
  pump(100);
  check(!selected(slider) && !bordered(), QStringLiteral("欄の外をクリックすると選択が終わる"));
  reset(0.5);
  wheelOn(slider, QPoint(midX, cy));
  check(std::abs(row->value() - 0.5) < 1e-9, QStringLiteral("終わったあとのホイールでは変わらない: %1").arg(row->value()));

  // 7. 数値をクリックして選ぶ → 同じ欄の見出しを押しても選んだまま → Esc でやめる
  clickOn(row, valueLabel->geometry().center());
  pump(100);
  check(selected(slider), QStringLiteral("数値をクリックすると選ぶ"));
  clickOn(row, QPoint(5, valueLabel->geometry().center().y()));
  check(selected(slider), QStringLiteral("同じ欄の見出しを押しても、選んだまま"));
  {
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(slider, &esc);
  }
  check(!selected(slider), QStringLiteral("Esc でやめる"));
  clickOn(row, valueLabel->geometry().center());
  {
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(slider, &enter);
  }
  check(!selected(slider), QStringLiteral("Enter でもやめる"));

  // 8. 別のスライダーを押すと、そちらに移る
  clickOn(slider, QPoint(midX, cy));
  clickOn(slider2, QPoint(slider2->width() / 2, cy));
  pump(100);
  check(!selected(slider) && selected(slider2) && !bordered(), QStringLiteral("別のスライダーを押すと、選ぶのがそちらに移る"));

  // 8b. Tab キーで来たときは選ぶ（ウィンドウが有効なときだけ確かめられる）
  if (area.isActiveWindow() && slider2->hasFocus()) {
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(slider2, &tab);
    pump(100);
    QWidget* f = QApplication::focusWidget();
    check(f == hs && selected(hs) && !selected(slider2), QStringLiteral("Tab キーで次のスライダーへ移ると、そちらを選ぶ"));
  } else {
    logLine(QStringLiteral("SKIP Tab キー（ウィンドウが有効でない）"));
  }

  // 9. 数値を隠したスライダーも、押せば選べて、つまみに印が付く
  clickOn(content, QPoint(150, content->height() - 50));
  clickOn(hs, QPoint(hs->width() / 2, cy));
  pump(100);
  check(selected(hs), QStringLiteral("数値を隠したスライダー（字間）も選べて、つまみに印"));
  area.grab().save(g_out + QStringLiteral("/hidden-selected.png"));

  logLine(QStringLiteral("== 結果: %1").arg(g_fail == 0 ? QStringLiteral("すべて OK") : QStringLiteral("%1 件 FAIL").arg(g_fail)));
  area.hide();
  return g_fail == 0 ? 0 : 1;
}

}  // namespace kv
