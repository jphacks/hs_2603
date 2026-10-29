#include "ui/effectsview.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDrag>
#include <QEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "core/editor.h"
#include "core/effects.h"
#include "core/transitions.h"
#include "ui/effectlist.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

namespace {

/** トランジションのカードの見本の大きさ（2 列に並べても、欄の一番狭い幅に入る） */
constexpr int kThumbW = 88;
constexpr int kThumbH = 50;

/** トランジションの見本に使う 2 枚（A: 前のクリップ、B: 次のクリップ）。画面の倍率ごとに作っておく */
const QImage& sampleFrame(bool second, qreal dpr) {
  static std::map<int, QImage> cache[2];
  const int key = int(std::lround(dpr * 100));
  auto& slot = cache[second ? 1 : 0];
  auto it = slot.find(key);
  if (it != slot.end()) return it->second;
  const QSize px(int(std::lround(kThumbW * dpr)), int(std::lround(kThumbH * dpr)));
  QImage img(px, QImage::Format_RGB32);
  {
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient g(0, 0, px.width(), px.height());
    if (second) {
      g.setColorAt(0, QColor(0xf0, 0x8a, 0x3c));
      g.setColorAt(1, QColor(0xd9, 0x46, 0x8f));
    } else {
      g.setColorAt(0, QColor(0x23, 0x5a, 0xc0));
      g.setColorAt(1, QColor(0x22, 0xb8, 0xc8));
    }
    p.fillRect(img.rect(), g);
    QFont f = QApplication::font();
    f.setBold(true);
    f.setPixelSize(std::max(8, int(px.height() * 0.56)));
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 235));
    p.drawText(img.rect(), Qt::AlignCenter, second ? QStringLiteral("B") : QStringLiteral("A"));
  }
  return slot.emplace(key, img).first->second;
}

/** トランジションの見本（progress は xfade と同じ進み具合。1 = A だけ → 0 = B だけ） */
QIcon transitionThumb(TransitionType type, float progress, qreal dpr) {
  const QImage out = blendTransition(sampleFrame(false, dpr), sampleFrame(true, dpr), type, progress);
  QPixmap pm = QPixmap::fromImage(out);
  pm.setDevicePixelRatio(dpr);
  return QIcon(pm);
}

/** ひらがな・カタカナ、大文字・小文字を区別せずに探せる形にする */
QString searchKey(const QString& s) {
  QString out = s.toCaseFolded();
  for (QChar& c : out) {
    const ushort u = c.unicode();
    if (u >= 0x30A1 && u <= 0x30F6) c = QChar(ushort(u - 0x60));  // カタカナ → ひらがな
  }
  return out;
}

/**
 * カードの操作を見張る: 押したまま動かしたらドラッグ（タイムラインへ落とす）、ダブルクリック・Enter で付ける、
 * カーソルを合わせたとき・外したときに hover を呼ぶ
 */
class CardFilter : public QObject {
 public:
  CardFilter(QAbstractButton* button, QString payload, std::function<void()> apply, std::function<void(bool)> hover)
      : QObject(button), button_(button), payload_(std::move(payload)), apply_(std::move(apply)), hover_(std::move(hover)) {}

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override {
    if (obj != button_) return false;
    switch (e->type()) {
      case QEvent::MouseButtonPress: {
        auto* m = static_cast<QMouseEvent*>(e);
        pressed_ = m->button() == Qt::LeftButton;
        pressPos_ = m->position().toPoint();
        break;
      }
      case QEvent::MouseButtonRelease:
        pressed_ = false;
        break;
      case QEvent::MouseButtonDblClick: {
        auto* m = static_cast<QMouseEvent*>(e);
        if (m->button() == Qt::LeftButton) {
          pressed_ = false;
          apply_();
          return true;
        }
        break;
      }
      case QEvent::MouseMove: {
        auto* m = static_cast<QMouseEvent*>(e);
        if (pressed_ && (m->buttons() & Qt::LeftButton) &&
            (m->position().toPoint() - pressPos_).manhattanLength() >= QApplication::startDragDistance()) {
          pressed_ = false;
          startDrag();
          return true;
        }
        break;
      }
      case QEvent::KeyPress: {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
          apply_();
          return true;
        }
        break;
      }
      case QEvent::Enter:
        if (hover_) hover_(true);
        break;
      case QEvent::Leave:
        if (hover_) hover_(false);
        break;
      default:
        break;
    }
    return false;
  }

 private:
  void startDrag() {
    if (hover_) hover_(false);
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kEffectMime), payload_.toUtf8());
    // カーソルに付ける見本: カードを半透明にしたもの
    const QPixmap shot = button_->grab();
    QPixmap ghost(shot.size());
    ghost.setDevicePixelRatio(shot.devicePixelRatio());
    ghost.fill(Qt::transparent);
    {
      QPainter p(&ghost);
      p.setOpacity(0.85);
      p.drawPixmap(0, 0, shot);
    }
    auto* drag = new QDrag(button_);
    drag->setMimeData(mime);
    drag->setPixmap(ghost);
    drag->setHotSpot(QPoint(button_->width() / 2, button_->height() / 2));
    drag->exec(Qt::CopyAction);  // Esc で中止できる。付けられない所で落としたときは何もしない
    button_->setDown(false);
  }

  QAbstractButton* button_;
  QString payload_;
  std::function<void()> apply_;
  std::function<void(bool)> hover_;
  QPoint pressPos_;
  bool pressed_ = false;
};

