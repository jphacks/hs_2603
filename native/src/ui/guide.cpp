#include "ui/guide.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDropEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <algorithm>
#include <cmath>

#include <windows.h>

#include "core/editor.h"
#include "ui/inspector.h"
#include "ui/mainwindow.h"
#include "ui/perspective.h"
#include "ui/theme.h"
#include "ui/timelinewidget.h"
#include "ui/widgets.h"

namespace kv {

using ai::GuideKind;
using ai::GuideNeed;
using ai::GuideTarget;

namespace {

/** 示す場所を見直す間隔（スクロール・メニューを開いた などに付いていく） */
constexpr int kTickMs = 120;
/** この間、手順が進まなければ案内をやめる */
constexpr qint64 kIdleMs = 120000;
/** 準備の案内をたどる深さの上限（準備の準備…が、ぐるぐる回らないように） */
constexpr int kMaxDepth = 5;
/** 同じ部品を、またスクロールして見せるまでの間（ユーザーが自分でスクロールしたのを、すぐには戻さない） */
constexpr qint64 kRevealAgainMs = 2500;
constexpr double kPi = 3.14159265358979323846;

QRect toGlobal(const QWidget* w, const QRect& local) {
  if (!w || local.isEmpty()) return {};
  return QRect(w->mapToGlobal(local.topLeft()), local.size());
}

/** 部品の、見えている所（画面の座標）。親の外にはみ出して隠れている所は除く。見えていなければ空 */
QRect shownRect(const QWidget* w) {
  if (!w || !w->isVisible()) return {};
  QRect r = w->rect();
  QPoint off(0, 0);
  for (const QWidget* cur = w; cur && !cur->isWindow(); cur = cur->parentWidget()) {
    off += cur->pos();
    const QWidget* p = cur->parentWidget();
    if (!p) break;
    r = r.intersected(p->rect().translated(-off));
    if (r.isEmpty()) return {};
  }
  return toGlobal(w, r);
}

/** 案内で探す名前の部品（objectName か、プロパティ kvGuide の値）。見えているものを先に、無ければ隠れているものを返す */
QWidget* findNamed(QWidget* root, const QString& name) {
  if (!root || name.isEmpty()) return nullptr;
  QWidget* hidden = nullptr;
  for (QWidget* w : root->findChildren<QWidget*>(name)) {
    if (w->isVisible()) return w;
    if (!hidden) hidden = w;
  }
  if (hidden) return hidden;
  for (QWidget* w : root->findChildren<QWidget*>()) {
    if (w->property("kvGuide").toString() != name) continue;
    if (w->isVisible()) return w;
    if (!hidden) hidden = w;
  }
  return hidden;
}

/** メニューの項目の文字（「ファイル(&F)」→「ファイル」。キーの表示と「...」は外す） */
QString menuText(QString t) {
  const qsizetype tab = t.indexOf(QLatin1Char('\t'));
  if (tab >= 0) t = t.left(tab);
  static const QRegularExpression mnemonic(QStringLiteral("\\(&.\\)"));
  t.remove(mnemonic);
  t.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
  t.remove(QLatin1Char('&'));
  t.replace(QLatin1Char('\x01'), QLatin1Char('&'));
  t.remove(QStringLiteral("..."));
  return t.trimmed();
}

/** メニューの項目 a までの道（out は、メニューバーの見出しのメニューから、a の入っているメニューまで） */
bool menuPath(QAction* a, QMenuBar** bar, std::vector<QMenu*>* out, int depth = 0) {
  if (!a || depth > 8) return false;
  for (QObject* o : a->associatedObjects()) {
    if (auto* b = qobject_cast<QMenuBar*>(o)) {
      *bar = b;
      return true;
    }
  }
  for (QObject* o : a->associatedObjects()) {
    auto* m = qobject_cast<QMenu*>(o);
    if (!m || !menuPath(m->menuAction(), bar, out, depth + 1)) continue;
    out->push_back(m);
    return true;
  }
  return false;
}

/** 部品を、入っているスクロールの欄の中で見えるようにする */
void revealWidget(QWidget* w) {
  for (QWidget* p = w ? w->parentWidget() : nullptr; p; p = p->parentWidget()) {
    auto* area = qobject_cast<QScrollArea*>(p);
    if (area && area->widget() && area->widget()->isAncestorOf(w)) area->ensureWidgetVisible(w, 16, 32);
    if (p->isWindow()) break;
  }
}

/** 一覧なら最初の項目の所、ほかは部品の見えている所（画面の座標） */
QRect targetRect(QWidget* w) {
  if (auto* view = qobject_cast<QAbstractItemView*>(w); view && view->model() && view->model()->rowCount() > 0) {
    const QRect item = view->visualRect(view->model()->index(0, 0)).intersected(view->viewport()->rect());
    if (!item.isEmpty()) return toGlobal(view->viewport(), item);
  }
  return shownRect(w);
}

bool hasTransition() {
  const auto& items = editor().layout().items;
  return std::any_of(items.begin() + (items.empty() ? 0 : 1), items.end(), [](const ClipLayout& l) { return l.clip.transIn.type != TransitionType::None; });
}

bool hasAsset(bool audio) {
  for (const auto& [id, a] : editor().assets()) {
    if ((a.kind == AssetKind::Audio) == audio && a.status != AssetStatus::Error && a.status != AssetStatus::Missing) return true;
  }
  return false;
}

/** 矢印の先を置く所（小さいものは真ん中、横に長いもの・高いものは左上寄り。メニューの項目のような細長いものは、文字にかからない左端寄り） */
QPointF tipFor(const QRect& r) {
  const bool strip = r.width() > 160 && r.height() <= 40;
  const double x = r.left() + (strip ? 30.0 : std::min(r.width() * 0.5, 44.0));
  const double y = r.height() > 64 ? r.top() + 22.0 : r.center().y() + 0.5;
  return {x, y};
}

}  // namespace

// ---- 案内の窓（画面の上に重ねる、マウスを通す透明な窓。矢印・枠・吹き出しを描く） ------------------------

class GuideOverlay : public QWidget {
 public:
  explicit GuideOverlay(QWidget* owner)
      : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus |
                           Qt::NoDropShadowWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setWindowTitle(QStringLiteral("操作の案内"));
    clock_.start();
    anim_.setTimerType(Qt::PreciseTimer);
    anim_.setInterval(16);
    QObject::connect(&anim_, &QTimer::timeout, this, [this] { step(); });
    headFont_ = font();
    headFont_.setPointSizeF(8);
    headFont_.setBold(true);
    mainFont_ = font();
    mainFont_.setPointSizeF(10);
    subFont_ = font();
    subFont_.setPointSizeF(8.5);
  }

