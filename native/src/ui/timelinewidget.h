#pragma once

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QPair>
#include <QPixmap>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <map>
#include <optional>
#include <vector>

#include "core/editor.h"

class QContextMenuEvent;
class QDropEvent;
class QMimeData;

namespace kv {

class TimelineView : public QAbstractScrollArea {
  Q_OBJECT
 public:
  struct Lanes {
    std::map<QString, int> lane;
    int count = 1;
  };
  /** 前回のオーディオの段の割り当て（段と、そのときの時間）。並べ直すときに、動いていない物の段を変えないために使う */
  struct LaneMemo {
    int lane = 0;
    double start = 0;
    double end = 0;
  };

  explicit TimelineView(QWidget* parent = nullptr);
  void fitAll();
  /** その時間が見えていなければ、見える位置まで横にスクロールする */
  void revealTime(double t);

  // ---- 段の高さ（テキスト・ビデオ・オーディオ）。左の見出しの列の境目をドラッグするか、キーで変えられる。設定に覚える ----
  /** すべての段の高さを factor 倍にする（範囲に収める） */
  void scaleTrackHeights(double factor);
  /** 段の高さを、はじめの高さに戻す */
  void resetTrackHeights();

  /** クリップ・テキスト・オーディオ（や音量キーフレーム）をドラッグして動かしている最中か（再生位置の赤い線のドラッグは含めない） */
  bool isDragging() const;
  /** その最中のドラッグをやめて、元の位置・長さに戻す（履歴には残さない）。ドラッグ中でなければ false */
  bool cancelDrag();

