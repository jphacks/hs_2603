#pragma once

#include <QAbstractScrollArea>
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
  void wheelEvent(QWheelEvent*) override;
  void dragEnterEvent(QDragEnterEvent*) override;
  void dragMoveEvent(QDragMoveEvent*) override;
  void dragLeaveEvent(QDragLeaveEvent*) override;
  void dropEvent(QDropEvent*) override;
  void scrollContentsBy(int dx, int dy) override;

 private:
  enum class Row { None, Ruler, Text, Video, Audio };
  enum class Drag { None, Scrub, ClipMove, ClipTrim, ItemMove, ItemTrim, KeyMove };
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
  int videoY() const;
  int audioY() const;
  int textH() const;
  int audioH() const;
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

  void paintRuler(QPainter& p, const QRect& area);
  void paintVideo(QPainter& p);
  void paintItems(QPainter& p, Selection::Type type);
  void paintVolume(QPainter& p, const AudioItem& a, const QRectF& r);
  void paintDropGhost(QPainter& p);

  /** 映像・文字のバーに、アニメーションのキーフレームがある時間の小さな ◆ を並べる（表示だけ。編集は右の設定パネル） */
  void paintKeyMarkers(QPainter& p);

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
  /** ドラッグを受け付けるか（見出しの上など、置けない位置では false） */
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
    std::vector<GhostItem> items;
    /** オーディオのとき: 各素材の長さと名前（動きのたびに引き直さない） */
    std::vector<double> audioDurations;
    std::vector<QString> audioNames;
  };
  std::optional<DropGhost> drop_;
  QPointF dropPos_;
  QTimer* autoScroll_ = nullptr;

  Lanes textLanes_;
  Lanes audioLanes_;
  std::map<QString, std::vector<QPixmap>> thumbCache_;

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
  std::vector<double> dragSnap_;
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
