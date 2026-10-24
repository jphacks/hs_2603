#include "ui/previewwidget.h"

#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "core/editor.h"
#include "core/timeline.h"
#include "render/imageops.h"
#include "render/previewengine.h"
#include "render/textrender.h"
#include "ui/librarypanel.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

PreviewStage::PreviewStage(PreviewEngine* engine, QWidget* parent) : QWidget(parent), engine_(engine) {
  setAcceptDrops(true);
  setMinimumSize(320, 180);
  setMouseTracking(true);  // 枠の上でカーソルを変える
  setFocusPolicy(Qt::ClickFocus);
  setAccessibleName(QStringLiteral("プレビュー"));
  connect(engine_, &PreviewEngine::frameReady, this, qOverload<>(&QWidget::update));
  Editor& ed = editor();
  connect(&ed, &Editor::projectChanged, this, [this] {
    const bool isEmpty = editor().project().clips.empty() && editor().project().texts.empty();
    empty_->setVisible(isEmpty);
    updateCanvasSize();
    update();
  });
  connect(&ed, &Editor::selectionChanged, this, qOverload<>(&QWidget::update));

  // 何も無いときの案内
  empty_ = new QWidget(this);
  auto* v = new QVBoxLayout(empty_);
  v->setAlignment(Qt::AlignCenter);
  v->setSpacing(10);
  auto* add = new QPushButton(theme::icon(QStringLiteral("plus"), Qt::white), QStringLiteral("メディアを追加"), empty_);
  add->setProperty("primary", true);
  add->setMinimumHeight(34);
  connect(add, &QPushButton::clicked, this, [this] { LibraryPanel::pickAndImport(this); });
  auto* hint = makeLabel(QStringLiteral("またはファイルをここにドロップ"), "dim", empty_);
  hint->setAlignment(Qt::AlignCenter);
  v->addWidget(add, 0, Qt::AlignHCenter);
  v->addWidget(hint, 0, Qt::AlignHCenter);
  empty_->setVisible(ed.project().clips.empty() && ed.project().texts.empty());
}

QRectF PreviewStage::stageRect() const {
  const double ratio = aspectValue(editor().project().settings.aspect);
  const QRectF area = QRectF(rect()).adjusted(12, 12, -12, -12);
  double w = area.width();
  double h = w / ratio;
  if (h > area.height()) {
    h = area.height();
    w = h * ratio;
  }
  return {std::floor(area.center().x() - w / 2), std::floor(area.center().y() - h / 2), std::floor(w), std::floor(h)};
}

void PreviewStage::updateCanvasSize() {
  const QRectF r = stageRect();
  // 高解像度の画面でも重くならないよう、長い辺は 1600px までにする
  const qreal dpr = devicePixelRatioF();
  double w = r.width() * dpr;
  double h = r.height() * dpr;
  const double k = std::min(1.0, 1600.0 / std::max({w, h, 1.0}));
  engine_->setCanvasSize(QSize(std::max(16, int(w * k)), std::max(16, int(h * k))));
  empty_->setGeometry(r.toRect());
}

void PreviewStage::resizeEvent(QResizeEvent*) {
  updateCanvasSize();
}

void PreviewStage::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), QColor(0x0d, 0x0d, 0x0d));
  const QRectF r = stageRect();
  const QImage& frame = engine_->frame();
  if (!frame.isNull()) {
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(r, frame);
  } else {
    p.fillRect(r, Qt::black);
  }
  const Editor& ed = editor();
  if (empty_->isVisible()) p.fillRect(r, QColor(0x16, 0x16, 0x16));
  // 選んでいる映像・テキストの操作用の枠（ドラッグで移動、四隅で拡大・縮小、上の丸で回転）
  QPolygonF quad;
  if (!ed.playing() && selectedQuad(&quad)) {
    QPolygonF w;
    for (const QPointF& q : quad) w << toWidget(q);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 130), 3));
    p.drawPolygon(w);
    p.setPen(QPen(QColor(255, 255, 255, 230), 1));
    p.drawPolygon(w);
    // 回転ハンドルへの線と丸
    const QPointF top = (w[0] + w[1]) / 2;
    const QPointF rot = rotateHandle(quad);
    p.setPen(QPen(QColor(255, 255, 255, 200), 1));
    p.drawLine(top, rot);
    p.setPen(QPen(QColor(0x1d, 0x4d, 0xb0), 1.2));
    p.setBrush(QColor(255, 255, 255));
    p.drawEllipse(rot, 5.5, 5.5);
    // 四隅の四角（拡大・縮小）
    p.setBrush(QColor(255, 255, 255));
    for (const QPointF& c : w) p.drawRect(QRectF(c.x() - 4.5, c.y() - 4.5, 9, 9));
  }
  if (guideV_) p.fillRect(QRectF(r.center().x(), r.top(), 1, r.height()), QColor(0xff, 0x4f, 0xd8));
  if (guideH_) p.fillRect(QRectF(r.left(), r.center().y(), r.width(), 1), QColor(0xff, 0x4f, 0xd8));
  if (dropHover_) {
    p.setPen(QPen(theme::accent, 2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(6, 6, -7, -7));
  }
}

