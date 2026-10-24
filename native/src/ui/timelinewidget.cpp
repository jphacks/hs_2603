#include "ui/timelinewidget.h"

#include <QAction>
#include <QDragEnterEvent>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "core/exportplan.h"
#include "core/textstyles.h"
#include "core/timeline.h"
#include "media/mediamanager.h"
#include "ui/librarypanel.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

static constexpr int kHeadW = 108;
static constexpr int kRulerH = 26;
static constexpr int kVideoH = 66;
static constexpr int kTextLaneH = 28;
static constexpr int kAudioLaneH = 52;  // 音量ラインを動かしやすいよう、少し高くする
static constexpr double kSnapPx = 8;
static constexpr double kZoomMin = 4;
static constexpr double kZoomMax = 400;

TimelineView::TimelineView(QWidget* parent) : QAbstractScrollArea(parent) {
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  viewport()->setAcceptDrops(true);
  setAcceptDrops(true);
  viewport()->setMouseTracking(true);
  setFocusPolicy(Qt::ClickFocus);
  setAccessibleName(QStringLiteral("タイムライン"));
  horizontalScrollBar()->setSingleStep(20);
  Editor& ed = editor();
  connect(&ed, &Editor::projectChanged, this, [this] {
    relayout();
    viewport()->update();
  });
  connect(&ed, &Editor::assetsChanged, this, [this] { viewport()->update(); });
  connect(&ed, &Editor::selectionChanged, this, [this] { viewport()->update(); });
  connect(&ed, &Editor::thumbsChanged, this, [this](const QString& id) {
    thumbCache_.erase(id);
    viewport()->update();
  });
  connect(&ed, &Editor::waveformChanged, this, [this] { viewport()->update(); });
  connect(&ed, &Editor::timeChanged, this, [this] {
    followPlayhead();
    viewport()->update();
  });
  connect(&ed, &Editor::zoomChanged, this, [this] {
    updateScrollBars();
    viewport()->update();
  });
  connect(&ed, &Editor::fitRequested, this, &TimelineView::fitAll);
  // タブを切り替える前の表示位置は編集場所に覚えさせ、切り替えたあとは、そのタブの位置に戻す
  connect(&ed, &Editor::workspaceAboutToChange, this,
          [this] { editor().setViewScroll(horizontalScrollBar()->value(), verticalScrollBar()->value()); });
  // ドラッグ中、端に近づいたら自動でスクロールする
  autoScroll_ = new QTimer(this);
  autoScroll_->setInterval(30);
  connect(autoScroll_, &QTimer::timeout, this, [this] {
    if (!drop_) {
      autoScroll_->stop();
      return;
    }
    const double edge = 56;
    const double left = kHeadW + edge;
    const double right = viewport()->width() - edge;
    double dx = 0;
    if (dropPos_.x() < left) dx = -std::min(26.0, (left - dropPos_.x()) / edge * 24.0 + 2);
    else if (dropPos_.x() > right) dx = std::min(26.0, (dropPos_.x() - right) / edge * 24.0 + 2);
    if (dx == 0) return;
    auto* sb = horizontalScrollBar();
    const int before = sb->value();
    sb->setValue(before + int(dx));
    if (sb->value() != before) updateDrop(dropPos_);  // スクロールで、同じマウス位置の時間が変わる
  });
  connect(&ed, &Editor::workspaceChanged, this, [this] {
    drag_ = Drag::None;
    keySel_ = {};
    dropX_.reset();
    drop_.reset();
    autoScroll_->stop();
    // 見えていなかったあいだに届いたサムネイルを反映させ、別のタブのぶんの画像は手放す
    thumbCache_.clear();
    relayout();
    const QPoint pos = editor().viewScroll();
    horizontalScrollBar()->setValue(pos.x());
    verticalScrollBar()->setValue(pos.y());
    viewport()->update();
  });
  relayout();
}

/** テキストの段は、各テキストが持つ段（track）をそのまま使う。時間が重なっても、並べ直さない */
static TimelineView::Lanes textLanesOf(const std::vector<TextItem>& texts) {
  TimelineView::Lanes lanes;
  for (const auto& t : texts) lanes.lane[t.id] = t.track;
  lanes.count = textTrackCount(texts);
  return lanes;
}

static TimelineView::Lanes assignLanesFor(const std::vector<std::pair<QString, std::pair<double, double>>>& items) {
  std::vector<std::pair<QString, std::pair<double, double>>> sorted = items;
  std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.first < b.second.first; });
  std::vector<double> laneEnds;
  TimelineView::Lanes lanes;
  for (const auto& [id, range] : sorted) {
    int i = -1;
    for (size_t k = 0; k < laneEnds.size(); ++k) {
      if (laneEnds[k] <= range.first + 1e-6) {
        i = int(k);
        break;
      }
    }
    if (i < 0) {
      i = int(laneEnds.size());
      laneEnds.push_back(0);
    }
    laneEnds[size_t(i)] = range.second;
    lanes.lane[id] = i;
  }
  lanes.count = std::max(1, int(laneEnds.size()));
  return lanes;
}

void TimelineView::relayout() {
  const ProjectData& p = editor().project();
  textLanes_ = textLanesOf(p.texts);
  std::vector<std::pair<QString, std::pair<double, double>>> a;
  for (const auto& x : p.audios) a.push_back({x.id, {x.start, x.start + audioDuration(x)}});
  audioLanes_ = assignLanesFor(a);
  updateScrollBars();
}

int TimelineView::textH() const {
  return textLanes_.count * kTextLaneH + 4;
}
int TimelineView::audioH() const {
  return audioLanes_.count * kAudioLaneH + 4;
}
int TimelineView::textY() const {
  return kRulerH - verticalScrollBar()->value();
}
int TimelineView::videoY() const {
  return textY() + textH();
}
int TimelineView::audioY() const {
  return videoY() + kVideoH;
}

double TimelineView::xOf(double t) const {
  return kHeadW + t * pps() - horizontalScrollBar()->value();
}

double TimelineView::tOf(double x) const {
  return (x - kHeadW + horizontalScrollBar()->value()) / pps();
}

void TimelineView::updateScrollBars() {
  const double end = editor().contentEnd();
  const int viewW = viewport()->width() - kHeadW;
  const int contentW = int(std::max<double>(viewW, (end + 6) * pps() + 240));
  horizontalScrollBar()->setPageStep(std::max(1, viewW));
  horizontalScrollBar()->setRange(0, std::max(0, contentW - viewW));
  const int contentH = kRulerH + textH() + kVideoH + audioH();
  verticalScrollBar()->setPageStep(viewport()->height());
  verticalScrollBar()->setRange(0, std::max(0, contentH - viewport()->height()));
}

void TimelineView::resizeEvent(QResizeEvent* e) {
  QAbstractScrollArea::resizeEvent(e);
  updateScrollBars();
}

void TimelineView::scrollContentsBy(int, int) {
  viewport()->update();
}

void TimelineView::fitAll() {
  const double end = editor().contentEnd();
  const double avail = viewport()->width() - kHeadW - 60;
  editor().setPxPerSec(avail / std::max(3.0, end));
  horizontalScrollBar()->setValue(0);
}

void TimelineView::followPlayhead() {
  if (!editor().playing()) return;
  const double x = editor().currentTime() * pps();
  auto* sb = horizontalScrollBar();
  const double visibleW = viewport()->width() - kHeadW;
  if (x > sb->value() + visibleW - 40 || x < sb->value()) sb->setValue(int(std::max(0.0, x - 80)));
}

void TimelineView::zoomAt(double factor, double mouseX) {
  const double viewX = mouseX - kHeadW;
  const double tAtMouse = (horizontalScrollBar()->value() + viewX) / pps();
  const double next = std::clamp(pps() * factor, kZoomMin, kZoomMax);
  editor().setPxPerSec(next);
  horizontalScrollBar()->setValue(int(std::max(0.0, tAtMouse * next - viewX)));
}

TimelineView::Row TimelineView::rowAt(double y) const {
  if (y < kRulerH) return Row::Ruler;
  if (y < videoY()) return Row::Text;
  if (y < audioY()) return Row::Video;
  if (y < audioY() + audioH()) return Row::Audio;
  return Row::None;
}

QRectF TimelineView::clipRect(const ClipLayout& l) const {
  return {xOf(l.start), double(videoY() + 4), std::max(2.0, l.duration * pps()), double(kVideoH - 8)};
}

QRectF TimelineView::itemRect(Selection::Type type, double start, double duration, int lane) const {
  const int laneH = type == Selection::Text ? kTextLaneH : kAudioLaneH;
  const int top = type == Selection::Text ? textY() : audioY();
  return {xOf(start), double(top + 2 + lane * laneH), std::max(6.0, duration * pps()), double(laneH - 4)};
}

