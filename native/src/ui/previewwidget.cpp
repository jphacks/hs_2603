#include "ui/previewwidget.h"

#include <QClipboard>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethod>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QTextBoundaryFinder>
#include <QTextCharFormat>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

#include "core/editor.h"
#include "core/textstyles.h"
#include "core/timeline.h"
#include "media/mediamanager.h"
#include "mask/maskcontroller.h"
#include "mask/masktrack.h"
#include "render/imageops.h"
#include "render/maskframes.h"
#include "render/previewengine.h"
#include "render/textrender.h"
#include "ui/clipdialogs.h"
#include "ui/keymap.h"
#include "ui/librarypanel.h"
#include "ui/objectmenu.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

/** 画面に出すものが何も無いか（何も無いときは、メディアを追加する案内を出す） */
static bool projectIsEmpty(const ProjectData& p) {
  return p.clips.empty() && p.overlays.empty() && p.texts.empty();
}

PreviewStage::PreviewStage(PreviewEngine* engine, QWidget* parent) : QWidget(parent), engine_(engine) {
  setAcceptDrops(true);
  setMinimumSize(320, 180);
  setMouseTracking(true);  // 枠の上でカーソルを変える
  setFocusPolicy(Qt::ClickFocus);
  setAccessibleName(QStringLiteral("プレビュー"));
  connect(engine_, &PreviewEngine::frameReady, this, qOverload<>(&QWidget::update));
  Editor& ed = editor();
  connect(&ed, &Editor::projectChanged, this, [this] {
    const bool isEmpty = projectIsEmpty(editor().project());
    empty_->setVisible(isEmpty && !fullScreen_);
    updateCanvasSize();
    if (isEditingText() && !applying_) {
      // 編集中の文字が、ほかの操作（元に戻す、設定欄での入力など）で変わった / 消えた
      const TextItem* t = editor().text(editId_);
      if (!t) {
        endTextEdit(true);
      } else {
        caret_ = std::min(caret_, int(t->text.size()));
        anchor_ = std::min(anchor_, int(t->text.size()));
        syncEngine();
        updateInputMethod();
      }
    }
    update();
  });
  connect(&ed, &Editor::selectionChanged, this, [this] {
    // 別のものを選んだら、編集は確定する
    const Selection& s = editor().selection();
    if (isEditingText() && !(s.type == Selection::Text && s.id == editId_)) endTextEdit(true);
    update();
  });
  // 再生を始めたとき・編集場所（タブ）を切り替えるとき・文字が今の再生位置から外れたときは、編集を確定して終える
  connect(&ed, &Editor::playingChanged, this, [this] {
    if (editor().playing()) endTextEdit(true);
  });
  connect(&ed, &Editor::workspaceAboutToChange, this, [this] { endTextEdit(true); });
  connect(&ed, &Editor::guidesChanged, this, qOverload<>(&QWidget::update));
  // スポイト: 始めたら、Esc を受け取れるようにこの部品へ入力を向け、カーソルを十字にする
  auto pickChanged = [this] {
    if (picking()) {
      endTextEdit(true);
      grab_ = Grab::None;
      guideV_ = guideH_ = false;
      setCursor(Qt::CrossCursor);
      setFocus(Qt::OtherFocusReason);
    } else {
      unsetCursor();
    }
    // 被写体を選ぶ: 今のフレームの AI の準備を先に始める（カーソルを合わせたときに、すぐマスクが出るように）
    maskCursorIn_ = false;
    mask::maskController().clearHover();
    if (maskPicking()) {
      double pts = 0;
      const QString id = editor().maskPick();
      const QImage frame = engine_->rawFrame(id, &pts);
      if (!frame.isNull()) mask::maskController().prepare(id, frame, pts);
    }
    update();
  };
  connect(&ed, &Editor::chromaPickChanged, this, pickChanged);
  connect(&ed, &Editor::maskPickChanged, this, pickChanged);
  connect(&mask::maskController(), &mask::MaskController::hoverChanged, this, qOverload<>(&QWidget::update));
  connect(&mask::maskController(), &mask::MaskController::preparingChanged, this, qOverload<>(&QWidget::update));
  connect(&mask::maskController(), &mask::MaskController::trackingChanged, this, qOverload<>(&QWidget::update));
  connect(&ed, &Editor::timeChanged, this, [this] {
    if (!isEditingText()) return;
    const TextItem* t = editor().text(editId_);
    if (!t || !isTextVisible(*t, editor().currentTime())) endTextEdit(true);
  });
  blink_ = new QTimer(this);
  blink_->setInterval(530);
  connect(blink_, &QTimer::timeout, this, [this] {
    blinkOn_ = !blinkOn_;
    update();
  });

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
  empty_->setVisible(projectIsEmpty(ed.project()));
}

void PreviewStage::setFullScreenMode(bool on) {
  if (fullScreen_ == on) return;
  if (on) endTextEdit(true);  // 全画面では、編集用の表示も操作も出さない
  if (on) stopPicking();
  fullScreen_ = on;
  // 全画面では、編集用の操作（枠・ハンドル・ガイド・ドロップ・案内）はすべて止める。映像の描画は通常のままで、同じ処理を使う
  grab_ = Grab::None;
  guideV_ = guideH_ = false;
  dropHover_ = false;
  setAcceptDrops(!on);
  unsetCursor();
  const bool isEmpty = projectIsEmpty(editor().project());
  empty_->setVisible(isEmpty && !on);
  updateCanvasSize();
  update();
}

QRectF PreviewStage::stageRect() const {
  const double ratio = aspectValue(editor().project().settings.aspect);
  // 全画面では余白なし。映像は比率を保って画面に収め、余った所は黒
  const double margin = fullScreen_ ? 0 : 12;
  const QRectF area = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
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
  // 高解像度の画面でも重くならないよう、長い辺は 1600px までにする（全画面は、書き出しに近い見た目にするため 2560px まで）
  const qreal dpr = devicePixelRatioF();
  double w = r.width() * dpr;
  double h = r.height() * dpr;
  const double k = std::min(1.0, (fullScreen_ ? 2560.0 : 1600.0) / std::max({w, h, 1.0}));
  engine_->setCanvasSize(QSize(std::max(16, int(w * k)), std::max(16, int(h * k))));
  empty_->setGeometry(r.toRect());
}

void PreviewStage::resizeEvent(QResizeEvent*) {
  updateCanvasSize();
}

void PreviewStage::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.fillRect(rect(), fullScreen_ ? QColor(Qt::black) : QColor(0x0d, 0x0d, 0x0d));
  const QRectF r = stageRect();
  const QImage& frame = engine_->frame();
  if (!frame.isNull()) {
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(r, frame);
  } else {
    p.fillRect(r, Qt::black);
  }
  if (fullScreen_) return;  // 全画面では、選択枠・ハンドル・ガイド線などの編集用の表示は出さない
  const Editor& ed = editor();
  if (empty_->isVisible()) p.fillRect(r, QColor(0x16, 0x16, 0x16));
  // ガイド（見るためだけ。書き出す動画には出ない）: 三分割のグリッドと、セーフマージン（アクション 90%・タイトル 80%）
  if (!empty_->isVisible() && (ed.gridVisible() || ed.safeMarginsVisible())) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setBrush(Qt::NoBrush);
    if (ed.gridVisible()) {
      p.setPen(QPen(QColor(255, 255, 255, 95), 1, Qt::DashLine));
      for (int i = 1; i <= 2; ++i) {
        const double x = std::round(r.left() + r.width() * i / 3.0) + 0.5;
        const double y = std::round(r.top() + r.height() * i / 3.0) + 0.5;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
      }
    }
    if (ed.safeMarginsVisible()) {
      p.setPen(QPen(QColor(255, 255, 255, 140), 1));
      p.drawRect(r.adjusted(r.width() * 0.05, r.height() * 0.05, -r.width() * 0.05, -r.height() * 0.05));
      p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::DashLine));
      p.drawRect(r.adjusted(r.width() * 0.1, r.height() * 0.1, -r.width() * 0.1, -r.height() * 0.1));
      // 画面の真ん中の十字
      const QPointF m = r.center();
      p.setPen(QPen(QColor(255, 255, 255, 140), 1));
      p.drawLine(m - QPointF(7, 0), m + QPointF(7, 0));
      p.drawLine(m - QPointF(0, 7), m + QPointF(0, 7));
    }
    p.restore();
  }
  // 選んでいる映像・テキストの操作用の枠（ドラッグで移動、四隅で拡大・縮小、上の丸で回転）
  QPolygonF quad;
  if (isEditingText()) {
    // 文字を直接編集している間は、移動・拡大・回転のハンドルは出さず、編集用の枠・選択範囲・入力位置を出す
    paintTextEdit(p);
  } else if (!ed.playing() && selectedQuad(&quad)) {
    QPolygonF w;
    for (const QPointF& q : quad) w << toWidget(q);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 130), 3));
    p.drawPolygon(w);
    p.setPen(QPen(QColor(255, 255, 255, 230), 1));
    p.drawPolygon(w);
    // スポイトの間は、どの映像から色を選ぶかがわかる枠だけを出す（動かす・拡大するためのつまみは出さない）
    if (!picking()) {
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
      // 映像は、辺の真ん中の丸（幅・高さだけを変える）
      if (selectionStretchable()) {
        for (int i = 0; i < 4; ++i) {
          const QPointF m = (w[i] + w[(i + 1) % 4]) / 2;
          p.drawEllipse(m, 4.2, 4.2);
        }
      }
    }
  }
  if (maskPicking()) {
    paintMaskPick(p, r);
  } else if (picking()) {
    // スポイトの案内（映像の上の端）
    const QString msg = QStringLiteral("スポイト: 透明にしたい色（背景など）をクリックしてください　Esc・右クリックでやめる");
    const QFontMetricsF fm(p.font());
    const double bw = std::min(r.width() - 16, fm.horizontalAdvance(msg) + 28);
    const QRectF box(r.center().x() - bw / 2, r.top() + 10, bw, fm.height() + 14);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 200));
    p.drawRoundedRect(box, 7, 7);
    p.setPen(QColor(255, 255, 255));
    p.drawText(box.adjusted(10, 0, -10, 0), Qt::AlignCenter, fm.elidedText(msg, Qt::ElideRight, bw - 20));
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
  const LayerState ls = layerStateAt(item, t - item.start);
  TextItem centered = item;
  centered.x = 0.5;
  centered.y = 0.5;
  centered.letterSpacing = ls.letterSpacing;  // 字間が変わると文字の幅も変わるので、枠も今の字間で求める
  const QRectF b = layoutText(centered, d.width(), d.height()).bounds.adjusted(-pad, -pad, pad, pad);
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