// ---- 映像・テキストの位置・大きさ・回転の直接操作 ---------------------------------------------
// 文字は「画面の中央に置いたときの範囲」を、位置・拡大率・回転の変換で動かした四角として扱う。
// 映像は画面いっぱいのレイヤーを同じ変換で動かした四角として扱う。

static constexpr double kRotateOffsetPx = 26;  // 回転ハンドルを枠の上端から離す距離（画面のピクセル）
static constexpr double kHandlePx = 9;         // ハンドルを掴める範囲

QPointF PreviewStage::toWidget(QPointF design) const {
  const QRectF r = stageRect();
  const QSize d = designSize(editor().project().settings.aspect);
  const double k = r.width() / d.width();
  return {r.x() + design.x() * k, r.y() + design.y() * k};
}

QPointF PreviewStage::toDesign(QPointF widget) const {
  const QRectF r = stageRect();
  const QSize d = designSize(editor().project().settings.aspect);
  const double k = r.width() / d.width();
  return {(widget.x() - r.x()) / k, (widget.y() - r.y()) / k};
}

/** 文字の枠（基準解像度の座標）。pad は枠の余白（基準座標） */
static QPolygonF textQuad(const TextItem& item, double t, QSize d, const QString& editingId, double pad) {
  TextItem centered = item;
  centered.x = 0.5;
  centered.y = 0.5;
  const QRectF b = layoutText(centered, d.width(), d.height()).bounds.adjusted(-pad, -pad, pad, pad);
  const LayerState ls = layerStateAt(item, t - item.start);
  const AnimState st = previewTextState(item, t, editingId);
  const QTransform m = layerMatrix(ls, d, QPointF(0, st.offsetY * d.height()));
  QPolygonF poly;
  poly << b.topLeft() << b.topRight() << b.bottomRight() << b.bottomLeft();
  return m.map(poly);
}

static QPolygonF clipQuad(const VideoClip& c, double local, QSize d) {
  const QTransform m = layerMatrix(layerStateAt(c, local), d);
  QPolygonF poly;
  poly << QPointF(0, 0) << QPointF(d.width(), 0) << QPointF(d.width(), d.height()) << QPointF(0, d.height());
  return m.map(poly);
}

bool PreviewStage::selectedQuad(QPolygonF* quad) const {
  const Editor& ed = editor();
  const Selection& s = ed.selection();
  const QSize d = designSize(ed.project().settings.aspect);
  const double t = ed.currentTime();
  const double k = stageRect().width() / d.width();
  if (s.type == Selection::Text) {
    const TextItem* item = ed.text(s.id);
    if (!item || !isTextVisible(*item, t)) return false;
    *quad = textQuad(*item, t, d, s.id, 4 / k);
    return true;
  }
  if (s.type == Selection::Clip) {
    const ClipLayout* cur = findClipAt(ed.layout(), t);
    if (!cur || cur->clip.id != s.id) return false;
    *quad = clipQuad(cur->clip, std::clamp(t - cur->start, 0.0, cur->duration), d);
    return true;
  }
  return false;
}

