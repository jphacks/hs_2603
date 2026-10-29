#include "ui/effectlist.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "core/editor.h"
#include "core/effects.h"
#include "core/timeline.h"
#include "core/transitions.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

QString effectIconName(EffectType t) {
  switch (t) {
    case EffectType::Mosaic: return QStringLiteral("grid-3x3");
    case EffectType::Blur: return QStringLiteral("droplet");
    case EffectType::Sharpen: return QStringLiteral("focus");
    case EffectType::Vignette: return QStringLiteral("aperture");
    case EffectType::Invert: return QStringLiteral("contrast");
    case EffectType::Posterize: return QStringLiteral("palette");
    case EffectType::Noise: return QStringLiteral("sparkles");
    case EffectType::RgbShift: return QStringLiteral("blend");
    case EffectType::Fill: return QStringLiteral("square");
  }
  return QStringLiteral("sparkles");
}

QString addEffectToClip(const QString& clipId, EffectType type) {
  const VideoClip* c = editor().clip(clipId);
  if (!c) return {};
  const OverlayItem* o = editor().overlay(clipId);
  const EffectInfo& info = effectInfo(type);
  // 塗りつぶしは、範囲を隠すためのもの（エフェクトレイヤーだけ）
  if (!(o && o->isEffectLayer()) && !info.clip) return {};
  const Effect e = makeEffect(type);
  editor().updateClip(clipId, [&](VideoClip& x) { x.effects.push_back(e); });
  editor().toast(QStringLiteral("「%1」を掛けました（右の設定欄の「エフェクト」で強さを変えられます）").arg(info.label));
  return e.id;
}

int addEffectToSelection(EffectType type) {
  Editor& ed = editor();
  const EffectInfo& info = effectInfo(type);
  std::vector<QString> ids;
  auto consider = [&](const Selection& s) {
    if (s.type != Selection::Clip && s.type != Selection::Overlay) return;
    if (std::find(ids.begin(), ids.end(), s.id) != ids.end()) return;
    const OverlayItem* o = ed.overlay(s.id);
    if (o && o->isEffectLayer() ? info.layer : info.clip) ids.push_back(s.id);
  };
  consider(ed.selection());
  for (const Selection& s : ed.selectionSet()) consider(s);
  if (ids.empty()) {
    ed.toast(QStringLiteral("「%1」を掛ける映像を、タイムラインで選んでください（映像へドラッグしても掛けられます）").arg(info.label));
    return 0;
  }
  // 一緒に選んだものにも掛ける。元に戻すときは 1 回でまとめて戻るよう、同じまとまりの名前で記録する
  const QString merge = QStringLiteral("fxadd-") + newId();
  for (const QString& id : ids) {
    const Effect e = makeEffect(type);
    ed.updateClip(id, [&](VideoClip& x) { x.effects.push_back(e); }, merge);
  }
  ed.endMerge();
  ed.toast(ids.size() > 1 ? QStringLiteral("%1 個の映像に「%2」を掛けました").arg(ids.size()).arg(info.label)
                          : QStringLiteral("「%1」を掛けました（右の設定欄の「エフェクト」で強さを変えられます）").arg(info.label));
  return int(ids.size());
}

bool applyTransitionNearPlayhead(TransitionType type) {
  Editor& ed = editor();
  const TransitionInfo& info = transitionInfo(type);
  QString target;
  if (ed.selection().type == Selection::Transition) {
    target = ed.selection().id;
  } else {
    target = ed.nearestCut(ed.currentTime(), std::numeric_limits<double>::infinity());
  }
  if (target.isEmpty()) {
    ed.toast(QStringLiteral("トランジションを付ける切れ目がありません（ビデオにクリップを 2 つ以上並べてください）"));
    return false;
  }
  const VideoClip* c = ed.clip(target);
  if (c && c->transIn.type == type) {
    ed.select({Selection::Transition, target});
    ed.toast(QStringLiteral("この切れ目には、もう「%1」が付いています").arg(info.label));
    return true;
  }
  if (!ed.setTransition(target, type)) return false;
  double at = 0;
  for (const auto& l : ed.layout().items) {
    if (l.clip.id == target) at = l.start;
  }
  ed.toast(QStringLiteral("%1 の切れ目に「%2」を付けました（右の設定欄で長さを変えられます）").arg(formatTime(at), info.label));
  return true;
}

QString addEffectLayerAtPlayhead(const QString& presetId) {
  Editor& ed = editor();
  const QString id = ed.addEffectLayer(presetId);  // 置いたものを選ぶ
  if (id.isEmpty()) return {};
  QString label = presetId;
  for (const EffectLayerPreset& p : effectLayerPresets()) {
    if (p.id == presetId) label = p.label;
  }
  ed.toast(QStringLiteral("再生位置に「%1」のエフェクトレイヤーを置きました（プレビューの枠で、範囲を動かせます）").arg(label));
  return id;
}