/** 素材の見た目の大きさ（比率だけを使う）。わからなければ 16:9 */
static QSizeF mediaSizeOf(const VideoClip& c) {
  if (const Asset* a = editor().asset(c.assetId); a && a->width > 0 && a->height > 0) return QSizeF(a->width, a->height);
  const QImage img = c.isImage ? media().previewImage(c.assetId) : QImage();
  if (!img.isNull()) return img.size();
  return QSizeF(16, 9);
}

bool PreviewStage::overlayQuad(const OverlayItem& o, double t, QPolygonF* quad) const {
  const Editor& ed = editor();
  const int fps = ed.project().settings.fps;
  // 非表示の段のものは、映っていないので枠も出さない（プレビューで押しても選ばない）
  if (!isOverlayVisible(o, t, fps) || isOverlayHidden(ed.project(), o)) return false;
  const QSize d = designSize(ed.project().settings.aspect);
  const double local = std::clamp(t - o.start, 0.0, overlayDuration(o, fps));
  if (o.isEffectLayer()) {
    // エフェクトレイヤー: 効果を掛ける範囲（画面いっぱいの形を、位置・拡大率・伸縮・回転で動かしたもの。合成と同じ）。
    // 「画面全体」は動かしても範囲が変わらないので、枠を出さない
    if (o.region.shape == RegionShape::Full) return false;
    *quad = clipQuad(o, local, d);
    return quad->size() == 4;
  }
  *quad = clipMediaQuad(mediaSizeOf(o), cropSourceRect(o, QSizeF(1, 1)), layerStateAt(o, local), d);
  return quad->size() == 4;
}

QString PreviewStage::hitOverlay(QPointF pos) const {
  const Editor& ed = editor();
  const QPointF dp = toDesign(pos);
  const double t = ed.currentTime();
  // 前に重なっているもの（上の段・あとから始まるもの）から探す
  std::vector<const OverlayItem*> order;
  for (const auto& o : ed.project().overlays) order.push_back(&o);
  std::stable_sort(order.begin(), order.end(), [](const OverlayItem* a, const OverlayItem* b) {
    if (a->track != b->track) return a->track > b->track;
    return a->start > b->start;
  });
  for (const OverlayItem* o : order) {
    QPolygonF quad;
    if (overlayQuad(*o, t, &quad) && quad.containsPoint(dp, Qt::WindingFill)) return o->id;
  }
  return {};
}

bool PreviewStage::selectionStretchable() const {
  const Selection::Type t = editor().selection().type;
  return t == Selection::Clip || t == Selection::Overlay;
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
    if (isTextHidden(ed.project(), *item) && !(isEditingText() && s.id == editId_)) return false;  // 非表示の段の文字
    *quad = textQuad(isEditingText() && s.id == editId_ ? editedItem() : *item, t, d, s.id, 4 / k);
    return true;
  }
  if (s.type == Selection::Clip) {
    if (ed.project().mainVideoHidden) return false;  // メインのビデオを非表示にしている
    const ClipLayout* cur = findClipAt(ed.layout(), t);
    if (!cur || cur->clip.id != s.id) return false;
    *quad = clipQuad(cur->clip, std::clamp(t - cur->start, 0.0, cur->duration), d);
    return true;
  }
  if (s.type == Selection::Overlay) {
    const OverlayItem* o = ed.overlay(s.id);
    return o && overlayQuad(*o, t, quad);
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
  // 映像の枠の辺の真ん中: 幅（左右の辺）・高さ（上下の辺）だけを変える
  if (selectionStretchable()) {
    for (int i = 0; i < 4; ++i) {
      const QPointF m = (toWidget(quad[i]) + toWidget(quad[(i + 1) % 4])) / 2;
      if (std::abs(pos.x() - m.x()) <= kHandlePx && std::abs(pos.y() - m.y()) <= kHandlePx) return i % 2 == 0 ? Grab::StretchY : Grab::StretchX;
    }
  }
  QPolygonF poly;
  for (const QPointF& c : quad) poly << toWidget(c);
  return poly.containsPoint(pos, Qt::WindingFill) ? Grab::Move : Grab::None;
}

bool PreviewStage::picking() const {
  return !fullScreen_ && (!editor().chromaPick().isEmpty() || !editor().maskPick().isEmpty());
}

bool PreviewStage::maskPicking() const {
  return !fullScreen_ && !editor().maskPick().isEmpty();
}

void PreviewStage::stopPicking() {
  editor().setChromaPick({});
  editor().setMaskPick({});
}

QString PreviewStage::hitText(QPointF pos) const {
  const Editor& ed = editor();
  const QSize d = designSize(ed.project().settings.aspect);
  const QPointF dp = toDesign(pos);
  const double t = ed.currentTime();
  const QString editing = ed.selection().type == Selection::Text ? ed.selection().id : QString();
  const auto& texts = ed.project().texts;
  // 手前に描いている文字（タイムラインの上の段）から探す。描く順の逆
  const std::vector<size_t> order = textPaintOrder(texts);
  for (auto it = order.rbegin(); it != order.rend(); ++it) {
    const TextItem& item = texts[*it];
    if (!isTextVisible(item, t) || item.text.isEmpty() || isTextHidden(ed.project(), item)) continue;  // 非表示の段の文字は押せない
    if (textQuad(item, t, d, editing, 0).containsPoint(dp, Qt::WindingFill)) return item.id;
  }
  return {};
}