/** 回転ハンドルの位置（画面のピクセル）。枠の上端の中央から、枠の「上」の向きへ離した場所 */
QPointF PreviewStage::rotateHandle(const QPolygonF& quad) const {
  const QPointF a = toWidget(quad[0]);
  const QPointF b = toWidget(quad[1]);
  const QPointF dl = toWidget(quad[3]);
  QPointF up = a - dl;
  const double len = std::hypot(up.x(), up.y());
  if (len > 1e-6) up /= len;
  else up = QPointF(0, -1);
  QPointF h = (a + b) / 2 + up * kRotateOffsetPx;
  // 画面いっぱいの映像のときも、ハンドルがこの部品の外に出て掴めなくならないようにする
  const QRectF lim = QRectF(rect()).adjusted(10, 10, -10, -10);
  h.setX(std::clamp(h.x(), lim.left(), std::max(lim.left(), lim.right())));
  h.setY(std::clamp(h.y(), lim.top(), std::max(lim.top(), lim.bottom())));
  return h;
}

PreviewStage::Grab PreviewStage::grabAt(QPointF pos, const QPolygonF& quad) const {
  if (std::hypot(pos.x() - rotateHandle(quad).x(), pos.y() - rotateHandle(quad).y()) <= kHandlePx) return Grab::Rotate;
  for (const QPointF& c : quad) {
    const QPointF w = toWidget(c);
    if (std::abs(pos.x() - w.x()) <= kHandlePx && std::abs(pos.y() - w.y()) <= kHandlePx) return Grab::Scale;
  }
  QPolygonF poly;
  for (const QPointF& c : quad) poly << toWidget(c);
  return poly.containsPoint(pos, Qt::WindingFill) ? Grab::Move : Grab::None;
}

QString PreviewStage::hitText(QPointF pos) const {
  const Editor& ed = editor();
  const QSize d = designSize(ed.project().settings.aspect);
  const QPointF dp = toDesign(pos);
  const double t = ed.currentTime();
  const QString editing = ed.selection().type == Selection::Text ? ed.selection().id : QString();
  const auto& texts = ed.project().texts;
  for (auto it = texts.rbegin(); it != texts.rend(); ++it) {
    if (!isTextVisible(*it, t) || it->text.isEmpty()) continue;
    if (textQuad(*it, t, d, editing, 0).containsPoint(dp, Qt::WindingFill)) return it->id;
  }
  return {};
}

void PreviewStage::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  Editor& ed = editor();
  grab_ = Grab::None;
  // 選んでいるものの枠を掴んだとき: 拡大・縮小 / 回転 / 移動
  QPolygonF quad;
  Grab g = Grab::None;
  if (selectedQuad(&quad)) {
    g = grabAt(e->position(), quad);
    // ハンドル、または選んでいる文字の枠の中: そのまま操作する
    if (g == Grab::Rotate || g == Grab::Scale || (g == Grab::Move && ed.selection().type == Selection::Text)) {
      beginGrab(g, e->position());
      return;
    }
  }
  // 文字をクリックしたら、その文字を選んで動かせるようにする（映像の枠の中でも、文字が優先）
  const QString id = hitText(e->position());
  if (!id.isEmpty()) {
    ed.pause();
    ed.select({Selection::Text, id});
    if (selectedQuad(&quad)) beginGrab(Grab::Move, e->position());
    return;
  }
  // 選んでいる映像の枠の中: 映像を動かす
  if (g == Grab::Move) {
    beginGrab(g, e->position());
    return;
  }
  // それ以外: 今の再生位置にある映像を選ぶ（選んでから、枠をドラッグして動かす）。枠の外をクリックしたら選択を外す
  const ClipLayout* cur = findClipAt(ed.layout(), ed.currentTime());
  if (cur && ed.selection().id != cur->clip.id) {
    ed.pause();
    ed.select({Selection::Clip, cur->clip.id});
    return;
  }
  if (ed.selection().type == Selection::Text || ed.selection().type == Selection::Clip) ed.select({});
}

void PreviewStage::beginGrab(Grab g, QPointF pos) {
  Editor& ed = editor();
  ed.pause();
  grab_ = g;
  dragStart_ = pos;
  dragMerge_ = QStringLiteral("preview-%1-").arg(int(g)) + newId();
  origState_ = ed.currentLayerState();
  const QSize d = designSize(ed.project().settings.aspect);
  center_ = toWidget(QPointF(origState_.x * d.width(), origState_.y * d.height()));
  startDist_ = std::max(1.0, std::hypot(pos.x() - center_.x(), pos.y() - center_.y()));
  startAngle_ = std::atan2(pos.y() - center_.y(), pos.x() - center_.x()) * 180.0 / M_PI;
  lastAngle_ = startAngle_;
  accumRot_ = 0;
}