struct Card {
  QAbstractButton* button = nullptr;
  QString key;  // 探すときに比べる文字（名前・説明・分類）
};

/** 見出し（小さな分類名）と、カードを並べる格子 */
struct Group {
  QWidget* box = nullptr;
  QGridLayout* grid = nullptr;
  int columns = 2;
  std::vector<Card> cards;
  /** 大きなまとまり（トランジション・エフェクト・エフェクトレイヤー）の番号 */
  int section = 0;
};

const QString kThumbCardStyle = QStringLiteral(
    "QToolButton{background:transparent;border:1px solid transparent;border-radius:6px;padding:3px;color:#b4b4b4;font-size:8pt}"
    "QToolButton:hover{border-color:#9b9b9b;color:#ffffff}"
    "QToolButton:focus{border-color:#2680eb}");
const QString kIconCardStyle = QStringLiteral(
    "QPushButton{background:#202020;border:1px solid #2e2e2e;border-radius:6px;padding:6px 8px;color:#cfcfcf;text-align:left}"
    "QPushButton:hover{border-color:#9b9b9b;color:#ffffff}"
    "QPushButton:focus{border-color:#2680eb}");

QString layerIconName(const QString& presetId) {
  if (presetId == u"mosaic") return QStringLiteral("scan-face");
  if (presetId == u"blur") return QStringLiteral("droplet");
  if (presetId == u"fill") return QStringLiteral("square");
  if (presetId == u"spotlight") return QStringLiteral("sun");
  if (presetId == u"adjust") return QStringLiteral("sliders");
  return QStringLiteral("layers");
}

}  // namespace

