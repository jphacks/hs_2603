#include "ui/animpanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

static const QColor kKeyColor(0xff, 0xc8, 0x3d);

// ---- KeyButton -------------------------------------------------------------------

KeyButton::KeyButton(Kind kind, QWidget* parent) : QAbstractButton(parent), kind_(kind) {
  setFixedSize(24, 24);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
}

void KeyButton::setActive(bool on) {
  if (active_ == on) return;
  active_ = on;
  update();
}

void KeyButton::enterEvent(QEnterEvent*) {
  hover_ = true;
  update();
}

void KeyButton::leaveEvent(QEvent*) {
  hover_ = false;
  update();
}

void KeyButton::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QPointF c(width() / 2.0, height() / 2.0);
  if (hover_ && isEnabled()) {
    p.setPen(Qt::NoPen);
    p.setBrush(theme::hover);
    p.drawRoundedRect(QRectF(1, 1, width() - 2, height() - 2), 4, 4);
  }
  if (hasFocus()) {
    p.setPen(QPen(theme::accent, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(1.5, 1.5, width() - 3, height() - 3), 4, 4);
  }
  if (kind_ == Kind::Diamond) {
    const double r = 6.5;
    QPolygonF d;
    d << QPointF(c.x(), c.y() - r) << QPointF(c.x() + r, c.y()) << QPointF(c.x(), c.y() + r) << QPointF(c.x() - r, c.y());
    if (active_) {
      p.setPen(QPen(QColor(0x8a, 0x62, 0x00), 1.2));
      p.setBrush(kKeyColor);
    } else {
      p.setPen(QPen(hover_ ? theme::text : theme::muted, 1.4));
      p.setBrush(Qt::NoBrush);
    }
    if (!isEnabled()) p.setOpacity(0.4);
    p.drawPolygon(d);
    return;
  }
  const double s = 4.5;
  const double dir = kind_ == Kind::Prev ? -1 : 1;
  QPolygonF tri;
  tri << QPointF(c.x() + dir * s, c.y() - s) << QPointF(c.x() - dir * s, c.y()) << QPointF(c.x() + dir * s, c.y() + s);
  p.setPen(Qt::NoPen);
  p.setBrush(isEnabled() ? (hover_ ? theme::text : theme::muted) : theme::dim);
  p.drawPolygon(tri);
}

// ---- VolumeKeyControls -----------------------------------------------------------

VolumeKeyControls::VolumeKeyControls(QWidget* parent) : QWidget(parent) {
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  prev_ = new KeyButton(KeyButton::Kind::Prev, this);
  diamond_ = new KeyButton(KeyButton::Kind::Diamond, this);
  next_ = new KeyButton(KeyButton::Kind::Next, this);
  prev_->setToolTip(QStringLiteral("前の音量キーフレームへ"));
  next_->setToolTip(QStringLiteral("次の音量キーフレームへ"));
  diamond_->setToolTip(QStringLiteral("再生位置に音量のキーフレームを追加 / 削除"));
  prev_->setAccessibleName(QStringLiteral("前の音量キーフレームへ"));
  next_->setAccessibleName(QStringLiteral("次の音量キーフレームへ"));
  diamond_->setAccessibleName(QStringLiteral("音量のキーフレームを追加または削除"));
  h->addWidget(prev_);
  h->addWidget(diamond_);
  h->addWidget(next_);

  connect(diamond_, &QAbstractButton::clicked, this, [this] {
    double start = 0;
    const auto* pts = points(&start);
    if (!pts) return;
    Editor& ed = editor();
    double dur = 0;
    if (const AudioItem* a = ed.audio(id_)) {
      dur = audioDuration(*a);
    } else {
      for (const auto& l : ed.layout().items) {
        if (l.clip.id == id_) dur = l.duration;
      }
    }
    const double local = ed.currentTime() - start;
    if (local < -1e-6 || local > dur + 1e-6) {
      ed.toast(QStringLiteral("再生位置が、選んでいるものの外にあります。範囲の中へ動かしてください"));
      return;
    }
    for (size_t i = 0; i < pts->size(); ++i) {
      if (std::abs((*pts)[i].t - local) < kMinVolumePointGap * 0.99) {
        ed.removeVolumePoint(id_, int(i));
        return;
      }
    }
    ed.addVolumePoint(id_, local);
  });
  auto seekTo = [this](bool forward) {
    double start = 0;
    const auto* pts = points(&start);
    if (!pts) return;
    Editor& ed = editor();
    const double local = ed.currentTime() - start;
    if (forward) {
      for (const auto& p : *pts) {
        if (p.t > local + 1e-3) {
          ed.seek(start + p.t);
          return;
        }
      }
    } else {
      for (auto it = pts->rbegin(); it != pts->rend(); ++it) {
        if (it->t < local - 1e-3) {
          ed.seek(start + it->t);
          return;
        }
      }
    }
  };
  connect(prev_, &QAbstractButton::clicked, this, [seekTo] { seekTo(false); });
  connect(next_, &QAbstractButton::clicked, this, [seekTo] { seekTo(true); });
  connect(&editor(), &Editor::timeChanged, this, [this] {
    if (isVisible()) refresh();
  });
}

