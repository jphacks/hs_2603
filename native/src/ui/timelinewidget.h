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
  enum class Drag { None, Scrub, ClipMove, ClipTrim, ItemMove, ItemTrim, KeyMove, Fade, Range, RangeEdge, TrackResize, LaneSwap };
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
  /**
   * テキスト（kind = Text）かオーディオ（kind = Audio）を上下にドラッグしているときの、縦の位置にあたる行き先。段の中ほどならその段、
   * 段の境目（一番上の段の上・一番下の段の下も）ならそこに新しい段を差し込む。その種類の段から大きく外れて、ほかの種類の段
   * （テキストなら重ねるトラック、オーディオならビデオ）に入ったら nullopt
   */
  std::optional<TextTrackTarget> laneTargetAt(Selection::Type kind, double y) const;
  /** index の段の上の境目の縦の位置（段の数を渡すと、一番下の段の下の境目） */
  double textBoundaryY(int index) const;
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
  /**
   * テキスト・オーディオを動かす（wantStart は吸着させたあとの始まり、floorStart は一緒に動かすものも 0:00 より前に出ない始まり）。
   * 左右に動かしているあいだは段を変えず、同じ段のほかのものに重ならない位置までにする（すぐに編集データへ反映する）。
   * はっきり上下に動かしたら、行き先（段・段の境目）を決めて半透明で見せ、離したときに段を移す
   */
  void laneMoveTo(QPointF pos, double wantStart, double floorStart);
  /**
   * 押したものの始まり start（時間をずらしたあと）を、左端か右端のうち、吸着したほうで、ずれが小さいほうに合わせる（吸着の印も付ける）。
   * floor より前には出さない
   */
  double snapMoveStart(double start, double duration, double floor, bool snapOn);
  /** 押したもの（始まり start。lane が 0 以上なら、重ねるトラックの段も）と一緒に動かすもの（delta だけずらす）を、p に反映する */
  bool applyMove(ProjectData& p, double start, double delta, int lane) const;
  /**
   * 左右に動かすときに、同じ段のほかのものと重ならないよう調べる材料（一緒に動かすテキスト・オーディオの、動かす前の段・始まり・長さ。
   * includeMain なら、押したものも入れる）。exclude には、重なりを調べる相手から外すもの（押したもの・一緒に動かすもの）の ID を入れる
   */
  std::vector<ShiftItem> shiftMovers(bool includeMain, std::vector<QString>* exclude) const;
  /** 重ならないようにずらしたあと: 吸着の印を、当たった端（吸着先と同じ所なら）に付け直す */
  void fixSnapMark(double start, double dur);
  /** ドラッグして動かしている途中のものか（半透明で描く） */
  bool movingItem(Selection::Type type, const QString& id) const;
  /** テキスト・オーディオを動かしている途中か */
  bool laneDragActive() const;
  /**
   * 動かしているテキスト・オーディオ（と一緒に動かすテキスト・オーディオ）を、元の場所には描かず、行き先に半透明で描く途中か
   * （テキストはいつも。オーディオは段を変えるときだけで、左右に動かしているあいだは、波形ごとその場で描く）
   */
  bool laneGhosts() const;
  /** 動かしているテキスト・オーディオを、行き先に半透明で描く。段を変えるときは、行き先の段・差し込む境目も描く */
  void paintLaneDrag(QPainter& p);
  /**
   * メインのビデオと重ねるトラックの間で動かしている途中の、行き先の半透明のバー・差し込む位置と、カーソルの横の札
   * （ビデオへ移せないときは、その理由）
   */
  void paintCrossDrag(QPainter& p);
  /** ビデオのクリップを重ねるトラックへ動かしているときの、縦の位置にあたる段（重ねるトラックより上なら、一番上の段のさらに上） */
  int crossOverlayLane(double y) const;
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

  // ---- 段の入れ替え（左の見出しの列で、段の名前を上下にドラッグする。テキスト・オーディオの段が 2 つ以上あるとき） ----
  struct LaneRef {
    Selection::Type kind = Selection::None;  // Text / Audio
    int index = 0;                           // 段（0 が一番上）
    bool operator==(const LaneRef&) const = default;
  };
  /** 見出しの列の、その位置にある段の名前（段の境目の高さを変える所は除く）。無ければ nullopt */
  std::optional<LaneRef> laneHeadAt(QPointF pos) const;
  /** その種類の段の、縦の位置にあたる段（段の外なら、近いほうの端の段） */
  int laneAtY(Selection::Type kind, double y) const;
  /** その種類の段 index の、上端の縦の位置と高さ */
  double laneTop(Selection::Type kind, int index) const;
  int laneHeight(Selection::Type kind) const;
  int laneCount(Selection::Type kind) const;
  /** 見出しに出す段の名前（「テキスト 1」「オーディオ 2」。段の番号は中身と一緒に動く） */
  QString laneName(Selection::Type kind, int index) const;
  /** 見出しの列の、段ごとの名前（段が 2 つ以上のとき） */
  void paintLaneHeads(QPainter& p, Selection::Type kind, int sectionY, int sectionH, const QString& icon, const QString& label);
  /** 入れ替えのドラッグ中の、元の段・行き先の段の印と、札 */
  void paintLaneSwap(QPainter& p);
  /** 入れ替えのドラッグ中に、上下の端へ近づいたときの縦の自動スクロールの速さ（px/秒。上へは負。端から遠いときは 0） */
  double laneScrollSpeed(double y) const;
  LaneRef swapFrom_;
  int swapTo_ = 0;
  /** カーソルの下の見出しの段（掴める目印を出す） */
  std::optional<LaneRef> laneHover_;

  // ---- 見出しの列の上のホイールで、縦にスクロールする（目標の位置まで、なめらかに動かす） ----
  void scrollVerticallyBy(double dy);
  void vScrollTick();
  void stopVScroll();
  QTimer* vScroll_ = nullptr;
  QElapsedTimer vScrollClock_;
  double vScrollPos_ = 0;     // 今の位置（端数も持つ）
  double vScrollTarget_ = 0;  // 目標の位置

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
  // テキスト・オーディオのドラッグ（左右は時間だけ。はっきり上下に動かしたら段を変える。段の境目なら新しい段を差し込む）
  TextTrackTarget laneTarget_;                 // 今の行き先（Stay なら段を変えない）
  std::optional<TextTrackTarget> laneIntent_;  // 最後にカーソルの縦の位置から決めた行き先（段の外へ出たときは、これを使い続ける）
  bool laneEngaged_ = false;                   // はっきり上下に動かした（ここから段を変えられる。ビデオと重ねるトラックの間の移動も）
  int laneOrigLane_ = 0;                       // ドラッグを始めたときの段
  double laneGhostStart_ = 0;                  // 行き先での始まり
  double moveShift_ = 0;                       // 今のずらし量（押したものの、始めたときからの時間の差）
  // メインのビデオと重ねるトラックの間の移動（はっきり上下に動かして、もう一方の段に入れたとき。離すまで編集データは変えない）
  enum class Cross {
    None,
    ToOverlay,   // ビデオのクリップを、重ねるトラックへ
    ToMain,      // 重ねるトラックの映像を、ビデオへ
    MainBlocked  // 重ねるトラックの映像をビデオの段に入れたが、一緒に動かすものがあるので移せない
  };
  Cross cross_ = Cross::None;
  int crossLane_ = 0;        // 重ねるトラックへ移す段（段の数なら、一番上に新しい段）
  int crossIndex_ = 0;       // ビデオへ移すときの、差し込む位置（クリップの番号）
  double crossStart_ = 0;    // 行き先のバーの始まり（重ねるトラックへ移すときは、置く時間）
  double clipOrigStart_ = 0;  // 押したビデオのクリップの、ドラッグを始めたときの始まり
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