// ---- 音量キーフレーム --------------------------------------------------------

static constexpr double kKeyRadius = 4.5;  // 丸の大きさ
static constexpr double kKeyGrab = 10;     // 掴める範囲（見た目より広くとって、狙いやすくする）
static constexpr double kKeyPad = 7;       // 音量ラインの上下の余白

/** 音量 → 縦の位置。100% が一番上、0% が一番下。音量ラインの高さと実際の音量が一致する */
static double volumeToY(const QRectF& r, double v) {
  const double top = r.top() + kKeyPad;
  const double bottom = r.bottom() - kKeyPad;
  return bottom - std::clamp(v, 0.0, kMaxVolume) / kMaxVolume * (bottom - top);
}

static double yToVolume(const QRectF& r, double y) {
  const double top = r.top() + kKeyPad;
  const double bottom = r.bottom() - kKeyPad;
  return std::clamp((bottom - y) / (bottom - top), 0.0, 1.0) * kMaxVolume;
}

QRectF TimelineView::audioRect(const AudioItem& a) const {
  return itemRect(Selection::Audio, a.start, audioDuration(a), audioLanes_.lane.count(a.id) ? audioLanes_.lane.at(a.id) : 0);
}

TimelineView::KeyHit TimelineView::keyHitTest(QPointF pos) const {
  KeyHit best;
  if (pos.x() < kHeadW) return best;
  double bestD = kKeyGrab * kKeyGrab;
  const auto& audios = editor().project().audios;
  for (auto it = audios.rbegin(); it != audios.rend(); ++it) {
    const AudioItem& a = *it;
    if (a.volumePoints.empty()) continue;
    const QRectF r = audioRect(a);
    if (!r.adjusted(-kKeyGrab, -kKeyGrab, kKeyGrab, kKeyGrab).contains(pos)) continue;
    for (size_t i = 0; i < a.volumePoints.size(); ++i) {
      const QPointF kp(xOf(a.start + a.volumePoints[i].t), volumeToY(r, a.volumePoints[i].volume));
      const double d = (kp.x() - pos.x()) * (kp.x() - pos.x()) + (kp.y() - pos.y()) * (kp.y() - pos.y());
      if (d <= bestD) {
        bestD = d;
        best.id = a.id;
        best.index = int(i);
      }
    }
    if (best.index >= 0) return best;  // 上に重なっているオーディオを優先する
  }
  return best;
}

bool TimelineView::keySelectionValid() const {
  const AudioItem* a = keySel_.index >= 0 ? editor().audio(keySel_.id) : nullptr;
  return a && keySel_.index < int(a->volumePoints.size());
}

void TimelineView::paintVolume(QPainter& p, const AudioItem& a, const QRectF& r) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF content(kHeadW, kRulerH, viewport()->width() - kHeadW, viewport()->height() - kRulerH);
  if (a.volumePoints.empty()) {
    // キーフレームが無いときは、クリップ全体の音量の高さに、うすい線を引く
    p.setClipRect(r.intersected(content));
    const double y = volumeToY(r, a.volume);
    p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::DashLine));
    p.drawLine(QPointF(r.left() + 3, y), QPointF(r.right() - 3, y));
    p.restore();
    return;
  }
  // 音量ライン: 最初のキーフレームより前と最後より後は、端の音量のまま。間は直線
  const auto& pts = a.volumePoints;
  QPainterPath line;
  line.moveTo(r.left(), volumeToY(r, pts.front().volume));
  for (const auto& pt : pts) line.lineTo(xOf(a.start + pt.t), volumeToY(r, pt.volume));
  line.lineTo(r.right(), volumeToY(r, pts.back().volume));
  p.setClipRect(r.intersected(content));
  p.setPen(QPen(QColor(255, 255, 255, 235), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);
  p.drawPath(line);
  p.setClipRect(content);
  // キーフレーム（丸い点）。選んでいるものは大きく、色を変えて強調する
  const bool selectedClip = editor().selection().type == Selection::Audio && editor().selection().id == a.id;
  const bool hasSel = keySel_.id == a.id && keySelectionValid();
  for (size_t i = 0; i < pts.size(); ++i) {
    const QPointF c(xOf(a.start + pts[i].t), volumeToY(r, pts[i].volume));
    if (c.x() < r.left() - kKeyRadius || c.x() > r.right() + kKeyRadius || c.x() < kHeadW - kKeyRadius) continue;
    const bool sel = hasSel && keySel_.index == int(i);
    const double rad = sel ? kKeyRadius + 2 : kKeyRadius;
    p.setPen(QPen(sel ? QColor(Qt::white) : QColor(0x1d, 0x4d, 0x38), sel ? 2 : 1.4));
    p.setBrush(sel ? theme::accent : (selectedClip ? QColor(Qt::white) : QColor(0xe6, 0xf5, 0xee)));
    p.drawEllipse(c, rad, rad);
  }
  // 選んでいるキーフレームの音量（ドラッグ中は動きに合わせて変わる）
  if (hasSel) {
    const VolumePoint& pt = pts[size_t(keySel_.index)];
    const QPointF c(xOf(a.start + pt.t), volumeToY(r, pt.volume));
    QFont f = font();
    f.setPointSizeF(8.5);
    f.setBold(true);
    p.setFont(f);
    const QString text = QStringLiteral("%1%").arg(std::lround(pt.volume * 100));
    const double w = QFontMetricsF(f).horizontalAdvance(text) + 12;
    // 点の上に出す。上に余白が無いときは下に出す
    const bool above = c.y() - 26 > r.top() - 14;
    QRectF bubble(c.x() - w / 2, above ? c.y() - 28 : c.y() + 10, w, 18);
    bubble.moveLeft(std::max(double(kHeadW) + 2, bubble.left()));
    p.setPen(Qt::NoPen);
    p.setBrush(theme::accent);
    p.drawRoundedRect(bubble, 4, 4);
    p.setPen(Qt::white);
    p.drawText(bubble, Qt::AlignCenter, text);
  }
  p.restore();
}

static QPolygonF diamondShape(QPointF c, double r) {
  QPolygonF d;
  d << QPointF(c.x(), c.y() - r) << QPointF(c.x() + r, c.y()) << QPointF(c.x(), c.y() + r) << QPointF(c.x() - r, c.y());
  return d;
}

static const QColor kKeyGold(0xff, 0xc8, 0x3d);
static const QColor kKeyGoldEdge(0x7a, 0x56, 0x00);

/** 映像・文字のバーに、キーフレームがある時間の小さな ◆ を並べる（表示だけ。タイムラインの高さは変わらない） */
void TimelineView::paintKeyMarkers(QPainter& p) {
  const Editor& ed = editor();
  p.save();
  p.setClipRect(QRect(kHeadW, kRulerH, viewport()->width() - kHeadW, viewport()->height() - kRulerH));
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(kKeyGoldEdge, 1));
  p.setBrush(kKeyGold);
  auto marks = [&](const Animation& a, double start, double dur, const QRectF& r) {
    std::vector<double> times;
    for (const auto& track : a.tracks) {
      for (const auto& k : track) times.push_back(k.t);
    }
    std::sort(times.begin(), times.end());
    double lastX = -1e9;
    for (double t : times) {
      if (t < -1e-9 || t > dur + 1e-9) continue;
      const double x = xOf(start + t);
      if (x - lastX < 1.5) continue;
      lastX = x;
      p.drawPolygon(diamondShape(QPointF(x, r.bottom() - 6), 3.6));
    }
  };
  for (const auto& l : ed.layout().items) {
    if (!l.clip.anim.empty()) marks(l.clip.anim, l.start, l.duration, clipRect(l));
  }
  for (const auto& t : ed.project().texts) {
    if (t.anim.empty()) continue;
    const QRectF r = itemRect(Selection::Text, t.start, t.duration, textLanes_.lane.count(t.id) ? textLanes_.lane.at(t.id) : 0);
    marks(t.anim, t.start, t.duration, r);
  }
  p.restore();
}