void PreviewStage::mousePressEvent(QMouseEvent* e) {
  if (fullScreen_) {
    emit activity();  // 全画面では編集しない（選択も変えない）。マウスが動いたことだけ知らせる
    return;
  }
  if (e->button() != Qt::LeftButton) return;
  if (maskPicking()) {
    maskClickAt(e->position(), e->modifiers());  // 被写体を選ぶ（選択や移動はしない）
    return;
  }
  if (picking()) {
    pickChromaAt(e->position());  // スポイト: クリックした所の色を、透明にする色にする（選択や移動はしない）
    return;
  }
  if (isEditingText()) {
    // 文字の編集中: 文字の上のクリック・ドラッグは入力位置と範囲選択（オブジェクトは動かさない）。外側のクリックは編集を確定する
    if (insideEditedText(e->position())) {
      commitPreedit();
      bool up = false;
      const int idx = editIndexAt(e->position(), &up);
      moveCaret(idx, e->modifiers() & Qt::ShiftModifier, up);
      grab_ = Grab::Select;
      return;
    }
    endTextEdit(true);  // そのまま、ふつうのクリック（ほかのものを選ぶなど）として扱う
  }
  Editor& ed = editor();
  grab_ = Grab::None;
  // 選んでいるものの枠を掴んだとき: 拡大・縮小 / 回転 / 移動
  QPolygonF quad;
  Grab g = Grab::None;
  if (selectedQuad(&quad)) {
    g = grabAt(e->position(), quad);
    // ハンドル、または選んでいる文字の枠の中: そのまま操作する
    if (g == Grab::Rotate || g == Grab::Scale || g == Grab::StretchX || g == Grab::StretchY ||
        (g == Grab::Move && ed.selection().type == Selection::Text)) {
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
  // 選んでいる重ねるトラックの映像の枠の中: そのまま動かす（下に別の映像があっても、選んでいるものを優先）
  if (g == Grab::Move && ed.selection().type == Selection::Overlay) {
    beginGrab(g, e->position());
    return;
  }
  // 重ねるトラックの映像をクリック: 選んで動かせるようにする（メインの映像より前にあるので、こちらが優先）
  if (const QString ov = hitOverlay(e->position()); !ov.isEmpty()) {
    ed.pause();
    ed.select({Selection::Overlay, ov});
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
  if (ed.selection().type == Selection::Text || ed.selection().type == Selection::Clip || ed.selection().type == Selection::Overlay) ed.select({});
}

/** 中心から見た pos の、回転を戻した向きでの位置（幅の向き = x、高さの向き = y） */
static QPointF unrotated(QPointF pos, QPointF center, double rotationDeg) {
  const double a = -rotationDeg * M_PI / 180.0;
  const QPointF v = pos - center;
  return {v.x() * std::cos(a) - v.y() * std::sin(a), v.x() * std::sin(a) + v.y() * std::cos(a)};
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
  if (g == Grab::StretchX || g == Grab::StretchY) {
    const VideoClip* c = ed.clip(ed.selection().id);
    origStretchX_ = c ? c->stretchX : 1;
    origStretchY_ = c ? c->stretchY : 1;
    const QPointF u = unrotated(pos, center_, origState_.rotation);
    stretchStart_ = std::max(1.0, std::abs(g == Grab::StretchX ? u.x() : u.y()));
  }
}

void PreviewStage::mouseMoveEvent(QMouseEvent* e) {
  if (fullScreen_) {
    emit activity();  // 再生コントロールを出す合図
    return;
  }
  if (picking()) {
    setCursor(Qt::CrossCursor);
    if (maskPicking()) maskHoverAt(e->position(), e->modifiers());
    return;
  }
  if (isEditingText()) {
    if (grab_ == Grab::Select && (e->buttons() & Qt::LeftButton)) {
      bool up = false;
      const int idx = editIndexAt(e->position(), &up);
      if (idx != caret_ || up != upstream_) moveCaret(idx, true, up);  // ドラッグで範囲を選ぶ
      return;
    }
    setCursor(insideEditedText(e->position()) ? Qt::IBeamCursor : Qt::ArrowCursor);
    return;
  }
  Editor& ed = editor();
  if (grab_ == Grab::None) {
    // 枠の上では、できる操作に合わせたカーソルにする
    QPolygonF quad;
    Qt::CursorShape shape = Qt::ArrowCursor;
    if (!ed.playing() && selectedQuad(&quad)) {
      switch (grabAt(e->position(), quad)) {
        case Grab::Rotate: shape = Qt::CrossCursor; break;
        case Grab::Scale: shape = Qt::SizeFDiagCursor; break;
        case Grab::StretchX: shape = Qt::SizeHorCursor; break;
        case Grab::StretchY: shape = Qt::SizeVerCursor; break;
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
  } else if (grab_ == Grab::StretchX || grab_ == Grab::StretchY) {
    // 辺のつまみ: 中心からの（回転を戻した向きでの）距離の比で、幅・高さだけを変える（中心は動かさない）
    const QPointF u = unrotated(e->position(), center_, origState_.rotation);
    const double now = std::max(1.0, std::abs(grab_ == Grab::StretchX ? u.x() : u.y()));
    double v = std::clamp((grab_ == Grab::StretchX ? origStretchX_ : origStretchY_) * now / stretchStart_, kMinStretch, kMaxStretch);
    v = std::round(v * 1000) / 1000;
    const bool x = grab_ == Grab::StretchX;
    ed.updateClip(ed.selection().id, [&](VideoClip& c) {
      if (x) c.stretchX = v;
      else c.stretchY = v;
    }, dragMerge_);
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
  if (fullScreen_ || picking()) return;
  if (isEditingText()) {
    // 文字の編集中は、変更の記録（元に戻す履歴のまとまり）を区切らない
    if (grab_ == Grab::Select) grab_ = Grab::None;
    return;
  }
  grab_ = Grab::None;
  guideV_ = guideH_ = false;
  editor().endMerge();
  update();
}

void PreviewStage::mouseDoubleClickEvent(QMouseEvent* e) {
  if (fullScreen_) {
    emit fullScreenToggleRequested();  // 全画面のダブルクリックで、全画面を終える
    return;
  }
  if (e->button() != Qt::LeftButton || picking()) return;
  if (isEditingText()) {
    // 編集中のダブルクリック: 文字の上なら単語を選ぶ。外側なら編集を確定する
    if (insideEditedText(e->position())) {
      commitPreedit();
      bool up = false;
      selectWordAt(editIndexAt(e->position(), &up));
      grab_ = Grab::None;
    } else {
      endTextEdit(true);
    }
    return;
  }
  // 文字をダブルクリックしたら、その場で文字を編集する（押した位置に入力位置を置く）
  const QString id = hitText(e->position());
  if (id.isEmpty()) return;
  Editor& ed = editor();
  ed.pause();
  if (!(ed.selection().type == Selection::Text && ed.selection().id == id)) ed.select({Selection::Text, id});
  beginTextEdit(id, 0);
  if (!isEditingText()) return;
  bool up = false;
  const int idx = editIndexAt(e->position(), &up);
  moveCaret(idx, false, up);
}

void PreviewStage::contextMenuEvent(QContextMenuEvent* e) {
  // スポイト・被写体を選んでいる間の右クリックは、やめる
  if (picking()) {
    stopPicking();
    return;
  }
  // 全画面と、文字を直接編集している間は出さない（編集中の右クリックは、編集の操作と紛らわしい）
  if (fullScreen_ || isEditingText() || grab_ != Grab::None) return;
  Editor& ed = editor();
  const QPointF pos = e->pos();
  // 文字が優先（左クリックで選ぶときと同じ）。無ければ、今の再生位置の映像（その枠の中のとき）
  Selection sel;
  if (const QString id = hitText(pos); !id.isEmpty()) {
    sel = {Selection::Text, id};
  } else if (const QString ov = hitOverlay(pos); !ov.isEmpty()) {
    sel = {Selection::Overlay, ov};
  } else if (const ClipLayout* cur = findClipAt(ed.layout(), ed.currentTime())) {
    const QSize d = designSize(ed.project().settings.aspect);
    const QPolygonF quad = clipQuad(cur->clip, std::clamp(ed.currentTime() - cur->start, 0.0, cur->duration), d);
    QPolygonF onScreen;
    for (const QPointF& c : quad) onScreen << toWidget(c);
    if (onScreen.containsPoint(pos, Qt::WindingFill)) sel = {Selection::Clip, cur->clip.id};
  }
  if (!sel) return;
  const QString id = sel.id;
  execObjectMenu(this, sel, e->globalPos(), ObjectMenuPlace::Preview, [this, id] {
    // 文字の終わりに入力位置を置いて、その場で編集する
    const TextItem* t = editor().text(id);
    if (!t) return;
    beginTextEdit(id, int(t->text.size()));
  });
  update();
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

// ---- 文字の直接編集 -----------------------------------------------------------------------------
// 文字の内容は、1 文字入力するごとに編集データ（Editor）へ反映する。右の設定欄・タイムラインなどは、これまでどおり
// 編集データを見て更新される（同じデータを使うので、どちらから編集しても同じ）。元に戻す履歴は、同じ merge の間は 1 回分。
// 日本語入力の変換中の文字だけは、確定するまで編集データに入れず、プレビューの描画（PreviewEngine の上書き）にだけ出す。

namespace {

/** 入力・貼り付けの文字を整える。改行は \n にそろえ、改行以外の制御文字は捨てる */
QString sanitizeInput(QString s) {
  s.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  s.replace(QChar(0x2028), QLatin1Char('\n'));
  s.replace(QChar(0x2029), QLatin1Char('\n'));
  QString out;
  out.reserve(s.size());
  for (QChar c : s) {
    if (c == QLatin1Char('\n') || (c.unicode() >= 0x20 && c.unicode() != 0x7f && !(c.unicode() >= 0x80 && c.unicode() < 0xa0))) out += c;
  }
  return out;
}

/** 文字の境目（1 文字 = 結合文字や絵文字を含むひとまとまり）、または単語の境目へ動く */
int boundaryStep(const QString& s, int pos, bool forward, QTextBoundaryFinder::BoundaryType type) {
  QTextBoundaryFinder f(type, s);
  f.setPosition(std::clamp(pos, 0, int(s.size())));
  const int r = forward ? f.toNextBoundary() : f.toPreviousBoundary();
  return r < 0 ? (forward ? int(s.size()) : 0) : r;
}

/** 次 / 前の単語の端。空白だけのところは飛ばす */
int wordStep(const QString& s, int pos, bool forward) {
  int p = pos;
  for (int i = 0; i < 64; ++i) {
    const int n = boundaryStep(s, p, forward, QTextBoundaryFinder::Word);
    const QString seg = forward ? s.mid(p, n - p) : s.mid(n, p - n);
    p = n;
    if (n <= 0 || n >= s.size() || !seg.trimmed().isEmpty()) break;
  }
  return p;
}

}  // namespace

QString PreviewStage::committedText() const {
  const TextItem* t = editor().text(editId_);
  return t ? t->text : QString();
}

TextItem PreviewStage::editedItem() const {
  const TextItem* t = editor().text(editId_);
  if (!t) return {};
  TextItem it = *t;
  // 変換中の文字は、入力位置に差し込んだ形で見せる（一部だけ別のフォントにした範囲も、差し込んだ分だけずらす）
  if (!preedit_.isEmpty()) {
    it.text = t->text.left(caret_) + preedit_ + t->text.mid(caret_);
    adjustFontSpans(t->text, it.text, it.fontSpans);
  }
  return it;
}

bool PreviewStage::editGeometry(TextItem* item, TextLayout* layout, QTransform* toWidget) const {
  if (!isEditingText()) return false;
  const Editor& ed = editor();
  const TextItem* base = ed.text(editId_);
  const double t = ed.currentTime();
  if (!base || !isTextVisible(*base, t)) return false;
  const QSize d = designSize(ed.project().settings.aspect);
  const TextItem shown = editedItem();
  // 描画・枠と同じ配置: 画面の中央に置いたときの配置を、位置・拡大率・回転の変換で動かす
  const LayerState ls = layerStateAt(shown, t - shown.start);
  TextItem centered = shown;
  centered.x = 0.5;
  centered.y = 0.5;
  centered.letterSpacing = ls.letterSpacing;
  if (item) *item = centered;
  if (layout) *layout = layoutText(centered, d.width(), d.height());
  if (toWidget) {
    const QRectF r = stageRect();
    const double k = r.width() / d.width();
    QTransform w;
    w.translate(r.x(), r.y());
    w.scale(k, k);
    *toWidget = layerMatrix(ls, d) * w;
  }
  return true;
}

int PreviewStage::editIndexAt(QPointF pos, bool* upstream) const {
  if (upstream) *upstream = false;
  TextItem item;
  TextLayout L;
  QTransform T;
  if (!editGeometry(&item, &L, &T)) return caret_;
  bool ok = false;
  const QTransform inv = T.inverted(&ok);
  if (!ok) return caret_;
  return textIndexAt(item, L, inv.map(pos), upstream);
}

bool PreviewStage::insideEditedText(QPointF pos) const {
  TextItem item;
  TextLayout L;
  QTransform T;
  if (!editGeometry(&item, &L, &T)) return false;
  const QSize d = designSize(editor().project().settings.aspect);
  const double k = stageRect().width() / d.width();
  const QRectF b = L.bounds.adjusted(-8 / k, -8 / k, 8 / k, 8 / k);
  QPolygonF poly;
  poly << b.topLeft() << b.topRight() << b.bottomRight() << b.bottomLeft();
  return T.map(poly).containsPoint(pos, Qt::WindingFill);
}

void PreviewStage::beginTextEdit(const QString& id, int caret) {
  Editor& ed = editor();
  const TextItem* t = ed.text(id);
  if (!t || fullScreen_) return;
  if (isEditingText()) endTextEdit(true);
  ed.pause();
  if (!(ed.selection().type == Selection::Text && ed.selection().id == id)) ed.select({Selection::Text, id});
  ed.beginEditSession();
  editId_ = id;
  editMerge_ = QStringLiteral("pvtext-") + id;
  editOriginal_ = t->text;
  caret_ = anchor_ = std::clamp(caret, 0, int(t->text.size()));
  upstream_ = false;
  wantX_ = -1;
  preedit_.clear();
  preeditAttrs_.clear();
  preeditCursor_ = 0;
  undo_.clear();
  redo_.clear();
  lastKind_ = EditKind::None;
  grab_ = Grab::None;
  guideV_ = guideH_ = false;
  // 日本語入力（IME）を使えるようにして、キーボードの入力を受ける
  setAttribute(Qt::WA_InputMethodEnabled, true);
  setFocus(Qt::MouseFocusReason);
  QGuiApplication::inputMethod()->update(Qt::ImEnabled | Qt::ImHints);
  setCursor(Qt::IBeamCursor);
  blinkReset();
  syncEngine();
  updateInputMethod();
  update();
}

void PreviewStage::endTextEdit(bool commit) {
  if (!isEditingText()) return;
  Editor& ed = editor();
  // 変換中の文字は、確定するときは文字として入れ、キャンセルのときは捨てる
  if (commit) commitPreedit();
  preedit_.clear();
  preeditAttrs_.clear();
  QGuiApplication::inputMethod()->reset();
  const TextItem* t = ed.text(editId_);
  const bool changed = t && t->text != editOriginal_;
  editId_.clear();
  blink_->stop();
  grab_ = Grab::None;
  undo_.clear();
  redo_.clear();
  setAttribute(Qt::WA_InputMethodEnabled, false);
  QGuiApplication::inputMethod()->update(Qt::ImEnabled);
  engine_->setTextOverride(QString(), QString());
  if (!t) ed.endEditSession();                      // 文字が無くなっていたら、戻すものは無い
  else if (commit && changed) ed.endEditSession();  // 確定: 始めてからの変更が、1 つの編集操作として残る
  else ed.cancelEditSession();                      // キャンセル（または、元の内容に戻っていた）: 履歴に残さない
  unsetCursor();
  engine_->refreshNow();
  update();
}

void PreviewStage::blinkReset() {
  blinkOn_ = true;
  if (blink_ && isEditingText()) blink_->start();
}

void PreviewStage::syncEngine() {
  if (!isEditingText()) return;
  engine_->setTextOverride(editId_, editedItem().text);
  engine_->refreshNow();
}

void PreviewStage::updateInputMethod() {
  if (!isEditingText() || !hasFocus()) return;
  QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle | Qt::ImCursorPosition | Qt::ImAnchorPosition | Qt::ImSurroundingText |
                                         Qt::ImCurrentSelection | Qt::ImTextBeforeCursor | Qt::ImTextAfterCursor | Qt::ImFont);
}

void PreviewStage::afterEdit() {
  blinkReset();
  syncEngine();
  updateInputMethod();
  update();
}

void PreviewStage::pushUndo(EditKind kind, bool hadSelection) {
  const bool coalesce = kind == lastKind_ && (kind == EditKind::Type || kind == EditKind::Backspace || kind == EditKind::Delete) && !hadSelection && !undo_.empty();
  if (!coalesce) {
    undo_.push_back({committedText(), caret_, anchor_});
    if (undo_.size() > 200) undo_.erase(undo_.begin());
  }
  redo_.clear();
  lastKind_ = kind;
}

void PreviewStage::applyEdit(int from, int to, const QString& with, EditKind kind) {
  if (!isEditingText()) return;
  const QString cur = committedText();
  from = std::clamp(from, 0, int(cur.size()));
  to = std::clamp(to, from, int(cur.size()));
  if (from == to && with.isEmpty()) return;
  pushUndo(kind, from != to);
  const QString next = cur.left(from) + with + cur.mid(to);
  applying_ = true;
  editor().updateText(editId_, [&](TextItem& t) { t.text = next; }, editMerge_);
  applying_ = false;
  caret_ = anchor_ = from + int(with.size());
  upstream_ = false;
  wantX_ = -1;
  afterEdit();
}

void PreviewStage::restoreEdit(const EditState& s) {
  applying_ = true;
  editor().updateText(editId_, [&](TextItem& t) { t.text = s.text; }, editMerge_);
  applying_ = false;
  const int len = int(committedText().size());
  caret_ = std::clamp(s.caret, 0, len);
  anchor_ = std::clamp(s.anchor, 0, len);
  upstream_ = false;
  wantX_ = -1;
  lastKind_ = EditKind::None;
  afterEdit();
}

void PreviewStage::localUndo(bool redo) {
  if (!preedit_.isEmpty()) return;
  auto& from = redo ? redo_ : undo_;
  auto& to = redo ? undo_ : redo_;
  if (from.empty()) return;
  to.push_back({committedText(), caret_, anchor_});
  const EditState s = from.back();
  from.pop_back();
  restoreEdit(s);
}

void PreviewStage::moveCaret(int index, bool extend, bool upstream, bool keepWantX) {
  caret_ = std::clamp(index, 0, int(committedText().size()));
  if (!extend) anchor_ = caret_;
  upstream_ = upstream;
  if (!keepWantX) wantX_ = -1;
  lastKind_ = EditKind::None;
  blinkReset();
  updateInputMethod();
  update();
}

void PreviewStage::insertText(const QString& text, EditKind kind) {
  const QString s = sanitizeInput(text);
  if (s.isEmpty()) return;
  if (hasSelection()) applyEdit(std::min(caret_, anchor_), std::max(caret_, anchor_), s, kind);
  else applyEdit(caret_, caret_, s, kind);
}

void PreviewStage::commitPreedit() {
  if (preedit_.isEmpty()) return;
  const QString s = preedit_;
  preedit_.clear();
  preeditAttrs_.clear();
  QGuiApplication::inputMethod()->reset();
  insertText(s, EditKind::Type);
}

void PreviewStage::copySelection(bool cut) {
  if (!hasSelection()) return;
  const int a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
  QGuiApplication::clipboard()->setText(committedText().mid(a, b - a));
  if (cut) applyEdit(a, b, QString(), EditKind::Other);
}

void PreviewStage::pasteClipboard() {
  const QString s = sanitizeInput(QGuiApplication::clipboard()->text());
  if (!s.isEmpty()) insertText(s, EditKind::Other);
}

void PreviewStage::selectWordAt(int index) {
  const QString s = committedText();
  if (s.isEmpty()) return;
  int i = std::clamp(index, 0, int(s.size()));
  if (i >= s.size()) i = int(s.size()) - 1;
  QTextBoundaryFinder f(QTextBoundaryFinder::Word, s);
  f.setPosition(i);
  const int start = f.isAtBoundary() ? i : f.toPreviousBoundary();
  int end = f.toNextBoundary();
  if (end < 0) end = int(s.size());
  anchor_ = std::max(0, start);
  caret_ = end;
  upstream_ = false;
  wantX_ = -1;
  lastKind_ = EditKind::None;
  blinkReset();
  updateInputMethod();
  update();
}

bool PreviewStage::editsKey(const QKeyEvent* e) const {
  const Qt::KeyboardModifiers m = e->modifiers();
  const bool ctrl = m & Qt::ControlModifier;
  const bool alt = m & Qt::AltModifier;
  switch (e->key()) {
    case Qt::Key_Escape:
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Backspace:
    case Qt::Key_Delete:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End:
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return !alt || ctrl;
    case Qt::Key_A:
    case Qt::Key_C:
    case Qt::Key_X:
    case Qt::Key_V:
    case Qt::Key_Z:
    case Qt::Key_Y: return ctrl && !alt;
    default: break;
  }
  // 文字キー（Alt だけを押しているときはメニューの操作なので、渡す）
  const QString t = e->text();
  return !t.isEmpty() && t.at(0).isPrint() && !(m & Qt::MetaModifier) && !(ctrl && !alt) && (!alt || ctrl);
}

bool PreviewStage::event(QEvent* e) {
  // スポイトの Esc（やめる）は、メニューなどのショートカットより先に受け取る
  if (picking() && e->type() == QEvent::ShortcutOverride && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape) {
    e->accept();
    return true;
  }
  // 編集中に使うキーは、メニューなどのショートカット（Ctrl+Z の「元に戻す」、Delete の「削除」など）より先に受け取る
  if (isEditingText() && e->type() == QEvent::ShortcutOverride) {
    if (editsKey(static_cast<QKeyEvent*>(e))) {
      e->accept();
      return true;
    }
  }
  return QWidget::event(e);
}

bool PreviewStage::focusNextPrevChild(bool next) {
  if (isEditingText()) return false;
  return QWidget::focusNextPrevChild(next);
}

void PreviewStage::focusOutEvent(QFocusEvent* e) {
  QWidget::focusOutEvent(e);
  // ほかの部品を選んだら編集を確定する（ウィンドウの切り替えや日本語入力の候補の表示では、編集を続ける）
  if (isEditingText() && e->reason() != Qt::ActiveWindowFocusReason && e->reason() != Qt::PopupFocusReason) endTextEdit(true);
}

void PreviewStage::keyPressEvent(QKeyEvent* e) {
  if (picking() && e->key() == Qt::Key_Escape) {
    stopPicking();  // スポイト・被写体を選ぶのをやめる
    e->accept();
    return;
  }
  if (maskPicking() && (e->key() == Qt::Key_Shift || e->key() == Qt::Key_Alt || e->key() == Qt::Key_Control)) {
    // 足す・外すの案内を出し直す（カーソルを動かさなくても）
    maskHoverAt(maskCursor_, e->modifiers());
    e->accept();
    return;
  }
  if (!isEditingText()) {
    QWidget::keyPressEvent(e);
    return;
  }
  handleEditKey(e);
}

void PreviewStage::pickChromaAt(QPointF pos) {
  Editor& ed = editor();
  const QString id = ed.chromaPick();
  const QRectF r = stageRect();
  if (id.isEmpty() || r.isEmpty() || !r.contains(pos)) return;
  const QPointF at((pos.x() - r.x()) / r.width(), (pos.y() - r.y()) / r.height());
  const std::optional<QColor> col = engine_->sampleSource(id, at);
  if (!col) {
    ed.toast(QStringLiteral("色を選ぶ映像の上をクリックしてください（今の再生位置に出ている所）"));
    return;
  }
  ed.updateClip(id, [&](VideoClip& c) {
    c.chroma.color = *col;
    c.chroma.enabled = true;
  });
  ed.setChromaPick({});
}

// ---- オブジェクトマスクの被写体を選ぶ ---------------------------------------------------------------
// 素材のフレーム（0〜1 の割合）→ 反転 → 表示方法で収めた四角 → 位置・拡大率・回転 → ウィジェット。合成（composeClipLayer・drawClipMedia）と同じ並び

bool PreviewStage::sourceToWidget(const QString& clipId, QTransform* m, QPainterPath* visible) const {
  const Editor& ed = editor();
  const double t = ed.currentTime();
  const int fps = ed.project().settings.fps;
  const QSize d = designSize(ed.project().settings.aspect);
  const QRectF r = stageRect();
  if (r.isEmpty()) return false;
  const VideoClip* c = nullptr;
  LayerState ls;
  bool main = false;
  if (const OverlayItem* o = ed.overlay(clipId)) {
    if (o->isEffectLayer() || !isOverlayVisible(*o, t, fps) || isOverlayHidden(ed.project(), *o)) return false;
    c = o;
    ls = layerStateAt(*o, std::clamp(t - o->start, 0.0, overlayDuration(*o, fps)));
  } else {
    if (ed.project().mainVideoHidden) return false;
    const ClipLayout* cur = findClipAt(ed.layout(), t);
    if (!cur || cur->clip.id != clipId) return false;
    c = &cur->clip;
    ls = layerStateAt(cur->clip, std::clamp(t - cur->start, 0.0, cur->duration));
    main = true;
  }
  const QSizeF src = mediaSizeOf(*c);
  // メインの「拡大」は画面いっぱいに広げた四角、ほか（全体表示・ぼかし背景の手前・重ねるトラック）は収めた四角
  const QRectF fitted = main && c->fit == FitMode::Cover ? coverRect(src, QSizeF(d)) : containRect(src, QSizeF(d));
  if (fitted.isEmpty()) return false;
  const QTransform flip = QTransform::fromScale(c->flipX ? -1 : 1, c->flipY ? -1 : 1) * QTransform::fromTranslate(c->flipX ? 1 : 0, c->flipY ? 1 : 0);
  const QTransform fit = QTransform::fromScale(fitted.width(), fitted.height()) * QTransform::fromTranslate(fitted.x(), fitted.y());
  const QTransform layer = layerMatrix(ls, QSizeF(d));
  const double k = r.width() / d.width();
  const QTransform widget = QTransform::fromScale(k, k) * QTransform::fromTranslate(r.x(), r.y());
  *m = flip * fit * layer * widget;
  if (visible) {
    // 切り抜いて残した部分（反転したあとの向き）。メインの映像は画面いっぱいの 1 枚にしてから動かすので、その外は見えない
    QPainterPath path;
    path.addPolygon((fit * layer * widget).map(QPolygonF(cropSourceRect(*c, QSizeF(1, 1)))));
    if (main) {
      QPainterPath canvas;
      canvas.addPolygon((layer * widget).map(QPolygonF(QRectF(QPointF(0, 0), QSizeF(d)))));
      path = path.intersected(canvas);
    }
    QPainterPath stage;
    stage.addRect(r);
    *visible = path.intersected(stage);
  }
  return true;
}

bool PreviewStage::widgetToSource(const QString& clipId, QPointF pos, QPointF* src) const {
  QTransform m;
  QPainterPath visible;
  if (!sourceToWidget(clipId, &m, &visible) || !visible.contains(pos)) return false;
  bool invertible = false;
  const QTransform inv = m.inverted(&invertible);
  if (!invertible) return false;
  const QPointF u = inv.map(pos);
  if (u.x() < 0 || u.y() < 0 || u.x() > 1 || u.y() > 1) return false;
  *src = u;
  return true;
}

void PreviewStage::maskHoverAt(QPointF pos, Qt::KeyboardModifiers mods) {
  maskCursor_ = pos;
  maskMods_ = mods;
  const QString id = editor().maskPick();
  QPointF src;
  maskCursorIn_ = widgetToSource(id, pos, &src);
  // 足す・外す（Shift / Alt / Ctrl）の間は、今のマスクを出したまま（新しく選ぶ候補は探さない）
  const bool refine = mods & (Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier);
  double pts = 0;
  const QImage frame = maskCursorIn_ ? engine_->rawFrame(id, &pts) : QImage();
  if (maskCursorIn_ && !refine && !frame.isNull()) mask::maskController().hover(id, frame, pts, src);
  else mask::maskController().clearHover();
  update();
}

void PreviewStage::maskClickAt(QPointF pos, Qt::KeyboardModifiers mods) {
  Editor& ed = editor();
  const QString id = ed.maskPick();
  QPointF src;
  double pts = 0;
  const QImage frame = engine_->rawFrame(id, &pts);
  if (frame.isNull() || !widgetToSource(id, pos, &src)) {
    ed.toast(QStringLiteral("マスクする被写体を、選んだ映像の上でクリックしてください（今の再生位置に出ている所）"));
    return;
  }
  const mask::PickClick kind = (mods & (Qt::AltModifier | Qt::ControlModifier)) ? mask::PickClick::Remove
                               : (mods & Qt::ShiftModifier)                     ? mask::PickClick::Add
                                                                                : mask::PickClick::Replace;
  mask::maskController().click(id, pts, src, kind);
  // 新しく選んだら、選ぶのをやめる（トラックが始まる）。足す・外すときは続けて直せるように、そのまま
  if (kind == mask::PickClick::Replace) {
    ed.setMaskPick({});
    ed.toast(QStringLiteral("被写体を選びました。AI がトラックしています（右の「オブジェクトマスク」で進み具合が見られます）"));
  }
}

QImage PreviewStage::tintMask(const QImage& mask, const QColor& color) {
  QImage out(mask.size(), QImage::Format_ARGB32_Premultiplied);
  const int w = mask.width(), h = mask.height();
  for (int y = 0; y < h; ++y) {
    const uchar* m = mask.constScanLine(y);
    const uchar* up = mask.constScanLine(std::max(0, y - 1));
    const uchar* down = mask.constScanLine(std::min(h - 1, y + 1));
    QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
    for (int x = 0; x < w; ++x) {
      const bool in = m[x] >= 128;
      // 縁（隣が外側）は、はっきりした線にする
      const bool edge = in && (up[x] < 128 || down[x] < 128 || m[std::max(0, x - 1)] < 128 || m[std::min(w - 1, x + 1)] < 128);
      if (edge) {
        o[x] = qRgba(255, 255, 255, 255);
      } else {
        const int a = m[x] * 105 / 255;
        o[x] = qRgba(color.red() * a / 255, color.green() * a / 255, color.blue() * a / 255, a);
      }
    }
  }
  return out;
}

void PreviewStage::paintMaskPick(QPainter& p, const QRectF& r) {
  const Editor& ed = editor();
  const QString id = ed.maskPick();
  mask::MaskController& mc = mask::maskController();
  double pts = 0;
  const QImage frame = engine_->rawFrame(id, &pts);
  QTransform m;
  QPainterPath visible;
  const bool shown = !frame.isNull() && sourceToWidget(id, &m, &visible);
  const bool refine = maskMods_ & (Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier);
  // 重ねるマスク: カーソルを合わせた所の候補。足す・外すの間は、今のマスク（このフレームの結果）
  std::vector<int8_t> logits;
  QString key;
  QColor tint = theme::accent;
  if (shown && refine) {
    const VideoClip* c = ed.clip(id);
    if (c && c->mask.has()) {
      const FrameMask fm = frameMaskFor(*c, ed.asset(c->assetId), pts);
      if (fm.logits) {
        logits = *fm.logits;
        key = fm.key;
        tint = (maskMods_ & Qt::ShiftModifier) ? QColor(0x3d, 0xd6, 0x8c) : QColor(0xff, 0x6b, 0x6b);
      }
    }
  } else if (shown && maskCursorIn_ && mc.hoverClip() == id && std::abs(mc.hoverPts() - pts) < 1e-3 && !mc.hoverLogits().empty()) {
    logits = mc.hoverLogits();
    key = QStringLiteral("hover|%1").arg(mc.hoverSerial());
  }
  if (!logits.empty()) {
    const QString tintKey = key + QStringLiteral("|%1").arg(tint.name());
    if (tintKey != maskTintKey_) {
      // 素材の縦横比で、程よい大きさに描く（縁の線が細くなりすぎないように）
      const QSizeF src = mediaSizeOf(*ed.clip(id));
      const int w = 640;
      const int h = std::max(16, int(std::lround(w * src.height() / std::max(1.0, src.width()))));
      maskTint_ = tintMask(mask::renderMask(logits, QSize(w, std::min(h, 1280)), mask::MaskLook{}), tint);
      maskTintKey_ = tintKey;
    }
    p.save();
    p.setClipPath(visible);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setTransform(m, true);
    p.drawImage(QRectF(0, 0, 1, 1), maskTint_);
    p.restore();
  }
  // 案内（映像の上の端）
  QString msg;
  if (frame.isNull() || !shown) {
    msg = QStringLiteral("オブジェクトマスク: 選んだ映像が、今の再生位置に出ていません　Esc・右クリックでやめる");
  } else if (mc.preparing()) {
    msg = QStringLiteral("オブジェクトマスク: AI を準備しています…");
  } else if (refine) {
    msg = (maskMods_ & Qt::ShiftModifier) ? QStringLiteral("クリックした所を、マスクに足します")
                                          : QStringLiteral("クリックした所を、マスクから外します");
  } else {
    msg = QStringLiteral("オブジェクトマスク: 被写体にカーソルを合わせてクリック（Shift+クリックで足す・Alt+クリックで外す）　Esc・右クリックでやめる");
  }
  const QFontMetricsF fm(p.font());
  const double bw = std::min(r.width() - 16, fm.horizontalAdvance(msg) + 28);
  const QRectF box(r.center().x() - bw / 2, r.top() + 10, bw, fm.height() + 14);
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0, 0, 0, 200));
  p.drawRoundedRect(box, 7, 7);
  p.setPen(QColor(255, 255, 255));
  p.drawText(box.adjusted(10, 0, -10, 0), Qt::AlignCenter, fm.elidedText(msg, Qt::ElideRight, bw - 20));
  // 足す・外すの印（カーソルの右下）
  if (refine && maskCursorIn_) {
    const QPointF c = maskCursor_ + QPointF(14, 14);
    p.setPen(Qt::NoPen);
    p.setBrush(tint);
    p.drawEllipse(c, 8, 8);
    p.setPen(QPen(Qt::white, 2));
    p.drawLine(c - QPointF(4, 0), c + QPointF(4, 0));
    if (maskMods_ & Qt::ShiftModifier) p.drawLine(c - QPointF(0, 4), c + QPointF(0, 4));
  }
  p.restore();
}

void PreviewStage::handleEditKey(QKeyEvent* e) {
  const Qt::KeyboardModifiers m = e->modifiers();
  const bool ctrl = m & Qt::ControlModifier;
  const bool shift = m & Qt::ShiftModifier;
  const bool alt = m & Qt::AltModifier;
  const QString text = committedText();
  const int len = int(text.size());
  const int selFrom = std::min(caret_, anchor_), selTo = std::max(caret_, anchor_);
  e->accept();
  // 変換中の文字があるときのキーは、日本語入力（IME）が受け取るので、ここへは来ない。来た場合は先に確定する
  switch (e->key()) {
    case Qt::Key_Escape: endTextEdit(false); return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      if (ctrl) {
        endTextEdit(true);  // Ctrl+Enter: 編集を確定する
      } else {
        commitPreedit();
        insertText(QStringLiteral("\n"), EditKind::Other);  // Enter / Shift+Enter: 改行
      }
      return;
    case Qt::Key_Backspace:
      commitPreedit();
      if (hasSelection()) applyEdit(selFrom, selTo, QString(), EditKind::Other);
      else if (caret_ > 0) applyEdit(ctrl ? wordStep(text, caret_, false) : boundaryStep(text, caret_, false, QTextBoundaryFinder::Grapheme), caret_, QString(), ctrl ? EditKind::Other : EditKind::Backspace);
      return;
    case Qt::Key_Delete:
      commitPreedit();
      if (hasSelection()) applyEdit(selFrom, selTo, QString(), EditKind::Other);
      else if (caret_ < len) applyEdit(caret_, ctrl ? wordStep(text, caret_, true) : boundaryStep(text, caret_, true, QTextBoundaryFinder::Grapheme), QString(), ctrl ? EditKind::Other : EditKind::Delete);
      return;
    case Qt::Key_Left:
    case Qt::Key_Right: {
      commitPreedit();
      const bool fwd = e->key() == Qt::Key_Right;
      if (hasSelection() && !shift) {
        moveCaret(fwd ? selTo : selFrom, false);  // 選択をやめて、端に入力位置を置く
      } else {
        const int to = ctrl ? wordStep(text, caret_, fwd) : boundaryStep(text, caret_, fwd, QTextBoundaryFinder::Grapheme);
        moveCaret(to, shift);
      }
      return;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
      commitPreedit();
      TextItem item;
      TextLayout L;
      if (!editGeometry(&item, &L, nullptr)) return;
      if (wantX_ < 0) wantX_ = textCaretLine(item, L, caret_, upstream_).x1();
      bool up = false;
      const int to = textMoveVertical(item, L, caret_, upstream_, e->key() == Qt::Key_Up ? -1 : 1, wantX_, &up);
      moveCaret(to, shift, up, true);
      return;
    }
    case Qt::Key_Home:
    case Qt::Key_End: {
      commitPreedit();
      const bool end = e->key() == Qt::Key_End;
      if (ctrl) {
        moveCaret(end ? len : 0, shift);
        return;
      }
      TextItem item;
      TextLayout L;
      if (!editGeometry(&item, &L, nullptr)) return;
      bool up = false;
      moveCaret(textLineEdge(item, L, caret_, upstream_, end, &up), shift, up);
      return;
    }
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return;
    default: break;
  }
  if (ctrl && !alt) {
    switch (e->key()) {
      case Qt::Key_A:
        commitPreedit();
        anchor_ = 0;
        moveCaret(int(committedText().size()), true);
        return;
      case Qt::Key_C: copySelection(false); return;
      case Qt::Key_X: commitPreedit(); copySelection(true); return;
      case Qt::Key_V: commitPreedit(); pasteClipboard(); return;
      case Qt::Key_Z: localUndo(shift); return;
      case Qt::Key_Y: localUndo(true); return;
      default: return;
    }
  }
  if ((m & Qt::MetaModifier) || (alt && !ctrl)) return;
  const QString t = e->text();
  if (!t.isEmpty() && t.at(0).isPrint()) {
    commitPreedit();
    insertText(t, EditKind::Type);
  }
}

void PreviewStage::inputMethodEvent(QInputMethodEvent* e) {
  if (!isEditingText()) {
    e->ignore();
    return;
  }
  e->accept();
  const QString commit = sanitizeInput(e->commitString());
  const int repStart = e->replacementStart();
  const int repLen = e->replacementLength();
  // この通知は、前の変換中の文字を置き換える
  preedit_.clear();
  preeditAttrs_.clear();
  preeditCursor_ = 0;
  if (!commit.isEmpty() || repLen > 0) {
    if (repLen > 0) {
      const int from = caret_ + repStart;
      applyEdit(from, from + repLen, commit, EditKind::Type);
    } else if (hasSelection()) {
      applyEdit(std::min(caret_, anchor_), std::max(caret_, anchor_), commit, EditKind::Type);
    } else {
      applyEdit(caret_, caret_, commit, EditKind::Type);
    }
  }
  const QString pre = sanitizeInput(e->preeditString());
  if (!pre.isEmpty()) {
    // 変換を始めるとき、選んでいた文字は置き換える
    if (hasSelection()) applyEdit(std::min(caret_, anchor_), std::max(caret_, anchor_), QString(), EditKind::Other);
    preedit_ = pre;
    int cursor = int(pre.size());
    for (const auto& a : e->attributes()) {
      if (a.type == QInputMethodEvent::Cursor) cursor = a.start;
      else if (a.type == QInputMethodEvent::TextFormat) preeditAttrs_.push_back(a);
    }
    preeditCursor_ = std::clamp(cursor, 0, int(pre.size()));
    lastKind_ = EditKind::None;
  }
  afterEdit();
}

QVariant PreviewStage::inputMethodQuery(Qt::InputMethodQuery q) const {
  if (!isEditingText()) return QWidget::inputMethodQuery(q);
  const QString s = committedText();
  const int a = std::min(caret_, anchor_), b = std::max(caret_, anchor_);
  switch (q) {
    case Qt::ImEnabled: return true;
    case Qt::ImHints: return int(Qt::ImhMultiLine);
    case Qt::ImCursorPosition: return caret_;
    case Qt::ImAnchorPosition: return anchor_;
    case Qt::ImSurroundingText: return s;
    case Qt::ImCurrentSelection: return s.mid(a, b - a);
    case Qt::ImTextBeforeCursor: return s.left(caret_);
    case Qt::ImTextAfterCursor: return s.mid(caret_);
    case Qt::ImInputItemClipRectangle: return rect();
    case Qt::ImCursorRectangle:
    case Qt::ImFont: {
      TextItem item;
      TextLayout L;
      QTransform T;
      if (!editGeometry(&item, &L, &T)) return q == Qt::ImFont ? QVariant(font()) : QVariant(QRect());
      if (q == Qt::ImFont) return textFont(item, std::max(4, int(L.fontPx * std::hypot(T.m11(), T.m12()))));
      // 変換の候補は、入力位置（変換中なら、変換中の文字の中の位置）のすぐ下に出る。回転・拡大した文字でも画面上の位置で渡す
      const QLineF cl = textCaretLine(item, L, caret_ + (preedit_.isEmpty() ? 0 : preeditCursor_), preedit_.isEmpty() && upstream_);
      const QRectF r = QRectF(T.map(cl.p1()), T.map(cl.p2())).normalized();
      return QRectF(r.x(), r.y(), std::max(1.0, r.width()), std::max(1.0, r.height())).toAlignedRect();
    }
    default: return QWidget::inputMethodQuery(q);
  }
}

void PreviewStage::paintTextEdit(QPainter& p) const {
  TextItem item;
  TextLayout L;
  QTransform T;
  if (!editGeometry(&item, &L, &T)) return;
  const QSize d = designSize(editor().project().settings.aspect);
  const double k = stageRect().width() / d.width();
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  // 選択範囲
  if (hasSelection()) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(theme::accent.red(), theme::accent.green(), theme::accent.blue(), 120));
    for (const QRectF& r : textRangeRects(item, L, caret_, anchor_)) {
      QPolygonF poly;
      poly << r.topLeft() << r.topRight() << r.bottomRight() << r.bottomLeft();
      p.drawPolygon(T.map(poly));
    }
  }
  // 変換中の文字: 下線（いま変換している文節は、色を付けて見分ける）
  if (!preedit_.isEmpty()) {
    for (const auto& a : preeditAttrs_) {
      if (a.type != QInputMethodEvent::TextFormat || a.length <= 0 || !a.value.canConvert<QTextFormat>()) continue;
      const QTextCharFormat f = qvariant_cast<QTextFormat>(a.value).toCharFormat();
      if (f.background().style() == Qt::NoBrush) continue;
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(theme::accent.red(), theme::accent.green(), theme::accent.blue(), 110));
      for (const QRectF& r : textRangeRects(item, L, caret_ + a.start, caret_ + a.start + a.length)) {
        QPolygonF poly;
        poly << r.topLeft() << r.topRight() << r.bottomRight() << r.bottomLeft();
        p.drawPolygon(T.map(poly));
      }
    }
    p.setPen(QPen(QColor(255, 255, 255, 235), 2));
    for (const QRectF& r : textRangeRects(item, L, caret_, caret_ + int(preedit_.size()))) {
      p.drawLine(T.map(r.bottomLeft()), T.map(r.bottomRight()));
    }
  }
  // 編集中の枠: ふつうの選択枠（白）とは違う、アクセントの色。ハンドルは出さない
  {
    const double pad = 4 / k;
    const QRectF b = L.bounds.adjusted(-pad, -pad, pad, pad);
    QPolygonF poly;
    poly << b.topLeft() << b.topRight() << b.bottomRight() << b.bottomLeft();
    const QPolygonF w = T.map(poly);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 130), 3.5));
    p.drawPolygon(w);
    p.setPen(QPen(theme::accent, 2));
    p.drawPolygon(w);
  }
  // 入力位置（背景に埋もれないよう、黒い縁取りをつけた白）
  if (blinkOn_ && !hasSelection()) {
    const QLineF cl = textCaretLine(item, L, caret_ + (preedit_.isEmpty() ? 0 : preeditCursor_), preedit_.isEmpty() && upstream_);
    const QPointF a = T.map(cl.p1()), b = T.map(cl.p2());
    p.setPen(QPen(QColor(0, 0, 0, 200), 4, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(a, b);
    p.setPen(QPen(QColor(255, 255, 255), 2, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(a, b);
  }
  p.restore();
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
  // 操作の案内（ai/guidecatalog.cpp）で探す名前。Segmented の objectName は見た目に使うので、名前は別に持たせる
  aspect_->setProperty("kvGuide", QStringLiteral("TransportAspect"));
  // ループ再生（終わりまで行ったら始めに戻る。書き出す範囲があれば、その範囲をくり返す）と、ガイド（セーフマージン・グリッド）
  loop_ = makeToolButton(QStringLiteral("repeat"), QString(), this);
  loop_->setCheckable(true);
  loop_->setAccessibleName(QStringLiteral("ループ再生"));
  loop_->setObjectName(QStringLiteral("TransportLoop"));
  guides_ = makeToolButton(QStringLiteral("scan"), QStringLiteral("ガイド（セーフマージン・三分割のグリッド。書き出す動画には出ません）"), this);
  guides_->setAccessibleName(QStringLiteral("ガイド"));
  guides_->setObjectName(QStringLiteral("TransportGuides"));
  guides_->setPopupMode(QToolButton::InstantPopup);
  guides_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px}"));
  auto* guideMenu = new QMenu(guides_);
  safe_ = guideMenu->addAction(QStringLiteral("セーフマージン（アクション 90%・タイトル 80%）"));
  safe_->setCheckable(true);
  grid_ = guideMenu->addAction(QStringLiteral("三分割のグリッド"));
  grid_->setCheckable(true);
  guides_->setMenu(guideMenu);
  connect(safe_, &QAction::toggled, this, [](bool on) { editor().setSafeMarginsVisible(on); });
  connect(grid_, &QAction::toggled, this, [](bool on) { editor().setGridVisible(on); });
  connect(loop_, &QToolButton::toggled, this, [](bool on) { editor().setLoopPlayback(on); });
  // オブジェクトマスク: 押すと、プレビューで被写体をクリックして選べる（AI がマスクして、動きを追いかける）
  objMask_ = makeToolButton(QStringLiteral("scan-face"), QString(), this);
  objMask_->setCheckable(true);
  objMask_->setAccessibleName(QStringLiteral("オブジェクトマスク"));
  objMask_->setObjectName(QStringLiteral("TransportObjectMask"));
  connect(objMask_, &QToolButton::clicked, this, [this] {
    toggleObjectMaskPick();
    refresh();  // 始められなかったときは、押した印を戻す
  });
  auto* right = new QWidget(this);
  auto* rh = new QHBoxLayout(right);
  rh->setContentsMargins(0, 0, 0, 0);
  rh->setSpacing(2);
  rh->addWidget(loop_);
  rh->addWidget(guides_);
  rh->addWidget(objMask_);
  rh->addStretch(1);
  rh->addWidget(aspect_);
  rh->addSpacing(8);
  // 全画面ボタンは、比率の切り替えの右（プレビューの右下）に置く
  auto* full = makeToolButton(QStringLiteral("maximize"), QStringLiteral("全画面表示"), this);
  full->setObjectName(QStringLiteral("fullScreenButton"));
  full->setAccessibleName(QStringLiteral("全画面表示"));
  rh->addWidget(full);
  h->addWidget(right, 1);
  connect(full, &QToolButton::clicked, this, &TransportBar::fullScreenRequested);

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
  connect(&ed, &Editor::loopChanged, this, &TransportBar::refresh);
  connect(&ed, &Editor::guidesChanged, this, &TransportBar::refresh);
  connect(&ed, &Editor::maskPickChanged, this, &TransportBar::refresh);
  refresh();
}

void TransportBar::refresh() {
  const Editor& ed = editor();
  time_->setText(QStringLiteral("<span style='color:#e6e6e6'>%1</span><span style='color:#6b6b6b'>&nbsp;&nbsp;/&nbsp;&nbsp;</span>"
                                "<span style='color:#9b9b9b'>%2</span>")
                     .arg(formatTime(ed.currentTime()), formatTime(ed.layout().total)));
  // アイコン・ツールチップは、状態が変わったときだけ作り直す（再生中は、時間が進むたびにここへ来る）
  if (const int playing = ed.playing() ? 1 : 0; playing != playIcon_) {
    playIcon_ = playing;
    play_->setIcon(theme::icon(playing ? QStringLiteral("pause") : QStringLiteral("play"), QColor(0x11, 0x11, 0x11)));
    play_->setToolTip(playing ? QStringLiteral("一時停止 (Space)") : QStringLiteral("再生 (Space)"));
    play_->setAccessibleName(playing ? QStringLiteral("一時停止") : QStringLiteral("再生"));
  }
  play_->setEnabled(ed.playEnd() > 0);
  aspect_->setValue(int(ed.project().settings.aspect));
  const bool loop = ed.loopPlayback();
  if (int(loop) != loopIcon_) {
    loopIcon_ = int(loop);
    {
      const QSignalBlocker b(loop_);
      loop_->setChecked(loop);
    }
    loop_->setIcon(theme::icon(QStringLiteral("repeat"), loop ? theme::accent : theme::text));
  }
  const QString keys = keymap::text(keymap::keys(QStringLiteral("play.loop")));
  const QString tip = loop ? QStringLiteral("ループ再生: オン（終わりまで行ったら始めに戻ります。書き出す範囲があれば、その範囲をくり返します）")
                           : QStringLiteral("ループ再生: オフ");
  const QString loopTip = keys.isEmpty() ? tip : QStringLiteral("%1 (%2)").arg(tip, keys);
  if (loop_->toolTip() != loopTip) loop_->setToolTip(loopTip);
  const int guides = (ed.safeMarginsVisible() ? 1 : 0) | (ed.gridVisible() ? 2 : 0);
  if (guides != guidesIcon_) {
    guidesIcon_ = guides;
    {
      const QSignalBlocker a(safe_);
      const QSignalBlocker b(grid_);
      safe_->setChecked(guides & 1);
      grid_->setChecked(guides & 2);
    }
    guides_->setIcon(theme::icon(QStringLiteral("scan"), guides ? theme::accent : theme::text));
  }
  const bool picking = !ed.maskPick().isEmpty();
  if (objMask_->isChecked() != picking) {
    const QSignalBlocker b(objMask_);
    objMask_->setChecked(picking);
  }
  if (int(picking) != objMaskIcon_) {
    objMaskIcon_ = int(picking);
    objMask_->setIcon(theme::icon(QStringLiteral("scan-face"), picking ? theme::accent : theme::text));
  }
  const QString maskKeys = keymap::text(keymap::keys(QStringLiteral("edit.objectMask")));
  const QString maskTip = picking ? QStringLiteral("オブジェクトマスク: 被写体を選んでいます（プレビューで被写体をクリック。もう一度押すとやめます）")
                                  : QStringLiteral("オブジェクトマスク（AI）: 被写体をクリックするだけで、マスクして動きを追いかけます");
  const QString maskFull = maskKeys.isEmpty() ? maskTip : QStringLiteral("%1 (%2)").arg(maskTip, maskKeys);
  if (objMask_->toolTip() != maskFull) objMask_->setToolTip(maskFull);
}

// ---- 全画面表示 --------------------------------------------------------------

namespace {

/** シークバー。押した位置へそのまま移り、そのままドラッグできる */
class SeekSlider : public QSlider {
 public:
  explicit SeekSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {
    setRange(0, 10000);
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(24);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(QStringLiteral("シークバー"));
  }

 protected:
  void mousePressEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton) return;
    setSliderDown(true);
    moveTo(e->position().x());
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (isSliderDown()) moveTo(e->position().x());
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() != Qt::LeftButton || !isSliderDown()) return;
    moveTo(e->position().x());
    setSliderDown(false);
  }

 private:
  void moveTo(double x) {
    const double half = 7;  // つまみの半分の幅
    const double span = std::max(1.0, width() - half * 2);
    setSliderPosition(int(std::lround(std::clamp((x - half) / span, 0.0, 1.0) * maximum())));
  }
};

}  // namespace