  /** 示す所と吹き出しの文（画面の座標）。visible が false なら、覚えるだけで窓は出さない（ほかのアプリを使っている間） */
  void present(const GuideSpot& s, const QString& head, const QString& main, const QString& sub, bool visible) {
    const QPointF tip = tipFor(s.rect);
    const bool textChanged = main != main_ || sub != sub_ || head != head_;
    const bool same = has_ && !ending_ && QLineF(tip, to_).length() < 48 && !textChanged && s.final == final_ && s.drop.isEmpty() == drop_.isEmpty();
    target_ = s.rect;
    drop_ = s.drop;
    avoid_ = s.avoid;
    final_ = s.final;
    head_ = head;
    main_ = main;
    sub_ = sub;
    ending_ = false;
    fade_ = 1;
    useScreen(s.rect);
    if (textChanged) layoutBubble();
    const qint64 now = clock_.elapsed();
    if (!has_) {
      // はじめは、右下から近づける
      pos_ = from_ = tip + QPointF(96, 72);
      to_ = tip;
      glideStart_ = now;
      glideMs_ = 700;
      arrived_ = -1;
      bubbleAlpha_ = 0;
      has_ = true;
    } else if (!same) {
      from_ = pos_;
      to_ = tip;
      glideStart_ = now;
      glideMs_ = std::clamp(qint64(QLineF(from_, to_).length() * 0.8), qint64(380), qint64(760));
      arrived_ = -1;
      if (textChanged) bubbleAlpha_ = 0;  // 新しい説明は、着いてから出す
    } else {
      // 同じ所（スクロールで少し動いた など）: 付いていく
      const QPointF d = tip - to_;
      to_ = tip;
      if (arrived_ >= 0) pos_ = tip;
      else from_ += d;
    }
    bubble_ = placeBubble();
    shown_ = true;
    if (!anim_.isActive()) anim_.start();
    if (visible && !isVisible()) {
      show();
      raiseTop();
    } else if (!visible && isVisible()) {
      hide();
    }
    update(dirtyRect().united(lastDirty_));
  }

  /** 何も出さない（案内は続いている。場所が見つからない・ほかのアプリを使っている） */
  void conceal() {
    shown_ = false;
    ending_ = false;
    anim_.stop();
    if (isVisible()) hide();
    lastDirty_ = QRect();
  }

  /** 案内が終わった（少しずつ消す）。次の案内は、また右下から近づける */
  void end() {
    has_ = false;
    if (!shown_ || !isVisible()) {
      conceal();
      return;
    }
    ending_ = true;
    endStart_ = clock_.elapsed();
    if (!anim_.isActive()) anim_.start();
  }

  /** 開いたメニューなどより手前に出す（アプリを切り替えない） */
  void raiseTop() {
    if (!isVisible()) return;
    SetWindowPos(reinterpret_cast<HWND>(winId()), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  }

  void forget() {
    has_ = false;
    conceal();
  }

 protected:
  void paintEvent(QPaintEvent* e) override {
    QPainter p(this);
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillRect(e->rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (!shown_ || fade_ <= 0) return;
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-area_.topLeft());  // 画面の座標で描く
    p.setOpacity(fade_);
    const qint64 now = clock_.elapsed();
    const double pulse = 0.5 + 0.5 * std::sin(double(now) / 1600.0 * 2 * kPi);
    paintBox(p, target_, pulse, !final_);
    Pointer ptr = pointer(now);
    if (!drop_.isEmpty()) {
      paintBox(p, drop_, pulse, true);
      paintPath(p);
    }
    for (const Ripple& r : ptr.ripples) paintRipple(p, r);
    paintPointer(p, ptr);
    if (bubbleAlpha_ > 0) paintBubble(p);
  }

 private:
  struct Ripple {
    QPointF at;
    double t = 0;  // 0〜1
  };
  struct Pointer {
    QPointF pos;
    double alpha = 1;
    double scale = 1;
    bool carrying = false;
    std::vector<Ripple> ripples;
  };

  /** 矢印の形（先が 0, 0） */
  static const QPainterPath& arrowPath() {
    static const QPainterPath path = [] {
      QPainterPath a;
      a.moveTo(0, 0);
      a.lineTo(0, 17.5);
      a.lineTo(4.4, 13.6);
      a.lineTo(7.4, 20.2);
      a.lineTo(10.4, 18.9);
      a.lineTo(7.5, 12.4);
      a.lineTo(13.2, 12.4);
      a.closeSubpath();
      return a;
    }();
    return path;
  }

  /** ドラッグを見せるときの、行き先の矢印の先 */
  QPointF dropTip() const { return tipFor(drop_); }

  /** 今の矢印の位置・濃さと、波紋（押す・離すの合図） */
  Pointer pointer(qint64 now) const {
    Pointer ptr;
    ptr.pos = pos_;
    if (arrived_ < 0) return ptr;
    const qint64 since = now - arrived_;
    if (drop_.isEmpty()) {
      // 着いたら、ときどき押す合図（少し縮む矢印と、広がる輪）
      const qint64 c = since % 2200;
      if (c < 700) ptr.ripples.push_back({to_, double(c) / 700.0});
      if (c < 180) ptr.scale = 1 - 0.14 * std::sin(kPi * double(c) / 180.0);
      return ptr;
    }
    // ドラッグ: 押す → 行き先まで運ぶ → 離す → 消えて、元の所からくり返す
    const QPointF src = to_;
    const QPointF dst = dropTip();
    const qint64 c = since % 2800;
    if (c < 350) {
      ptr.pos = src;
      ptr.ripples.push_back({src, double(c) / 350.0 * 0.6});
      if (c < 180) ptr.scale = 1 - 0.14 * std::sin(kPi * double(c) / 180.0);
      ptr.carrying = c > 180;
    } else if (c < 1450) {
      const double u = double(c - 350) / 1100.0;
      const double e = u < 0.5 ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3) / 2;
      ptr.pos = curve(src, dst, e);
      ptr.carrying = true;
    } else if (c < 1800) {
      ptr.pos = dst;
      ptr.ripples.push_back({dst, double(c - 1450) / 350.0});
    } else if (c < 2300) {
      ptr.pos = dst;
      ptr.alpha = 1 - double(c - 1800) / 500.0;
    } else {
      ptr.pos = src;
      ptr.alpha = double(c - 2300) / 500.0;
    }
    return ptr;
  }