void fillAddEffectMenu(QMenu* menu, const QString& clipId, bool layer) {
  for (const EffectInfo& info : effectInfos()) {
    if (layer ? !info.layer : !info.clip) continue;
    const EffectType type = info.type;
    QAction* a = menu->addAction(theme::icon(effectIconName(type)), info.label, menu, [clipId, type] { addEffectToClip(clipId, type); });
    a->setToolTip(info.hint);
  }
  menu->setToolTipsVisible(true);
}

namespace {

/** エフェクト 1 つの値を変える（同じ merge の間は、元に戻す 1 回にまとめる） */
void updateEffect(const QString& clipId, const QString& effectId, const std::function<void(Effect&)>& fn, const QString& merge = QString()) {
  editor().updateClip(clipId, [&](VideoClip& c) {
    for (auto& e : c.effects) {
      if (e.id == effectId) {
        fn(e);
        return;
      }
    }
  }, merge.isEmpty() ? QString() : merge + effectId + clipId);
}

QString formatParam(const EffectParam& p, double v) {
  if (p.unit == EffectParam::Unit::Integer) return QString::number(std::lround(v));
  return formatPercent(v);
}

}  // namespace

/** 一覧の 1 行（エフェクト 1 つ）。見出し（オン/オフ・名前・上へ・下へ・削除）と、種類ごとの値 */
class EffectRow : public QFrame {
 public:
  EffectRow(const QString& clipId, const Effect& e, QWidget* parent) : QFrame(parent), clipId_(clipId), effectId_(e.id), type_(e.type) {
    setObjectName(QStringLiteral("EffectRow"));
    setStyleSheet(QStringLiteral("QFrame#EffectRow{background:#202020;border:1px solid #2e2e2e;border-radius:6px}"));
    const EffectInfo& info = effectInfo(e.type);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(8, 6, 8, 8);
    v->setSpacing(6);
    auto* head = new QHBoxLayout();
    head->setSpacing(2);
    auto* icon = new QLabel(this);
    icon->setPixmap(theme::iconPixmap(effectIconName(e.type), 14, theme::muted, devicePixelRatioF()));
    on_ = new QCheckBox(info.label, this);
    on_->setToolTip(QStringLiteral("%1（チェックを外すと、消さずに一時的に止めます）").arg(info.hint));
    on_->setAccessibleName(QStringLiteral("%1 を使う").arg(info.label));
    up_ = new QToolButton(this);
    up_->setText(QStringLiteral("▲"));
    up_->setToolTip(QStringLiteral("上へ（先に掛ける）"));
    down_ = new QToolButton(this);
    down_->setText(QStringLiteral("▼"));
    down_->setToolTip(QStringLiteral("下へ（あとに掛ける）"));
    remove_ = makeToolButton(QStringLiteral("x"), QStringLiteral("このエフェクトを削除"), this);
    for (QToolButton* b : {up_, down_}) {
      b->setAutoRaise(true);
      b->setFocusPolicy(Qt::NoFocus);
      b->setFixedSize(22, 22);
    }
    head->addWidget(icon);
    head->addSpacing(4);
    head->addWidget(on_, 1);
    head->addWidget(up_);
    head->addWidget(down_);
    head->addWidget(remove_);
    v->addLayout(head);
    // 種類ごとの値（使わない値は出さない）
    const EffectParam* params[3] = {&info.amount, &info.size, &info.softness};
    for (int i = 0; i < 3; ++i) {
      const EffectParam& p = *params[i];
      if (p.label.isEmpty()) continue;
      auto* s = new SliderRow(p.label, p.min, p.max, p.step, [p](double x) { return formatParam(p, x); }, this);
      s->setDefault(p.def);
      s->setUnit(p.step, p.unit == EffectParam::Unit::Integer ? 0 : p.step / 10);
      s->setValueBubble(true);
      sliders_[i] = s;
      v->addWidget(s);
      static const char* kMerge[3] = {"fx-amount-", "fx-size-", "fx-soft-"};
      const QString merge = QString::fromLatin1(kMerge[i]);
      connect(s, &SliderRow::changed, this, [this, i, merge](double x) {
        updateEffect(clipId_, effectId_, [&](Effect& e) {
          if (i == 0) e.amount = x;
          else if (i == 1) e.size = x;
          else e.softness = x;
        }, merge);
      });
      connect(s, &SliderRow::released, this, [] { editor().endMerge(); });
    }
    if (info.usesColor) {
      color_ = new ColorSwatches(QStringLiteral("塗りつぶす色"), this);
      v->addWidget(color_);
      connect(color_, &ColorSwatches::changed, this, [this](const QColor& c) { updateEffect(clipId_, effectId_, [&](Effect& e) { e.color = c; }); });
    }
    connect(on_, &QCheckBox::toggled, this, [this](bool on) { updateEffect(clipId_, effectId_, [&](Effect& e) { e.enabled = on; }); });
    connect(up_, &QToolButton::clicked, this, [this] { move(-1); });
    connect(down_, &QToolButton::clicked, this, [this] { move(1); });
    connect(remove_, &QToolButton::clicked, this, [this] {
      const QString clip = clipId_;
      const QString id = effectId_;
      editor().updateClip(clip, [&](VideoClip& c) {
        c.effects.erase(std::remove_if(c.effects.begin(), c.effects.end(), [&](const Effect& x) { return x.id == id; }), c.effects.end());
      });
    });
  }