/**
 * 全画面表示用のウィンドウ（黒背景）。プレビューを付け替えて、画面いっぱいに映像だけを出す。
 * マウスを動かしたときだけ、下に再生コントロールを出す。このウィンドウではメインウィンドウのショートカットが効かないので、
 * Space（再生・一時停止）と Esc / F / F11（終了）はここで受ける
 */
class FullScreenWindow : public QWidget {
 public:
  std::function<void()> onExit;

  FullScreenWindow() : QWidget(nullptr, Qt::Window) {
    setObjectName(QStringLiteral("fullScreenWindow"));
    setWindowTitle(QStringLiteral("プレビュー（全画面）"));
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    bar_ = new QFrame(this);
    bar_->setObjectName(QStringLiteral("fsBar"));
    bar_->setStyleSheet(QStringLiteral("#fsBar{background:rgba(20,20,20,225);border:1px solid #333333;border-radius:10px}"));
    bar_->setCursor(Qt::ArrowCursor);
    auto* h = new QHBoxLayout(bar_);
    h->setContentsMargins(14, 0, 14, 0);
    h->setSpacing(12);

    play_ = new QToolButton(bar_);
    play_->setObjectName(QStringLiteral("playButton"));
    play_->setFixedSize(38, 38);
    play_->setIconSize(QSize(18, 18));
    play_->setFocusPolicy(Qt::NoFocus);
    QFont mono = font();
    mono.setStyleHint(QFont::Monospace);
    mono.setFeature("tnum", 1);
    cur_ = new QLabel(bar_);
    len_ = new QLabel(bar_);
    for (auto* l : {cur_, len_}) {
      l->setFont(mono);
      l->setMinimumWidth(l->fontMetrics().horizontalAdvance(QStringLiteral("00:00.0")) + 4);
    }
    cur_->setStyleSheet(QStringLiteral("color:#e6e6e6"));
    len_->setStyleSheet(QStringLiteral("color:#9b9b9b"));
    cur_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cur_->setAccessibleName(QStringLiteral("再生位置"));
    len_->setAccessibleName(QStringLiteral("動画の長さ"));
    seek_ = new SeekSlider(bar_);
    auto* exit = makeToolButton(QStringLiteral("minimize"), QStringLiteral("全画面表示を終了 (Esc)"), bar_);
    exit->setObjectName(QStringLiteral("fullScreenExitButton"));
    exit->setAccessibleName(QStringLiteral("全画面表示を終了"));
    exit->setFocusPolicy(Qt::NoFocus);
    h->addWidget(play_);
    h->addWidget(cur_);
    h->addWidget(seek_, 1);
    h->addWidget(len_);
    h->addWidget(exit);
    bar_->hide();

    hideTimer_.setSingleShot(true);
    hideTimer_.setInterval(2500);
    connect(&hideTimer_, &QTimer::timeout, this, [this] { hideControls(); });
    connect(play_, &QToolButton::clicked, this, [this] {
      editor().togglePlay();
      showControls();
    });
    connect(exit, &QToolButton::clicked, this, [this] { requestExit(); });
    connect(seek_, &QSlider::sliderMoved, this, [this](int v) {
      editor().seek(editor().layout().total * v / double(seek_->maximum()));
      showControls();
    });
    Editor& ed = editor();
    connect(&ed, &Editor::timeChanged, this, [this] { refresh(); });
    connect(&ed, &Editor::playingChanged, this, [this] { refresh(); });
    connect(&ed, &Editor::projectChanged, this, [this] { refresh(); });
    refresh();
  }