  /** ドラッグの道（少し上にふくらむ曲線） */
  static QPointF curve(const QPointF& a, const QPointF& b, double t) {
    const QPointF mid = (a + b) / 2 + QPointF(0, -std::min(80.0, QLineF(a, b).length() * 0.18));
    const double u = 1 - t;
    return a * (u * u) + mid * (2 * u * t) + b * (t * t);
  }

  void paintBox(QPainter& p, const QRect& r, double pulse, bool dashed) const {
    if (r.isEmpty()) return;
    QColor fill = theme::accent;
    fill.setAlphaF(float(0.05 + 0.06 * pulse));
    QColor line = theme::accent;
    line.setAlphaF(float(0.38 + 0.30 * pulse));
    p.setBrush(fill);
    p.setPen(QPen(line, 2, dashed ? Qt::DashLine : Qt::SolidLine));
    p.drawRoundedRect(QRectF(r).adjusted(-3, -3, 3, 3), 6, 6);
  }

  void paintPath(QPainter& p) const {
    const QPointF a = to_;
    const QPointF b = dropTip();
    QPainterPath path(a);
    const QPointF mid = (a + b) / 2 + QPointF(0, -std::min(80.0, QLineF(a, b).length() * 0.18));
    path.quadTo(mid, b);
    QColor c = theme::accent;
    c.setAlphaF(0.45f);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(c, 1.6, Qt::DashLine, Qt::RoundCap));
    p.drawPath(path);
    // 行き先の矢じり
    const QPointF dir = b - mid;
    const double len = std::max(1.0, std::hypot(dir.x(), dir.y()));
    const QPointF u(dir.x() / len, dir.y() / len);
    const QPointF n(-u.y(), u.x());
    QPolygonF head;
    head << b << b - u * 9 + n * 4.5 << b - u * 9 - n * 4.5;
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawPolygon(head);
  }

  void paintRipple(QPainter& p, const Ripple& r) const {
    const double t = std::clamp(r.t, 0.0, 1.0);
    const double radius = 4 + 18 * t;
    QColor ring = theme::accent;
    ring.setAlphaF(float(0.55 * (1 - t)));
    QColor fill = theme::accent;
    fill.setAlphaF(float(0.16 * (1 - t)));
    p.setPen(QPen(ring, 2));
    p.setBrush(fill);
    p.drawEllipse(r.at, radius, radius);
  }

  void paintPointer(QPainter& p, const Pointer& ptr) const {
    if (ptr.alpha <= 0) return;
    p.save();
    p.setOpacity(p.opacity() * ptr.alpha);
    p.translate(ptr.pos);
    if (ptr.carrying) {
      // 運んでいるもの（うすいカード）
      QColor c(255, 255, 255, 60);
      QColor line = theme::accent;
      line.setAlphaF(0.7f);
      p.setPen(QPen(line, 1.2));
      p.setBrush(c);
      p.drawRoundedRect(QRectF(9, 13, 30, 20), 3, 3);
    }
    p.scale(1.15 * ptr.scale, 1.15 * ptr.scale);
    const QPainterPath& arrow = arrowPath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawPath(arrow.translated(1.2, 1.8));
    p.setBrush(QColor(255, 255, 255, 205));
    QColor line = theme::accent;
    line.setAlphaF(0.95f);
    p.setPen(QPen(line, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(arrow);
    p.restore();
  }

  // ---- 吹き出し ----
  static constexpr int kPadX = 11;
  static constexpr int kPadY = 8;
  static constexpr int kTextW = 270;

  void layoutBubble() {
    const QFontMetrics hm(headFont_);
    const QFontMetrics mm(mainFont_);
    const QFontMetrics sm(subFont_);
    const QString esc = QStringLiteral("Esc でやめる");
    headW_ = hm.horizontalAdvance(head_) + 14 + QFontMetrics(subFont_).horizontalAdvance(esc);
    headH_ = hm.height();
    mainRect_ = mm.boundingRect(QRect(0, 0, kTextW, 2000), Qt::TextWordWrap, main_);
    subRect_ = sub_.isEmpty() ? QRect() : sm.boundingRect(QRect(0, 0, kTextW, 2000), Qt::TextWordWrap, sub_);
    const int w = std::max({headW_, mainRect_.width(), subRect_.width(), 120});
    const int h = kPadY + headH_ + 4 + mainRect_.height() + (sub_.isEmpty() ? 0 : 3 + subRect_.height()) + kPadY;
    bubbleSize_ = QSize(w + 2 * kPadX, h);
  }

  /** 吹き出しの場所（示す所の下・右・上・左のうち、画面に収まり、開いているメニューに重ならない所） */
  QRect placeBubble() const {
    const QRect area = area_.adjusted(6, 6, -6, -6);
    const QSize s = bubbleSize_;
    const QRect t = target_;
    auto clampX = [&](int x) { return std::clamp(x, area.left(), std::max(area.left(), area.right() - s.width())); };
    auto clampY = [&](int y) { return std::clamp(y, area.top(), std::max(area.top(), area.bottom() - s.height())); };
    // 矢印は示す所の中から右下へ伸びるので、下に置くときは少し離す。
    // 下・上を先に試す（横に置くと、同じ段のほかのクリップなどに重なりやすい）。開いているメニュー・ダイアログ（avoid）には重ねず、
    // 重なってしまうときは、その外の横に置く
    const int below = std::max(t.bottom(), int(tipFor(t).y()) + 24) + 10;
    const int midY = clampY(t.center().y() - s.height() / 2);
    const QRect cands[] = {
        QRect(QPoint(clampX(t.left()), below), s),
        QRect(QPoint(clampX(t.left()), t.top() - 10 - s.height()), s),
        QRect(QPoint(t.right() + 16, midY), s),
        QRect(QPoint(t.left() - 12 - s.width(), midY), s),
        QRect(QPoint(avoid_.right() + 14, midY), s),
        QRect(QPoint(avoid_.left() - 14 - s.width(), midY), s),
    };
    for (const QRect& c : cands) {
      if (area.contains(c) && !c.intersects(avoid_) && !c.intersects(t) && (drop_.isEmpty() || !c.intersects(drop_))) return c;
    }
    for (const QRect& c : cands) {
      if (area.contains(c) && !c.intersects(t)) return c;
    }
    return QRect(QPoint(clampX(t.left()), clampY(below)), s);
  }

  void paintBubble(QPainter& p) const {
    p.save();
    p.setOpacity(p.opacity() * bubbleAlpha_);
    const QRectF b = bubble_;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 60));
    p.drawRoundedRect(b.translated(0, 3).adjusted(-1, 0, 1, 2), 9, 9);
    p.setBrush(QColor(0x1f, 0x2d, 0x40, 238));
    p.setPen(QPen(QColor(0x26, 0x80, 0xeb, 200), 1));
    p.drawRoundedRect(b.adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    const int x = int(b.left()) + kPadX;
    int y = int(b.top()) + kPadY;
    const int w = int(b.width()) - 2 * kPadX;
    p.setFont(headFont_);
    p.setPen(QColor(0x7c, 0xb6, 0xff));
    p.drawText(QRect(x, y, w, headH_), Qt::AlignLeft | Qt::AlignVCenter, head_);
    p.setFont(subFont_);
    p.setPen(QColor(0x80, 0x93, 0xa8));
    p.drawText(QRect(x, y, w, headH_), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Esc でやめる"));
    y += headH_ + 4;
    p.setFont(mainFont_);
    p.setPen(QColor(0xf2, 0xf2, 0xf2));
    p.drawText(QRect(x, y, w, mainRect_.height()), Qt::TextWordWrap, main_);
    y += mainRect_.height() + 3;
    if (!sub_.isEmpty()) {
      p.setFont(subFont_);
      p.setPen(QColor(0xaa, 0xbc, 0xd1));
      p.drawText(QRect(x, y, w, subRect_.height()), Qt::TextWordWrap, sub_);
    }
    p.restore();
  }

  /** 示す所がある画面（の、タスクバーを除いた所）に窓を合わせる */
  void useScreen(const QRect& spot) {
    QScreen* sc = QGuiApplication::screenAt(spot.center());
    if (!sc && parentWidget()) sc = parentWidget()->screen();
    if (!sc) sc = QGuiApplication::primaryScreen();
    if (!sc) return;
    const QRect a = sc->availableGeometry();
    if (a == area_) return;
    area_ = a;
    setGeometry(a);
    lastDirty_ = QRect();
    update();
  }

  /** 描き直す所（この窓の座標） */
  QRect dirtyRect() const {
    QRect r = target_.adjusted(-8, -8, 8, 8);
    if (!drop_.isEmpty()) {
      r |= drop_.adjusted(-8, -8, 8, 8);
      // ドラッグの道（ふくらむ所も）と、運んでいるカード
      const QPointF a = to_;
      const QPointF b = dropTip();
      r |= QRectF(a, b).normalized().adjusted(-30, -100, 60, 60).toAlignedRect();
    }
    r |= QRectF(pos_ - QPointF(26, 26), QSizeF(80, 80)).toAlignedRect();
    r |= QRectF(to_ - QPointF(26, 26), QSizeF(80, 80)).toAlignedRect();
    if (bubbleAlpha_ > 0 || shown_) r |= bubble_.adjusted(-4, -4, 6, 8);
    return r.translated(-area_.topLeft());
  }

  void step() {
    const qint64 now = clock_.elapsed();
    bool busy = false;
    if (arrived_ < 0) {
      busy = true;
      const double t = glideMs_ > 0 ? std::clamp(double(now - glideStart_) / double(glideMs_), 0.0, 1.0) : 1.0;
      const double e = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
      pos_ = from_ + (to_ - from_) * e;
      if (t >= 1) {
        arrived_ = now;
        pos_ = to_;
      }
    }
    if (arrived_ >= 0 && bubbleAlpha_ < 1) {
      bubbleAlpha_ = std::min(1.0, bubbleAlpha_ + 0.14);
      busy = true;
    }
    if (arrived_ >= 0) {
      // 押す合図・ドラッグの動きの間は、なめらかに描く
      const qint64 since = now - arrived_;
      busy = busy || !drop_.isEmpty() || since % 2200 < 760;
    }
    if (ending_) {
      busy = true;
      fade_ = 1 - double(now - endStart_) / 450.0;
      if (fade_ <= 0) {
        fade_ = 0;
        conceal();
        return;
      }
    }
    const QRect dirty = dirtyRect();
    update(dirty.united(lastDirty_));
    lastDirty_ = dirty;
    anim_.setInterval(busy ? 16 : 40);
  }

  QTimer anim_;
  QElapsedTimer clock_;
  /** この窓の場所（画面の座標） */
  QRect area_;
  QRect target_;
  QRect drop_;
  QRect avoid_;
  bool final_ = false;
  QString head_;
  QString main_;
  QString sub_;
  /** 何かを示している */
  bool shown_ = false;
  /** 矢印の位置がある（はじめて示すときは、右下から近づける） */
  bool has_ = false;
  QPointF from_;
  QPointF to_;
  QPointF pos_;
  qint64 glideStart_ = 0;
  qint64 glideMs_ = 0;
  /** 行き先に着いた時刻（-1 は動いている途中） */
  qint64 arrived_ = -1;
  double bubbleAlpha_ = 0;
  QSize bubbleSize_;
  QRect bubble_;
  QRect mainRect_;
  QRect subRect_;
  int headW_ = 0;
  int headH_ = 0;
  QFont headFont_;
  QFont mainFont_;
  QFont subFont_;
  QRect lastDirty_;
  double fade_ = 1;
  bool ending_ = false;
  qint64 endStart_ = 0;
};

// ---- 案内の進め方 ---------------------------------------------------------------------------

GuideController::GuideController(MainWindow* window) : QObject(window), window_(window), overlay_(new GuideOverlay(window)) {
  tick_.setInterval(kTickMs);
  connect(&tick_, &QTimer::timeout, this, &GuideController::tick);
  clock_.start();
  // ほかのアプリに切り替えたら隠す（アプリに戻ったら、また出す）
  connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
    appActive_ = state == Qt::ApplicationActive;
    if (active()) tick();
  });
  // 選んだもの・編集データが変わったら、すぐに示し直す（準備の案内が変わる）
  auto soon = [this] {
    if (active()) QTimer::singleShot(0, this, &GuideController::tick);
  };
  connect(&editor(), &Editor::selectionChanged, this, soon);
  connect(&editor(), &Editor::projectChanged, this, soon);
}

