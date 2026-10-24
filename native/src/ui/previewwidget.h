#pragma once

#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

#include <QPolygonF>

#include "core/model.h"
#include "core/timeline.h"

namespace kv {

class PreviewEngine;
class Segmented;

/** プレビュー画面（テキストはドラッグで動かせる） */
class PreviewStage : public QWidget {
  Q_OBJECT
 public:
  explicit PreviewStage(PreviewEngine* engine, QWidget* parent = nullptr);
  /** 画面内の映像の範囲（ウィジェット座標） */
  QRectF stageRect() const;

 signals:
  void requestTextFocus();

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void dragEnterEvent(QDragEnterEvent*) override;
  void dragLeaveEvent(QDragLeaveEvent*) override;
  void dropEvent(QDropEvent*) override;

 private:
  enum class Grab { None, Move, Scale, Rotate };
  void updateCanvasSize();
  QString hitText(QPointF pos) const;
  QPointF toWidget(QPointF design) const;
  QPointF toDesign(QPointF widget) const;
  /** 選んでいる映像・テキストの枠（基準解像度の座標、左上・右上・右下・左下の順）。無ければ false */
  bool selectedQuad(QPolygonF* quad) const;
  QPointF rotateHandle(const QPolygonF& quad) const;
  Grab grabAt(QPointF pos, const QPolygonF& quad) const;
  void beginGrab(Grab g, QPointF pos);

  PreviewEngine* engine_;
  QWidget* empty_;
  Grab grab_ = Grab::None;
  QPointF dragStart_;
  QPointF center_;
  LayerState origState_;
  double startDist_ = 1;
  double startAngle_ = 0;
  double lastAngle_ = 0;
  double accumRot_ = 0;
  QString dragMerge_;
  bool guideV_ = false, guideH_ = false;
  bool dropHover_ = false;
};

/** 再生ボタン・時刻・比率の切り替え */
class TransportBar : public QWidget {
  Q_OBJECT
 public:
  explicit TransportBar(QWidget* parent = nullptr);

 private:
  void refresh();
  QLabel* time_;
  QToolButton* play_;
  Segmented* aspect_;
};

class PreviewPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PreviewPanel(PreviewEngine* engine, QWidget* parent = nullptr);
  PreviewStage* stage() const { return stage_; }

 private:
  PreviewStage* stage_;
};

}  // namespace kv