void PreviewStage::mouseMoveEvent(QMouseEvent* e) {
  Editor& ed = editor();
  if (grab_ == Grab::None) {
    // 枠の上では、できる操作に合わせたカーソルにする
    QPolygonF quad;
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (!ed.playing() && selectedQuad(&quad)) {
      switch (grabAt(e->position(), quad)) {
        case Grab::Rotate: shape = Qt::CrossCursor; break;
        case Grab::Scale: shape = Qt::SizeFDiagCursor; break;
        case Grab::Move: shape = Qt::SizeAllCursor; break;
        default: break;
      }
    }
    setCursor(shape);
    return;
  }
  const QRectF r = stageRect();
  if (grab_ == Grab::Move) {
    double x = origState_.x + (e->position().x() - dragStart_.x()) / r.width();
    double y = origState_.y + (e->position().y() - dragStart_.y()) / r.height();
    // 画面の中央にそろえる（ガイドの線を出す）
    guideV_ = std::abs(x - 0.5) < 0.015;
    guideH_ = std::abs(y - 0.5) < 0.015;
    if (guideV_) x = 0.5;
    if (guideH_) y = 0.5;
    ed.setPropValue(AnimProp::X, x, dragMerge_);
    ed.setPropValue(AnimProp::Y, y, dragMerge_);
  } else if (grab_ == Grab::Scale) {
    // 中心からの距離の比で、拡大率を変える
    const double dist = std::max(1.0, std::hypot(e->position().x() - center_.x(), e->position().y() - center_.y()));
    ed.setPropValue(AnimProp::Scale, origState_.scale * dist / startDist_, dragMerge_);
  } else if (grab_ == Grab::Rotate) {
    // 中心から見た向きの変化ぶんだけ回す（一周しても途切れないよう、少しずつ足す）。Shift で 15 度刻み
    const double ang = std::atan2(e->position().y() - center_.y(), e->position().x() - center_.x()) * 180.0 / M_PI;
    double dlt = ang - lastAngle_;
    while (dlt > 180) dlt -= 360;
    while (dlt < -180) dlt += 360;
    accumRot_ += dlt;
    lastAngle_ = ang;
    double rot = origState_.rotation + accumRot_;
    if (e->modifiers() & Qt::ShiftModifier) rot = std::round(rot / 15.0) * 15.0;
    ed.setPropValue(AnimProp::Rotation, rot, dragMerge_);
  }
  update();
}

void PreviewStage::mouseReleaseEvent(QMouseEvent*) {
  grab_ = Grab::None;
  guideV_ = guideH_ = false;
  editor().endMerge();
  update();
}

void PreviewStage::mouseDoubleClickEvent(QMouseEvent* e) {
  if (!hitText(e->position()).isEmpty()) emit requestTextFocus();
}

void PreviewStage::dragEnterEvent(QDragEnterEvent* e) {
  if (e->mimeData()->hasFormat(QString::fromLatin1(kAssetMime))) {
    e->acceptProposedAction();
    dropHover_ = true;
    update();
  }
}

void PreviewStage::dragLeaveEvent(QDragLeaveEvent*) {
  dropHover_ = false;
  update();
}

void PreviewStage::dropEvent(QDropEvent* e) {
  dropHover_ = false;
  update();
  const QByteArray data = e->mimeData()->data(QString::fromLatin1(kAssetMime));
  if (data.isEmpty()) return;
  std::vector<QString> ids;
  for (const auto& s : QString::fromUtf8(data).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) ids.push_back(s);
  editor().addToTimeline(ids);
  e->acceptProposedAction();
}

// ---- TransportBar -----------------------------------------------------------