GuideController::~GuideController() {
  if (active()) qApp->removeEventFilter(this);
}

QWidget* GuideController::overlay() const {
  return overlay_;
}

int GuideController::start(const std::vector<ai::GuideStep>& steps) {
  if (active()) finish(false);
  steps_.clear();
  for (const ai::GuideStep& s : steps) {
    if (ai::guideTarget(s.target)) steps_.push_back(s);
  }
  ++session_;
  overlay_->forget();
  if (steps_.empty()) {
    emit ended(session_, false);
    return session_;
  }
  // 示している所が押されたのを知るため、アプリのすべての入力を見る（何も止めない。Esc だけ受け取る）
  qApp->installEventFilter(this);
  tick_.start();
  goTo(0);
  return session_;
}

void GuideController::stop() {
  finish(false);
}

void GuideController::next() {
  if (active()) complete(session_, index_);
}

void GuideController::goTo(int index) {
  index_ = index;
  revealed_.clear();
  pressIndex_ = -1;
  notice_.clear();
  idle_.restart();
  watchStep();
  emit progressed(session_, index_, count());
  tick();
}

void GuideController::completeLater(int index) {
  const int session = session_;
  QTimer::singleShot(0, this, [this, session, index] { complete(session, index); });
}

void GuideController::complete(int session, int index) {
  if (!active() || session != session_ || index != index_) return;
  if (index_ + 1 >= count()) {
    finish(true);
    return;
  }
  goTo(index_ + 1);
}