  /** プレビューを受け取る */
  void adopt(PreviewStage* stage) {
    stage_ = stage;
    stage->setParent(this);
    stage->setGeometry(rect());
    conns_ << connect(stage, &PreviewStage::activity, this, [this] { showControls(); });
    conns_ << connect(stage, &PreviewStage::fullScreenToggleRequested, this, [this] { requestExit(); });
    stage->lower();
    bar_->raise();
  }

  /** プレビューを手放す（付け替えは呼び出し側が行う） */
  void release() {
    for (const auto& c : conns_) disconnect(c);
    conns_.clear();
    hideTimer_.stop();
    stage_ = nullptr;
  }

  void showControls() {
    unsetCursor();
    layoutBar();
    bar_->show();
    bar_->raise();
    hideTimer_.start();
  }

  void hideControls() {
    // コントロールの上にマウスがあるとき・シークバーをドラッグしているときは隠さない
    if (bar_->underMouse() || seek_->isSliderDown()) {
      hideTimer_.start();
      return;
    }
    bar_->hide();
    setCursor(Qt::BlankCursor);
  }

  bool controlsVisible() const { return bar_->isVisible(); }

 protected:
  void resizeEvent(QResizeEvent*) override {
    if (stage_) stage_->setGeometry(rect());
    layoutBar();
  }
  void keyPressEvent(QKeyEvent* e) override {
    if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
      QWidget::keyPressEvent(e);
      return;
    }
    switch (e->key()) {
      case Qt::Key_Space:
        if (!e->isAutoRepeat()) {
          editor().togglePlay();
          showControls();
        }
        break;
      case Qt::Key_Escape:
      case Qt::Key_F:
      case Qt::Key_F11:
        if (!e->isAutoRepeat()) requestExit();
        break;
      default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
  }
  void mouseMoveEvent(QMouseEvent*) override { showControls(); }
  void closeEvent(QCloseEvent* e) override {
    e->ignore();  // 閉じるのではなく、全画面表示を終えて編集画面へ戻る
    requestExit();
  }