TimelineView::Hit TimelineView::hitTest(QPointF p) const {
  Hit h;
  h.row = rowAt(p.y());
  if (p.x() < kHeadW || h.row == Row::Ruler || h.row == Row::None) return h;
  const Editor& ed = editor();
  auto handleOf = [&](const QRectF& r) {
    if (p.x() <= r.left() + 8) return -1;
    if (p.x() >= r.right() - 8) return 1;
    return 0;
  };
  if (h.row == Row::Video) {
    for (const auto& l : ed.layout().items) {
      const QRectF r = clipRect(l);
      if (r.contains(p)) {
        h.type = Selection::Clip;
        h.id = l.clip.id;
        h.handle = handleOf(r);
        return h;
      }
    }
  } else if (h.row == Row::Text) {
    for (const auto& t : ed.project().texts) {
      const QRectF r = itemRect(Selection::Text, t.start, t.duration, textLanes_.lane.count(t.id) ? textLanes_.lane.at(t.id) : 0);
      if (r.contains(p)) {
        h.type = Selection::Text;
        h.id = t.id;
        h.handle = handleOf(r);
      }
    }
  } else if (h.row == Row::Audio) {
    for (const auto& a : ed.project().audios) {
      const QRectF r = itemRect(Selection::Audio, a.start, audioDuration(a), audioLanes_.lane.count(a.id) ? audioLanes_.lane.at(a.id) : 0);
      if (r.contains(p)) {
        h.type = Selection::Audio;
        h.id = a.id;
        h.handle = handleOf(r);
      }
    }
  }
  return h;
}

int TimelineView::insertionIndex(double x, const QString& exclude) const {
  int n = 0;
  for (const auto& l : editor().layout().items) {
    if (l.clip.id == exclude) continue;
    if (xOf((l.start + l.end) / 2) < x) ++n;
  }
  return n;
}

double TimelineView::indicatorX(int index, const QString& exclude) const {
  std::vector<const ClipLayout*> others;
  for (const auto& l : editor().layout().items) {
    if (l.clip.id != exclude) others.push_back(&l);
  }
  if (others.empty()) return xOf(0);
  if (index >= int(others.size())) return xOf(others.back()->end);
  return xOf(others[size_t(index)]->start);
}

// 吸着先の「内容の点」: 0:00、クリップの境目（最後のクリップの終わりがビデオの終わり）、テキストとオーディオの端（exclude の自分は除く）。
// withPlayhead のときは今の再生位置も入れる（赤い線のドラッグでは、赤い線自身は入れない）
std::vector<double> TimelineView::snapPoints(const QString& exclude, bool withPlayhead) const {
  const Editor& ed = editor();
  std::vector<double> pts{0};
  if (withPlayhead) pts.push_back(ed.currentTime());
  for (const auto& l : ed.layout().items) {
    pts.push_back(l.start);
    pts.push_back(l.end);
  }
  for (const auto& t : ed.project().texts) {
    if (t.id == exclude) continue;
    pts.push_back(t.start);
    pts.push_back(t.start + t.duration);
  }
  for (const auto& a : ed.project().audios) {
    if (a.id == exclude) continue;
    pts.push_back(a.start);
    pts.push_back(a.start + audioDuration(a));
  }
  return pts;
}

// t を吸着させた時刻。しきい値は画面上の 8px。内容の点のうち一番近いものを先に使い、
// なければ目盛りの数字の位置（細かい線は使わない）。どちらもなければ空
std::optional<double> TimelineView::snap(double t, const std::vector<double>& points) const {
  const double range = kSnapPx / pps();
  std::optional<double> best;
  double bestD = range;
  for (double p : points) {
    const double d = std::abs(p - t);
    if (d < bestD) {
      best = p;
      bestD = d;
    }
  }
  if (best) return best;
  // 目盛りの数字の位置（paintRuler と同じ間隔）。念のためフレームの境目にそろえる
  const double step = rulerStep();
  const double g = frameRound(std::round(t / step) * step, editor().project().settings.fps);
  if (g >= 0 && std::abs(g - t) < range) return g;
  return std::nullopt;
}

const std::vector<QPixmap>& TimelineView::thumbPixmaps(const QString& assetId, int height) {
  auto it = thumbCache_.find(assetId);
  if (it != thumbCache_.end() && (it->second.empty() || it->second.front().height() == height)) return it->second;
  std::vector<QPixmap> out;
  if (const auto* imgs = editor().thumbs(assetId)) {
    for (const QImage& img : *imgs) out.push_back(QPixmap::fromImage(img.scaledToHeight(height, Qt::SmoothTransformation)));
  }
  thumbCache_[assetId] = out;
  return thumbCache_[assetId];
}

// ---- 描画 -------------------------------------------------------------------

static const double kSteps[] = {0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200};

// 目盛りの数字の間隔: 1 間隔が 70px 以上になる一番小さい値。吸着する「キリのいい秒」もこの間隔にする
double TimelineView::rulerStep() const {
  for (double s : kSteps) {
    if (s * pps() >= 70) return s;
  }
  return 1200;
}

void TimelineView::paintRuler(QPainter& p, const QRect& area) {
  const Editor& ed = editor();
  p.fillRect(QRect(0, 0, area.width(), kRulerH), theme::panel);
  p.fillRect(QRect(0, kRulerH - 1, area.width(), 1), theme::border);
  const double total = ed.layout().total;
  const double step = rulerStep();
  const double minor = step / 5;
  const double left = horizontalScrollBar()->value();
  const int w = area.width() - kHeadW;
  const long long i0 = std::max(0LL, (long long)std::floor(left / pps() / minor));
  const long long i1 = (long long)std::ceil((left + w) / pps() / minor) + 1;
  QFont f = font();
  f.setPointSizeF(8);
  p.setFont(f);
  for (long long i = i0; i <= i1; ++i) {
    const double t = i * minor;
    const double x = std::round(xOf(t)) + 0.5;
    if (x < kHeadW) continue;
    const bool major = i % 5 == 0;
    // 赤い線のドラッグで目盛りの数字に吸着しているときは、その数字と線を挿入位置の線と同じ黄色にする
    const bool snapped = major && drag_ == Drag::Scrub && snapMark_ && std::abs(double(i / 5) * step - *snapMark_) < 1e-6;
    p.setPen(snapped ? QColor(0xff, 0xd4, 0x4a) : t > total + 1e-6 && total > 0 ? QColor(0x35, 0x35, 0x35) : major ? QColor(0x7a, 0x7a, 0x7a) : QColor(0x45, 0x45, 0x45));
    p.drawLine(QPointF(x, major ? 12 : 18), QPointF(x, kRulerH - 1));
    if (major) {
      p.setPen(snapped ? QColor(0xff, 0xd4, 0x4a) : QColor(0x9a, 0x9a, 0x9a));
      p.drawText(QPointF(x + 3, 11), formatTime(double(i / 5) * step, step < 1));
    }
  }
}