void GuideController::finish(bool finished) {
  if (!active()) return;
  unwatch();
  tick_.stop();
  qApp->removeEventFilter(this);
  const int session = session_;
  const int n = count();
  index_ = -1;
  spot_ = GuideSpot();
  pressIndex_ = -1;
  if (finished) overlay_->end();
  else overlay_->forget();
  if (finished) emit progressed(session, n, n);
  emit ended(session, finished);
}

void GuideController::watchStep() {
  unwatch();
  const GuideTarget* t = active() ? ai::guideTarget(steps_[size_t(index_)].target) : nullptr;
  QAction* a = t && t->kind == GuideKind::Action ? window_->commandAction(t->ref) : nullptr;
  if (!a) return;
  const int index = index_;
  // メニューから選んでも、ショートカットキーで行っても、次へ進む（押せないときは、行われない）。
  // サブメニューそのものの手順は、中の項目を選んだら次へ
  if (QMenu* sub = a->menu()) {
    watchedMenu_ = sub;
    watchConn_ = connect(sub, &QMenu::triggered, this, [this, index](QAction*) { completeLater(index); });
  } else {
    watchedAction_ = a;
    watchConn_ = connect(a, &QAction::triggered, this, [this, index] { completeLater(index); });
  }
}

void GuideController::unwatch() {
  if (watchConn_) disconnect(watchConn_);
  watchConn_ = {};
  watchedAction_ = nullptr;
  watchedMenu_ = nullptr;
}

bool GuideController::mayReveal(const QString& key) const {
  const qint64 now = clock_.elapsed();
  const auto it = revealed_.constFind(key);
  if (it != revealed_.constEnd() && now - *it < kRevealAgainMs) return false;
  revealed_.insert(key, now);
  return true;
}

QWidget* GuideController::dialogWidget(const QString& name) const {
  for (QWidget* top : QApplication::topLevelWidgets()) {
    if (top == overlay_ || top == window_ || !top->isVisible()) continue;
    QWidget* w = findNamed(top, name);
    if (w && w->isVisible()) return w;
  }
  return nullptr;
}

bool GuideController::satisfied(const GuideTarget& t) const {
  const Selection sel = editor().selection();
  switch (t.kind) {
    case GuideKind::View:
      return window_->perspective()->reachView(t.ref).visible;
    case GuideKind::Timeline:
      if (t.ref == u"clip") return sel.type == Selection::Clip || sel.type == Selection::Overlay;
      if (t.ref == u"transition") return sel.type == Selection::Transition;
      return false;
    default:
      return false;
  }
}

GuideSpot GuideController::resolve(const QString& id) const {
  const GuideTarget* t = ai::guideTarget(id);
  return t ? resolveTarget(*t, 0, false) : GuideSpot();
}

QString GuideController::unavailable(const QString& id) const {
  const GuideTarget* t = ai::guideTarget(id);
  if (!t) return QStringLiteral("知らない場所です");
  const GuideSpot s = resolveTarget(*t, 0, false);
  return s.unavailable ? s.hint : QString();
}

GuideSpot GuideController::widgetSpot(QWidget* w, const QString& key, bool reveal) const {
  GuideSpot s;
  QRect r = targetRect(w);
  // ほとんど隠れていたら（スクロールの外など）、見えるようにスクロールする
  const int want = std::min(w->height(), 28);
  if ((r.isEmpty() || r.height() < want) && reveal && mayReveal(key)) {
    revealWidget(w);
    r = targetRect(w);
  }
  if (r.isEmpty()) {
    s.hint = QStringLiteral("スクロールすると出てきます");
    return s;
  }
  s.ok = true;
  s.final = true;
  s.rect = r;
  return s;
}

GuideSpot GuideController::timelineSpot(const QString& part, bool reveal) const {
  GuideSpot s;
  TimelineView* tv = window_->timeline()->view();
  if (!tv->isVisible()) {
    s.hint = QStringLiteral("タイムラインが見えていません");
    return s;
  }
  QRect r = tv->guideRect(part);
  if (r.isEmpty() && reveal && mayReveal(QStringLiteral("timeline:") + part)) {
    tv->revealGuidePart(part);
    r = tv->guideRect(part);
  }
  if (r.isEmpty()) {
    s.hint = QStringLiteral("タイムラインの、今見えている所にはありません");
    return s;
  }
  s.ok = true;
  s.final = true;
  s.rect = toGlobal(tv->viewport(), r);
  return s;
}

QRect GuideController::dropRect(const QString& part, bool reveal) const {
  return timelineSpot(part, reveal).rect;
}