  void setValues(const Effect& e, int index, int count) {
    {
      const QSignalBlocker b(on_);
      on_->setChecked(e.enabled);
    }
    const double values[3] = {e.amount, e.size, e.softness};
    for (int i = 0; i < 3; ++i) {
      if (sliders_[i]) {
        sliders_[i]->setValue(values[i]);
        sliders_[i]->setEnabled(e.enabled);
      }
    }
    if (color_) color_->setColor(e.color);
    up_->setEnabled(index > 0);
    down_->setEnabled(index + 1 < count);
  }

  const QString& effectId() const { return effectId_; }

 private:
  void move(int delta) {
    const QString clip = clipId_;
    const QString id = effectId_;
    editor().updateClip(clip, [&](VideoClip& c) {
      auto it = std::find_if(c.effects.begin(), c.effects.end(), [&](const Effect& x) { return x.id == id; });
      if (it == c.effects.end()) return;
      const int from = int(it - c.effects.begin());
      const int to = std::clamp(from + delta, 0, int(c.effects.size()) - 1);
      if (to != from) std::swap(c.effects[size_t(from)], c.effects[size_t(to)]);
    });
  }

  QString clipId_;
  QString effectId_;
  EffectType type_;
  QCheckBox* on_;
  QToolButton* up_;
  QToolButton* down_;
  QToolButton* remove_;
  SliderRow* sliders_[3] = {nullptr, nullptr, nullptr};
  ColorSwatches* color_ = nullptr;
};

EffectList::EffectList(QWidget* parent) : QWidget(parent) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(8);
  rows_ = new QVBoxLayout();
  rows_->setContentsMargins(0, 0, 0, 0);
  rows_->setSpacing(6);
  v->addLayout(rows_);
  empty_ = makeLabel(QStringLiteral("エフェクトはまだありません。「エフェクトを追加」か、左の「エフェクト」タブからタイムラインの映像へドラッグして足せます"),
                     "note", this);
  empty_->setWordWrap(true);
  v->addWidget(empty_);
  add_ = new QPushButton(theme::icon(QStringLiteral("plus")), QStringLiteral("エフェクトを追加"), this);
  addMenu_ = new QMenu(add_);
  add_->setMenu(addMenu_);
  v->addWidget(add_);
  connect(addMenu_, &QMenu::aboutToShow, this, [this] {
    addMenu_->clear();
    fillAddEffectMenu(addMenu_, clipId_, layer_);
  });
}

void EffectList::setTarget(const QString& clipId, bool layer) {
  if (clipId != clipId_ || layer != layer_) structure_.clear();
  clipId_ = clipId;
  layer_ = layer;
}

void EffectList::refresh() {
  const VideoClip* c = editor().clip(clipId_);
  if (!c) return;
  QString structure;
  for (const Effect& e : c->effects) structure += e.id + QLatin1Char(':') + QString::number(int(e.type)) + QLatin1Char(';');
  if (structure != structure_ || items_.size() != c->effects.size()) {
    structure_ = structure;
    rebuild(c->effects);
  }
  for (size_t i = 0; i < items_.size() && i < c->effects.size(); ++i) items_[i]->setValues(c->effects[i], int(i), int(c->effects.size()));
  empty_->setVisible(c->effects.empty());
}

void EffectList::rebuild(const std::vector<Effect>& effects) {
  for (EffectRow* r : items_) {
    rows_->removeWidget(r);
    r->hide();
    r->deleteLater();
  }
  items_.clear();
  for (const Effect& e : effects) {
    auto* r = new EffectRow(clipId_, e, this);
    rows_->addWidget(r);
    items_.push_back(r);
  }
}

}  // namespace kv