void TimelineView::paintVideo(QPainter& p) {
  const Editor& ed = editor();
  const QString selected = ed.selection().type == Selection::Clip ? ed.selection().id : QString();
  const int innerH = kVideoH - 8 - 4;
  for (const auto& l : ed.layout().items) {
    const VideoClip& c = l.clip;
    const Asset* a = ed.asset(c.assetId);
    QRectF r = clipRect(l);
    if (drag_ == Drag::ClipMove && dragMoved_ && c.id == dragId_) r.translate(dragDx_, 0);
    if (r.right() < kHeadW || r.left() > viewport()->width()) continue;
    const bool sel = c.id == selected;
    const bool broken = !a || a->status == AssetStatus::Missing || a->status == AssetStatus::Error;
    QPainterPath shape;
    shape.addRoundedRect(r, 4, 4);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipPath(shape);
    p.fillRect(r, broken ? QColor(0x4a, 0x20, 0x26) : QColor(0x27, 0x35, 0x4d));
    // サムネイルを並べる
    if (a && !broken) {
      const auto& thumbs = thumbPixmaps(c.assetId, innerH);
      if (!thumbs.empty()) {
        const double ratio = a->width > 0 && a->height > 0 ? double(a->width) / a->height : 16.0 / 9;
        const int tileW = std::max(28, int(std::lround(innerH * std::clamp(ratio, 0.5, 2.2))));
        const int n = std::min(400, std::max(1, int(std::ceil(r.width() / tileW))));
        for (int k = 0; k < n; ++k) {
          const double tx = r.left() + k * tileW;
          if (tx + tileW < kHeadW || tx > viewport()->width()) continue;
          size_t idx = 0;
          if (!c.isImage && a->duration > 0) {
            const double local = (k + 0.5) * tileW / pps();
            const double src = c.in + local * c.speed;
            idx = size_t(std::clamp(int(std::floor(src / a->duration * double(thumbs.size()))), 0, int(thumbs.size()) - 1));
          }
          const QPixmap& pm = thumbs[idx];
          const QRectF dst(tx, r.top() + 2, tileW, innerH);
          const QRectF srcR = QRectF(QPointF(0, 0), QSizeF(pm.size()) / pm.devicePixelRatio());
          const double k2 = std::max(dst.width() / srcR.width(), dst.height() / srcR.height());
          const QSizeF sz(dst.width() / k2, dst.height() / k2);
          p.setOpacity(0.92);
          p.drawPixmap(dst, pm, QRectF((srcR.width() - sz.width()) / 2, (srcR.height() - sz.height()) / 2, sz.width(), sz.height()));
          p.setOpacity(1);
        }
      }
    }
    // フェード
    if (c.fadeIn > 0) {
      const double w = std::min(r.width() / 2, c.fadeIn * pps());
      QLinearGradient g(r.left(), 0, r.left() + w, 0);
      g.setColorAt(0, QColor(0, 0, 0, 190));
      g.setColorAt(1, QColor(0, 0, 0, 0));
      p.fillRect(QRectF(r.left(), r.top(), w, r.height()), g);
    }
    if (c.fadeOut > 0) {
      const double w = std::min(r.width() / 2, c.fadeOut * pps());
      QLinearGradient g(r.right() - w, 0, r.right(), 0);
      g.setColorAt(0, QColor(0, 0, 0, 0));
      g.setColorAt(1, QColor(0, 0, 0, 190));
      p.fillRect(QRectF(r.right() - w, r.top(), w, r.height()), g);
    }
    // 名前・長さ
    QFont f = font();
    f.setPointSizeF(8);
    p.setFont(f);
    const QFontMetricsF fm(f);
    if (r.width() > 46) {
      QString label = a ? a->name : QStringLiteral("ファイルが見つかりません");
      double lx = r.left() + 8;
      p.drawPixmap(QPointF(lx, r.top() + 5), theme::iconPixmap(c.isImage ? QStringLiteral("image") : QStringLiteral("film"), 12, Qt::white, devicePixelRatioF()));
      lx += 16;
      double avail = r.right() - 8 - lx;
      QString badge;
      if (c.speed != 1 && !c.isImage) badge = num6(c.speed) + QLatin1Char('x');
      if (!badge.isEmpty()) avail -= fm.horizontalAdvance(badge) + 12;
      if ((c.muted || c.volume == 0) && !c.isImage) avail -= 16;
      label = fm.elidedText(label, Qt::ElideRight, std::max(0.0, avail - 8));
      if (!label.isEmpty()) {
        const QRectF br(lx, r.top() + 4, fm.horizontalAdvance(label) + 8, 15);
        p.fillRect(br, QColor(0, 0, 0, 140));
        p.setPen(Qt::white);
        p.drawText(br, Qt::AlignCenter, label);
        lx = br.right() + 4;
      }
      if (!badge.isEmpty()) {
        const QRectF br(lx, r.top() + 4, fm.horizontalAdvance(badge) + 8, 15);
        p.fillRect(br, QColor(0, 0, 0, 180));
        p.setPen(Qt::white);
        p.drawText(br, Qt::AlignCenter, badge);
        lx = br.right() + 4;
      }
      if ((c.muted || c.volume == 0) && !c.isImage) {
        p.drawPixmap(QPointF(lx, r.top() + 5), theme::iconPixmap(QStringLiteral("volume-x"), 12, Qt::white, devicePixelRatioF()));
      }
    }
    if (r.width() > 70) {
      const QString d = formatDuration(l.duration);
      const QRectF br(r.right() - fm.horizontalAdvance(d) - 16, r.bottom() - 18, fm.horizontalAdvance(d) + 8, 15);
      p.fillRect(br, QColor(0, 0, 0, 150));
      p.setPen(Qt::white);
      p.drawText(br, Qt::AlignCenter, d);
    }
    p.restore();
    // 枠と端のつまみ
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(sel ? QColor(Qt::white) : broken ? QColor(0x93, 0x34, 0x3f) : QColor(0x3a, 0x50, 0x79), 2));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 4, 4);
    if (sel) {
      p.setPen(Qt::NoPen);
      for (int side : {-1, 1}) {
        const QRectF hr = side < 0 ? QRectF(r.left(), r.top(), 8, r.height()) : QRectF(r.right() - 8, r.top(), 8, r.height());
        p.setBrush(Qt::white);
        p.drawRect(hr);
        p.setBrush(QColor(0x77, 0x77, 0x77));
        p.drawRect(QRectF(hr.center().x() - 1, hr.center().y() - 6, 2, 12));
      }
    }
    p.restore();
  }
}