GuideSpot GuideController::actionSpot(QAction* a) const {
  GuideSpot s;
  QMenuBar* bar = nullptr;
  std::vector<QMenu*> menus;
  if (!a || !menuPath(a, &bar, &menus) || menus.empty() || !bar) {
    s.unavailable = true;
    s.hint = QStringLiteral("この操作は、今の画面にはありません");
    return s;
  }
  // 隠しているメニュー（AI サーバーを使えないときのワークショップ など）
  bool hidden = !a->isVisible() || !bar->isVisible();
  for (QMenu* m : menus) hidden = hidden || !m->menuAction()->isVisible();
  if (hidden) {
    s.unavailable = true;
    s.hint = QStringLiteral("今は、このメニューは出ていません");
    return s;
  }
  // サブメニューそのものを示すとき: 開いていれば、その中から選んでもらう
  QMenu* sub = a->menu();
  if (sub && sub->isVisible()) {
    s.ok = true;
    s.final = true;
    s.rect = sub->geometry();
    s.avoid = sub->geometry();
    s.menu = sub;
    return s;
  }
  // 開いているいちばん深いメニューから、次に開くもの
  int open = -1;
  for (int i = int(menus.size()) - 1; i >= 0; --i) {
    if (menus[size_t(i)]->isVisible()) {
      open = i;
      break;
    }
  }
  const int last = int(menus.size()) - 1;
  if (open == last) {
    QMenu* m = menus[size_t(last)];
    s.rect = toGlobal(m, m->actionGeometry(a));
    s.avoid = m->geometry();
    s.ok = !s.rect.isEmpty();
    if (sub) {
      s.hint = QStringLiteral("「%1」にカーソルを合わせます").arg(menuText(a->text()));
      return s;
    }
    s.final = true;
    s.action = a;
    if (!a->isEnabled()) s.hint = QStringLiteral("今は押せない状態です");
    return s;
  }
  if (open >= 0) {
    QMenu* m = menus[size_t(open)];
    QAction* next = menus[size_t(open + 1)]->menuAction();
    s.rect = toGlobal(m, m->actionGeometry(next));
    s.avoid = m->geometry();
    s.ok = !s.rect.isEmpty();
    s.hint = QStringLiteral("「%1」にカーソルを合わせます").arg(menuText(next->text()));
    return s;
  }
  // メニューを開いていない: 一番上のメニューの見出しから
  QAction* top = menus.front()->menuAction();
  s.rect = toGlobal(bar, bar->actionGeometry(top));
  s.ok = !s.rect.isEmpty();
  s.hint = QStringLiteral("上の「%1」を押します").arg(menuText(top->text()));
  return s;
}

GuideSpot GuideController::viewRoute(const QString& view) const {
  const PerspectiveManager::ViewReach reach = window_->perspective()->reachView(view);
  GuideSpot s;
  if (reach.closed) {
    // 閉じている: 「表示」→「ビュー」から開く
    s = actionSpot(window_->commandAction(QStringLiteral("view.open.") + view));
    if (s.final) s.hint = QStringLiteral("「%1」を押して、欄に開きます").arg(reach.title);
    s.final = false;
    s.action = nullptr;
    s.menu = nullptr;
    return s;
  }
  s.ok = !reach.rect.isEmpty();
  s.rect = reach.rect;
  s.hint = reach.collapsed ? QStringLiteral("「%1」を押して、左の欄を開きます").arg(reach.title) : QStringLiteral("「%1」のタブを押します").arg(reach.title);
  return s;
}

