#include "ui/timelinewidget.h"

#include <QAction>
#include <QCursor>
#include <QDragEnterEvent>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QHBoxLayout>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <iterator>

#include "core/exportplan.h"
#include "core/textstyles.h"
#include "core/timeline.h"
#include "media/mediamanager.h"
#include "ui/librarypanel.h"
#include "ui/objectmenu.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

static constexpr int kHeadW = 108;
static constexpr int kRulerH = 26;
// 段の高さの、はじめの値と範囲（左の見出しの列の境目をドラッグするか、キーで変えられる）
static constexpr int kVideoH = 66;
static constexpr int kTextLaneH = 28;
static constexpr int kOverlayLaneH = 44;  // 重ねるトラックの 1 段（サムネイルが見える高さ）
static constexpr int kAudioLaneH = 52;  // 音量ラインを動かしやすいよう、少し高くする
static constexpr int kMinVideoH = 40;
static constexpr int kMaxVideoH = 200;
static constexpr int kMinTextLaneH = 20;
static constexpr int kMaxTextLaneH = 64;
static constexpr int kMinOverlayLaneH = 28;
static constexpr int kMaxOverlayLaneH = 120;
static constexpr int kMinAudioLaneH = 36;
static constexpr int kMaxAudioLaneH = 140;
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
  connect(&ed, &Editor::rangeChanged, this, [this] { viewport()->update(); });
  loadTrackHeights();  // 前回の段の高さ
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
  connect(&ed, &Editor::workspaceAboutToChange, this, [this] {
    editor().setViewScroll(horizontalScrollBar()->value(), verticalScrollBar()->value());
    if (dragSession_) {
      editor().endEditSession();  // ドラッグ中に編集場所が変わった: そこまでの変更は残す
      dragSession_ = false;
    }
  });
  // ドラッグ中、端に近づいたら自動でスクロールする（約 60 回/秒。端に近い間だけ動かす）
  autoScroll_ = new QTimer(this);
  autoScroll_->setTimerType(Qt::PreciseTimer);
  autoScroll_->setInterval(16);
  connect(autoScroll_, &QTimer::timeout, this, &TimelineView::autoScrollTick);
  connect(&ed, &Editor::workspaceChanged, this, [this] {
    drag_ = Drag::None;
    dragSession_ = false;
    libDrag_ = false;
    dragReach_ = 0;
    keepEnd_ = 0;
    audioMemo_.clear();  // 別の編集場所の段の並びは引き継がない
    laneHoldId_.clear();
    laneHold_ = -1;
    keySel_ = {};
    dropX_.reset();
    drop_.reset();
    stopAutoScroll();
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

namespace {
struct LaneSpan {
  QString id;
  double start = 0;
  double end = 0;
};
}  // namespace

/**
 * オーディオの段。段は編集データには無く、時間の重なりから決める（重なるものは別の段に並べる）。
 * 段がころころ入れ替わらないよう、前回の割り当て（memo）を引き継ぐ:
 *  - 前回から時間が変わっていないものは、前回の段のまま（ほかの物を動かしても、勝手に段を移らない）
 *  - ドラッグ中の物（holdId）は、ドラッグを始めたときの段（holdLane）が空いていればそこ、ふさがっていれば空いている一番上の段
 *  - ほかに時間が変わったもの・新しいものは、始まりの早い順に、空いている一番上の段へ入れる（元に戻すと、元の段に戻る）
 *  - 何も入っていない段は詰める（上下の順はそのまま）
 * 前回の割り当てが無いとき（開いた直後など）は、これまでと同じ並べ方（始まりの早い順に、空いている一番上の段）になる
 */
static TimelineView::Lanes stableLanes(const std::vector<LaneSpan>& items, const std::map<QString, TimelineView::LaneMemo>& memo,
                                       const QString& holdId = QString(), int holdLane = -1) {
  constexpr double kEps = 1e-6;
  std::vector<std::vector<std::pair<double, double>>> used;  // 段ごとの、入っている時間の範囲
  auto fits = [&](int lane, double s, double e) {
    if (lane < 0) return false;
    if (lane >= int(used.size())) return true;
    for (const auto& [a, b] : used[size_t(lane)]) {
      if (s < b - kEps && a < e - kEps) return false;  // 端が接しているだけなら重ならない
    }
    return true;
  };
  std::map<QString, int> lane;
  auto put = [&](const LaneSpan& it, int l) {
    if (l >= int(used.size())) used.resize(size_t(l) + 1);
    used[size_t(l)].push_back({it.start, it.end});
    lane[it.id] = l;
  };
  std::vector<const LaneSpan*> moved;
  for (const auto& it : items) {
    const auto m = memo.find(it.id);
    const bool same = m != memo.end() && it.id != holdId && std::abs(m->second.start - it.start) < kEps && std::abs(m->second.end - it.end) < kEps;
    if (same && fits(m->second.lane, it.start, it.end)) put(it, m->second.lane);
    else moved.push_back(&it);
  }
  std::stable_sort(moved.begin(), moved.end(), [](const LaneSpan* a, const LaneSpan* b) { return a->start < b->start; });
  for (const LaneSpan* it : moved) {
    const int want = !holdId.isEmpty() && it->id == holdId ? holdLane : -1;
    int l = fits(want, it->start, it->end) ? want : 0;
    while (!fits(l, it->start, it->end)) ++l;
    put(*it, l);
  }
  std::vector<int> remap(used.size(), 0);
  int count = 0;
  for (size_t k = 0; k < used.size(); ++k) {
    remap[k] = count;
    if (!used[k].empty()) ++count;
  }
  TimelineView::Lanes out;
  for (const auto& [id, l] : lane) out.lane[id] = remap[size_t(l)];
  out.count = std::max(1, count);
  return out;
}

void TimelineView::relayout() {
  const ProjectData& p = editor().project();
  textLanes_ = textLanesOf(p.texts);
  // 重ねるトラックの段も、各映像が持つ段（track）をそのまま使う
  overlayLanes_ = Lanes();
  for (const auto& o : p.overlays) overlayLanes_.lane[o.id] = o.track;
  overlayLanes_.count = overlayTrackCount(p.overlays);
  std::vector<LaneSpan> a;
  for (const auto& x : p.audios) a.push_back({x.id, x.start, x.start + audioDuration(x)});
  audioLanes_ = stableLanes(a, audioMemo_, laneHoldId_, laneHold_);
  // 次に並べ直すときのために、今の段と時間を覚えておく
  audioMemo_.clear();
  for (const auto& s : a) audioMemo_[s.id] = {audioLanes_.lane.at(s.id), s.start, s.end};
  updateScrollBars();
}

int TimelineView::textLaneAt(double y) const {
  // テキストの段の中だけ（一番下の段のさらに下へ移せるよう、下の段に少しはみ出したところまで）。外なら -1
  if (y < textY() || y >= overlayY() + std::max(8, int(textLaneH_ * 0.6))) return -1;
  int lastOther = -1;
  for (const auto& t : editor().project().texts) {
    if (t.id != dragId_) lastOther = std::max(lastOther, t.track);
  }
  const int lane = int(std::floor((y - textY() - 2) / textLaneH_));
  return std::clamp(lane, 0, lastOther + 1);
}

int TimelineView::overlayLaneAt(double y) const {
  // 重ねるトラックの段の中だけ（一番上の段のさらに上へ移せるよう、テキストの段に少しはみ出したところまで）。外なら -1
  const int tol = std::max(8, int(overlayLaneH_ * 0.6));
  if (y < overlayY() - tol || y >= videoY()) return -1;
  int lastOther = -1;
  for (const auto& o : editor().project().overlays) {
    if (o.id != dragId_) lastOther = std::max(lastOther, o.track);
  }
  // 段は下から 0, 1, 2 …（上の段ほど前に重なる）
  const int fromTop = int(std::floor((y - overlayY() - 2) / overlayLaneH_));
  const int lane = overlayLanes_.count - 1 - fromTop;
  return std::clamp(lane, 0, lastOther + 1);
}

int TimelineView::textH() const {
  return textLanes_.count * textLaneH_ + 4;
}
int TimelineView::overlayH() const {
  return overlayLanes_.count * overlayLaneH_ + 4;
}
int TimelineView::audioH() const {
  return audioLanes_.count * audioLaneH_ + 4;
}
int TimelineView::textY() const {
  return kRulerH - verticalScrollBar()->value();
}
int TimelineView::overlayY() const {
  return textY() + textH();
}
int TimelineView::videoY() const {
  return overlayY() + overlayH();
}
int TimelineView::audioY() const {
  return videoY() + videoH_;
}

double TimelineView::xOf(double t) const {
  return kHeadW + t * pps() - horizontalScrollBar()->value();
}

double TimelineView::tOf(double x) const {
  return (x - kHeadW + horizontalScrollBar()->value()) / pps();
}

void TimelineView::updateScrollBars() {
  // ドラッグ・ドロップ中の物が、今の終わりより先へ行くときは、そこまでスクロールできるよう範囲を広げる。
  // ドラッグを終えたときの表示位置も、範囲に残しておく（keepEnd_）
  const double end = std::max({editor().contentEnd(), dragReach_, keepEnd_});
  const int viewW = viewport()->width() - kHeadW;
  const int contentW = int(std::max<double>(viewW, (end + 6) * pps() + 240));
  int maxX = std::max(0, contentW - viewW);
  // ドラッグの途中で中身が短くなっても（いちばん後ろの物を前へ動かした等）、範囲を縮めて今の表示位置を動かすことはしない。
  // 動かすと、表示もドラッグしている物も勝手に飛んでしまう
  if (dragActive()) maxX = std::max(maxX, horizontalScrollBar()->value());
  horizontalScrollBar()->setPageStep(std::max(1, viewW));
  horizontalScrollBar()->setRange(0, maxX);
  const int contentH = kRulerH + textH() + overlayH() + videoH_ + audioH();
  verticalScrollBar()->setPageStep(viewport()->height());
  verticalScrollBar()->setRange(0, std::max(0, contentH - viewport()->height()));
}

void TimelineView::resizeEvent(QResizeEvent* e) {
  QAbstractScrollArea::resizeEvent(e);
  updateScrollBars();
}

void TimelineView::scrollContentsBy(int, int) {
  // ドラッグを終えたあとに広げたままにしていた範囲は、表示が左へ戻ったぶんだけ縮める（今の表示位置は動かさない）
  if (keepEnd_ > 0 && !dragActive() && !horizontalScrollBar()->isSliderDown()) {
    const double need = std::max(0.0, viewNeedEnd());
    if (need < keepEnd_) {
      keepEnd_ = need;
      updateScrollBars();
    }
  }
  viewport()->update();
}

double TimelineView::viewNeedEnd() const {
  const int viewW = viewport()->width() - kHeadW;
  return (horizontalScrollBar()->value() + viewW - 240 + 1) / pps() - 6;
}

void TimelineView::rememberView() {
  keepEnd_ = std::max(keepEnd_, viewNeedEnd());
}

bool TimelineView::dragActive() const {
  return drag_ != Drag::None || drop_.has_value() || libDrag_;
}

void TimelineView::fitAll() {
  keepEnd_ = 0;
  const double end = editor().contentEnd();
  const double avail = viewport()->width() - kHeadW - 60;
  editor().setPxPerSec(avail / std::max(3.0, end));
  horizontalScrollBar()->setValue(0);
}

// ---- 段の高さ ----------------------------------------------------------------------

// AI の設定・キーの割り当てと同じ settings.ini に覚える（アプリ全体の設定。プロジェクトには保存しない）
static QString timelineSettingsPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("settings.ini"));
}