void TimelineView::paintItems(QPainter& p, Selection::Type type) {
  const Editor& ed = editor();
  const QString selected = ed.selection().type == type ? ed.selection().id : QString();
  QFont f = font();
  f.setPointSizeF(8);
  p.setFont(f);
  const QFontMetricsF fm(f);
  auto drawOne = [&](const QString& id, double start, double dur, int lane, const QString& label, const AudioItem* audio) {
    const QRectF r = itemRect(type, start, dur, lane);
    if (r.right() < kHeadW || r.left() > viewport()->width()) return;
    const bool sel = id == selected;
    QPainterPath shape;
    shape.addRoundedRect(r, 3, 3);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipPath(shape);
    p.fillRect(r, type == Selection::Text ? QColor(0x8f, 0x63, 0x27) : QColor(0x1d, 0x4d, 0x38));
    if (audio) {
      const Waveform* w = ed.waveform(audio->assetId);
      if (w && !w->peaks.empty() && w->peaksPerSecond > 0) {
        float maxPeak = 0.01f;
        for (float v : w->peaks) maxPeak = std::max(maxPeak, v);
        const double gain = std::clamp(0.95 / maxPeak, 1.0, 4.0);
        const int pps2 = w->peaksPerSecond;
        const double x0 = std::max(r.left(), double(kHeadW));
        const double x1 = std::min(r.right(), double(viewport()->width()));
        QPainterPath wave;
        const double mid = r.center().y();
        const double half = r.height() / 2 - 2;
        std::vector<QPointF> top;
        for (double x = x0; x <= x1; x += 2) {
          const double t0 = audio->in + (x - r.left()) / pps();
          const double t1 = audio->in + (x + 2 - r.left()) / pps();
          int a = int(std::floor(t0 * pps2));
          int b = std::max(a + 1, int(std::ceil(t1 * pps2)));
          float peak = 0;
          for (int k = std::max(0, a); k < b && k < int(w->peaks.size()); ++k) peak = std::max(peak, w->peaks[size_t(k)]);
          top.push_back(QPointF(x, std::max(1.5, std::min(1.0, peak * gain) * half)));
        }
        if (!top.empty()) {
          wave.moveTo(top.front().x(), mid - top.front().y());
          for (const auto& pt : top) wave.lineTo(pt.x(), mid - pt.y());
          for (auto it = top.rbegin(); it != top.rend(); ++it) wave.lineTo(it->x(), mid + it->y());
          wave.closeSubpath();
          p.fillPath(wave, QColor(130, 220, 170, 115));
        }
      }
      if (audio->fadeIn > 0) {
        const double w2 = std::min(r.width() / 2, audio->fadeIn * pps());
        QLinearGradient g(r.left(), 0, r.left() + w2, 0);
        g.setColorAt(0, QColor(0, 0, 0, 190));
        g.setColorAt(1, QColor(0, 0, 0, 0));
        p.fillRect(QRectF(r.left(), r.top(), w2, r.height()), g);
      }
      if (audio->fadeOut > 0) {
        const double w2 = std::min(r.width() / 2, audio->fadeOut * pps());
        QLinearGradient g(r.right() - w2, 0, r.right(), 0);
        g.setColorAt(0, QColor(0, 0, 0, 0));
        g.setColorAt(1, QColor(0, 0, 0, 190));
        p.fillRect(QRectF(r.right() - w2, r.top(), w2, r.height()), g);
      }
    }
    if (r.width() > 28) {
      const QColor fg = type == Selection::Text ? QColor(0xff, 0xf3, 0xe2) : QColor(0xe0, 0xf5, 0xea);
      p.drawPixmap(QPointF(r.left() + 9, r.center().y() - 6),
                   theme::iconPixmap(type == Selection::Text ? QStringLiteral("type") : QStringLiteral("music"), 12, fg, devicePixelRatioF()));
      p.setPen(fg);
      const QString el = fm.elidedText(label, Qt::ElideRight, std::max(0.0, r.width() - 36));
      p.drawText(QRectF(r.left() + 25, r.top(), r.width() - 30, r.height()), Qt::AlignVCenter | Qt::AlignLeft, el);
    }
    p.restore();
    // 音量ライン（音声波形の上）とキーフレーム
    if (audio) paintVolume(p, *audio, r);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(sel ? QColor(Qt::white) : type == Selection::Text ? QColor(0xb0, 0x7d, 0x36) : QColor(0x2b, 0x6e, 0x50), sel ? 2 : 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    if (sel) {
      p.setPen(Qt::NoPen);
      for (int side : {-1, 1}) {
        const QRectF hr = side < 0 ? QRectF(r.left(), r.top(), 8, r.height()) : QRectF(r.right() - 8, r.top(), 8, r.height());
        p.setBrush(Qt::white);
        p.drawRect(hr);
        p.setBrush(QColor(0x77, 0x77, 0x77));
        p.drawRect(QRectF(hr.center().x() - 1, hr.center().y() - 5, 2, 10));
      }
    }
    p.restore();
  };
  if (type == Selection::Text) {
    for (const auto& t : ed.project().texts) {
      QString label = t.text;
      label.replace(QLatin1Char('\n'), QLatin1Char(' '));
      if (label.isEmpty()) label = QStringLiteral("テキスト");
      drawOne(t.id, t.start, t.duration, textLanes_.lane.count(t.id) ? textLanes_.lane.at(t.id) : 0, label, nullptr);
    }
  } else {
    for (const auto& a : ed.project().audios) {
      const Asset* as = ed.asset(a.assetId);
      drawOne(a.id, a.start, audioDuration(a), audioLanes_.lane.count(a.id) ? audioLanes_.lane.at(a.id) : 0,
              as ? as->name : QStringLiteral("ファイルが見つかりません"), &a);
    }
  }
}

void TimelineView::paintEvent(QPaintEvent*) {
  QPainter p(viewport());
  const QRect area = viewport()->rect();
  const Editor& ed = editor();
  p.fillRect(area, QColor(0x15, 0x15, 0x15));
  // トラックの背景
  const int ty = textY();
  const int vy = videoY();
  const int ay = audioY();
  if (audioDropHover_) p.fillRect(QRect(kHeadW, ay, area.width(), audioH()), QColor(63, 182, 123, 30));
  p.fillRect(QRect(kHeadW, vy, area.width(), kVideoH), QColor(0x12, 0x12, 0x12));
  for (int y : {vy, ay, ay + audioH()}) p.fillRect(QRect(0, y - 1, area.width(), 1), theme::border);
  if (ed.layout().items.empty()) {
    p.setPen(theme::dim);
    p.drawText(QRect(kHeadW + 12, vy, 400, kVideoH), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("メディアをここにドラッグ"));
  }

  paintItems(p, Selection::Text);
  paintVideo(p);
  paintItems(p, Selection::Audio);
  paintKeyMarkers(p);

  // 挿入位置
  std::optional<double> ind;
  if (drag_ == Drag::ClipMove && dragMoved_) ind = indicatorX(dragIndex_, dragId_);
  if (dropX_) ind = dropX_;
  if (ind) p.fillRect(QRectF(*ind - 1, vy, 2, kVideoH), QColor(0xff, 0xd4, 0x4a));

  // 終わりの位置
  const double total = ed.layout().total;
  if (total > 0) {
    const double ex = xOf(total);
    if (ex >= kHeadW) {
      p.setPen(QPen(QColor(255, 255, 255, 76), 1, Qt::DashLine));
      p.drawLine(QPointF(ex, kRulerH), QPointF(ex, area.height()));
    }
  }

  // 吸着した位置（テキストとオーディオのドラッグ中だけ。挿入位置の線と同じ色。見出しの列の下には隠れる）
  if (snapMark_ && (drag_ == Drag::ItemMove || drag_ == Drag::ItemTrim)) p.fillRect(QRectF(xOf(*snapMark_) - 1, kRulerH, 2, area.height() - kRulerH), QColor(0xff, 0xd4, 0x4a));

  // 見出し（左に固定）
  p.fillRect(QRect(0, kRulerH, kHeadW, area.height()), theme::panel);
  p.fillRect(QRect(kHeadW - 1, 0, 1, area.height()), theme::border);
  QFont f = font();
  f.setPointSizeF(8.5);
  p.setFont(f);
  auto head = [&](int y, int h, const QString& icon, const QString& label) {
    p.fillRect(QRect(0, y + h - 1, kHeadW, 1), theme::border);
    p.drawPixmap(QPointF(10, y + h / 2.0 - 6.5), theme::iconPixmap(icon, 13, theme::muted, devicePixelRatioF()));
    p.setPen(theme::muted);
    p.drawText(QRect(29, y, kHeadW - 30, h), Qt::AlignVCenter | Qt::AlignLeft, label);
  };
  head(ty, textH(), QStringLiteral("type"), QStringLiteral("テキスト"));
  head(vy, kVideoH, QStringLiteral("film"), QStringLiteral("ビデオ"));
  head(ay, audioH(), QStringLiteral("music"), QStringLiteral("オーディオ"));

  // 目盛り（上に固定）
  paintRuler(p, area);
  p.fillRect(QRect(0, 0, kHeadW, kRulerH), theme::panel);
  p.fillRect(QRect(kHeadW - 1, 0, 1, kRulerH), theme::border);
  p.fillRect(QRect(0, kRulerH - 1, kHeadW, 1), theme::border);
  p.setPen(theme::dim);
  p.drawText(QRect(10, 0, kHeadW - 12, kRulerH), Qt::AlignVCenter | Qt::AlignLeft, formatTime(total, false));

  // 再生ヘッド（見出しの列と左上の角より上に描くので、見出しの上でも画面の左端まで見える）
  const double px = xOf(ed.currentTime());
  if (px >= -1) p.fillRect(QRectF(px - 1, kRulerH, 2, area.height() - kRulerH), theme::playhead);
  // 再生ヘッドのつまみ（左上の合計時間より上に描く）
  if (px >= -8 && px <= area.width() + 8) {
    QPainterPath knob;
    knob.moveTo(px - 6, 0);
    knob.lineTo(px + 6, 0);
    knob.lineTo(px + 6, 12);
    knob.lineTo(px, 18);
    knob.lineTo(px - 6, 12);
    knob.closeSubpath();
    p.setRenderHint(QPainter::Antialiasing);
    p.fillPath(knob, theme::playhead);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(QRectF(px - 1, 17, 2, kRulerH - 17), theme::playhead);
  }

  // テキストスタイル・オーディオ素材をドラッグしているときの、追加予定の表示（いちばん上に重ねる）
  paintDropGhost(p);
}

// ---- 操作 -------------------------------------------------------------------

// 赤い線を x の位置へ動かす。snapOn のときは、押したときに作った吸着先（dragSnap_）と目盛りの数字に吸着させる
void TimelineView::scrubTo(double x, bool snapOn) {
  Editor& ed = editor();
  const double t = tOf(x);
  snapMark_ = snapOn ? snap(t, dragSnap_) : std::nullopt;
  ed.seek(snapMark_.value_or(t));
  // 内容の終わりより後ろの数字に吸着して、seek で終わりに止められたときは目印を出さない
  if (snapMark_ && ed.currentTime() != *snapMark_) snapMark_.reset();
}

void TimelineView::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  Editor& ed = editor();
  const QPointF pos = e->position();
  pressPos_ = pos;
  dragMoved_ = false;
  dragDx_ = 0;
  snapMark_.reset();
  merge_ = QStringLiteral("tl-") + newId();
  // 見出しの列のクリックは無視する。左上の角（目盛りの行）だけは、目盛りと同じく再生位置を動かす
  if (pos.x() < kHeadW && pos.y() >= kRulerH) return;
  ed.pause();
  // 音量キーフレーム（丸い点）を掴んだら、クリップは動かさずに、点だけを動かす
  if (const KeyHit kh = keyHitTest(pos); kh.index >= 0) {
    ed.select({Selection::Audio, kh.id});
    keySel_ = kh;
    dragKey_ = kh.index;
    dragId_ = kh.id;
    dragType_ = Selection::Audio;
    origAudio_ = *ed.audio(kh.id);
    drag_ = Drag::KeyMove;
    viewport()->update();
    return;
  }
  const Hit h = hitTest(pos);
  if (h.row == Row::Ruler || h.type == Selection::None) {
    keySel_ = {};
    if (h.row != Row::Ruler) ed.select({});
    // 押した瞬間も吸着させる（Alt を押しているときは吸着しない）。赤い線自身は吸着先にしない
    dragSnap_ = snapPoints(QString(), false);
    scrubTo(pos.x(), !(e->modifiers() & Qt::AltModifier));
    drag_ = Drag::Scrub;
    return;
  }
  if (keySel_.id != h.id) keySel_ = {};  // ほかのものを選んだら、キーフレームの選択は外す
  ed.select({h.type, h.id});
  dragId_ = h.id;
  dragType_ = h.type;
  dragSide_ = h.handle;
  pressTime_ = tOf(pos.x());
  dragSnap_ = snapPoints(h.id, true);
  if (h.type == Selection::Clip) {
    origClip_ = *ed.clip(h.id);
    drag_ = h.handle != 0 ? Drag::ClipTrim : Drag::ClipMove;
  } else if (h.type == Selection::Text) {
    origText_ = *ed.text(h.id);
    drag_ = h.handle != 0 ? Drag::ItemTrim : Drag::ItemMove;
  } else {
    origAudio_ = *ed.audio(h.id);
    drag_ = h.handle != 0 ? Drag::ItemTrim : Drag::ItemMove;
  }
}