const std::vector<VolumePoint>* VolumeKeyControls::points(double* start) const {
  const Editor& ed = editor();
  if (const AudioItem* a = ed.audio(id_)) {
    *start = a->start;
    return &a->volumePoints;
  }
  if (const VideoClip* c = ed.clip(id_)) {
    for (const auto& l : ed.layout().items) {
      if (l.clip.id == id_) *start = l.start;
    }
    return &c->volumePoints;
  }
  return nullptr;
}

void VolumeKeyControls::refresh() {
  double start = 0;
  const auto* pts = points(&start);
  if (!pts) return;
  const double local = editor().currentTime() - start;
  bool on = false;
  bool hasPrev = false;
  bool hasNext = false;
  for (const auto& p : *pts) {
    if (std::abs(p.t - local) < kMinVolumePointGap * 0.99) on = true;
    if (p.t < local - 1e-3) hasPrev = true;
    if (p.t > local + 1e-3) hasNext = true;
  }
  diamond_->setActive(on);
  prev_->setEnabled(hasPrev);
  next_->setEnabled(hasNext);
}

// ---- AnimPanel -------------------------------------------------------------------

static bool s_panelOpen = true;

static QString positionText(double v, bool horizontal) {
  const QSize d = designSize(editor().project().settings.aspect);
  return QString::number(std::lround(v * (horizontal ? d.width() : d.height())));
}