 signals:
  void requestTextFocus();

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void contextMenuEvent(QContextMenuEvent*) override;
  void leaveEvent(QEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void dragEnterEvent(QDragEnterEvent*) override;
  void dragMoveEvent(QDragMoveEvent*) override;
  void dragLeaveEvent(QDragLeaveEvent*) override;
  void dropEvent(QDropEvent*) override;
  void scrollContentsBy(int dx, int dy) override;

 private:
  /** Overlay は重ねるトラックの段（テキストの段とビデオの段の間。下の段ほどメインに近く、上の段ほど前に重なる） */
  enum class Row { None, Ruler, Text, Overlay, Video, Audio };
  enum class Drag { None, Scrub, ClipMove, ClipTrim, ItemMove, ItemTrim, KeyMove, Fade, Range, RangeEdge, TrackResize };
  struct Hit {
    Row row = Row::None;
    Selection::Type type = Selection::None;
    QString id;
    int handle = 0;  // -1 左端 / 1 右端
  };

  void relayout();
  void updateScrollBars();
  double pps() const { return editor().pxPerSec(); }
  double xOf(double t) const;
  double tOf(double x) const;
  int textY() const;
  int overlayY() const;
  int videoY() const;
  int audioY() const;
  int textH() const;
  int overlayH() const;
  int audioH() const;
  /** テキストをドラッグしているときの、縦の位置にあたる段（テキストの段の外なら -1） */
  int textLaneAt(double y) const;
  /** 重ねるトラックの映像をドラッグしているときの、縦の位置にあたる段（重ねるトラックの段の外なら -1） */
  int overlayLaneAt(double y) const;
  QRectF overlayRect(const OverlayItem& o) const;
  void paintOverlays(QPainter& p);
  /** 主に選んでいるもの以外で、一緒に扱うもの（一緒に選んだもの・同じグループのもの）の ID */
  std::vector<QString> coSelectedIds() const;
  /** グループの印（バーの下の端の色の線）と、一緒に選んでいるものの点線の枠 */
  void paintGroupMarks(QPainter& p, const QRectF& r, const QString& group, bool coSelected);
  Row rowAt(double y) const;
  Hit hitTest(QPointF p) const;
  QRectF clipRect(const ClipLayout& l) const;
  QRectF itemRect(Selection::Type type, double start, double duration, int lane) const;
  int insertionIndex(double x, const QString& exclude) const;
  double indicatorX(int index, const QString& exclude) const;
  std::vector<double> snapPoints(const QString& exclude, bool withPlayhead) const;
  std::optional<double> snap(double t, const std::vector<double>& points) const;
  double rulerStep() const;
  const std::vector<QPixmap>& thumbPixmaps(const QString& assetId, int height);
  void followPlayhead();
  void zoomAt(double factor, double mouseX);
  void scrubTo(double x, bool snapOn);

  /** マウスの位置に合わせて、ドラッグの途中経過を反映する（押した位置の時間からの差で動かす） */
  void dragTo(QPointF pos, Qt::KeyboardModifiers mods);
  /** 音量キーフレームの点を、マウスの位置へ動かす */
  void keyMoveTo(QPointF pos);
  /** 最後のマウスの位置で、今のドラッグの途中経過を引き直す（自動スクロールのたびに呼ぶ） */
  void dragStep();
  /** ドラッグを終える（離した位置 pos で確定する）。離した合図を受け取りそこねたときも、これで終える */
  void finishDrag(QPointF pos);
  /**
   * ドラッグで使うマウスの横位置。見出しの列の上やタイムラインの外にあるときは、見えている編集領域の端にとどめる
   * （ドラッグしている物が見出しの下に隠れたり、表示の外へ飛んだりしない。離したときも、見えている端の時間に置く）
   */
  double dragX(double x) const;
  /** ドラッグ（タイムラインの中の物、または外から持ってきた物）の途中か */
  bool dragActive() const;
  /** 今の表示位置がスクロール範囲の右端になる、内容の終わりの時間（updateScrollBars の計算の逆） */
  double viewNeedEnd() const;
  /** ドラッグを終える前に呼ぶ: 中身が短くなっても、今の表示位置をスクロール範囲に残す（終えた瞬間に表示が飛ばない） */
  void rememberView();

  // ドラッグ中の自動スクロール: マウスが表示領域の左右の端に近づいたら、近いほど速く横へスクロールする。
  // マウスを止めていても、端にいる間は続ける。スクロールで同じマウス位置の時間が変わるので、そのたびにドラッグの結果を引き直す
  /** 自動スクロールの左端・右端（表示領域の座標）。ウィンドウが画面からはみ出していれば、画面の端 */
  QPair<double, double> autoScrollEdges() const;
  /** 端からの距離に応じた、スクロールの目標の速さ（px/秒。左へは負、右へは正。端から遠いときは 0） */
  double autoScrollTarget(double x) const;
  /** 自動スクロールが必要なドラッグ・ドロップの途中なら true にして、そのマウス位置を返す */
  bool autoScrollSource(QPointF* pos, bool* dragging = nullptr) const;
  /**
   * 自動スクロールの「構え」。ドラッグを始めた（外から入ってきた）位置が端の近くだったときは、その端の方へ動かすか、
   * 一度内側へ出るまでは、その端ではスクロールしない（端の近くの物を押しただけ・通りかかっただけで、勝手に流れないように）
   */
  void armAutoScroll(double x);
  void updateArming(double x);
  void startAutoScroll();
  void stopAutoScroll();
  void autoScrollTick();
  /** 素材一覧から動画・写真などをドラッグしてきているときの、置く位置の表示を引き直す */
  void refreshLibraryDrop(QPointF pos);

  void paintRuler(QPainter& p, const QRect& area);
  void paintVideo(QPainter& p);
  void paintItems(QPainter& p, Selection::Type type);
  void paintVolume(QPainter& p, const AudioItem& a, const QRectF& r);
  void paintDropGhost(QPainter& p);

  /** 映像・文字のバーに、アニメーションのキーフレームがある時間の小さな ◆ を並べる（表示だけ。編集は右の設定パネル） */
  void paintKeyMarkers(QPainter& p);

  // ---- フェードのつまみ（映像・オーディオのバーの上の角。左右にドラッグして、フェードの長さを変える） ----
  struct FadeHit {
    Selection::Type type = Selection::None;
    QString id;
    int side = 0;  // -1 フェードイン / 1 フェードアウト
  };
  /** つまみの位置（画面の座標）。バーが狭くてつまみを出せないときは false */
  bool fadeHandlePos(const QRectF& r, double fadeIn, double fadeOut, int side, QPointF* pos) const;
  /** その位置にあるフェードのつまみ（無ければ type が None） */
  FadeHit fadeHitTest(QPointF pos) const;
  /** つまみと、フェードの傾きの線を描く（選んでいる物と、カーソルの下の物だけ） */
  void paintFadeHandles(QPainter& p, const QRectF& r, double fadeIn, double fadeOut, bool active);
  /** ドラッグ中のフェードの長さの表示 */
  void paintFadeLabel(QPainter& p);
  /** フェードのつまみをドラッグして、長さを変える */
  void fadeTo(QPointF pos);
  int fadeSide_ = 0;
  /** カーソルの下にある物（フェードのつまみを出す） */
  QString hoverId_;

  // ---- 書き出す範囲（目盛りの上に表示。Shift+ドラッグで作り、両端をドラッグで動かす。I / O キーでも決められる） ----
  /** 目盛りの上で、範囲の端にあたる位置なら -1（開始）/ 1（終了）。無ければ 0 */
  int rangeEdgeAt(QPointF pos) const;
  void paintRange(QPainter& p, const QRect& area);
  /** 範囲のドラッグ: 押した時間（Shift+ドラッグで作るとき） */
  double rangeAnchor_ = 0;
  int rangeEdge_ = 0;
  /** ドラッグを始める前の範囲（Esc で戻す） */
  std::optional<Editor::TimeRange> origRange_;
  /** 目盛りの右クリックメニュー（範囲の開始・終了・解除） */
  void execRulerMenu(QPointF pos, const QPoint& globalPos);

  // ---- 段の高さ ----
  int textLaneH_ = 28;
  int overlayLaneH_ = 44;
  int videoH_ = 66;
  int audioLaneH_ = 52;
  /**
   * 左の見出しの列の上で、段の境目にあたる位置なら 0（テキストの下）/ 1（重ねるトラックの下）/ 2（ビデオの下）/ 3（オーディオの下）。
   * 無ければ -1
   */
  int trackBorderAt(QPointF pos) const;
  void setTrackHeights(int textLane, int overlayLane, int video, int audioLane);
  void loadTrackHeights();
  void saveTrackHeights() const;
  /** 見出しの列の右クリックメニュー（段の高さ） */
  void execHeaderMenu(const QPoint& globalPos);
  int resizeWhich_ = -1;
  double resizeStartY_ = 0;
  int resizeOrig_[4] = {0, 0, 0, 0};

  // オーディオの音量キーフレーム（音量ライン上の丸い点）
  struct KeyHit {
    QString id;
    int index = -1;
  };
  /** その位置にあるキーフレーム（なければ index が -1）。掴みやすいよう、見た目より広い範囲で判定する */
  KeyHit keyHitTest(QPointF pos) const;
  QRectF audioRect(const AudioItem& a) const;
  /** 選んでいるキーフレームが、まだあるか */
  bool keySelectionValid() const;
  KeyHit keySel_;
  int dragKey_ = -1;

  // テキストスタイル・オーディオ素材のドラッグ＆ドロップ（落とす前に、追加予定の位置を半透明で見せる）
  static bool isTextPresetDrag(const QMimeData* m);
  /** オーディオ素材だけのドラッグなら、その素材のメディア ID を返す（そうでなければ空） */
  static std::vector<QString> audioAssetsOf(const QMimeData* m);
  void beginDrop(Selection::Type type, const std::vector<QString>& audioIds);
  void updateDrop(QPointF pos);
  void endDrop();
  void dropTextPreset(QDropEvent* e);
  void dropAudioAssets(QDropEvent* e, const std::vector<QString>& ids);
  /** ドラッグを受け付けるか（見出しの列の上でも、見えている左端の時間に置けるので受け付ける） */
  bool dropAccepted() const { return drop_ && drop_->valid; }

  /** ドラッグ中の、追加予定の 1 つ分 */
  struct GhostItem {
    double start = 0;
    double duration = 0;
    int lane = 0;
    QString label;
  };
  /** ドラッグ中の、追加予定（テキスト 1 つ、または並べて置くオーディオ素材） */
  struct DropGhost {
    Selection::Type type = Selection::Text;
    bool valid = false;
    /** テキストのとき: ドラッグしているスタイル・テンプレートの ID（テンプレートは、保存したときの長さで追加する） */
    QString presetId;
    std::vector<GhostItem> items;
    /** オーディオのとき: 各素材の長さと名前（動きのたびに引き直さない） */
    std::vector<double> audioDurations;
    std::vector<QString> audioNames;
  };
  std::optional<DropGhost> drop_;
  QPointF dropPos_;
  QTimer* autoScroll_ = nullptr;
  QElapsedTimer autoClock_;
  double scrollVel_ = 0;        // 今の自動スクロールの速さ（px/秒）。目標へなめらかに近づける
  double scrollRemainder_ = 0;  // スクロールバーは整数なので、端数は次に持ち越す
  /** 素材一覧から動画・写真・オーディオなどをドラッグしてきている最中 */
  bool libDrag_ = false;
  /** ドラッグ・ドロップ中の物の終わりの時間。ここまでスクロールできるよう、スクロール範囲を広げる（0 は広げない） */
  double dragReach_ = 0;
  /** ドラッグを終えたときの表示位置を保つための、内容の終わりの時間（0 は保たない）。表示を左へ戻すと縮める */
  double keepEnd_ = 0;
  double armRefX_ = 0;     // ドラッグを始めた（外から入ってきた）ときの横位置
  bool armedLeft_ = true;  // 左端・右端で自動スクロールしてよいか
  bool armedRight_ = true;

  Lanes textLanes_;
  Lanes overlayLanes_;
  Lanes audioLanes_;
  std::map<QString, LaneMemo> audioMemo_;  // 前回のオーディオの段の割り当て
  QString laneHoldId_;                     // ドラッグ中のオーディオ。ドラッグを始めたときの段（laneHold_）にとどめる
  int laneHold_ = -1;
  /** サムネイル（素材 → 高さ → 並べる画像）。メインのビデオと重ねるトラックで高さが違うので、高さごとに持つ */
  std::map<QString, std::map<int, std::vector<QPixmap>>> thumbCache_;

  // ドラッグの状態
  Drag drag_ = Drag::None;
  bool dragMoved_ = false;
  QPointF pressPos_;
  double pressTime_ = 0;
  QString dragId_;
  Selection::Type dragType_ = Selection::None;
  int dragSide_ = 0;
  QString merge_;
  VideoClip origClip_;
  TextItem origText_;
  AudioItem origAudio_;
  OverlayItem origOverlay_;
  /** 一緒に動かすもの（一緒に選んだもの・同じグループのもの）と、ドラッグを始めたときの開始 */
  struct Companion {
    Selection sel;
    double start = 0;
  };
  std::vector<Companion> companions_;
  /** 一緒に選んでいたものを押した（動かさずに離したら、押したものだけを選ぶ） */
  bool pressInSet_ = false;
  /** 素材一覧から動画・写真を、重ねるトラックの段へドラッグしてきている（その時間に縦線を出す） */
  std::optional<double> overlayDropT_;
  std::vector<double> dragSnap_;
  QPointF dragPos_;                    // ドラッグ中の、最後のマウスの位置
  Qt::KeyboardModifiers dragMods_;     // そのときの修飾キー（自動スクロールで引き直すときに使う）
  bool dragSession_ = false;           // ドラッグのために編集データの「取り消せる作業のまとまり」を始めた（キャンセルで元に戻せる）
  std::optional<double> snapMark_;  // 吸着している時刻（目印を描く）。吸着していないときは空
  double dragDx_ = 0;
  int dragIndex_ = -1;
  std::optional<double> dropX_;
  bool audioDropHover_ = false;
};

class TimelineWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TimelineWidget(QWidget* parent = nullptr);
  TimelineView* view() const { return view_; }

 signals:
  void requestTextFocus();

 private:
  void refreshButtons();
  TimelineView* view_;
  QToolButton* split_;
  QToolButton* dup_;
  QToolButton* del_;
  QSlider* zoom_;
};

}  // namespace kv