 private:
  void requestExit() {
    if (onExit) onExit();
  }
  void layoutBar() {
    const int w = std::max(200, std::min(960, width() - 48));
    const int h = 64;
    bar_->setGeometry((width() - w) / 2, std::max(0, height() - h - 36), w, h);
  }
  void refresh() {
    const Editor& ed = editor();
    const double total = ed.layout().total;
    play_->setIcon(theme::icon(ed.playing() ? QStringLiteral("pause") : QStringLiteral("play"), QColor(0x11, 0x11, 0x11)));
    play_->setToolTip(ed.playing() ? QStringLiteral("一時停止 (Space)") : QStringLiteral("再生 (Space)"));
    play_->setAccessibleName(ed.playing() ? QStringLiteral("一時停止") : QStringLiteral("再生"));
    play_->setEnabled(ed.playEnd() > 0);
    cur_->setText(formatTime(ed.currentTime()));
    len_->setText(formatTime(total));
    if (!seek_->isSliderDown()) {
      QSignalBlocker b(seek_);
      seek_->setValue(total > 0 ? int(std::lround(std::clamp(ed.currentTime() / total, 0.0, 1.0) * seek_->maximum())) : 0);
    }
  }

  PreviewStage* stage_ = nullptr;
  QFrame* bar_;
  QToolButton* play_;
  QLabel* cur_;
  QLabel* len_;
  QSlider* seek_;
  QTimer hideTimer_;
  QList<QMetaObject::Connection> conns_;
};