QWidget* createEffectsView(QWidget* parent) {
  auto* root = new QWidget(parent);
  root->setMinimumWidth(220);
  auto* rv = new QVBoxLayout(root);
  rv->setContentsMargins(10, 10, 6, 0);
  rv->setSpacing(8);
  auto* search = new QLineEdit(root);
  search->setObjectName(QStringLiteral("EffectsSearch"));  // 操作の案内（ai/guidecatalog.cpp）で探す名前。カードも同じ
  search->setPlaceholderText(QStringLiteral("エフェクト・トランジションを探す"));
  search->setClearButtonEnabled(true);
  search->setAccessibleName(QStringLiteral("エフェクトを探す"));
  rv->addWidget(search);

  auto* scroll = new QScrollArea(root);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* page = new QWidget(scroll);
  auto* v = new QVBoxLayout(page);
  v->setContentsMargins(0, 0, 4, 10);
  v->setSpacing(6);
  scroll->setWidget(page);
  rv->addWidget(scroll, 1);

  auto groups = std::make_shared<std::vector<Group>>();
  groups->reserve(16);  // 作っている間、Group への参照を持つので、並びを作り直させない
  std::vector<QWidget*> sections;
  auto addSectionHead = [&](const QString& title, const QString& hint) {
    auto* box = new QWidget(page);
    auto* bv = new QVBoxLayout(box);
    bv->setContentsMargins(0, sections.empty() ? 0 : 10, 0, 0);
    bv->setSpacing(2);
    bv->addWidget(makeLabel(title, "title", box));
    auto* h = makeLabel(hint, "dim", box);
    h->setWordWrap(true);
    bv->addWidget(h);
    v->addWidget(box);
    sections.push_back(box);
  };
  auto addGroup = [&](const QString& title, int columns) -> Group& {
    Group g;
    g.box = new QWidget(page);
    auto* gv = new QVBoxLayout(g.box);
    gv->setContentsMargins(0, 4, 0, 0);
    gv->setSpacing(4);
    if (!title.isEmpty()) gv->addWidget(makeLabel(title, "muted", g.box));
    g.grid = new QGridLayout();
    g.grid->setContentsMargins(0, 0, 0, 0);
    g.grid->setSpacing(6);
    gv->addLayout(g.grid);
    g.columns = columns;
    g.section = int(sections.size()) - 1;
    v->addWidget(g.box);
    groups->push_back(g);
    return groups->back();
  };
  auto place = [](Group& g, QAbstractButton* b, const QString& key) {
    const int k = int(g.cards.size());
    g.grid->addWidget(b, k / g.columns, k % g.columns);
    g.cards.push_back({b, searchKey(key)});
  };

  // ---- トランジション（ビデオの切れ目へ） ----
  addSectionHead(QStringLiteral("トランジション"),
                 QStringLiteral("ビデオの切れ目へドラッグして付けます。ダブルクリックで、再生位置に一番近い切れ目（選んでいるトランジション）に付けます"));
  QString lastGroup;
  Group* tg = nullptr;
  for (const TransitionInfo& info : transitionInfos()) {
    if (!tg || info.group != lastGroup) {
      tg = &addGroup(info.group, 2);
      lastGroup = info.group;
    }
    const TransitionType type = info.type;
    auto* b = new QToolButton(page);
    b->setObjectName(QStringLiteral("EffectsTransitionCard"));
    b->setText(info.label);
    b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    b->setIconSize(QSize(kThumbW, kThumbH));
    b->setIcon(transitionThumb(type, 0.5f, b->devicePixelRatioF()));
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setStyleSheet(kThumbCardStyle);
    b->setFocusPolicy(Qt::TabFocus);
    b->setCursor(Qt::OpenHandCursor);
    b->setToolTip(QStringLiteral("%1\n%2\nビデオの切れ目へドラッグ（ダブルクリックで、再生位置に近い切れ目に付ける）").arg(info.label, info.hint));
    b->setAccessibleName(QStringLiteral("トランジション: %1").arg(info.label));
    b->setAccessibleDescription(info.hint);
    // カーソルを合わせている間は、切り替わる動きをくり返し見せる（前の映像 → 切り替え → 次の映像）
    auto* timer = new QTimer(b);
    timer->setInterval(33);
    auto phase = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, b, [b, type, phase] {
      *phase = (*phase + 1) % 66;
      float progress = 1;
      if (*phase >= 12 && *phase < 48) progress = 1.0f - float(*phase - 12) / 36.0f;
      else if (*phase >= 48) progress = 0;
      b->setIcon(transitionThumb(type, progress, b->devicePixelRatioF()));
    });
    auto hover = [b, timer, type, phase](bool on) {
      if (on) {
        *phase = 0;
        timer->start();
      } else if (timer->isActive()) {
        timer->stop();
        b->setIcon(transitionThumb(type, 0.5f, b->devicePixelRatioF()));
      }
    };
    b->installEventFilter(new CardFilter(b, QStringLiteral("transition:") + info.id, [type] { applyTransitionNearPlayhead(type); }, hover));
    const QString label = info.label;
    QObject::connect(b, &QToolButton::clicked, b, [label] {
      editor().toast(QStringLiteral("「%1」をビデオの切れ目へドラッグして付けます（ダブルクリックで、再生位置に近い切れ目に付けます）").arg(label));
    });
    place(*tg, b, info.label + QLatin1Char(' ') + info.hint + QLatin1Char(' ') + info.group + QStringLiteral(" トランジション"));
  }

  auto makeIconCard = [&](const QString& icon, const QString& label, const QString& tip) {
    auto* b = new QPushButton(theme::icon(icon), label, page);
    b->setIconSize(QSize(16, 16));
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    b->setStyleSheet(kIconCardStyle);
    b->setFocusPolicy(Qt::TabFocus);
    b->setCursor(Qt::OpenHandCursor);
    b->setToolTip(tip);
    b->setAccessibleName(label);
    return b;
  };

  // ---- エフェクト（映像へ） ----
  addSectionHead(QStringLiteral("エフェクト"),
                 QStringLiteral("タイムラインの映像へドラッグして掛けます。ダブルクリックで、選んでいる映像に掛けます"));
  Group& eg = addGroup(QString(), 2);
  for (const EffectInfo& info : effectInfos()) {
    if (!info.clip) continue;
    const EffectType type = info.type;
    auto* b = makeIconCard(effectIconName(type), info.label,
                           QStringLiteral("%1\n%2\n映像へドラッグ（ダブルクリックで、選んでいる映像に掛ける）").arg(info.label, info.hint));
    b->setObjectName(QStringLiteral("EffectsEffectCard"));
    b->installEventFilter(new CardFilter(b, QStringLiteral("effect:") + info.id, [type] { addEffectToSelection(type); }, {}));
    const QString label = info.label;
    QObject::connect(b, &QPushButton::clicked, b, [label] {
      editor().toast(QStringLiteral("「%1」をタイムラインの映像へドラッグして掛けます（ダブルクリックで、選んでいる映像に掛けます）").arg(label));
    });
    place(eg, b, info.label + QLatin1Char(' ') + info.hint + QStringLiteral(" エフェクト"));
  }
  // 左右反転・上下反転は、エフェクトではなく映像の設定（右の設定欄の「エフェクト」）
  auto* flipNote = makeLabel(QStringLiteral("左右反転・上下反転は、映像を選んで右の設定欄の「エフェクト」で切り替えます"), "dim", eg.box);
  flipNote->setWordWrap(true);
  static_cast<QVBoxLayout*>(eg.box->layout())->addWidget(flipNote);

  // ---- エフェクトレイヤー（重ねるトラックへ。範囲の中だけに掛ける） ----
  addSectionHead(QStringLiteral("エフェクトレイヤー"),
                 QStringLiteral("重ねるトラックへドラッグして置きます。下にあるすべての映像の、範囲の中（外）に効果が掛かります。"
                                "ダブルクリックで、再生位置に置きます"));
  Group& lg = addGroup(QString(), 2);
  for (const EffectLayerPreset& p : effectLayerPresets()) {
    const QString id = p.id;
    auto* b = makeIconCard(layerIconName(id), p.label,
                           QStringLiteral("%1\n%2\n重ねるトラックへドラッグ（ダブルクリックで、再生位置に置く）").arg(p.label, p.hint));
    b->setObjectName(QStringLiteral("EffectsLayerCard"));
    b->installEventFilter(new CardFilter(b, QStringLiteral("layer:") + id, [id] { addEffectLayerAtPlayhead(id); }, {}));
    const QString label = p.label;
    QObject::connect(b, &QPushButton::clicked, b, [label] {
      editor().toast(QStringLiteral("「%1」を重ねるトラックへドラッグして置きます（ダブルクリックで、再生位置に置きます）").arg(label));
    });
    place(lg, b, p.label + QLatin1Char(' ') + p.hint + QStringLiteral(" エフェクトレイヤー レイヤー"));
  }

  auto* none = makeLabel(QStringLiteral("見つかりませんでした"), "dim", page);
  none->hide();
  v->addWidget(none);
  v->addStretch(1);

  // 探す: 名前・説明に含むカードだけを、詰めて並べ直す
  QObject::connect(search, &QLineEdit::textChanged, root, [groups, sections, none](const QString& text) {
    const QString q = searchKey(text.trimmed());
    std::vector<bool> sectionShown(sections.size(), false);
    bool any = false;
    for (Group& g : *groups) {
      for (Card& c : g.cards) g.grid->removeWidget(c.button);
      int k = 0;
      for (Card& c : g.cards) {
        const bool show = q.isEmpty() || c.key.contains(q);
        c.button->setVisible(show);
        if (show) {
          g.grid->addWidget(c.button, k / g.columns, k % g.columns);
          ++k;
        }
      }
      g.box->setVisible(k > 0);
      if (k > 0 && g.section >= 0 && g.section < int(sectionShown.size())) sectionShown[size_t(g.section)] = true;
      any = any || k > 0;
    }
    for (size_t i = 0; i < sections.size(); ++i) sections[i]->setVisible(sectionShown[i]);
    none->setVisible(!any);
  });
  return root;
}

}  // namespace kv