void TimelineView::mouseMoveEvent(QMouseEvent* e) {
  const QPointF pos = e->position();
  if (drag_ == Drag::None) {
    const Hit h = hitTest(pos);
    // 点の上では「動かせる」カーソル（上下で音量、左右で時間）
    viewport()->setCursor(keyHitTest(pos).index >= 0 ? Qt::SizeAllCursor : h.handle != 0 ? Qt::SizeHorCursor : Qt::ArrowCursor);
    return;
  }
  Editor& ed = editor();
  if (drag_ == Drag::KeyMove) {
    // 掴んだ点だけを動かす。上下 = 音量（0〜100%）、左右 = 音量が変わる時間。少し動くまでは何もしない（クリックで選ぶだけ）
    if (!dragMoved_ && std::hypot(pos.x() - pressPos_.x(), pos.y() - pressPos_.y()) < 3) return;
    dragMoved_ = true;
    const AudioItem* cur = ed.audio(dragId_);
    if (!cur) return;
    const QRectF r = audioRect(*cur);
    const double volume = std::round(yToVolume(r, pos.y()) * 100) / 100;  // 1% 刻み（表示も整数の %）
    ed.moveVolumePoint(dragId_, dragKey_, tOf(pos.x()) - cur->start, volume, merge_);
    viewport()->update();
    return;
  }
  const double dx = pos.x() - pressPos_.x();
  const double dt = dx / pps();
  const int threshold = drag_ == Drag::ClipMove ? 6 : drag_ == Drag::ItemMove ? 4 : 0;
  if (!dragMoved_ && std::abs(dx) < threshold) return;
  dragMoved_ = true;
  const int fps = ed.project().settings.fps;
  // Alt を押している間は、どの吸着も効かない
  const bool snapOn = !(e->modifiers() & Qt::AltModifier);
  switch (drag_) {
    case Drag::Scrub:
      scrubTo(pos.x(), snapOn);
      break;
    case Drag::ClipMove:
      dragDx_ = dx;
      dragIndex_ = insertionIndex(pos.x(), dragId_);
      viewport()->update();
      break;
    case Drag::ClipTrim: {
      const VideoClip o = origClip_;
      const Asset* a = ed.asset(o.assetId);
      const int side = dragSide_;
      ed.updateClip(o.id, [&](VideoClip& c) {
        double headCut = 0;  // 左端を詰めた長さ（タイムライン上の秒数。負なら広げた）
        if (o.isImage) {
          const double dur = o.out - o.in;
          const double nd = std::max(kMinClipDuration, side > 0 ? dur + dt : dur - dt);
          c.out = o.in + std::min(600.0, nd);
          if (side < 0) headCut = dur - (c.out - c.in);
        } else if (side > 0) {
          const double maxOut = a ? a->duration : o.out;
          c.out = std::min(maxOut, std::max(o.in + kMinClipDuration * o.speed, o.out + dt * o.speed));
        } else {
          c.in = std::max(0.0, std::min(o.out - kMinClipDuration * o.speed, o.in + dt * o.speed));
          headCut = (c.in - o.in) / o.speed;
        }
        // 左端を動かしたときは、キーフレームの時間もずらして、同じ映像の位置に合わせ直す
        c.anim = trimAnimationStart(o.anim, headCut);
        c.volumePoints = trimVolumePointsStart(o.volumePoints, o.volume, headCut);
      }, merge_);
      // 切った端の映像をプレビューに出す
      for (const auto& l : ed.layout().items) {
        if (l.clip.id != o.id) continue;
        ed.seek(side < 0 ? l.start : std::max(l.start, l.end - 1.0 / fps));
      }
      break;
    }
    case Drag::ItemMove: {
      const double origStart = dragType_ == Selection::Text ? origText_.start : origAudio_.start;
      const double dur = dragType_ == Selection::Text ? origText_.duration : audioDuration(origAudio_);
      double start = std::max(0.0, origStart + dt);
      // 左端と右端のうち、実際に吸着したほうで、ずれが小さいほうに合わせる
      const std::optional<double> s1 = snapOn ? snap(start, dragSnap_) : std::nullopt;        // 左端の吸着先
      const std::optional<double> s2 = snapOn ? snap(start + dur, dragSnap_) : std::nullopt;  // 右端の吸着先
      snapMark_.reset();
      if (s1 && (!s2 || std::abs(*s1 - start) <= std::abs(*s2 - (start + dur)))) {
        start = *s1;
        snapMark_ = s1;
      } else if (s2) {
        start = *s2 - dur;
        snapMark_ = s2;
      }
      start = std::max(0.0, start);
      if (dragType_ == Selection::Text) ed.updateText(dragId_, [&](TextItem& t) { t.start = start; }, merge_);
      else ed.updateAudio(dragId_, [&](AudioItem& a) { a.start = start; }, merge_);
      break;
    }
    case Drag::ItemTrim: {
      // 動かしている端を吸着させる。長さの制限などで端が吸着先からずれたときは、目印を出さない
      snapMark_.reset();
      if (dragType_ == Selection::Text) {
        const TextItem o = origText_;
        ed.updateText(o.id, [&](TextItem& t) {
          if (dragSide_ > 0) {
            const double raw = o.start + o.duration + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            const double end = s.value_or(raw);
            t.duration = std::max(kMinTextDuration, end - o.start);
            if (s && std::abs(t.start + t.duration - *s) < 1e-6) snapMark_ = s;
          } else {
            const double raw = o.start + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            const double start = std::clamp(s.value_or(raw), 0.0, o.start + o.duration - kMinTextDuration);
            t.start = start;
            t.duration = o.start + o.duration - start;
            // 左端を動かしたときは、キーフレームの時間もずらして、同じ時間（タイムライン上）の動きのままにする
            t.anim = trimAnimationStart(o.anim, start - o.start);
            if (s && std::abs(start - *s) < 1e-6) snapMark_ = s;
          }
        }, merge_);
      } else {
        const AudioItem o = origAudio_;
        const Asset* a = ed.asset(o.assetId);
        const double assetDur = a ? a->duration : o.out;
        ed.updateAudio(o.id, [&](AudioItem& x) {
          if (dragSide_ > 0) {
            const double raw = o.start + (o.out - o.in) + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            const double end = s.value_or(raw);
            x.out = std::min(assetDur, std::max(o.in + kMinClipDuration, o.in + (end - o.start)));
            if (s && std::abs(x.start + audioDuration(x) - *s) < 1e-6) snapMark_ = s;
          } else {
            const double raw = o.start + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            double start = s.value_or(raw);
            double inPoint = o.in + (start - o.start);
            if (inPoint < 0) {
              start -= inPoint;
              inPoint = 0;
            }
            if (start < 0) {
              inPoint -= start;
              start = 0;
            }
            inPoint = std::min(inPoint, o.out - kMinClipDuration);
            x.start = o.start + (inPoint - o.in);
            x.in = inPoint;
            // 頭を切ったぶん、音量キーフレームの時間をずらして、同じ音の位置に合わせ直す
            x.volumePoints = shiftVolumePoints(o.volumePoints, o.start - x.start);
            if (s && std::abs(x.start - *s) < 1e-6) snapMark_ = s;
          }
        }, merge_);
      }
      break;
    }
    default:
      break;
  }
}

void TimelineView::mouseReleaseEvent(QMouseEvent* e) {
  Editor& ed = editor();
  if (drag_ == Drag::ClipMove) {
    if (dragMoved_) ed.moveClip(dragId_, insertionIndex(e->position().x(), dragId_));
    else ed.seek(pressTime_);
  } else if (drag_ == Drag::ItemMove && !dragMoved_) {
    ed.seek(pressTime_);
  }
  drag_ = Drag::None;
  dragMoved_ = false;
  dragDx_ = 0;
  snapMark_.reset();
  ed.endMerge();
  viewport()->update();
}