bool GuideController::needSpot(GuideNeed need, int depth, bool reveal, GuideSpot* out) const {
  const Editor& ed = editor();
  const Selection sel = ed.selection();
  const ProjectData& p = ed.project();
  // 準備の場所へ（その場所にも準備が要れば、そちらが先）
  auto routeTo = [&](const char* id, const QString& hint) {
    const GuideTarget* r = ai::guideTarget(QString::fromLatin1(id));
    GuideSpot s = r ? resolveTarget(*r, depth + 1, reveal) : GuideSpot();
    if (s.final || s.hint.isEmpty()) s.hint = hint;
    s.final = false;
    s.action = nullptr;
    s.menu = nullptr;
    *out = s;
    return true;
  };
  auto routePart = [&](const char* part, const QString& hint) {
    GuideSpot s = timelineSpot(QString::fromLatin1(part), reveal);
    s.hint = hint;
    s.final = false;
    *out = s;
    return true;
  };
  const bool visual = sel.type == Selection::Clip || sel.type == Selection::Overlay;
  switch (need) {
    case GuideNeed::None:
      return false;
    case GuideNeed::Visual:
      if (visual) return false;
      if (!p.clips.empty() || !p.overlays.empty()) return routePart("clip", QStringLiteral("先に、タイムラインで映像をクリックして選びます"));
      return needSpot(GuideNeed::Clips, depth, reveal, out);
    case GuideNeed::Video: {
      const VideoClip* c = visual ? ed.clip(sel.id) : nullptr;
      const OverlayItem* o = sel.type == Selection::Overlay ? ed.overlay(sel.id) : nullptr;
      if (c && !c->isImage && !(o && o->isEffectLayer())) return false;
      const bool anyVideo = std::any_of(p.clips.begin(), p.clips.end(), [](const VideoClip& x) { return !x.isImage; }) ||
                            std::any_of(p.overlays.begin(), p.overlays.end(), [](const OverlayItem& x) { return !x.isImage && !x.isEffectLayer(); });
      if (anyVideo) {
        return routePart("videoClip", c ? QStringLiteral("写真ではなく、動画をクリックして選びます") : QStringLiteral("先に、タイムラインで動画をクリックして選びます"));
      }
      if (!p.clips.empty() && hasAsset(false)) return routeTo("media.list", QStringLiteral("先に、動画をタイムラインへドラッグして入れます"));
      return needSpot(GuideNeed::Clips, depth, reveal, out);
    }
    case GuideNeed::Photo: {
      const VideoClip* c = visual ? ed.clip(sel.id) : nullptr;
      const OverlayItem* o = sel.type == Selection::Overlay ? ed.overlay(sel.id) : nullptr;
      if (c && c->isImage && !(o && o->isEffectLayer())) return false;
      const bool anyPhoto = std::any_of(p.clips.begin(), p.clips.end(), [](const VideoClip& x) { return x.isImage; }) ||
                            std::any_of(p.overlays.begin(), p.overlays.end(), [](const OverlayItem& x) { return x.isImage && !x.isEffectLayer(); });
      if (anyPhoto) {
        return routePart("photoClip", c ? QStringLiteral("動画ではなく、写真をクリックして選びます") : QStringLiteral("先に、タイムラインで写真をクリックして選びます"));
      }
      if (hasAsset(false)) return routeTo("media.list", QStringLiteral("先に、写真をタイムラインへドラッグして入れます"));
      return needSpot(GuideNeed::Media, depth, reveal, out);
    }
    case GuideNeed::Text:
      if (sel.type == Selection::Text) return false;
      if (!p.texts.empty()) return routePart("textItem", QStringLiteral("先に、タイムラインでテキストをクリックして選びます"));
      return routeTo("timeline.addText", QStringLiteral("先に、「テキスト」でテキストを追加します"));
    case GuideNeed::Audio:
      if (sel.type == Selection::Audio) return false;
      if (!p.audios.empty()) return routePart("audioItem", QStringLiteral("先に、タイムラインでオーディオをクリックして選びます"));
      if (hasAsset(true)) return routeTo("audio.list", QStringLiteral("先に、オーディオの一覧から、音をタイムラインへドラッグして入れます"));
      return routeTo("timeline.addAudio", QStringLiteral("先に、「オーディオ」で音を追加します"));
    case GuideNeed::Transition:
      if (sel.type == Selection::Transition) return false;
      if (hasTransition()) return routePart("transition", QStringLiteral("先に、切れ目のトランジションの印をクリックして選びます"));
      return needSpot(GuideNeed::HasTransition, depth, reveal, out);
    case GuideNeed::HasTransition:
      if (hasTransition()) return false;
      if (p.clips.size() < 2) return needSpot(GuideNeed::TwoClips, depth, reveal, out);
      return routeTo("menu.seq.addTransition", QStringLiteral("先に、切れ目にトランジションを付けます"));
    case GuideNeed::Fadable:
      if (visual || sel.type == Selection::Audio) return false;
      if (!p.clips.empty()) return routePart("clip", QStringLiteral("先に、タイムラインで映像かオーディオをクリックして選びます"));
      if (!p.audios.empty()) return routePart("audioItem", QStringLiteral("先に、タイムラインでオーディオをクリックして選びます"));
      return needSpot(GuideNeed::Clips, depth, reveal, out);
    case GuideNeed::Item:
      if (sel && sel.type != Selection::Transition) return false;
      if (!p.clips.empty()) return routePart("clip", QStringLiteral("先に、タイムラインでクリックして選びます"));
      if (!p.texts.empty()) return routePart("textItem", QStringLiteral("先に、タイムラインでクリックして選びます"));
      if (!p.audios.empty()) return routePart("audioItem", QStringLiteral("先に、タイムラインでクリックして選びます"));
      return needSpot(GuideNeed::Clips, depth, reveal, out);
    case GuideNeed::Nothing: {
      if (!sel) return false;
      routePart("empty", QStringLiteral("先に、何も無い所をクリックするか Esc を押して、選ぶのをやめます"));
      if (!out->ok) {
        // 何も無い所が見えていない: タイムラインを指して、Esc を勧める
        out->ok = true;
        out->rect = shownRect(window_->timeline()->view()->viewport());
        out->hint = QStringLiteral("先に、Esc を押して、選ぶのをやめます");
      }
      return true;
    }
    case GuideNeed::Clips:
      if (!p.clips.empty()) return false;
      if (hasAsset(false)) return routeTo("media.list", QStringLiteral("先に、メディアの動画や写真を、タイムラインへドラッグして入れます"));
      return needSpot(GuideNeed::Media, depth, reveal, out);
    case GuideNeed::TwoClips:
      if (p.clips.size() >= 2) return false;
      if (p.clips.empty()) return needSpot(GuideNeed::Clips, depth, reveal, out);
      if (hasAsset(false)) return routeTo("media.list", QStringLiteral("先に、もう 1 つ動画や写真をタイムラインへドラッグして、後ろにつなげます"));
      return needSpot(GuideNeed::Media, depth, reveal, out);
    case GuideNeed::Media:
      if (hasAsset(false)) return false;
      return routeTo("media.add", QStringLiteral("先に、「ファイルを選択」で動画や写真を読み込みます"));
    case GuideNeed::AudioMedia:
      if (hasAsset(true)) return false;
      return routeTo("audio.add", QStringLiteral("先に、「ファイルを選択」で音を登録します"));
    case GuideNeed::Marker:
      if (!p.markers.empty()) return false;
      return routeTo("timeline.marker", QStringLiteral("先に、しおりのボタンで、再生位置にマーカーを置きます"));
  }
  return false;
}

GuideSpot GuideController::resolveTarget(const GuideTarget& t, int depth, bool reveal) const {
  GuideSpot s;
  if (depth > kMaxDepth) return s;
  // ダイアログの中: 開いていれば、準備は済んでいる。開いていなければ、開く場所から
  if (t.kind == GuideKind::Dialog) {
    if (QWidget* w = dialogWidget(t.ref)) {
      // 吹き出しは、ダイアログの中の説明や部品に重ねない（外の横に置く）
      s = widgetSpot(w, t.id, reveal);
      s.avoid = w->window()->frameGeometry();
      return s;
    }
    const GuideTarget* via = ai::guideTarget(t.via);
    if (!via) {
      s.unavailable = true;
      s.hint = QStringLiteral("この画面は開けません");
      return s;
    }
    s = resolveTarget(*via, depth + 1, reveal);
    if (s.final) s.hint = QStringLiteral("先に、%1を押して、画面を開きます").arg(via->place);
    s.final = false;
    s.action = nullptr;
    s.menu = nullptr;
    return s;
  }
  // 準備（選ぶ・入れる など）
  if (needSpot(t.need, depth, reveal, &s)) return s;
  // ビュー（タブ）の中のもの: タブが前に出ていなければ、先にタブを押す
  if (!t.view.isEmpty() && t.kind != GuideKind::View && !window_->perspective()->reachView(t.view).visible) return viewRoute(t.view);
  switch (t.kind) {
    case GuideKind::Action:
      return actionSpot(window_->commandAction(t.ref));
    case GuideKind::View: {
      const PerspectiveManager::ViewReach reach = window_->perspective()->reachView(t.ref);
      if (reach.closed) return viewRoute(t.ref);
      s.ok = !reach.rect.isEmpty();
      s.final = true;
      s.rect = reach.rect;
      return s;
    }
    case GuideKind::Widget: {
      QWidget* w = findNamed(window_, t.ref);
      if (!w || !w->isVisible()) {
        s.unavailable = true;
        s.hint = QStringLiteral("今の画面には出ていません");
        return s;
      }
      s = widgetSpot(w, t.id, reveal);
      if (s.ok && !t.drop.isEmpty()) s.drop = dropRect(t.drop, reveal);
      return s;
    }
    case GuideKind::Timeline:
      return timelineSpot(t.ref, reveal);
    case GuideKind::Property: {
      Inspector* ins = window_->inspector();
      QWidget* w = nullptr;
      if (t.ref.startsWith(QLatin1Char('#'))) {
        w = findNamed(ins, t.ref.mid(1));
      } else {
        for (QWidget* c : ins->findChildren<QWidget*>()) {
          auto* sec = dynamic_cast<Section*>(c);
          if (sec && sec->title() == t.ref && sec->isVisible()) {
            w = sec;
            break;
          }
        }
      }
      if (!w || !w->isVisible()) {
        // 今選んでいるものには、その設定が無い（写真の「速度」など）
        s.ok = true;
        s.rect = shownRect(ins->viewport());
        s.hint = QStringLiteral("今選んでいるものには、この設定がありません");
        return s;
      }
      return widgetSpot(w, t.id, reveal);
    }
    case GuideKind::Dialog:
      break;
  }
  return s;
}