void TimelineView::setTrackHeights(int textLane, int overlayLane, int video, int audioLane) {
  textLane = std::clamp(textLane, kMinTextLaneH, kMaxTextLaneH);
  overlayLane = std::clamp(overlayLane, kMinOverlayLaneH, kMaxOverlayLaneH);
  video = std::clamp(video, kMinVideoH, kMaxVideoH);
  audioLane = std::clamp(audioLane, kMinAudioLaneH, kMaxAudioLaneH);
  if (textLane == textLaneH_ && overlayLane == overlayLaneH_ && video == videoH_ && audioLane == audioLaneH_) return;
  textLaneH_ = textLane;
  overlayLaneH_ = overlayLane;
  videoH_ = video;
  audioLaneH_ = audioLane;
  updateScrollBars();
  viewport()->update();
}

void TimelineView::loadTrackHeights() {
  QSettings s(timelineSettingsPath(), QSettings::IniFormat);
  setTrackHeights(s.value(QStringLiteral("timeline/textLaneHeight"), kTextLaneH).toInt(),
                  s.value(QStringLiteral("timeline/overlayLaneHeight"), kOverlayLaneH).toInt(),
                  s.value(QStringLiteral("timeline/videoHeight"), kVideoH).toInt(),
                  s.value(QStringLiteral("timeline/audioLaneHeight"), kAudioLaneH).toInt());
}

void TimelineView::saveTrackHeights() const {
  QDir().mkpath(QFileInfo(timelineSettingsPath()).absolutePath());
  QSettings s(timelineSettingsPath(), QSettings::IniFormat);
  s.setValue(QStringLiteral("timeline/textLaneHeight"), textLaneH_);
  s.setValue(QStringLiteral("timeline/overlayLaneHeight"), overlayLaneH_);
  s.setValue(QStringLiteral("timeline/videoHeight"), videoH_);
  s.setValue(QStringLiteral("timeline/audioLaneHeight"), audioLaneH_);
}

void TimelineView::scaleTrackHeights(double factor) {
  setTrackHeights(int(std::lround(textLaneH_ * factor)), int(std::lround(overlayLaneH_ * factor)), int(std::lround(videoH_ * factor)),
                  int(std::lround(audioLaneH_ * factor)));
  saveTrackHeights();
}

void TimelineView::resetTrackHeights() {
  setTrackHeights(kTextLaneH, kOverlayLaneH, kVideoH, kAudioLaneH);
  saveTrackHeights();
}

int TimelineView::trackBorderAt(QPointF pos) const {
  if (pos.x() >= kHeadW || pos.y() < kRulerH) return -1;
  const int borders[4] = {overlayY(), videoY(), audioY(), audioY() + audioH()};
  for (int i = 0; i < 4; ++i) {
    if (std::abs(pos.y() - borders[i]) <= 4) return i;
  }
  return -1;
}

void TimelineView::execHeaderMenu(const QPoint& globalPos) {
  QMenu menu(this);
  // 見出しの「重ねる」の列: 動画・写真を重ねる方法の案内
  if (rowAt(viewport()->mapFromGlobal(globalPos).y()) == Row::Overlay) {
    QAction* info = menu.addAction(QStringLiteral("左の「メディア」から、この段へ動画・写真をドラッグすると、映像の上に重ねられます"));
    info->setEnabled(false);
    menu.addSeparator();
  }
  menu.addAction(QStringLiteral("段を高くする"), this, [this] { scaleTrackHeights(1.2); });
  menu.addAction(QStringLiteral("段を低くする"), this, [this] { scaleTrackHeights(1 / 1.2); });
  menu.addAction(QStringLiteral("段の高さを元に戻す"), this, [this] { resetTrackHeights(); });
  menu.addSeparator();
  QAction* hint = menu.addAction(QStringLiteral("見出しの段の境目をドラッグしても、高さを変えられます"));
  hint->setEnabled(false);
  menu.exec(globalPos);
}

void TimelineView::revealTime(double t) {
  const double x = xOf(t);
  if (x >= kHeadW + 24 && x <= viewport()->width() - 48) return;
  // 見えている幅の左から 1/3 あたりに来るようにする
  horizontalScrollBar()->setValue(int(std::lround(t * pps() - (viewport()->width() - kHeadW) / 3.0)));
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
  if (y < overlayY()) return Row::Text;
  if (y < videoY()) return Row::Overlay;
  if (y < audioY()) return Row::Video;
  if (y < audioY() + audioH()) return Row::Audio;
  return Row::None;
}

QRectF TimelineView::clipRect(const ClipLayout& l) const {
  return {xOf(l.start), double(videoY() + 4), std::max(2.0, l.duration * pps()), double(videoH_ - 8)};
}

QRectF TimelineView::itemRect(Selection::Type type, double start, double duration, int lane) const {
  if (type == Selection::Overlay) {
    // 重ねるトラックは、下の段（メインに近い）から 0, 1, 2 …
    const int row = std::max(0, overlayLanes_.count - 1 - lane);
    return {xOf(start), double(overlayY() + 2 + row * overlayLaneH_), std::max(6.0, duration * pps()), double(overlayLaneH_ - 4)};
  }
  const int laneH = type == Selection::Text ? textLaneH_ : audioLaneH_;
  const int top = type == Selection::Text ? textY() : audioY();
  return {xOf(start), double(top + 2 + lane * laneH), std::max(6.0, duration * pps()), double(laneH - 4)};
}

QRectF TimelineView::overlayRect(const OverlayItem& o) const {
  return itemRect(Selection::Overlay, o.start, overlayDuration(o, editor().project().settings.fps),
                  overlayLanes_.lane.count(o.id) ? overlayLanes_.lane.at(o.id) : 0);
}

std::vector<QString> TimelineView::coSelectedIds() const {
  std::vector<QString> out;
  const Editor& ed = editor();
  for (const auto& s : ed.selectionSet()) {
    if (!(s == ed.selection())) out.push_back(s.id);
  }
  return out;
}

/** グループの色（グループごとに違う色。同じグループは同じ色） */
static QColor groupColor(const QString& g) {
  return QColor::fromHsv(int(qHash(g) % 360), 150, 235);
}