AnimPanel::AnimPanel(QWidget* parent) : QWidget(parent) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 12, 0, 12);
  v->setSpacing(8);

  // 見出し: 開閉ボタン・タイトル・自動記録
  auto* head = new QHBoxLayout();
  head->setContentsMargins(0, 0, 0, 0);
  head->setSpacing(4);
  toggle_ = new QToolButton(this);
  toggle_->setAutoRaise(true);
  toggle_->setCursor(Qt::PointingHandCursor);
  toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  toggle_->setText(QStringLiteral("アニメーション"));
  toggle_->setToolTip(QStringLiteral("アニメーションの設定を開く / 閉じる"));
  toggle_->setAccessibleName(QStringLiteral("アニメーションの設定を開閉"));
  toggle_->setStyleSheet(QStringLiteral("QToolButton{border:none;color:#9b9b9b;font-weight:bold;padding:2px 2px}QToolButton:hover{color:#e6e6e6}"));
  head->addWidget(toggle_);
  head->addStretch(1);
  autoKey_ = new QCheckBox(QStringLiteral("自動記録"), this);
  autoKey_->setToolTip(QStringLiteral("オンのとき、キーフレームのある項目を変えると、再生位置に自動でキーフレームを記録します。\nオフのときは、◆を押すまで増えません"));
  head->addWidget(autoKey_);
  v->addLayout(head);

  body_ = new QWidget(this);
  auto* b = new QVBoxLayout(body_);
  b->setContentsMargins(0, 0, 0, 0);
  b->setSpacing(8);
  hint_ = makeLabel(QString(), "note", body_);
  hint_->setWordWrap(true);
  b->addWidget(hint_);

  struct Def {
    AnimProp prop;
    double min;
    double max;
    double step;
  };
  const Def defs[] = {{AnimProp::X, -0.5, 1.5, 0.001},
                      {AnimProp::Y, -0.5, 1.5, 0.001},
                      {AnimProp::Scale, 0.05, 4.0, 0.01},
                      {AnimProp::Rotation, -360, 360, 1},
                      {AnimProp::Opacity, 0, 1, 0.01}};
  for (const Def& d : defs) {
    Row r;
    r.prop = d.prop;
    auto* row = new QHBoxLayout();
    row->setSpacing(2);
    r.label = makeLabel(propLabel(d.prop), nullptr, body_);
    r.label->setFixedWidth(58);
    const AnimProp prop = d.prop;
    std::function<QString(double)> fmt;
    switch (prop) {
      case AnimProp::X: fmt = [](double x) { return positionText(x, true); }; break;
      case AnimProp::Y: fmt = [](double x) { return positionText(x, false); }; break;
      case AnimProp::Scale: fmt = [](double x) { return formatPercent(x); }; break;
      case AnimProp::Rotation: fmt = [](double x) { return formatDegrees(x); }; break;
      case AnimProp::Opacity: fmt = [](double x) { return formatPercent(x); }; break;
    }
    r.slider = new SliderRow(propLabel(prop), d.min, d.max, d.step, fmt, body_);
    r.slider->setLabelVisible(false);
    r.slider->setDefault(propLimits(prop).def);
    r.slider->setValueBubble(true);
    // ホイール・矢印キーの単位: 拡大率 1%、回転 1°、不透明度 1%（Alt で 1/10）。位置は 1px で、refresh() で画面の大きさに合わせる
    if (prop == AnimProp::Scale || prop == AnimProp::Opacity) r.slider->setUnit(0.01, 0.001);
    if (prop == AnimProp::Rotation) r.slider->setUnit(1, 0.1);
    r.prev = new KeyButton(KeyButton::Kind::Prev, body_);
    r.diamond = new KeyButton(KeyButton::Kind::Diamond, body_);
    r.next = new KeyButton(KeyButton::Kind::Next, body_);
    r.prev->setToolTip(QStringLiteral("前のキーフレームへ"));
    r.next->setToolTip(QStringLiteral("次のキーフレームへ"));
    r.diamond->setToolTip(QStringLiteral("再生位置にキーフレームを追加 / 削除"));
    r.prev->setAccessibleName(propLabel(prop) + QStringLiteral(": 前のキーフレームへ"));
    r.next->setAccessibleName(propLabel(prop) + QStringLiteral(": 次のキーフレームへ"));
    r.diamond->setAccessibleName(propLabel(prop) + QStringLiteral(": キーフレームを追加または削除"));
    row->addWidget(r.label);
    row->addWidget(r.slider, 1);
    row->addSpacing(4);
    row->addWidget(r.prev);
    row->addWidget(r.diamond);
    row->addWidget(r.next);
    b->addLayout(row);

    // 最後に触った項目が「選んでいる項目」。その項目の、再生位置にあるキーフレームを、下の入力欄で編集する
    connect(r.slider, &SliderRow::changed, this, [this, prop](double x) {
      active_ = prop;
      // 受け付けなかったとき（キーフレームのある項目で、自動記録がOFF）は、表示を元の値に戻す
      if (!editor().setPropValue(prop, x, QStringLiteral("anim-%1-%2").arg(int(prop)).arg(editor().selection().id))) refresh();
    });
    connect(r.slider, &SliderRow::released, this, [] { editor().endMerge(); });
    connect(r.diamond, &QAbstractButton::clicked, this, [this, prop] {
      active_ = prop;
      editor().toggleKey(prop);
    });
    connect(r.prev, &QAbstractButton::clicked, this, [this, prop] {
      active_ = prop;
      editor().seekToKey(prop, false);
    });
    connect(r.next, &QAbstractButton::clicked, this, [this, prop] {
      active_ = prop;
      editor().seekToKey(prop, true);
    });
    rows_.push_back(r);
  }

  // 再生位置にあるキーフレーム（選んでいる項目の）の、時間・値・補間方式の入力欄。直接入力・ホイール・矢印キーで変えられる
  keyEdit_ = new QWidget(body_);
  auto* ke = new QVBoxLayout(keyEdit_);
  ke->setContentsMargins(0, 0, 0, 0);
  ke->setSpacing(4);
  keyTitle_ = makeLabel(QString(), "muted", keyEdit_);
  ke->addWidget(keyTitle_);
  auto* timeRow = new QHBoxLayout();
  timeRow->setSpacing(8);
  auto* timeLabel = makeLabel(QStringLiteral("時間"), "muted", keyEdit_);
  timeLabel->setFixedWidth(58);
  keyTime_ = new NumberSpin(keyEdit_);
  keyTime_->setDecimals(0);
  keyTime_->setUnit(1);  // 1 フレーム（Shift で 10 フレーム）
  keyTime_->setSuffix(QStringLiteral(" フレーム"));
  keyTime_->setAccessibleName(QStringLiteral("キーフレームの時間（フレーム）"));
  keyTime_->setToolTip(QStringLiteral("オブジェクトの先頭からのフレーム数。直接入力 / ホイール / 矢印キーで調整（Shift で10フレーム）"));
  timeRow->addWidget(timeLabel);
  timeRow->addWidget(keyTime_, 1);
  ke->addLayout(timeRow);
  auto* valueRow = new QHBoxLayout();
  valueRow->setSpacing(8);
  auto* valueLabel = makeLabel(QStringLiteral("値"), "muted", keyEdit_);
  valueLabel->setFixedWidth(58);
  keyValue_ = new NumberSpin(keyEdit_);
  keyValue_->setAccessibleName(QStringLiteral("キーフレームの値"));
  valueRow->addWidget(valueLabel);
  valueRow->addWidget(keyValue_, 1);
  ke->addLayout(valueRow);
  b->addWidget(keyEdit_);
  keyEdit_->setVisible(false);
  connect(keyTime_, &QDoubleSpinBox::valueChanged, this, [this](double frames) {
    const auto key = activeKey();
    if (!key) return;
    const double fps = editor().project().settings.fps;
    // キーフレームを動かすと、再生位置にあったキーフレームは別の時間へ移る。再生位置もそこへ合わせて、同じキーフレームを編集し続ける
    // （途中の状態では表示を更新しない）
    busy_ = true;
    if (editor().moveKeys({{*key, frames / fps}}, QStringLiteral("keytime-%1").arg(editor().selection().id))) editor().seek(editor().objectStart() + frames / fps);
    busy_ = false;
    refresh();
  });
  connect(keyValue_, &QDoubleSpinBox::valueChanged, this, [this](double shown) {
    const auto key = activeKey();
    if (!key) return;
    const QSize d = designSize(editor().project().settings.aspect);
    double v = shown;
    switch (key->prop) {
      case AnimProp::X: v = shown / d.width(); break;
      case AnimProp::Y: v = shown / d.height(); break;
      case AnimProp::Scale:
      case AnimProp::Opacity: v = shown / 100; break;
      case AnimProp::Rotation: break;
    }
    editor().setKeyValue(key->prop, key->index, v, QStringLiteral("keyval-%1").arg(editor().selection().id));
  });
  for (NumberSpin* sp : {keyTime_, keyValue_}) {
    connect(sp, &NumberSpin::stepFinished, this, [] { editor().endMerge(); });
    connect(sp, &QAbstractSpinBox::editingFinished, this, [] { editor().endMerge(); });
  }

  // 補間方式（選んでいるキーフレームから、次のキーフレームまでの変わり方）
  auto* easeRow = new QHBoxLayout();
  easeRow->setSpacing(8);
  auto* easeLabel = makeLabel(QStringLiteral("補間方式"), "muted", keyEdit_);
  easeLabel->setFixedWidth(58);
  easing_ = new QComboBox(keyEdit_);
  easing_->setAccessibleName(QStringLiteral("補間方式"));
  easing_->setToolTip(QStringLiteral("選んでいるキーフレームから、次のキーフレームまでの変わり方"));
  // 幅の狭いパネルでも収まるよう、名前は短くする（くわしい説明はツールチップ）
  static const char* const kShort[] = {"リニア", "イーズイン", "イーズアウト", "イーズインアウト", "ホールド"};
  for (int i = 0; i <= int(Easing::Hold); ++i) {
    easing_->addItem(QString::fromUtf8(kShort[i]), i);
    easing_->setItemData(i, easingLabel(static_cast<Easing>(i)), Qt::ToolTipRole);
  }
  easing_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  easing_->setMinimumContentsLength(6);
  disableWheel(easing_);
  easeRow->addWidget(easeLabel);
  easeRow->addWidget(easing_, 1);
  ke->addLayout(easeRow);

  // コピー・貼り付け・削除
  auto* tools = new QHBoxLayout();
  tools->setSpacing(6);
  copy_ = new QPushButton(QStringLiteral("コピー"), body_);
  paste_ = new QPushButton(QStringLiteral("貼り付け"), body_);
  clear_ = new QPushButton(QStringLiteral("すべて削除"), body_);
  copy_->setToolTip(QStringLiteral("再生位置にあるキーフレームをコピー"));
  paste_->setToolTip(QStringLiteral("コピーしたキーフレームを、再生位置から貼り付け"));
  clear_->setToolTip(QStringLiteral("このオブジェクトのキーフレームをすべて削除"));
  for (QPushButton* b : {copy_, paste_, clear_}) {
    b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    b->setStyleSheet(QStringLiteral("QPushButton{padding:5px 2px;font-size:8.5pt}"));
    tools->addWidget(b);
  }
  b->addLayout(tools);

  // プリセット
  b->addWidget(makeLabel(QStringLiteral("プリセット"), "muted", body_));
  auto* grid = new QGridLayout();
  grid->setSpacing(6);
  int i = 0;
  for (const auto& preset : Editor::animPresets()) {
    auto* btn = new QPushButton(preset.label, body_);
    // 幅の狭いパネルでも 2 列に収まるよう、ボタンの大きさは並べる側で決める（文字は小さめ）
    btn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    btn->setStyleSheet(QStringLiteral("QPushButton{padding:5px 2px;font-size:8.5pt}"));
    btn->setToolTip(preset.hint + QStringLiteral("。適用したあとも、キーフレームは自由に編集できます"));
    const QString id = preset.id;
    const QString label = preset.label;
    connect(btn, &QPushButton::clicked, this, [id, label] {
      if (editor().applyAnimPreset(id)) editor().toast(label + QStringLiteral("を追加しました"));
      else editor().toast(QStringLiteral("プリセットを追加できませんでした（長さが短すぎます）"));
    });
    grid->addWidget(btn, i / 2, i % 2);
    ++i;
  }
  b->addLayout(grid);
  v->addWidget(body_);

  connect(toggle_, &QToolButton::clicked, this, [this] { setOpen(!open_); });
  connect(autoKey_, &QCheckBox::toggled, this, [](bool on) { editor().setAutoKey(on); });
  connect(easing_, &QComboBox::activated, this, [this](int idx) {
    if (const auto key = activeKey()) editor().setKeyEasing({*key}, static_cast<Easing>(idx));
  });
  connect(copy_, &QPushButton::clicked, this, [this] {
    if (editor().copyKeys(keysAtPlayhead())) editor().toast(QStringLiteral("キーフレームをコピーしました"));
  });
  connect(paste_, &QPushButton::clicked, this, [] {
    Editor& ed = editor();
    const double local = std::clamp(ed.currentTime() - ed.objectStart(), 0.0, ed.objectDuration());
    const int n = ed.pasteKeys(local);
    ed.toast(n > 0 ? QStringLiteral("キーフレームを%1個貼り付けました").arg(n) : QStringLiteral("貼り付けられるキーフレームがありません"));
  });
  connect(clear_, &QPushButton::clicked, this, [] {
    const Animation* a = editor().animation();
    if (!a) return;
    std::vector<Editor::KeyRef> all;
    for (int pi = 0; pi < kAnimPropCount; ++pi) {
      const AnimProp prop = static_cast<AnimProp>(pi);
      for (size_t k = 0; k < a->keys(prop).size(); ++k) all.push_back({prop, int(k)});
    }
    editor().removeKeys(all);
  });

  Editor& ed = editor();
  auto refreshIfVisible = [this] {
    if (isVisible()) refresh();
  };
  connect(&ed, &Editor::timeChanged, this, refreshIfVisible);
  connect(&ed, &Editor::autoKeyChanged, this, refreshIfVisible);
  connect(&ed, &Editor::projectChanged, this, refreshIfVisible);

  setOpen(s_panelOpen);
}