void TimelineView::contextMenuEvent(QContextMenuEvent* e) {
  const QPointF pos = e->pos();
  const KeyHit kh = keyHitTest(pos);
  if (kh.index < 0) return;
  Editor& ed = editor();
  ed.select({Selection::Audio, kh.id});
  keySel_ = kh;
  viewport()->update();
  QMenu menu(this);
  QAction* del = menu.addAction(QStringLiteral("キーフレームを削除"));
  QAction* all = menu.addAction(QStringLiteral("このオーディオの音量キーフレームをすべて削除"));
  connect(del, &QAction::triggered, this, [this, kh] {
    editor().removeVolumePoint(kh.id, kh.index);
    keySel_ = {};
  });
  connect(all, &QAction::triggered, this, [this, kh] {
    editor().clearVolumePoints(kh.id);
    keySel_ = {};
  });
  menu.exec(e->globalPos());
  viewport()->update();
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent* e) {
  const Hit h = hitTest(e->position());
  // オーディオ(音量ラインの付近)をダブルクリック: その時間にキーフレームを追加する（今の音量のままなので、音は変わらない）
  if (h.row == Row::Audio && h.type == Selection::Audio && h.handle == 0 && e->position().x() > kHeadW && keyHitTest(e->position()).index < 0) {
    Editor& ed = editor();
    if (const AudioItem* a = ed.audio(h.id)) {
      const int idx = ed.addVolumePoint(h.id, tOf(e->position().x()) - a->start);
      if (idx >= 0) keySel_ = {h.id, idx};
      viewport()->update();
    }
    return;
  }
  if (h.row == Row::Text && h.type == Selection::None && e->position().x() > kHeadW) {
    editor().addText(QStringLiteral("basic"), std::max(0.0, tOf(e->position().x())));
    emit requestTextFocus();
  }
}

void TimelineView::wheelEvent(QWheelEvent* e) {
  const QPoint d = e->angleDelta();
  if (e->modifiers() & Qt::ControlModifier) {
    zoomAt(d.y() > 0 ? 1.15 : 1 / 1.15, e->position().x());
  } else if (e->modifiers() & Qt::ShiftModifier) {
    verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y() / 2);
  } else if (std::abs(d.y()) > std::abs(d.x())) {
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.y());
  } else {
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
  }
  e->accept();
}

// ---- テキストスタイルのドラッグ＆ドロップ ------------------------------------------

bool TimelineView::isTextPresetDrag(const QMimeData* m) {
  return m && m->hasFormat(QString::fromLatin1(kTextPresetMime));
}

std::vector<QString> TimelineView::audioAssetsOf(const QMimeData* m) {
  std::vector<QString> ids;
  if (!m || !m->hasFormat(QString::fromLatin1(kAssetMime))) return ids;
  for (const auto& s : QString::fromUtf8(m->data(QString::fromLatin1(kAssetMime))).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
    const Asset* a = editor().asset(s);
    if (!a || a->kind != AssetKind::Audio) return {};  // 動画や写真が混ざっているときは、これまでどおりの扱い
    ids.push_back(s);
  }
  return ids;
}

void TimelineView::updateDrop(QPointF pos) {
  if (!drop_) return;
  dropPos_ = pos;
  const Editor& ed = editor();
  DropGhost& g = *drop_;
  // 左の見出しの上には置けない（時間が 0 より前になる）
  g.valid = pos.x() >= kHeadW;
  // ズームとスクロールを含めて、マウスの位置を時間にする。フレームにそろえる
  const double start = frameRound(std::max(0.0, tOf(std::max<double>(pos.x(), kHeadW))), ed.project().settings.fps);
  g.items.clear();
  if (g.type == Selection::Text) {
    GhostItem it;
    it.start = start;
    it.duration = ed.newTextDuration(start);
    // 追加したあとに入る段（既存のテキストの段は変わらない）。重なるときは別の段になり、段が足りなければ増える
    it.lane = ed.textTrackFor(it.start, it.duration);
    it.label = defaultText();
    textLanes_ = textLanesOf(ed.project().texts);
    textLanes_.count = std::max(textLanes_.count, it.lane + 1);
    g.items.push_back(it);
  } else {
    // 追加したあとと同じ並べ方: 最初の素材を落とした位置に置き、次の素材はその終わりに続ける
    std::vector<std::pair<QString, std::pair<double, double>>> all;
    for (const auto& x : ed.project().audios) all.push_back({x.id, {x.start, x.start + audioDuration(x)}});
    double t = start;
    for (size_t i = 0; i < g.audioDurations.size(); ++i) {
      GhostItem it;
      it.start = t;
      it.duration = g.audioDurations[i];
      it.label = g.audioNames[i];
      all.push_back({QStringLiteral("__ghost-%1").arg(i), {t, t + it.duration}});
      g.items.push_back(it);
      t += it.duration;
    }
    audioLanes_ = assignLanesFor(all);  // 既存のオーディオが別の段に動くことはない（追加後も同じ計算）
    for (size_t i = 0; i < g.items.size(); ++i) g.items[i].lane = audioLanes_.lane.at(QStringLiteral("__ghost-%1").arg(i));
  }
  updateScrollBars();
  viewport()->update();
}

void TimelineView::endDrop() {
  autoScroll_->stop();
  if (!drop_) return;
  drop_.reset();
  relayout();  // 段の数を本来の状態に戻す
  viewport()->update();
}