void TimelineView::paintGroupMarks(QPainter& p, const QRectF& r, const QString& group, bool coSelected) {
  if (group.isEmpty() && !coSelected) return;
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  if (!group.isEmpty()) {
    // バーの下の端に、グループの色の線
    QPainterPath shape;
    shape.addRoundedRect(r, 3, 3);
    p.setClipPath(shape);
    p.fillRect(QRectF(r.left(), r.bottom() - 3, r.width(), 3), groupColor(group));
    p.setClipping(false);
  }
  if (coSelected) {
    // 一緒に扱うもの: 白い点線の枠
    p.setPen(QPen(QColor(255, 255, 255, 220), 1.4, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 3, 3);
  }
  p.restore();
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
  for (const auto& o : ed.project().overlays) {
    if (!o.anim.empty()) marks(o.anim, o.start, overlayDuration(o, ed.project().settings.fps), overlayRect(o));
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
  } else if (h.row == Row::Overlay) {
    for (const auto& o : ed.project().overlays) {
      const QRectF r = overlayRect(o);
      if (r.contains(p)) {
        h.type = Selection::Overlay;
        h.id = o.id;
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
  for (const auto& o : ed.project().overlays) {
    if (o.id == exclude) continue;
    pts.push_back(o.start);
    pts.push_back(o.start + overlayDuration(o, ed.project().settings.fps));
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
  auto& byHeight = thumbCache_[assetId];
  auto it = byHeight.find(height);
  if (it != byHeight.end()) return it->second;
  // 段の高さを変えると高さの違う画像が増えるので、古い高さのものは少しだけ残す
  if (byHeight.size() > 3) byHeight.clear();
  std::vector<QPixmap> out;
  if (const auto* imgs = editor().thumbs(assetId)) {
    for (const QImage& img : *imgs) out.push_back(QPixmap::fromImage(img.scaledToHeight(height, Qt::SmoothTransformation)));
  }
  return byHeight[height] = std::move(out);
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
  const int innerH = videoH_ - 8 - 4;
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
    // フェード（イン・アウトは重ならない長さにしてあるので、それぞれバーの幅まで描く）
    if (c.fadeIn > 0) {
      const double w = std::min(r.width(), c.fadeIn * pps());
      QLinearGradient g(r.left(), 0, r.left() + w, 0);
      g.setColorAt(0, QColor(0, 0, 0, 190));
      g.setColorAt(1, QColor(0, 0, 0, 0));
      p.fillRect(QRectF(r.left(), r.top(), w, r.height()), g);
    }
    if (c.fadeOut > 0) {
      const double w = std::min(r.width(), c.fadeOut * pps());
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
    // フェードのつまみ（選んでいるクリップと、カーソルの下のクリップ。動かしている途中のクリップには出さない）
    const bool moving = drag_ == Drag::ClipMove && dragMoved_ && c.id == dragId_;
    if (!moving && !broken) paintFadeHandles(p, r, c.fadeIn, c.fadeOut, sel || c.id == hoverId_);
  }
}

void TimelineView::paintItems(QPainter& p, Selection::Type type) {
  const Editor& ed = editor();
  const QString selected = ed.selection().type == type ? ed.selection().id : QString();
  const std::vector<QString> co = coSelectedIds();
  QFont f = font();
  f.setPointSizeF(8);
  p.setFont(f);
  const QFontMetricsF fm(f);
  auto drawOne = [&](const QString& id, double start, double dur, int lane, const QString& label, const AudioItem* audio, const QString& group) {
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
        const double w2 = std::min(r.width(), audio->fadeIn * pps());
        QLinearGradient g(r.left(), 0, r.left() + w2, 0);
        g.setColorAt(0, QColor(0, 0, 0, 190));
        g.setColorAt(1, QColor(0, 0, 0, 0));
        p.fillRect(QRectF(r.left(), r.top(), w2, r.height()), g);
      }
      if (audio->fadeOut > 0) {
        const double w2 = std::min(r.width(), audio->fadeOut * pps());
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
    // グループの印・一緒に選んでいるものの枠
    paintGroupMarks(p, r, group, std::find(co.begin(), co.end(), id) != co.end());
    // フェードのつまみ（オーディオだけ。選んでいる物と、カーソルの下の物）
    if (audio) paintFadeHandles(p, r, audio->fadeIn, audio->fadeOut, sel || id == hoverId_);
  };
  if (type == Selection::Text) {
    for (const auto& t : ed.project().texts) {
      QString label = t.text;
      label.replace(QLatin1Char('\n'), QLatin1Char(' '));
      if (label.isEmpty()) label = QStringLiteral("テキスト");
      drawOne(t.id, t.start, t.duration, textLanes_.lane.count(t.id) ? textLanes_.lane.at(t.id) : 0, label, nullptr, t.group);
    }
  } else {
    for (const auto& a : ed.project().audios) {
      const Asset* as = ed.asset(a.assetId);
      drawOne(a.id, a.start, audioDuration(a), audioLanes_.lane.count(a.id) ? audioLanes_.lane.at(a.id) : 0,
              as ? as->name : QStringLiteral("ファイルが見つかりません"), &a, a.group);
    }
  }
}

void TimelineView::paintOverlays(QPainter& p) {
  const Editor& ed = editor();
  const QString selected = ed.selection().type == Selection::Overlay ? ed.selection().id : QString();
  const std::vector<QString> co = coSelectedIds();
  const int innerH = std::max(8, overlayLaneH_ - 4 - 4);
  QFont f = font();
  f.setPointSizeF(8);
  p.setFont(f);
  const QFontMetricsF fm(f);
  for (const auto& o : ed.project().overlays) {
    const Asset* a = ed.asset(o.assetId);
    const QRectF r = overlayRect(o);
    if (r.right() < kHeadW || r.left() > viewport()->width()) continue;
    const bool sel = o.id == selected;
    const bool broken = !a || a->status == AssetStatus::Missing || a->status == AssetStatus::Error;
    QPainterPath shape;
    shape.addRoundedRect(r, 3, 3);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setClipPath(shape);
    p.fillRect(r, broken ? QColor(0x4a, 0x20, 0x26) : QColor(0x3d, 0x2f, 0x5c));
    // サムネイルを並べる（メインのビデオと同じ）
    if (a && !broken) {
      const auto& thumbs = thumbPixmaps(o.assetId, innerH);
      if (!thumbs.empty()) {
        const double ratio = a->width > 0 && a->height > 0 ? double(a->width) / a->height : 16.0 / 9;
        const int tileW = std::max(20, int(std::lround(innerH * std::clamp(ratio, 0.5, 2.2))));
        const int n = std::min(400, std::max(1, int(std::ceil(r.width() / tileW))));
        for (int k = 0; k < n; ++k) {
          const double tx = r.left() + k * tileW;
          if (tx + tileW < kHeadW || tx > viewport()->width()) continue;
          size_t idx = 0;
          if (!o.isImage && a->duration > 0) {
            const double local = (k + 0.5) * tileW / pps();
            const double src = o.in + local * o.speed;
            idx = size_t(std::clamp(int(std::floor(src / a->duration * double(thumbs.size()))), 0, int(thumbs.size()) - 1));
          }
          const QPixmap& pm = thumbs[idx];
          const QRectF dst(tx, r.top() + 2, tileW, innerH);
          const QRectF srcR = QRectF(QPointF(0, 0), QSizeF(pm.size()) / pm.devicePixelRatio());
          const double k2 = std::max(dst.width() / srcR.width(), dst.height() / srcR.height());
          const QSizeF sz(dst.width() / k2, dst.height() / k2);
          p.setOpacity(0.85);
          p.drawPixmap(dst, pm, QRectF((srcR.width() - sz.width()) / 2, (srcR.height() - sz.height()) / 2, sz.width(), sz.height()));
          p.setOpacity(1);
        }
      }
    }
    // フェード
    if (o.fadeIn > 0) {
      const double w = std::min(r.width(), o.fadeIn * pps());
      QLinearGradient g(r.left(), 0, r.left() + w, 0);
      g.setColorAt(0, QColor(0, 0, 0, 190));
      g.setColorAt(1, QColor(0, 0, 0, 0));
      p.fillRect(QRectF(r.left(), r.top(), w, r.height()), g);
    }
    if (o.fadeOut > 0) {
      const double w = std::min(r.width(), o.fadeOut * pps());
      QLinearGradient g(r.right() - w, 0, r.right(), 0);
      g.setColorAt(0, QColor(0, 0, 0, 0));
      g.setColorAt(1, QColor(0, 0, 0, 190));
      p.fillRect(QRectF(r.right() - w, r.top(), w, r.height()), g);
    }
    // 名前（クロマキー・速度の印も）
    if (r.width() > 40) {
      QString badge;
      if (o.chroma.enabled) badge += QStringLiteral("キー ");
      if (!o.isImage && std::abs(o.speed - 1) > 1e-9) badge += num6(o.speed) + QLatin1Char('x');
      QString label = a ? a->name : QStringLiteral("ファイルが見つかりません");
      const double lx = r.left() + 6;
      double avail = r.right() - 6 - lx - 16;
      if (!badge.isEmpty()) avail -= fm.horizontalAdvance(badge) + 10;
      label = fm.elidedText(label, Qt::ElideRight, std::max(0.0, avail - 8));
      double x = lx;
      p.drawPixmap(QPointF(x, r.top() + 4), theme::iconPixmap(o.isImage ? QStringLiteral("image") : QStringLiteral("film"), 12, Qt::white, devicePixelRatioF()));
      x += 16;
      if (!label.isEmpty()) {
        const QRectF br(x, r.top() + 3, fm.horizontalAdvance(label) + 8, 15);
        p.fillRect(br, QColor(0, 0, 0, 140));
        p.setPen(Qt::white);
        p.drawText(br, Qt::AlignCenter, label);
        x = br.right() + 4;
      }
      if (!badge.isEmpty()) {
        const QString b = badge.trimmed();
        const QRectF br(x, r.top() + 3, fm.horizontalAdvance(b) + 8, 15);
        p.fillRect(br, QColor(0x26, 0x80, 0xeb, 200));
        p.setPen(Qt::white);
        p.drawText(br, Qt::AlignCenter, b);
      }
    }
    p.restore();
    // 枠と端のつまみ
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(sel ? QColor(Qt::white) : broken ? QColor(0x93, 0x34, 0x3f) : QColor(0x6b, 0x55, 0x9e), sel ? 2 : 1));
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
    paintGroupMarks(p, r, o.group, std::find(co.begin(), co.end(), o.id) != co.end());
    if (!broken) paintFadeHandles(p, r, o.fadeIn, o.fadeOut, sel || o.id == hoverId_);
  }
}

void TimelineView::paintEvent(QPaintEvent*) {
  QPainter p(viewport());
  const QRect area = viewport()->rect();
  const Editor& ed = editor();
  p.fillRect(area, QColor(0x15, 0x15, 0x15));
  // トラックの背景
  const int ty = textY();
  const int oy = overlayY();
  const int vy = videoY();
  const int ay = audioY();
  if (audioDropHover_) p.fillRect(QRect(kHeadW, ay, area.width(), audioH()), QColor(63, 182, 123, 30));
  p.fillRect(QRect(kHeadW, oy, area.width(), overlayH()), QColor(0x17, 0x14, 0x1d));
  if (overlayDropT_) p.fillRect(QRect(kHeadW, oy, area.width(), overlayH()), QColor(0x8a, 0x6c, 0xd8, 36));
  p.fillRect(QRect(kHeadW, vy, area.width(), videoH_), QColor(0x12, 0x12, 0x12));
  for (int y : {oy, vy, ay, ay + audioH()}) p.fillRect(QRect(0, y - 1, area.width(), 1), theme::border);
  if (ed.layout().items.empty()) {
    p.setPen(theme::dim);
    p.drawText(QRect(kHeadW + 12, vy, 400, videoH_), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("メディアをここにドラッグ"));
  }
  if (ed.project().overlays.empty()) {
    p.setPen(theme::dim);
    p.drawText(QRect(kHeadW + 12, oy, std::max(100, area.width() - kHeadW - 24), overlayH()), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("写真・動画をここにドラッグすると、映像の上に重ねられます"));
  }

  paintItems(p, Selection::Text);
  paintOverlays(p);
  paintVideo(p);
  paintItems(p, Selection::Audio);
  // 重ねるトラックへドラッグしてきている素材の、置く時間
  if (overlayDropT_) {
    const double x = xOf(*overlayDropT_);
    if (x >= kHeadW) {
      p.setPen(QPen(QColor(0xc9, 0xb6, 0xff), 1.5, Qt::DashLine));
      p.drawLine(QPointF(x, oy), QPointF(x, vy));
    }
  }
  paintKeyMarkers(p);
  paintFadeLabel(p);

  // 挿入位置
  std::optional<double> ind;
  if (drag_ == Drag::ClipMove && dragMoved_) ind = indicatorX(dragIndex_, dragId_);
  if (dropX_) ind = dropX_;
  if (ind) p.fillRect(QRectF(*ind - 1, vy, 2, videoH_), QColor(0xff, 0xd4, 0x4a));

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
  head(oy, overlayH(), QStringLiteral("image"), QStringLiteral("重ねる"));
  head(vy, videoH_, QStringLiteral("film"), QStringLiteral("ビデオ"));
  head(ay, audioH(), QStringLiteral("music"), QStringLiteral("オーディオ"));

  // 目盛り（上に固定）
  paintRuler(p, area);
  p.fillRect(QRect(0, 0, kHeadW, kRulerH), theme::panel);
  p.fillRect(QRect(kHeadW - 1, 0, 1, kRulerH), theme::border);
  p.fillRect(QRect(0, kRulerH - 1, kHeadW, 1), theme::border);
  p.setPen(theme::dim);
  p.drawText(QRect(10, 0, kHeadW - 12, kRulerH), Qt::AlignVCenter | Qt::AlignLeft, formatTime(total, false));
  // 動画全体の再生速度を変えているときは、左上の角にその速さを出す（タイムラインの時間は元の速さのまま）
  if (const double sp = ed.project().settings.speed; std::abs(sp - 1) > 1e-9) {
    p.setPen(theme::accent);
    p.drawText(QRect(10, 0, kHeadW - 14, kRulerH), Qt::AlignVCenter | Qt::AlignRight, QString::number(sp, 'g', 4) + QLatin1Char('x'));
  }
  // 書き出す範囲（目盛りの上の帯と、両端の線）
  paintRange(p, area);

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
  const double t = tOf(dragX(x));  // 見出しの列の上では、見えている左端の時間（赤い線が見出しの下に隠れない）
  snapMark_ = snapOn ? snap(t, dragSnap_) : std::nullopt;
  ed.seek(snapMark_.value_or(t));
  // 内容の終わりより後ろの数字に吸着して、seek で終わりに止められたときは目印を出さない
  if (snapMark_ && ed.currentTime() != *snapMark_) snapMark_.reset();
}

void TimelineView::mousePressEvent(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton) return;
  // 前のドラッグで離した合図が届いていなかった: 先にそれを終えてから、新しく始める
  if (drag_ != Drag::None) finishDrag(dragPos_);
  Editor& ed = editor();
  const QPointF pos = e->position();
  armAutoScroll(pos.x());
  pressPos_ = pos;
  dragPos_ = pos;
  dragMods_ = e->modifiers();
  dragMoved_ = false;
  dragDx_ = 0;
  snapMark_.reset();
  stopAutoScroll();
  merge_ = QStringLiteral("tl-") + newId();
  // 見出しの列の、段の境目: ドラッグで段の高さを変える
  if (const int border = trackBorderAt(pos); border >= 0) {
    resizeWhich_ = border;
    resizeStartY_ = pos.y();
    resizeOrig_[0] = textLaneH_;
    resizeOrig_[1] = overlayLaneH_;
    resizeOrig_[2] = videoH_;
    resizeOrig_[3] = audioLaneH_;
    drag_ = Drag::TrackResize;
    return;
  }
  // 見出しの列のクリックは無視する。左上の角（目盛りの行）だけは、目盛りと同じく再生位置を動かす
  if (pos.x() < kHeadW && pos.y() >= kRulerH) return;
  ed.pause();
  // 目盛りの上: 書き出す範囲の端を掴んだら、その端を動かす。Shift を押しながらなら、新しく範囲を作る
  if (pos.y() < kRulerH && pos.x() >= kHeadW) {
    const int edge = rangeEdgeAt(pos);
    if (edge != 0 || (e->modifiers() & Qt::ShiftModifier)) {
      origRange_ = ed.exportRange();
      dragSnap_ = snapPoints(QString(), true);
      if (edge != 0) {
        rangeEdge_ = edge;
        drag_ = Drag::RangeEdge;
      } else {
        const double t0 = std::clamp(tOf(pos.x()), 0.0, ed.layout().total);
        const std::optional<double> s = (e->modifiers() & Qt::AltModifier) ? std::nullopt : snap(t0, dragSnap_);
        rangeAnchor_ = s.value_or(t0);
        drag_ = Drag::Range;
      }
      viewport()->setCursor(Qt::SizeHorCursor);
      return;
    }
  }
  // フェードのつまみ（バーの上の角）を掴んだら、フェードの長さを変える
  if (const FadeHit fh = fadeHitTest(pos); fh.type != Selection::None) {
    keySel_ = {};
    ed.select({fh.type, fh.id});
    dragId_ = fh.id;
    dragType_ = fh.type;
    fadeSide_ = fh.side;
    drag_ = Drag::Fade;
    viewport()->update();
    return;
  }
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
  const Selection hitSel{h.type, h.id};
  // Ctrl+クリック: 一緒に選ぶものに加える / 外す（テキスト・オーディオ・重ねるトラック。動かさない）
  if (e->modifiers() & Qt::ControlModifier) {
    ed.toggleSelect(hitSel);
    viewport()->update();
    return;
  }
  // 一緒に選んでいるもの・同じグループのものを押したときは、まとめて動かせるよう、一緒に選んだままにする
  pressInSet_ = h.handle == 0 && Editor::groupable(h.type) && ed.inSelectionSet(hitSel) && ed.selectionSet().size() > 1;
  if (pressInSet_) ed.focusInSelection(hitSel);
  else ed.select(hitSel);
  dragId_ = h.id;
  dragType_ = h.type;
  dragSide_ = h.handle;
  pressTime_ = tOf(pos.x());
  dragSnap_ = snapPoints(h.id, true);
  companions_.clear();
  if (h.type == Selection::Clip) {
    origClip_ = *ed.clip(h.id);
    drag_ = h.handle != 0 ? Drag::ClipTrim : Drag::ClipMove;
    return;
  }
  if (h.type == Selection::Text) {
    origText_ = *ed.text(h.id);
    drag_ = h.handle != 0 ? Drag::ItemTrim : Drag::ItemMove;
  } else if (h.type == Selection::Overlay) {
    origOverlay_ = *ed.overlay(h.id);
    drag_ = h.handle != 0 ? Drag::ItemTrim : Drag::ItemMove;
  } else {
    origAudio_ = *ed.audio(h.id);
    drag_ = h.handle != 0 ? Drag::ItemTrim : Drag::ItemMove;
    // ドラッグのあいだ、このオーディオは元の段にとどめる（ほかの物と重なる間だけ、空いている段へ移る）。ほかのオーディオの段は変えない
    laneHoldId_ = h.id;
    laneHold_ = audioLanes_.lane.count(h.id) ? audioLanes_.lane.at(h.id) : 0;
  }
  // 動かすときは、一緒に選んでいるもの・同じグループのものも、同じだけ動かす（端の長さの変更は、押したものだけ）
  if (drag_ == Drag::ItemMove) {
    const ProjectData& p = ed.project();
    for (const Selection& s : ed.selectionSet()) {
      if (s == hitSel || !Editor::groupable(s.type)) continue;
      double start = -1, end = -1;
      if (s.type == Selection::Text) {
        if (const TextItem* t = ed.text(s.id)) start = t->start, end = t->start + t->duration;
      } else if (s.type == Selection::Audio) {
        if (const AudioItem* a = ed.audio(s.id)) start = a->start, end = a->start + audioDuration(*a);
      } else if (const OverlayItem* o = ed.overlay(s.id)) {
        start = o->start, end = o->start + overlayDuration(*o, p.settings.fps);
      }
      if (start < 0) continue;
      companions_.push_back({s, start});
      // 一緒に動くものの端には吸着させない（動いていくので意味がない）
      dragSnap_.erase(std::remove_if(dragSnap_.begin(), dragSnap_.end(),
                                     [&](double v) { return std::abs(v - start) < 1e-9 || std::abs(v - end) < 1e-9; }),
                      dragSnap_.end());
    }
  }
}

void TimelineView::mouseMoveEvent(QMouseEvent* e) {
  const QPointF pos = e->position();
  if (drag_ == Drag::None) {
    const Hit h = hitTest(pos);
    const FadeHit fh = fadeHitTest(pos);
    // 点の上では「動かせる」カーソル（上下で音量、左右で時間）。端・フェードのつまみ・範囲の端では左右のカーソル。
    // 見出しの列の段の境目では、上下のカーソル（段の高さを変える）
    const bool horizontal = fh.type != Selection::None || h.handle != 0 || rangeEdgeAt(pos) != 0;
    viewport()->setCursor(trackBorderAt(pos) >= 0 ? Qt::SplitVCursor
                          : keyHitTest(pos).index >= 0 ? Qt::SizeAllCursor
                          : horizontal                 ? Qt::SizeHorCursor
                                                       : Qt::ArrowCursor);
    // カーソルの下の映像・オーディオには、フェードのつまみを出す
    const QString hover = fh.type != Selection::None                                                                   ? fh.id
                          : (h.type == Selection::Clip || h.type == Selection::Audio || h.type == Selection::Overlay) ? h.id
                                                                                                                       : QString();
    if (hover != hoverId_) {
      hoverId_ = hover;
      viewport()->update();
    }
    return;
  }
  // ボタンを離した合図を受け取りそこねていた（ドラッグ中にほかの画面が前に出た等）: 最後の位置でドラッグを終える
  if (!(e->buttons() & Qt::LeftButton)) {
    finishDrag(dragPos_);
    return;
  }
  // 見出しの列の上や、タイムラインの外（ウィンドウの外も）へ出ても、ボタンを押している間は、このビューにマウスの動きが届く
  // （押したときに Qt が自動でマウスをつかみ、離すまで放さない）。位置はこのビューの座標で、外なら負や幅より大きい値になる
  dragPos_ = pos;
  dragMods_ = e->modifiers();
  updateArming(pos.x());
  dragStep();
  // 端に近づいたら、自動スクロールを始める。マウスを止めても、端にいる間は続く（離れる・ドラッグが終わると止まる）
  QPointF src;
  if (autoScrollSource(&src) && autoScrollTarget(pos.x()) != 0) startAutoScroll();
}

void TimelineView::dragStep() {
  if (drag_ == Drag::KeyMove) keyMoveTo(dragPos_);
  else dragTo(dragPos_, dragMods_);
}

void TimelineView::keyMoveTo(QPointF pos) {
  Editor& ed = editor();
  // 掴んだ点だけを動かす。上下 = 音量（0〜100%）、左右 = 音量が変わる時間。少し動くまでは何もしない（クリックで選ぶだけ）
  if (!dragMoved_ && std::hypot(pos.x() - pressPos_.x(), pos.y() - pressPos_.y()) < 3) return;
  if (!dragMoved_ && !ed.editSessionActive()) {
    ed.beginEditSession();  // Esc でやめたときに、元に戻せるようにする
    dragSession_ = true;
  }
  dragMoved_ = true;
  const AudioItem* cur = ed.audio(dragId_);
  if (!cur) return;
  const QRectF r = audioRect(*cur);
  const double volume = std::round(yToVolume(r, pos.y()) * 100) / 100;  // 1% 刻み（表示も整数の %）
  ed.moveVolumePoint(dragId_, dragKey_, tOf(dragX(pos.x())) - cur->start, volume, merge_);
  viewport()->update();
}

void TimelineView::dragTo(QPointF pos, Qt::KeyboardModifiers mods) {
  Editor& ed = editor();
  // マウスの位置の時間 − 押したときの位置の時間。ピクセルではなく時間で測るので、スクロールやズームで表示が動いても、
  // ドラッグしているものがカーソルからずれない。見出しの列の上やタイムラインの外では、見えている端の位置にとどめる
  const double x = dragX(pos.x());
  const double dt = tOf(x) - pressTime_;
  const double dx = dt * pps();
  // 動かし始めの判定は、マウスが実際に動いた量で見る（端の近くで押して外へ出たときも、ちゃんと動かし始める）。
  // テキストは上下にも動かせる（段を変える）ので、縦の動きでも動かし始める
  const int threshold = drag_ == Drag::ClipMove ? 6 : drag_ == Drag::ItemMove ? 4 : 0;
  const bool vertical = drag_ == Drag::ItemMove && (dragType_ == Selection::Text || dragType_ == Selection::Overlay);
  const double moved = vertical ? std::max(std::abs(pos.x() - pressPos_.x()), std::abs(pos.y() - pressPos_.y())) : std::abs(pos.x() - pressPos_.x());
  if (!dragMoved_ && moved < threshold) return;
  const bool first = !dragMoved_;
  dragMoved_ = true;
  // 動かしながら編集データを変えるドラッグは、Esc でやめたときに元に戻せるよう、始めの状態を覚える
  if (first && (drag_ == Drag::ClipTrim || drag_ == Drag::ItemMove || drag_ == Drag::ItemTrim || drag_ == Drag::Fade) &&
      !ed.editSessionActive()) {
    ed.beginEditSession();
    dragSession_ = true;
  }
  const int fps = ed.project().settings.fps;
  // Alt を押している間は、どの吸着も効かない
  const bool snapOn = !(mods & Qt::AltModifier);
  switch (drag_) {
    case Drag::Scrub:
      scrubTo(x, snapOn);
      break;
    case Drag::TrackResize: {
      // 境目を動かした分だけ、その部分の高さを変える（テキスト・重ねるトラック・オーディオは段の数で割って、1 段の高さにする）
      const double dy = pos.y() - resizeStartY_;
      int h[4] = {resizeOrig_[0], resizeOrig_[1], resizeOrig_[2], resizeOrig_[3]};
      if (resizeWhich_ == 0) h[0] = int(std::lround(resizeOrig_[0] + dy / std::max(1, textLanes_.count)));
      else if (resizeWhich_ == 1) h[1] = int(std::lround(resizeOrig_[1] + dy / std::max(1, overlayLanes_.count)));
      else if (resizeWhich_ == 2) h[2] = int(std::lround(resizeOrig_[2] + dy));
      else h[3] = int(std::lround(resizeOrig_[3] + dy / std::max(1, audioLanes_.count)));
      setTrackHeights(h[0], h[1], h[2], h[3]);
      break;
    }
    case Drag::Fade:
      fadeTo(pos);
      break;
    case Drag::Range:
    case Drag::RangeEdge: {
      // 書き出す範囲は、内容の点と再生位置・目盛りの数字に吸着させる（Alt で吸着しない）。1 フレームより短くはしない
      const double total = ed.layout().total;
      const double frame = 1.0 / std::max(1, fps);
      double t = std::clamp(tOf(x), 0.0, total);
      if (snapOn) {
        if (const std::optional<double> s = snap(t, dragSnap_)) t = std::clamp(*s, 0.0, total);
      }
      if (drag_ == Drag::Range) {
        const double a = std::clamp(rangeAnchor_, 0.0, total);
        if (std::abs(t - a) >= frame - 1e-9) ed.setRange(a, t);
      } else if (origRange_) {
        const double a = rangeEdge_ < 0 ? std::min(t, origRange_->end - frame) : origRange_->start;
        const double b = rangeEdge_ < 0 ? origRange_->end : std::max(t, origRange_->start + frame);
        if (b - a >= frame - 1e-9 && a >= 0 && b <= total + 1e-9) ed.setRange(a, b);
      }
      break;
    }
    case Drag::ClipMove:
      dragDx_ = dx;
      dragIndex_ = insertionIndex(x, dragId_);
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
      const double origStart = dragType_ == Selection::Text      ? origText_.start
                               : dragType_ == Selection::Overlay ? origOverlay_.start
                                                                 : origAudio_.start;
      const double dur = dragType_ == Selection::Text      ? origText_.duration
                         : dragType_ == Selection::Overlay ? overlayDuration(origOverlay_, fps)
                                                           : audioDuration(origAudio_);
      // 一緒に動かすものがあるときは、どれも 0:00 より前へ出ないところまで
      double minStart = origStart;
      for (const Companion& c : companions_) minStart = std::min(minStart, c.start);
      const double floor0 = origStart - minStart;
      double start = std::max(floor0, origStart + dt);
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
      start = std::max(floor0, start);
      const double delta = start - origStart;
      // 上下に動かすと、カーソルのある段へ移す（テキストは一番下の段のさらに下へ、重ねるトラックは一番上の段のさらに上へ 1 段だけ
      // 増やせる）。段の外に出たときは、今の段のまま
      const int lane = dragType_ == Selection::Text ? textLaneAt(pos.y()) : dragType_ == Selection::Overlay ? overlayLaneAt(pos.y()) : -1;
      const QString id = dragId_;
      const Selection::Type type = dragType_;
      const std::vector<Companion> comps = companions_;
      ed.commit([&](ProjectData& p) {
        bool changed = false;
        auto moveOne = [&](Selection::Type ty, const QString& oid, double s, bool main) {
          if (ty == Selection::Text) {
            for (auto& t : p.texts) {
              if (t.id != oid) continue;
              t.start = s;
              if (main && lane >= 0) t.track = lane;
              changed = true;
            }
          } else if (ty == Selection::Overlay) {
            for (auto& o : p.overlays) {
              if (o.id != oid) continue;
              o.start = s;
              if (main && lane >= 0) o.track = lane;
              changed = true;
            }
          } else if (ty == Selection::Audio) {
            for (auto& a : p.audios) {
              if (a.id != oid) continue;
              a.start = s;
              changed = true;
            }
          }
        };
        moveOne(type, id, start, true);
        for (const Companion& c : comps) moveOne(c.sel.type, c.sel.id, std::max(0.0, c.start + delta), false);
        return changed;
      }, merge_);
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
      } else if (dragType_ == Selection::Overlay) {
        // 重ねるトラック: 右端は長さ（動画は素材の終わりまで）、左端は開始と切り出しの始まりを一緒に動かす（同じ時間に同じ映像が出る）
        const OverlayItem o = origOverlay_;
        const Asset* a = ed.asset(o.assetId);
        const double assetDur = a && a->duration > 0 ? a->duration : o.out;
        const double origDur = overlayDuration(o, fps);
        ed.updateOverlay(o.id, [&](OverlayItem& x) {
          if (dragSide_ > 0) {
            const double raw = o.start + origDur + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            const double nd = std::max(kMinClipDuration, s.value_or(raw) - o.start);
            if (o.isImage) x.out = o.in + std::min(600.0, nd);
            else x.out = std::min(assetDur, std::max(o.in + kMinClipDuration * o.speed, o.in + nd * o.speed));
            if (s && std::abs(x.start + overlayDuration(x, fps) - *s) < 1e-6) snapMark_ = s;
          } else {
            const double raw = o.start + dt;
            const std::optional<double> s = snapOn ? snap(raw, dragSnap_) : std::nullopt;
            double headCut = s.value_or(raw) - o.start;  // 左端を詰めた長さ（負なら広げた）
            double lo = -o.start;                        // 0:00 より前には出さない
            if (!o.isImage) lo = std::max(lo, -o.in / std::max(0.01, o.speed));  // 素材の始まりより前には広げない
            headCut = std::clamp(headCut, lo, origDur - kMinClipDuration);
            x.start = o.start + headCut;
            if (o.isImage) x.out = o.out - headCut;
            else x.in = o.in + headCut * o.speed;
            x.anim = trimAnimationStart(o.anim, headCut);
            x.volumePoints = trimVolumePointsStart(o.volumePoints, o.volume, headCut);
            if (s && std::abs(x.start - *s) < 1e-6) snapMark_ = s;
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
  if (e->button() != Qt::LeftButton) return;  // ドラッグは左ボタンで始めるので、左ボタンを離したときに終える
  finishDrag(e->position());
}

void TimelineView::finishDrag(QPointF pos) {
  Editor& ed = editor();
  stopAutoScroll();  // ドラッグが終わったら、自動スクロールは確実に止める
  if (drag_ != Drag::None) rememberView();  // このあと中身が短くなっても、今の表示位置は動かさない
  if (drag_ == Drag::TrackResize) saveTrackHeights();
  if (drag_ == Drag::ClipMove) {
    // スクロールしていても、いまの表示でマウスがある位置（そこの時間）に置く。見出しの列の上などで離したときは、見えている端の位置
    if (dragMoved_) ed.moveClip(dragId_, insertionIndex(dragX(pos.x()), dragId_));
    else ed.seek(pressTime_);
  } else if (drag_ == Drag::ItemMove && !dragMoved_) {
    // 一緒に選んでいたものを押して、動かさずに離した: 押したものだけを選ぶ（同じグループのものは一緒のまま）
    if (pressInSet_) ed.select({dragType_, dragId_});
    ed.seek(pressTime_);
  }
  pressInSet_ = false;
  companions_.clear();
  drag_ = Drag::None;
  dragMoved_ = false;
  dragDx_ = 0;
  dragIndex_ = -1;
  fadeSide_ = 0;
  rangeEdge_ = 0;
  origRange_.reset();
  snapMark_.reset();
  if (dragSession_) {
    dragSession_ = false;
    ed.endEditSession();
  }
  ed.endMerge();
  laneHoldId_.clear();
  laneHold_ = -1;
  updateScrollBars();
  viewport()->update();
}

bool TimelineView::isDragging() const {
  return drag_ == Drag::ClipMove || drag_ == Drag::ClipTrim || drag_ == Drag::ItemMove || drag_ == Drag::ItemTrim || drag_ == Drag::KeyMove ||
         drag_ == Drag::Fade || drag_ == Drag::Range || drag_ == Drag::RangeEdge || drag_ == Drag::TrackResize;
}

bool TimelineView::cancelDrag() {
  if (!isDragging()) return false;
  Editor& ed = editor();
  stopAutoScroll();
  rememberView();  // 元に戻して中身が短くなっても、今の表示位置は動かさない
  // 書き出す範囲のドラッグは、ドラッグを始める前の範囲に戻す（範囲は編集データではないので、履歴には関係しない）
  if (drag_ == Drag::Range || drag_ == Drag::RangeEdge) {
    if (origRange_) ed.setRange(origRange_->start, origRange_->end);
    else ed.clearRange();
  }
  // 段の高さのドラッグは、始める前の高さに戻す
  if (drag_ == Drag::TrackResize) setTrackHeights(resizeOrig_[0], resizeOrig_[1], resizeOrig_[2], resizeOrig_[3]);
  pressInSet_ = false;
  companions_.clear();
  drag_ = Drag::None;
  dragMoved_ = false;
  dragDx_ = 0;
  dragIndex_ = -1;
  fadeSide_ = 0;
  rangeEdge_ = 0;
  origRange_.reset();
  snapMark_.reset();
  // 動かしながら変えていた分は、始める前の状態に戻す（元に戻す履歴には何も残さない）。
  // オーディオは、ドラッグを始めたときの段に戻す（laneHoldId_ を消すのは、戻したあと）
  if (dragSession_) {
    dragSession_ = false;
    ed.cancelEditSession();
  }
  ed.endMerge();
  laneHoldId_.clear();
  laneHold_ = -1;
  updateScrollBars();
  viewport()->update();
  return true;
}

// ---- ドラッグ中の自動スクロール -----------------------------------------------------------------

namespace {
constexpr double kAutoEdge = 50;  // 表示領域の端からこの距離（px）以内に入ったら、スクロールを始める

/** 端からの距離 d（px。端の外は負）→ スクロールの速さ（px/秒）。50px で低速、30px で中速、15px で高速、端で最高速。間は直線でつなぐ */
double edgeSpeed(double d) {
  struct P {
    double d;
    double v;
  };
  static constexpr P pts[] = {{kAutoEdge, 60}, {30, 280}, {15, 700}, {0, 1200}, {-80, 1600}};
  if (d >= kAutoEdge) return 0;
  for (size_t i = 0; i + 1 < std::size(pts); ++i) {
    if (d >= pts[i + 1].d) {
      const double t = (pts[i].d - d) / (pts[i].d - pts[i + 1].d);
      return pts[i].v + (pts[i + 1].v - pts[i].v) * t;
    }
  }
  return pts[std::size(pts) - 1].v;  // 領域の外へ大きく出ても、速さはここまで
}
}  // namespace

QPair<double, double> TimelineView::autoScrollEdges() const {
  double left = kHeadW;  // 時間の目盛りが始まる位置（左の見出しの列の右）
  double right = viewport()->width();
  // ウィンドウが画面からはみ出していて、表示領域の端がどの画面にも入っていないときは、カーソルがいる画面の端を端とみなす
  // （そうしないと、カーソルが表示領域の端の近くまで行けず、自動スクロールが始まらない）
  const QScreen* here = QGuiApplication::screenAt(QCursor::pos());
  if (here) {
    const QRect g = here->geometry();
    const int y = viewport()->height() / 2;
    if (!QGuiApplication::screenAt(viewport()->mapToGlobal(QPoint(std::max(0, viewport()->width() - 1), y)))) {
      right = std::min(right, double(viewport()->mapFromGlobal(QPoint(g.right() + 1, g.top() + y)).x()));
    }
    if (!QGuiApplication::screenAt(viewport()->mapToGlobal(QPoint(0, y)))) {
      left = std::max(left, double(viewport()->mapFromGlobal(QPoint(g.left(), g.top() + y)).x()));
    }
  }
  return {left, right};
}

double TimelineView::autoScrollTarget(double x) const {
  const auto [left, right] = autoScrollEdges();
  // 左端の 50px より左（見出しの列の上・タイムラインの外も）は左へ、右端の 50px より右（外も）は右へ。端より外は最高速
  if (x < left + kAutoEdge) return armedLeft_ ? -edgeSpeed(x - left) : 0;
  if (x > right - kAutoEdge) return armedRight_ ? edgeSpeed(right - x) : 0;
  return 0;
}

double TimelineView::dragX(double x) const {
  const auto [left, right] = autoScrollEdges();
  return std::clamp(x, left, std::max(left, right));
}

namespace {
constexpr double kArmMove = 8;  // 端の近くで始めたドラッグは、その端の方へこれだけ動かすと、自動スクロールが効くようになる
}

void TimelineView::armAutoScroll(double x) {
  const auto [left, right] = autoScrollEdges();
  armRefX_ = x;
  armedLeft_ = x >= left + kAutoEdge;
  armedRight_ = x <= right - kAutoEdge;
}

void TimelineView::updateArming(double x) {
  const auto [left, right] = autoScrollEdges();
  if (!armedLeft_ && (x >= left + kAutoEdge || x <= armRefX_ - kArmMove)) armedLeft_ = true;
  if (!armedRight_ && (x <= right - kAutoEdge || x >= armRefX_ + kArmMove)) armedRight_ = true;
}

bool TimelineView::autoScrollSource(QPointF* pos, bool* dragging) const {
  if (dragging) *dragging = false;
  // 動かしている最中のもの。再生位置の赤い線（や、空いたところ・目盛りのドラッグ）も、音量キーフレームの点も含める
  const bool dragKind = drag_ == Drag::ClipMove || drag_ == Drag::ClipTrim || drag_ == Drag::ItemMove || drag_ == Drag::ItemTrim ||
                        drag_ == Drag::Scrub || drag_ == Drag::KeyMove || drag_ == Drag::Fade || drag_ == Drag::Range || drag_ == Drag::RangeEdge;
  if (dragMoved_ && dragKind) {
    *pos = dragPos_;
    if (dragging) *dragging = true;
    return true;
  }
  if (drop_ || libDrag_) {
    *pos = dropPos_;
    return true;
  }
  return false;
}

void TimelineView::startAutoScroll() {
  if (autoScroll_->isActive()) return;
  autoClock_.start();
  autoScroll_->start();
}

void TimelineView::stopAutoScroll() {
  if (autoScroll_) autoScroll_->stop();
  scrollVel_ = 0;
  scrollRemainder_ = 0;
}

void TimelineView::autoScrollTick() {
  QPointF pos;
  bool dragging = false;
  if (!autoScrollSource(&pos, &dragging)) {
    stopAutoScroll();
    return;
  }
  // ボタンはもう離されていた（ドラッグ中にダイアログが出て、そちらで離した等。このビューには離した合図が来ない）: そこで終える
  if (dragging && !(QGuiApplication::mouseButtons() & Qt::LeftButton)) {
    finishDrag(dragPos_);
    return;
  }
  // 実際に経った時間で動かすので、描画が遅れても速さは変わらない
  const double dt = std::clamp(autoClock_.restart() / 1000.0, 0.001, 0.05);
  const double target = autoScrollTarget(pos.x());
  // 目標の速さへなめらかに近づける（速くするときはゆっくり、遅くする・向きを変えるときは素早く）
  const bool speedingUp = std::abs(target) > std::abs(scrollVel_) && target * scrollVel_ >= 0;
  const double tau = speedingUp ? 0.10 : 0.05;
  scrollVel_ += (target - scrollVel_) * (1 - std::exp(-dt / tau));
  if (target == 0 && std::abs(scrollVel_) < 10) {
    stopAutoScroll();  // 端から離れた: 止める
    return;
  }
  scrollRemainder_ += scrollVel_ * dt;
  const int step = int(scrollRemainder_);  // スクロールバーは整数。端数は次へ持ち越す
  if (step == 0) return;
  scrollRemainder_ -= step;
  auto* sb = horizontalScrollBar();
  const int before = sb->value();
  sb->setValue(before + step);
  if (sb->value() == before) {  // スクロールできる端まで来た
    scrollVel_ = 0;
    scrollRemainder_ = 0;
    return;
  }
  // スクロールで、同じマウス位置の時間が変わった: ドラッグしているものを、カーソルに追従させ直す
  if (dragging) dragStep();
  else if (drop_) updateDrop(dropPos_);
  else refreshLibraryDrop(dropPos_);
}

void TimelineView::refreshLibraryDrop(QPointF pos) {
  dropPos_ = pos;
  updateArming(pos.x());
  const Row row = rowAt(pos.y());
  audioDropHover_ = row == Row::Audio;
  // 見出しの列の上では、見えている左端の位置に入れる
  if (row == Row::Video) dropX_ = indicatorX(insertionIndex(dragX(pos.x()), QString()), QString());
  else dropX_.reset();
  // 重ねるトラックの段の上: 落とした時間から重ねる（その時間に縦線を出す）
  if (row == Row::Overlay) overlayDropT_ = frameRound(std::max(0.0, tOf(dragX(pos.x()))), editor().project().settings.fps);
  else overlayDropT_.reset();
  viewport()->update();
}

void TimelineView::contextMenuEvent(QContextMenuEvent* e) {
  if (drag_ != Drag::None) return;  // ドラッグの途中（左ボタンを押したまま右クリック）では出さない
  const QPointF pos = e->pos();
  // 目盛りの上: 書き出す範囲のメニュー
  if (pos.y() < kRulerH && pos.x() >= kHeadW) {
    execRulerMenu(pos, e->globalPos());
    return;
  }
  // 左の見出しの列: 段の高さのメニュー
  if (pos.x() < kHeadW && pos.y() >= kRulerH) {
    execHeaderMenu(e->globalPos());
    return;
  }
  const KeyHit kh = keyHitTest(pos);
  if (kh.index < 0) {
    // クリップ・テキスト・オーディオの上: そのオブジェクトのメニュー（分割・複製・削除・フェードなど）
    const Hit h = hitTest(pos);
    if (h.type == Selection::None) return;
    keySel_ = {};
    execObjectMenu(this, {h.type, h.id}, e->globalPos(), ObjectMenuPlace::Timeline);
    viewport()->update();
    return;
  }
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

void TimelineView::execRulerMenu(QPointF pos, const QPoint& globalPos) {
  Editor& ed = editor();
  const double t = std::clamp(tOf(pos.x()), 0.0, ed.layout().total);
  QMenu menu(this);
  menu.addAction(QStringLiteral("ここを書き出す範囲の開始にする"), this, [t] { editor().setRangeStart(t); });
  menu.addAction(QStringLiteral("ここを書き出す範囲の終了にする"), this, [t] { editor().setRangeEnd(t); });
  QAction* clear = menu.addAction(QStringLiteral("書き出す範囲を解除"), this, [] { editor().clearRange(); });
  clear->setEnabled(ed.exportRange().has_value());
  menu.addSeparator();
  QAction* hint = menu.addAction(QStringLiteral("Shift を押しながら目盛りをドラッグしても決められます"));
  hint->setEnabled(false);
  menu.exec(globalPos);
}

void TimelineView::leaveEvent(QEvent* e) {
  QAbstractScrollArea::leaveEvent(e);
  if (hoverId_.isEmpty()) return;
  hoverId_.clear();
  viewport()->update();
}

// ---- フェードのつまみ -------------------------------------------------------------------

static constexpr double kFadeInset = 11;  // フェードが無いときのつまみの位置（端の長さを変えるつまみと重ならないよう、端から離す）
static constexpr double kFadeGrab = 6;    // つまみを掴める左右の範囲

bool TimelineView::fadeHandlePos(const QRectF& r, double fadeIn, double fadeOut, int side, QPointF* pos) const {
  if (r.width() < 2 * kFadeInset + 14) return false;  // バーが狭すぎるときは出さない（設定欄・右クリックで変えられる）
  const double y = r.top() + 5;
  if (side < 0) *pos = QPointF(r.left() + std::clamp(fadeIn * pps(), kFadeInset, r.width() - kFadeInset), y);
  else *pos = QPointF(r.right() - std::clamp(fadeOut * pps(), kFadeInset, r.width() - kFadeInset), y);
  return true;
}

TimelineView::FadeHit TimelineView::fadeHitTest(QPointF pos) const {
  FadeHit best;
  if (pos.x() < kHeadW) return best;
  const Editor& ed = editor();
  double bestD = kFadeGrab + 1;
  auto test = [&](Selection::Type type, const QString& id, const QRectF& r, double fadeIn, double fadeOut) {
    // つまみはバーの上の端だけ（その下は、端の長さの変更・音量キーフレームに使う）
    if (pos.y() < r.top() - 2 || pos.y() > r.top() + 9) return;
    for (int side : {-1, 1}) {
      QPointF hp;
      if (!fadeHandlePos(r, fadeIn, fadeOut, side, &hp)) continue;
      const double d = std::abs(pos.x() - hp.x());
      if (d <= kFadeGrab && d < bestD) {
        bestD = d;
        best = {type, id, side};
      }
    }
  };
  const Row row = rowAt(pos.y() + 2);
  if (row == Row::Video) {
    for (const auto& l : ed.layout().items) test(Selection::Clip, l.clip.id, clipRect(l), l.clip.fadeIn, l.clip.fadeOut);
  } else if (row == Row::Overlay) {
    for (const auto& o : ed.project().overlays) test(Selection::Overlay, o.id, overlayRect(o), o.fadeIn, o.fadeOut);
  } else if (row == Row::Audio) {
    for (const auto& a : ed.project().audios) test(Selection::Audio, a.id, audioRect(a), a.fadeIn, a.fadeOut);
  }
  return best;
}

void TimelineView::paintFadeHandles(QPainter& p, const QRectF& r, double fadeIn, double fadeOut, bool active) {
  if (!active || r.right() < kHeadW || r.left() > viewport()->width()) return;
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setClipRect(QRectF(kHeadW, 0, viewport()->width() - kHeadW, viewport()->height()));
  for (int side : {-1, 1}) {
    QPointF hp;
    if (!fadeHandlePos(r, fadeIn, fadeOut, side, &hp)) continue;
    const double fade = side < 0 ? fadeIn : fadeOut;
    // フェードの傾き（バーの下の端から、つまみの位置まで）
    if (fade > 0) {
      const double w = std::min(r.width(), fade * pps());
      const QPointF from = side < 0 ? QPointF(r.left() + 1, r.bottom() - 1) : QPointF(r.right() - 1, r.bottom() - 1);
      const QPointF to = side < 0 ? QPointF(r.left() + w, r.top() + 1) : QPointF(r.right() - w, r.top() + 1);
      p.setPen(QPen(QColor(255, 255, 255, 150), 1.2));
      p.drawLine(from, to);
    }
    p.setPen(QPen(QColor(0, 0, 0, 170), 1));
    p.setBrush(fade > 0 ? QColor(Qt::white) : QColor(0xc8, 0xc8, 0xc8));
    p.drawEllipse(hp, 4.2, 4.2);
  }
  p.restore();
}

void TimelineView::paintFadeLabel(QPainter& p) {
  if (drag_ != Drag::Fade || !dragMoved_) return;
  const Editor& ed = editor();
  QRectF r;
  double fadeIn = 0;
  double fadeOut = 0;
  if (dragType_ == Selection::Clip) {
    for (const auto& l : ed.layout().items) {
      if (l.clip.id != dragId_) continue;
      r = clipRect(l);
      fadeIn = l.clip.fadeIn;
      fadeOut = l.clip.fadeOut;
    }
  } else if (const OverlayItem* o = dragType_ == Selection::Overlay ? ed.overlay(dragId_) : nullptr) {
    r = overlayRect(*o);
    fadeIn = o->fadeIn;
    fadeOut = o->fadeOut;
  } else if (const AudioItem* a = ed.audio(dragId_)) {
    r = audioRect(*a);
    fadeIn = a->fadeIn;
    fadeOut = a->fadeOut;
  }
  QPointF hp;
  if (r.isNull() || !fadeHandlePos(r, fadeIn, fadeOut, fadeSide_, &hp)) return;
  const double v = fadeSide_ < 0 ? fadeIn : fadeOut;
  const QString text = QStringLiteral("フェード%1 %2").arg(fadeSide_ < 0 ? QStringLiteral("イン") : QStringLiteral("アウト"),
                                                          v < 0.005 ? QStringLiteral("なし") : QStringLiteral("%1秒").arg(v, 0, 'f', 2));
  QFont f = font();
  f.setPointSizeF(8);
  p.save();
  p.setFont(f);
  const QFontMetricsF fm(f);
  QRectF box(0, 0, fm.horizontalAdvance(text) + 12, 17);
  box.moveCenter(QPointF(hp.x(), hp.y() - 15));
  box.moveLeft(std::clamp(box.left(), double(kHeadW) + 2, std::max(double(kHeadW) + 2, viewport()->width() - box.width() - 2)));
  if (box.top() < kRulerH) box.moveTop(hp.y() + 9);  // 上に隠れるときは、つまみの下に出す
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(theme::accent);
  p.drawRoundedRect(box, 4, 4);
  p.setPen(Qt::white);
  p.drawText(box, Qt::AlignCenter, text);
  p.restore();
}

void TimelineView::fadeTo(QPointF pos) {
  Editor& ed = editor();
  const int fps = std::max(1, ed.project().settings.fps);
  const double t = tOf(dragX(pos.x()));
  // フレームにそろえる。長さは 0〜kMaxFade 秒、オブジェクトの長さ（もう一方のフェードの残り）までに収める
  auto frames = [fps](double v) { return std::round(std::max(0.0, v) * fps) / fps; };
  if (dragType_ == Selection::Clip || dragType_ == Selection::Overlay) {
    // メインのビデオクリップも、重ねるトラックの映像も（フェードは映像の設定）
    double start = 0;
    double duration = -1;
    if (!ed.clipSpan(dragId_, &start, &duration)) return;
    const double raw = frames(fadeSide_ < 0 ? t - start : start + duration - t);
    const int side = fadeSide_;
    ed.updateClip(dragId_, [&](VideoClip& c) {
      if (side < 0) c.fadeIn = fitFade(raw, duration, c.fadeOut);
      else c.fadeOut = fitFade(raw, duration, c.fadeIn);
    }, merge_);
  } else if (dragType_ == Selection::Audio) {
    const AudioItem* a = ed.audio(dragId_);
    if (!a) return;
    const double start = a->start;
    const double duration = audioDuration(*a);
    const double raw = frames(fadeSide_ < 0 ? t - start : start + duration - t);
    const int side = fadeSide_;
    ed.updateAudio(dragId_, [&](AudioItem& x) {
      if (side < 0) x.fadeIn = fitFade(raw, duration, x.fadeOut);
      else x.fadeOut = fitFade(raw, duration, x.fadeIn);
    }, merge_);
  }
  viewport()->update();
}

// ---- 書き出す範囲 -------------------------------------------------------------------------

int TimelineView::rangeEdgeAt(QPointF pos) const {
  if (pos.y() >= kRulerH || pos.x() < kHeadW) return 0;
  const std::optional<Editor::TimeRange> r = editor().exportRange();
  if (!r) return 0;
  const double ds = std::abs(pos.x() - xOf(r->start));
  const double de = std::abs(pos.x() - xOf(r->end));
  if (ds <= 5 && ds <= de) return -1;
  if (de <= 5) return 1;
  return 0;
}

void TimelineView::paintRange(QPainter& p, const QRect& area) {
  const std::optional<Editor::TimeRange> r = editor().exportRange();
  if (!r) return;
  const double x0 = xOf(r->start);
  const double x1 = xOf(r->end);
  if (x1 < kHeadW || x0 > area.width()) return;
  p.save();
  p.setClipRect(QRect(kHeadW, 0, area.width() - kHeadW, area.height()));
  QColor band = theme::accent;
  band.setAlpha(70);
  p.fillRect(QRectF(x0, 0, x1 - x0, kRulerH - 1), band);
  // 両端: 目盛りの上は太く、タイムラインの上は細い線
  QColor edge = theme::accent;
  p.fillRect(QRectF(x0 - 1, 0, 2, kRulerH), edge);
  p.fillRect(QRectF(x1 - 1, 0, 2, kRulerH), edge);
  edge.setAlpha(150);
  p.fillRect(QRectF(x0 - 0.5, kRulerH, 1, area.height() - kRulerH), edge);
  p.fillRect(QRectF(x1 - 0.5, kRulerH, 1, area.height() - kRulerH), edge);
  // 端のつまみ（目盛りの下の端に、内側へ向けた小さな三角）
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(theme::accent);
  QPolygonF a;
  a << QPointF(x0, kRulerH - 9) << QPointF(x0 + 6, kRulerH - 1) << QPointF(x0, kRulerH - 1);
  QPolygonF b;
  b << QPointF(x1, kRulerH - 9) << QPointF(x1 - 6, kRulerH - 1) << QPointF(x1, kRulerH - 1);
  p.drawPolygon(a);
  p.drawPolygon(b);
  p.restore();
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
  updateArming(pos.x());
  const Editor& ed = editor();
  DropGhost& g = *drop_;
  // 左の見出しの列の上では、見えている左端の時間に置く（自動スクロールで左へ戻していく途中でも、そのまま落とせる）
  g.valid = true;
  // ズームとスクロールを含めて、マウスの位置を時間にする。フレームにそろえる
  const double start = frameRound(std::max(0.0, tOf(dragX(pos.x()))), ed.project().settings.fps);
  g.items.clear();
  if (g.type == Selection::Text) {
    GhostItem it;
    it.start = start;
    it.duration = ed.newTextDuration(start, presetDuration(g.presetId));
    // 追加したあとに入る段（既存のテキストの段は変わらない）。重なるときは別の段になり、段が足りなければ増える
    it.lane = ed.textTrackFor(it.start, it.duration);
    it.label = defaultText();
    textLanes_ = textLanesOf(ed.project().texts);
    textLanes_.count = std::max(textLanes_.count, it.lane + 1);
    g.items.push_back(it);
  } else {
    // 追加したあとと同じ並べ方: 最初の素材を落とした位置に置き、次の素材はその終わりに続ける
    std::vector<LaneSpan> all;
    for (const auto& x : ed.project().audios) all.push_back({x.id, x.start, x.start + audioDuration(x)});
    double t = start;
    for (size_t i = 0; i < g.audioDurations.size(); ++i) {
      GhostItem it;
      it.start = t;
      it.duration = g.audioDurations[i];
      it.label = g.audioNames[i];
      all.push_back({QStringLiteral("__ghost-%1").arg(i), t, t + it.duration});
      g.items.push_back(it);
      t += it.duration;
    }
    // 今あるオーディオの段はそのまま。追加予定のものは、追加したあとと同じく空いている段へ入れる
    audioLanes_ = stableLanes(all, audioMemo_);
    for (size_t i = 0; i < g.items.size(); ++i) g.items[i].lane = audioLanes_.lane.at(QStringLiteral("__ghost-%1").arg(i));
  }
  // 追加予定の終わりまでスクロールできるように、範囲を広げる（まだ追加していないので、編集データの終わりには入っていない）
  dragReach_ = 0;
  for (const GhostItem& it : g.items) dragReach_ = std::max(dragReach_, it.start + it.duration);
  updateScrollBars();
  viewport()->update();
  // 端に近づいたら、自動でスクロールを始める
  if (autoScrollTarget(pos.x()) != 0) startAutoScroll();
}

void TimelineView::endDrop() {
  stopAutoScroll();
  if (drop_ || dragReach_ > 0) rememberView();  // 追加予定のぶん広げていた範囲を戻しても、今の表示位置は動かさない
  dragReach_ = 0;
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
  drop_->presetId = presetId;
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
}

void TimelineView::dragEnterEvent(QDragEnterEvent* e) {
  // 入ってきた位置が端の近く（見出しの列の上など）でも、通りかかっただけではスクロールしない
  armAutoScroll(e->position().x());
  if (isTextPresetDrag(e->mimeData())) {
    beginDrop(Selection::Text, {});
    drop_->presetId = QString::fromUtf8(e->mimeData()->data(QString::fromLatin1(kTextPresetMime)));
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
  if (e->mimeData()->hasFormat(QString::fromLatin1(kAssetMime))) {
    libDrag_ = true;  // 動画・写真などを素材一覧からドラッグしてきた
    dropPos_ = e->position();
    e->acceptProposedAction();
  } else {
    e->ignore();
  }
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
  libDrag_ = true;
  refreshLibraryDrop(e->position());
  e->acceptProposedAction();
  // 端に近づいたら、自動でスクロールを始める（マウスを止めても、端にいる間は続く）
  if (autoScrollTarget(e->position().x()) != 0) startAutoScroll();
}

void TimelineView::dragLeaveEvent(QDragLeaveEvent*) {
  // タイムラインの外へ出た・Esc で中止した: 追加予定の表示を消すだけで、何も追加しない
  libDrag_ = false;
  endDrop();
  dropX_.reset();
  overlayDropT_.reset();
  audioDropHover_ = false;
  viewport()->update();
}

void TimelineView::dropEvent(QDropEvent* e) {
  libDrag_ = false;
  stopAutoScroll();  // 落としたら、自動スクロールは止める
  if (isTextPresetDrag(e->mimeData())) {
    dropTextPreset(e);
    return;
  }
  if (const auto audio = audioAssetsOf(e->mimeData()); !audio.empty()) {
    dropAudioAssets(e, audio);
    return;
  }
  dropX_.reset();
  overlayDropT_.reset();
  audioDropHover_ = false;
  viewport()->update();
  const QByteArray data = e->mimeData()->data(QString::fromLatin1(kAssetMime));
  if (data.isEmpty()) return;
  std::vector<QString> ids;
  for (const auto& s : QString::fromUtf8(data).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) ids.push_back(s);
  const Row row = rowAt(e->position().y());
  const double x = dragX(e->position().x());  // 見出しの列の上で落としたときは、見えている左端の位置
  const double t = std::max(0.0, tOf(x));
  if (row == Row::Overlay) {
    // 重ねるトラックの段: 落とした段・時間から重ねる（ふさがっていれば、重ならない段へ）。オーディオは、これまでどおりオーディオに置く
    std::vector<QString> visual;
    std::vector<QString> sounds;
    for (const QString& id : ids) {
      const Asset* a = editor().asset(id);
      if (a && a->kind == AssetKind::Audio) sounds.push_back(id);
      else visual.push_back(id);
    }
    const int lane = overlayLaneAt(e->position().y());
    if (!visual.empty()) editor().addOverlays(visual, t, lane >= 0 ? std::optional<int>(lane) : std::nullopt);
    if (!sounds.empty()) editor().addToTimeline(sounds, std::nullopt, t);
  } else if (row == Row::Video) {
    editor().addToTimeline(ids, insertionIndex(x, QString()), t);
  } else {
    editor().addToTimeline(ids, std::nullopt, t);
  }
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