void AnimPanel::setOpen(bool open) {
  open_ = open;
  s_panelOpen = open;  // 映像と文字のパネルで、開閉をそろえる（設定ファイルには書かない）
  body_->setVisible(open);
  toggle_->setText((open ? QStringLiteral("▾ ") : QStringLiteral("▸ ")) + QStringLiteral("アニメーション"));
}

std::optional<Editor::KeyRef> AnimPanel::activeKey() const {
  const Editor& ed = editor();
  const Animation* a = ed.animation();
  if (!a) return std::nullopt;
  const int idx = findKey(a->keys(active_), ed.currentTime() - ed.objectStart());
  if (idx < 0) return std::nullopt;
  return Editor::KeyRef{active_, idx};
}

std::vector<Editor::KeyRef> AnimPanel::keysAtPlayhead() const {
  const Editor& ed = editor();
  std::vector<Editor::KeyRef> out;
  const Animation* a = ed.animation();
  if (!a) return out;
  const double local = ed.currentTime() - ed.objectStart();
  for (int pi = 0; pi < kAnimPropCount; ++pi) {
    const AnimProp prop = static_cast<AnimProp>(pi);
    const int idx = findKey(a->keys(prop), local);
    if (idx >= 0) out.push_back({prop, idx});
  }
  return out;
}