void TimelineView::paintDropGhost(QPainter& p) {
  if (!drop_ || drop_->items.empty()) return;
  const DropGhost& d = *drop_;
  const bool isText = d.type == Selection::Text;
  const QColor ok = isText ? QColor(0xc0, 0x84, 0x34) : QColor(0x2f, 0xa0, 0x70);
  const QColor base = d.valid ? ok : QColor(0xe5, 0x48, 0x4d);
  const double x = xOf(d.items.front().start);
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setClipRect(QRect(kHeadW, kRulerH, viewport()->width() - kHeadW, viewport()->height() - kRulerH));
  // 開始位置の縦線
  p.setPen(QPen(QColor(base.red(), base.green(), base.blue(), 210), 1, Qt::DashLine));
  p.drawLine(QPointF(x, kRulerH), QPointF(x, viewport()->height()));
  // 追加されるものの半透明のクリップ
  QFont lf = font();
  lf.setPointSizeF(8);
  p.setFont(lf);
  const QFontMetricsF lfm(lf);
  for (const GhostItem& it : d.items) {
    const QRectF r = itemRect(d.type, it.start, it.duration, it.lane);
    QPainterPath shape;
    shape.addRoundedRect(r, 3, 3);
    p.fillPath(shape, QColor(base.red(), base.green(), base.blue(), 150));
    p.setPen(QPen(base.lighter(135), 1.5, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawPath(shape);
    if (r.width() > 28) {
      p.setPen(QColor(0xff, 0xf3, 0xe2));
      p.drawText(r.adjusted(9, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, lfm.elidedText(it.label, Qt::ElideRight, r.width() - 13));
    }
  }
  p.restore();

  // 目盛りの上に、開始時間（置けないときはその旨）を出す
  const GhostItem first = d.items.front();
  const double dstart = first.start;
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  QFont f = font();
  f.setPointSizeF(8.5);
  f.setBold(true);
  p.setFont(f);
  const QString label = d.valid ? QStringLiteral("開始 ") + formatTime(dstart, true) : QStringLiteral("ここには置けません");
  const double w = QFontMetricsF(f).horizontalAdvance(label) + 14;
  const double bx = std::clamp(x - w / 2, double(kHeadW) + 2, std::max(double(kHeadW) + 2, viewport()->width() - w - 2));
  const QRectF bubble(bx, 3, w, kRulerH - 8);
  p.setPen(Qt::NoPen);
  p.setBrush(d.valid ? QColor(0x26, 0x80, 0xeb) : QColor(0xe5, 0x48, 0x4d));
  p.drawRoundedRect(bubble, 4, 4);
  p.setPen(Qt::white);
  p.drawText(bubble, Qt::AlignCenter, label);
  p.restore();
}

void TimelineView::dropTextPreset(QDropEvent* e) {
  const QString presetId = QString::fromUtf8(e->mimeData()->data(QString::fromLatin1(kTextPresetMime)));
  if (!drop_ || drop_->type != Selection::Text) beginDrop(Selection::Text, {});
  updateDrop(e->position());
  const std::optional<DropGhost> drop = drop_;
  endDrop();
  if (!drop || !drop->valid || drop->items.empty() || presetId.isEmpty()) {
    e->ignore();
    return;
  }
  // マウスを離した位置の時間に、そのスタイルで追加する。追加したテキストは選ばれ、プレビューに出る
  editor().addTextAt(presetId, drop->items.front().start);
  e->acceptProposedAction();
  emit requestTextFocus();
}

void TimelineView::dropAudioAssets(QDropEvent* e, const std::vector<QString>& ids) {
  if (!drop_ || drop_->type != Selection::Audio) beginDrop(Selection::Audio, ids);
  updateDrop(e->position());
  const std::optional<DropGhost> drop = drop_;
  endDrop();
  dropX_.reset();
  audioDropHover_ = false;
  if (!drop || !drop->valid || drop->items.empty()) {
    e->ignore();
    return;
  }
  // 落とした位置の時間から、見せていた位置にそのまま並べる。既存のオーディオは動かさない
  editor().addToTimeline(ids, std::nullopt, drop->items.front().start);
  e->acceptProposedAction();
}

void TimelineView::beginDrop(Selection::Type type, const std::vector<QString>& audioIds) {
  DropGhost g;
  g.type = type;
  for (const QString& id : audioIds) {
    const Asset* a = editor().asset(id);
    g.audioDurations.push_back(a ? a->duration : 0);
    g.audioNames.push_back(a ? a->name : QString());
  }
  drop_ = g;
  if (!autoScroll_->isActive()) autoScroll_->start();
}

void TimelineView::dragEnterEvent(QDragEnterEvent* e) {
  if (isTextPresetDrag(e->mimeData())) {
    beginDrop(Selection::Text, {});
    updateDrop(e->position());
    if (dropAccepted()) e->acceptProposedAction();
    else e->ignore();
    return;
  }
  if (const auto audio = audioAssetsOf(e->mimeData()); !audio.empty()) {
    beginDrop(Selection::Audio, audio);
    updateDrop(e->position());
    if (dropAccepted()) e->acceptProposedAction();
    else e->ignore();
    return;
  }
  if (e->mimeData()->hasFormat(QString::fromLatin1(kAssetMime))) e->acceptProposedAction();
  else e->ignore();
}

void TimelineView::dragMoveEvent(QDragMoveEvent* e) {
  if (drop_ && (isTextPresetDrag(e->mimeData()) || !audioAssetsOf(e->mimeData()).empty())) {
    updateDrop(e->position());
    // 置けない位置では「置けない」カーソルにする
    if (dropAccepted()) e->acceptProposedAction();
    else e->ignore();
    return;
  }
  if (!e->mimeData()->hasFormat(QString::fromLatin1(kAssetMime))) {
    e->ignore();
    return;
  }
  const Row row = rowAt(e->position().y());
  audioDropHover_ = row == Row::Audio;
  if (row == Row::Video) dropX_ = indicatorX(insertionIndex(e->position().x(), QString()), QString());
  else dropX_.reset();
  e->acceptProposedAction();
  viewport()->update();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent*) {
  // タイムラインの外へ出た・Esc で中止した: 追加予定の表示を消すだけで、何も追加しない
  endDrop();
  dropX_.reset();
  audioDropHover_ = false;
  viewport()->update();
}

void TimelineView::dropEvent(QDropEvent* e) {
  if (isTextPresetDrag(e->mimeData())) {
    dropTextPreset(e);
    return;
  }
  if (const auto audio = audioAssetsOf(e->mimeData()); !audio.empty()) {
    dropAudioAssets(e, audio);
    return;
  }
  dropX_.reset();
  audioDropHover_ = false;
  viewport()->update();
  const QByteArray data = e->mimeData()->data(QString::fromLatin1(kAssetMime));
  if (data.isEmpty()) return;
  std::vector<QString> ids;
  for (const auto& s : QString::fromUtf8(data).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) ids.push_back(s);
  const Row row = rowAt(e->position().y());
  const double t = std::max(0.0, tOf(e->position().x()));
  if (row == Row::Video) editor().addToTimeline(ids, insertionIndex(e->position().x(), QString()), t);
  else editor().addToTimeline(ids, std::nullopt, t);
  e->acceptProposedAction();
}

// ---- TimelineWidget ---------------------------------------------------------

static double zoomToSlider(double pps) {
  return std::log(pps / kZoomMin) / std::log(kZoomMax / kZoomMin);
}
static double sliderToZoom(double v) {
  return kZoomMin * std::pow(kZoomMax / kZoomMin, v);
}

TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
  setAutoFillBackground(true);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  auto* bar = new QWidget(this);
  auto* h = new QHBoxLayout(bar);
  h->setContentsMargins(8, 4, 8, 4);
  h->setSpacing(2);
  split_ = makeToolButton(QStringLiteral("scissors"), QStringLiteral("分割 (S)"), bar, QStringLiteral("分割"));
  dup_ = makeToolButton(QStringLiteral("copy"), QStringLiteral("複製 (Ctrl+D)"), bar, QStringLiteral("複製"));
  del_ = makeToolButton(QStringLiteral("trash"), QStringLiteral("削除 (Delete)"), bar, QStringLiteral("削除"));
  auto* text = makeToolButton(QStringLiteral("type"), QStringLiteral("テキストを追加 (T)"), bar, QStringLiteral("テキスト"));
  // 左の「オーディオ」タブの「追加」は一覧に登録するだけ。こちらは、登録して再生位置にすぐ置く近道（処理は同じ）
  auto* audio = makeToolButton(QStringLiteral("music"), QStringLiteral("オーディオを選んで再生位置に追加（左の「オーディオ」タブにも登録されます）"), bar, QStringLiteral("オーディオ"));
  for (auto* b : {split_, dup_, del_}) h->addWidget(b);
  auto* sep = new QFrame(bar);
  sep->setFixedSize(1, 20);
  sep->setStyleSheet(QStringLiteral("background:#3a3a3a"));
  h->addSpacing(6);
  h->addWidget(sep);
  h->addSpacing(6);
  h->addWidget(text);
  h->addWidget(audio);
  h->addStretch(1);
  auto* zout = makeToolButton(QStringLiteral("zoom-out"), QStringLiteral("縮小"), bar);
  zoom_ = new QSlider(Qt::Horizontal, bar);
  zoom_->setRange(0, 1000);
  zoom_->setFixedWidth(140);
  zoom_->setFocusPolicy(Qt::NoFocus);
  zoom_->setAccessibleName(QStringLiteral("タイムラインの拡大率"));
  auto* zin = makeToolButton(QStringLiteral("zoom-in"), QStringLiteral("拡大"), bar);
  auto* fit = makeToolButton(QStringLiteral("maximize"), QStringLiteral("全体を表示"), bar);
  for (QWidget* w : std::initializer_list<QWidget*>{zout, zoom_, zin, fit}) h->addWidget(w);
  v->addWidget(bar);
  auto* line = new QFrame(this);
  line->setFixedHeight(1);
  line->setStyleSheet(QStringLiteral("background:#2a2a2a"));
  v->addWidget(line);
  view_ = new TimelineView(this);
  v->addWidget(view_, 1);

  Editor& ed = editor();
  connect(split_, &QToolButton::clicked, this, [] { editor().splitAtPlayhead(); });
  connect(dup_, &QToolButton::clicked, this, [] { editor().duplicateSelection(); });
  connect(del_, &QToolButton::clicked, this, [] { editor().deleteSelection(); });
  connect(text, &QToolButton::clicked, this, [this] {
    editor().addText(QStringLiteral("basic"));
    emit requestTextFocus();
  });
  connect(audio, &QToolButton::clicked, this, [this] { LibraryPanel::pickAndImport(this, true); });
  connect(zout, &QToolButton::clicked, this, [] { editor().setPxPerSec(editor().pxPerSec() / 1.4); });
  connect(zin, &QToolButton::clicked, this, [] { editor().setPxPerSec(editor().pxPerSec() * 1.4); });
  connect(fit, &QToolButton::clicked, view_, &TimelineView::fitAll);
  connect(zoom_, &QSlider::valueChanged, this, [](int v) { editor().setPxPerSec(sliderToZoom(v / 1000.0)); });
  connect(view_, &TimelineView::requestTextFocus, this, &TimelineWidget::requestTextFocus);
  connect(&ed, &Editor::zoomChanged, this, [this] {
    const QSignalBlocker b(zoom_);
    zoom_->setValue(int(std::lround(zoomToSlider(editor().pxPerSec()) * 1000)));
  });
  connect(&ed, &Editor::selectionChanged, this, &TimelineWidget::refreshButtons);
  connect(&ed, &Editor::projectChanged, this, &TimelineWidget::refreshButtons);
  {
    const QSignalBlocker b(zoom_);
    zoom_->setValue(int(std::lround(zoomToSlider(ed.pxPerSec()) * 1000)));
  }
  refreshButtons();
}

void TimelineWidget::refreshButtons() {
  const Editor& ed = editor();
  split_->setEnabled(!ed.project().clips.empty() || ed.selection().type != Selection::None);
  dup_->setEnabled(bool(ed.selection()));
  del_->setEnabled(bool(ed.selection()));
}

}  // namespace kv