TransportBar::TransportBar(QWidget* parent) : QWidget(parent) {
  auto* h = new QHBoxLayout(this);
  h->setContentsMargins(12, 2, 12, 8);
  h->setSpacing(4);
  time_ = new QLabel(this);
  time_->setMinimumWidth(150);
  QFont mono = time_->font();
  mono.setStyleHint(QFont::Monospace);
  mono.setFeature("tnum", 1);
  time_->setFont(mono);
  h->addWidget(time_, 1);

  auto* home = makeToolButton(QStringLiteral("skip-back"), QStringLiteral("先頭へ (Home)"), this);
  auto* back = makeToolButton(QStringLiteral("rewind"), QStringLiteral("1フレーム戻る (←)"), this);
  play_ = new QToolButton(this);
  play_->setObjectName(QStringLiteral("playButton"));
  play_->setFixedSize(38, 38);
  play_->setIconSize(QSize(18, 18));
  play_->setFocusPolicy(Qt::NoFocus);
  auto* fwd = makeToolButton(QStringLiteral("fast-forward"), QStringLiteral("1フレーム進む (→)"), this);
  auto* end = makeToolButton(QStringLiteral("skip-forward"), QStringLiteral("末尾へ (End)"), this);
  for (auto* b : {home, back}) h->addWidget(b);
  h->addSpacing(6);
  h->addWidget(play_);
  h->addSpacing(6);
  for (auto* b : {fwd, end}) h->addWidget(b);

  aspect_ = new Segmented({{QVariant(int(Aspect::R16_9)), QStringLiteral("16:9")},
                           {QVariant(int(Aspect::R9_16)), QStringLiteral("9:16")},
                           {QVariant(int(Aspect::R1_1)), QStringLiteral("1:1")},
                           {QVariant(int(Aspect::R4_5)), QStringLiteral("4:5")}},
                          0, this);
  aspect_->setAccessibleName(QStringLiteral("比率"));
  auto* right = new QWidget(this);
  auto* rh = new QHBoxLayout(right);
  rh->setContentsMargins(0, 0, 0, 0);
  rh->addStretch(1);
  rh->addWidget(aspect_);
  h->addWidget(right, 1);

  Editor& ed = editor();
  connect(home, &QToolButton::clicked, this, [] { editor().seek(0); });
  connect(back, &QToolButton::clicked, this, [] {
    editor().pause();
    editor().seek(editor().currentTime() - 1.0 / editor().project().settings.fps);
  });
  connect(fwd, &QToolButton::clicked, this, [] {
    editor().pause();
    editor().seek(editor().currentTime() + 1.0 / editor().project().settings.fps);
  });
  connect(end, &QToolButton::clicked, this, [] { editor().seek(editor().playEnd()); });
  connect(play_, &QToolButton::clicked, this, [] { editor().togglePlay(); });
  connect(aspect_, &Segmented::changed, this, [](const QVariant& v) { editor().setAspect(Aspect(v.toInt())); });
  connect(&ed, &Editor::timeChanged, this, &TransportBar::refresh);
  connect(&ed, &Editor::projectChanged, this, &TransportBar::refresh);
  connect(&ed, &Editor::playingChanged, this, &TransportBar::refresh);
  refresh();
}

void TransportBar::refresh() {
  const Editor& ed = editor();
  time_->setText(QStringLiteral("<span style='color:#e6e6e6'>%1</span><span style='color:#6b6b6b'>&nbsp;&nbsp;/&nbsp;&nbsp;</span>"
                                "<span style='color:#9b9b9b'>%2</span>")
                     .arg(formatTime(ed.currentTime()), formatTime(ed.layout().total)));
  play_->setIcon(theme::icon(ed.playing() ? QStringLiteral("pause") : QStringLiteral("play"), QColor(0x11, 0x11, 0x11)));
  play_->setToolTip(ed.playing() ? QStringLiteral("一時停止 (Space)") : QStringLiteral("再生 (Space)"));
  play_->setAccessibleName(ed.playing() ? QStringLiteral("一時停止") : QStringLiteral("再生"));
  play_->setEnabled(ed.playEnd() > 0);
  aspect_->setValue(int(ed.project().settings.aspect));
}

PreviewPanel::PreviewPanel(PreviewEngine* engine, QWidget* parent) : QWidget(parent) {
  setAutoFillBackground(true);
  QPalette pal = palette();
  pal.setColor(QPalette::Window, QColor(0x0d, 0x0d, 0x0d));
  setPalette(pal);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  stage_ = new PreviewStage(engine, this);
  v->addWidget(stage_, 1);
  v->addWidget(new TransportBar(this));
}

}  // namespace kv