void AnimPanel::refresh() {
  if (busy_) return;
  const Editor& ed = editor();
  const Animation* a = ed.animation();
  if (!a) return;
  const LayerState st = ed.currentLayerState();
  const double vals[kAnimPropCount] = {st.x, st.y, st.scale, st.rotation, st.opacity};
  const double local = ed.currentTime() - ed.objectStart();
  const QSize design = designSize(ed.project().settings.aspect);
  bool anyKeys = false;
  for (auto& r : rows_) {
    const auto& keys = a->keys(r.prop);
    // 位置の単位は 1px（画面の大きさに合わせる）
    if (r.prop == AnimProp::X) r.slider->setUnit(1.0 / design.width());
    if (r.prop == AnimProp::Y) r.slider->setUnit(1.0 / design.height());
    r.slider->setValue(vals[int(r.prop)]);
    const bool animated = !keys.empty();
    anyKeys = anyKeys || animated;
    // アニメーションを設定している項目は、名前を黄色にして見分けられるようにする
    r.label->setStyleSheet(animated ? QStringLiteral("color:#ffc83d;font-weight:bold") : QString());
    r.diamond->setActive(findKey(keys, local) >= 0);
    bool hasPrev = false;
    bool hasNext = false;
    for (const auto& k : keys) {
      if (k.t < local - 1e-3) hasPrev = true;
      if (k.t > local + 1e-3) hasNext = true;
    }
    r.prev->setEnabled(hasPrev);
    r.next->setEnabled(hasNext);
  }
  {
    const QSignalBlocker blocker(autoKey_);
    autoKey_->setChecked(ed.autoKey());
  }
  // 選んでいる項目に、再生位置のキーフレームが無いときは、再生位置にキーフレームのある最初の項目を選ぶ
  if (!activeKey()) {
    for (int pi = 0; pi < kAnimPropCount; ++pi) {
      if (findKey(a->keys(static_cast<AnimProp>(pi)), local) >= 0) {
        active_ = static_cast<AnimProp>(pi);
        break;
      }
    }
  }
  const auto key = activeKey();
  copy_->setEnabled(!keysAtPlayhead().empty());
  paste_->setEnabled(ed.hasKeyClipboard());
  clear_->setEnabled(anyKeys);
  const double dur = ed.objectDuration();

  // 再生位置にキーフレームがあるときだけ、その時間・値・補間方式の入力欄を出す
  keyEdit_->setVisible(key.has_value());
  if (key) {
    const AnimProp prop = key->prop;
    const Keyframe& k = a->keys(prop)[size_t(key->index)];
    const double fps = ed.project().settings.fps;
    keyTitle_->setText(QStringLiteral("選んでいるキーフレーム: %1").arg(propLabel(prop)));
    const QSignalBlocker b0(easing_);
    easing_->setCurrentIndex(int(k.easing));
    const QSignalBlocker b1(keyTime_);
    const QSignalBlocker b2(keyValue_);
    keyTime_->setRange(0, std::max(0.0, std::round(dur * fps)));
    if (!keyTime_->hasFocus()) keyTime_->setValue(std::round(k.t * fps));
    // 値は、画面に出す単位（位置は px、拡大率と不透明度は %、回転は度）で扱う
    const PropLimits lim = propLimits(prop);
    switch (prop) {
      case AnimProp::X:
      case AnimProp::Y: {
        const double scale = prop == AnimProp::X ? design.width() : design.height();
        keyValue_->setDecimals(0);
        keyValue_->setRange(lim.min * scale, lim.max * scale);
        keyValue_->setUnit(1);
        keyValue_->setSuffix(QStringLiteral(" px"));
        if (!keyValue_->hasFocus()) keyValue_->setValue(k.value * scale);
        break;
      }
      case AnimProp::Scale:
      case AnimProp::Opacity:
        keyValue_->setDecimals(1);
        keyValue_->setRange(lim.min * 100, lim.max * 100);
        keyValue_->setUnit(1, 0.1);
        keyValue_->setSuffix(QStringLiteral(" %"));
        if (!keyValue_->hasFocus()) keyValue_->setValue(k.value * 100);
        break;
      case AnimProp::Rotation:
        keyValue_->setDecimals(1);
        keyValue_->setRange(lim.min, lim.max);
        keyValue_->setUnit(1, 0.1);
        keyValue_->setSuffix(QStringLiteral(" °"));
        if (!keyValue_->hasFocus()) keyValue_->setValue(k.value);
        break;
    }
  }
  if (local < -1e-6 || local > dur + 1e-6) {
    hint_->setText(QStringLiteral("再生位置が、選んでいるものの外にあります。範囲の中へ動かしてください"));
  } else if (!anyKeys) {
    hint_->setText(QStringLiteral("◆ を押すと、再生位置にキーフレームを置けます。2か所以上に置いて値を変えると、間が自動で動きます。下のプリセットからも作れます"));
  } else {
    hint_->setText(QStringLiteral("再生位置を動かして値を変えると、動きが作れます（自動記録がオンのとき）。黄色い項目はアニメーション中です"));
  }
}

}  // namespace kv