PreviewPanel::PreviewPanel(PreviewEngine* engine, QWidget* parent) : QWidget(parent) {
  setAutoFillBackground(true);
  QPalette pal = palette();
  pal.setColor(QPalette::Window, QColor(0x0d, 0x0d, 0x0d));
  setPalette(pal);
  layout_ = new QVBoxLayout(this);
  layout_->setContentsMargins(0, 0, 0, 0);
  layout_->setSpacing(0);
  stage_ = new PreviewStage(engine, this);
  stage_->setObjectName(QStringLiteral("PreviewStage"));  // 操作の案内で探す名前
  layout_->addWidget(stage_, 1);
  auto* bar = new TransportBar(this);
  layout_->addWidget(bar);
  connect(bar, &TransportBar::fullScreenRequested, this, &PreviewPanel::toggleFullScreen);
}

PreviewPanel::~PreviewPanel() {
  endFullScreen(false);  // 終了処理のときは、プレビューを元に戻すだけ（ウィンドウは出さない）
}

void PreviewPanel::toggleFullScreen() {
  if (fs_) exitFullScreen();
  else enterFullScreen();
}

void PreviewPanel::enterFullScreen() {
  if (fs_) return;
  QWidget* main = window();
  if (!main || main == this) return;
  mainWin_ = main;
  fs_ = new FullScreenWindow;
  fs_->onExit = [this] { exitFullScreen(); };
  // 編集画面と同じ画面で、全画面にする
  if (QScreen* sc = main->screen()) {
    fs_->setScreen(sc);
    fs_->setGeometry(sc->geometry());
  }
  // 映像・音声を出すエンジンはそのまま。プレビューの表示だけを別ウィンドウへ移す
  layout_->removeWidget(stage_);
  fs_->adopt(stage_);
  stage_->setFullScreenMode(true);
  fs_->showFullScreen();
  stage_->show();
  fs_->showControls();
  fs_->activateWindow();
  fs_->setFocus();
  // 新しいウィンドウを出してから編集画面を隠す（隠している間は描画されない。レイアウトや選択はそのまま）
  main->hide();
}

void PreviewPanel::exitFullScreen() {
  endFullScreen(true);
}

void PreviewPanel::endFullScreen(bool showMain) {
  if (!fs_) return;
  FullScreenWindow* fs = fs_;
  fs_ = nullptr;
  fs->release();
  stage_->setParent(this);
  if (!showMain) {
    // 終了処理のとき: 親ごとプレビューも消えるので、全画面のウィンドウだけを片付ける（エンジンはもう無いかもしれない）
    delete fs;
    return;
  }
  layout_->insertWidget(0, stage_, 1);
  stage_->setFullScreenMode(false);
  stage_->show();
  if (showMain && mainWin_) {
    mainWin_->show();
    mainWin_->raise();
    mainWin_->activateWindow();
  }
  fs->hide();
  fs->deleteLater();  // キー操作の処理の途中で呼ばれるため、すぐには消さない
}

}  // namespace kv