void GuideController::tick() {
  if (!active()) return;
  if (idle_.elapsed() > kIdleMs) {
    finish(false);
    return;
  }
  const GuideTarget* t = ai::guideTarget(steps_[size_t(index_)].target);
  if (!t) {
    complete(session_, index_);
    return;
  }
  // 後の手順のダイアログが、もう開いている（キーで開いた など）: そこまで進める
  if (!(t->kind == GuideKind::Dialog && dialogWidget(t->ref))) {
    for (int j = index_ + 1; j < count(); ++j) {
      const GuideTarget* n = ai::guideTarget(steps_[size_t(j)].target);
      if (n && n->kind == GuideKind::Dialog && dialogWidget(n->ref)) {
        goTo(j);
        return;
      }
    }
  }
  // もう済んでいる手順（タブがもう前に出ている など）は飛ばす。最後の手順は、済んでいても示す
  if (index_ + 1 < count() && satisfied(*t)) {
    complete(session_, index_);
    return;
  }
  spot_ = resolveTarget(*t, 0, true);
  // 今の画面に無い場所（出ていないメニュー など）は、AI の欄のカードに理由を出す（「次へ」で飛ばせる）
  const QString notice = spot_.unavailable ? spot_.hint : QString();
  if (notice != notice_) {
    notice_ = notice;
    emit noticed(session_, notice_);
  }
  showSpot();
}

void GuideController::showSpot() {
  if (!spot_.ok || spot_.rect.isEmpty()) {
    overlay_->conceal();
    return;
  }
  const ai::GuideStep& step = steps_[size_t(index_)];
  QString text = step.text;
  if (text.isEmpty()) {
    if (const GuideTarget* t = ai::guideTarget(step.target)) text = t->place;
  }
  // 手順の場所なら AI の説明を大きく出す。準備の場所なら、先にすることを大きく出し、AI の説明を下に添える
  const QString main = spot_.final ? text : spot_.hint;
  const QString sub = spot_.final ? spot_.hint : text;
  const QString head = QStringLiteral("案内 %1/%2").arg(index_ + 1).arg(count());
  overlay_->present(spot_, head, main, sub, appActive_);
}

bool GuideController::eventFilter(QObject* obj, QEvent* e) {
  if (!active()) return false;
  switch (e->type()) {
    case QEvent::MouseButtonPress: {
      if (!obj->isWidgetType()) break;
      auto* me = static_cast<QMouseEvent*>(e);
      if (me->button() != Qt::LeftButton || (me->timestamp() != 0 && me->timestamp() == pressStamp_)) break;
      pressStamp_ = me->timestamp();
      // 示している場所の中で押した（離したとき・落としたときに、次へ進む）
      pressIndex_ = -1;
      if (spot_.ok && spot_.final && spot_.rect.adjusted(-3, -3, 3, 3).contains(me->globalPosition().toPoint())) {
        pressIndex_ = index_;
        pressDrop_ = spot_.drop;
      }
      // 押すとメニューが開く・選ぶものが変わる などするので、すぐに示し直す
      QTimer::singleShot(40, this, &GuideController::tick);
      break;
    }
    case QEvent::MouseButtonRelease: {
      if (!obj->isWidgetType()) break;
      auto* me = static_cast<QMouseEvent*>(e);
      if (me->button() != Qt::LeftButton || (me->timestamp() != 0 && me->timestamp() == releaseStamp_)) break;
      releaseStamp_ = me->timestamp();
      if (pressIndex_ >= 0 && pressIndex_ == index_) {
        // ドラッグして使う場所は、行き先で離したときだけ
        if (pressDrop_.isEmpty() || pressDrop_.adjusted(-16, -16, 16, 16).contains(me->globalPosition().toPoint())) completeLater(index_);
      }
      pressIndex_ = -1;
      QTimer::singleShot(40, this, &GuideController::tick);
      break;
    }
    case QEvent::Drop: {
      // ほかの欄から運んで落とした（メディアの一覧 → タイムライン など。Windows のドラッグの間は、離した合図が来ない）
      if (!obj->isWidgetType() || pressIndex_ < 0 || pressIndex_ != index_ || pressDrop_.isEmpty()) break;
      auto* de = static_cast<QDropEvent*>(e);
      const QPoint g = static_cast<QWidget*>(obj)->mapToGlobal(de->position().toPoint());
      if (pressDrop_.adjusted(-16, -16, 16, 16).contains(g)) completeLater(index_);
      QTimer::singleShot(80, this, &GuideController::tick);
      break;
    }
    case QEvent::KeyPress: {
      // Esc で案内をやめる（メニュー・ダイアログを開いているときや、タイムラインでドラッグしているときは、そちらに使う）
      auto* ke = static_cast<QKeyEvent*>(e);
      if (ke->key() != Qt::Key_Escape || (ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) break;
      if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) break;
      if (!obj->isWidgetType() || static_cast<QWidget*>(obj)->window() != window_) break;
      if (window_->timeline()->view()->isDragging()) break;
      stop();
      return true;
    }
    case QEvent::Show:
    case QEvent::Hide: {
      // メニュー・ダイアログが開いた・閉じた: 示す所が変わる。開いたものより手前に出す
      if (!obj->isWidgetType() || obj == overlay_ || !static_cast<QWidget*>(obj)->isWindow()) break;
      if (e->type() == QEvent::Show) QTimer::singleShot(0, overlay_, [this] { overlay_->raiseTop(); });
      QTimer::singleShot(30, this, &GuideController::tick);
      break;
    }
    default:
      break;
  }
  return false;
}

}  // namespace kv
