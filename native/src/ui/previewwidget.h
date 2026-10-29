#pragma once

#include <QInputMethodEvent>
#include <QLabel>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QTransform>
#include <QVBoxLayout>
#include <QWidget>

#include <QPolygonF>

#include <QTimer>
#include <vector>

#include "core/model.h"
#include "core/timeline.h"
#include "render/textrender.h"

namespace kv {

class PreviewEngine;
class Segmented;
class FullScreenWindow;

/** プレビュー画面（テキストはドラッグで動かせる） */
class PreviewStage : public QWidget {
  Q_OBJECT
 public:
  explicit PreviewStage(PreviewEngine* engine, QWidget* parent = nullptr);
  /** 画面内の映像の範囲（ウィジェット座標） */
  QRectF stageRect() const;

  /**
   * 全画面モード: 編集用の枠・ハンドル・ガイド・案内・ドロップを止め、余白なしで映像だけを出す
   * （映像の描画は通常と同じ。編集はできない）
   */
  void setFullScreenMode(bool on);
  bool fullScreenMode() const { return fullScreen_; }

  // ---- 文字の直接編集 -----------------------------------------------------------------
  // 文字をダブルクリックすると、プレビューの上でそのまま文字を入力・修正できる。入力は 1 文字ずつ編集データに反映し
  // （右の設定欄・タイムラインにもすぐ出る）、元に戻す履歴は確定までの変更を 1 つにまとめる。
  // 日本語入力（IME）の変換中の文字は、確定するまで編集データに入れず、プレビューにだけ出す。
  /** 文字を直接編集している間 true（このあいだ、キー入力は文字の入力に使い、移動・拡大・回転の操作はしない） */
  bool isEditingText() const { return !editId_.isEmpty(); }
  QString editingTextId() const { return editId_; }
  /** id の文字の、caret の位置（文字列の何文字目か）から編集を始める */
  void beginTextEdit(const QString& id, int caret);
  /** 編集を終える。commit が false なら、キャンセル（始めたときの文字に戻し、履歴には残さない） */
  void endTextEdit(bool commit);
  /** 編集中の入力位置と選択範囲（確認用） */
  int editCaret() const { return caret_; }
  int editAnchor() const { return anchor_; }
  /** 変換中の文字（なければ空） */
  QString editPreedit() const { return preedit_; }

 signals:
  void requestTextFocus();
  /** 全画面モードで、マウスが動いた / 押された */
  void activity();
  /** 全画面モードで、ダブルクリックされた */
  void fullScreenToggleRequested();

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void inputMethodEvent(QInputMethodEvent*) override;
  QVariant inputMethodQuery(Qt::InputMethodQuery) const override;
  bool event(QEvent*) override;
  void focusOutEvent(QFocusEvent*) override;
  bool focusNextPrevChild(bool next) override;
  /** 文字・映像の上の右クリック: そのオブジェクトのメニュー（複製・削除・文字の編集・アニメーションなど） */
  void contextMenuEvent(QContextMenuEvent*) override;
  void dragEnterEvent(QDragEnterEvent*) override;
  void dragLeaveEvent(QDragLeaveEvent*) override;
  void dropEvent(QDropEvent*) override;

 private:
  /** StretchX / StretchY は、映像の枠の辺のつまみ（幅・高さだけを変える） */
  enum class Grab { None, Move, Scale, Rotate, Select, StretchX, StretchY };
  /** 文字の編集の種類（続けて同じ種類の入力は、元に戻す単位を 1 つにまとめる） */
  enum class EditKind { None, Type, Backspace, Delete, Other };
  struct EditState {
    QString text;
    int caret = 0;
    int anchor = 0;
  };
  /** 編集中の文字（変換中の文字を入れた、画面に出す内容） */
  TextItem editedItem() const;
  /** 編集中の文字の配置と、配置 → 画面の変換。文字が今の再生位置に出ていないときは false */
  bool editGeometry(TextItem* item, TextLayout* layout, QTransform* toWidget) const;
  int editIndexAt(QPointF pos, bool* upstream) const;
  bool insideEditedText(QPointF pos) const;
  void paintTextEdit(QPainter& p) const;
  bool hasSelection() const { return caret_ != anchor_; }
  QString committedText() const;
  void applyEdit(int from, int to, const QString& with, EditKind kind);
  void pushUndo(EditKind kind, bool hadSelection);
  void restoreEdit(const EditState& s);
  void localUndo(bool redo);
  void afterEdit();
  void syncEngine();
  void updateInputMethod();
  void moveCaret(int index, bool extend, bool upstream = false, bool keepWantX = false);
  void insertText(const QString& text, EditKind kind = EditKind::Type);
  void commitPreedit();
  void copySelection(bool cut);
  void pasteClipboard();
  void selectWordAt(int index);
  void handleEditKey(QKeyEvent* e);
  bool editsKey(const QKeyEvent* e) const;
  void blinkReset();
  void updateCanvasSize();
  QString hitText(QPointF pos) const;
  /** スポイト（クロマキーの透明にする色を、プレビューのクリックで選ぶ）か、オブジェクトマスクの被写体を選んでいる間 true */
  bool picking() const;
  /** オブジェクトマスクの被写体を選んでいる間 true */
  bool maskPicking() const;
  /** スポイト・被写体を選ぶのをやめる */
  void stopPicking();
  /** スポイト: pos（ウィジェット座標）に見えている元の色を、その映像の透明にする色にして、スポイトを終える */
  void pickChromaAt(QPointF pos);

  // ---- オブジェクトマスクの被写体を選ぶ ----
  /**
   * 映像 clipId の素材のフレーム（0〜1 の割合。反転の前）→ ウィジェット座標の変換と、その映像が見えている範囲（切り抜き・画面の中）。
   * 今の再生位置に出ていなければ false
   */
  bool sourceToWidget(const QString& clipId, QTransform* m, QPainterPath* visible) const;
  /** ウィジェットの点 → 素材のフレームの割合。その映像の見えている所でなければ false */
  bool widgetToSource(const QString& clipId, QPointF pos, QPointF* src) const;
  /** カーソルを合わせた所の被写体を探す（マスクは、できたらプレビューに薄く重ねる） */
  void maskHoverAt(QPointF pos, Qt::KeyboardModifiers mods);
  void maskClickAt(QPointF pos, Qt::KeyboardModifiers mods);
  void paintMaskPick(QPainter& p, const QRectF& r);
  /** マスク（Grayscale8）を、色を付けた半透明の画像と縁の線にする */
  static QImage tintMask(const QImage& mask, const QColor& color);
  /** その位置に見えている、重ねるトラックの映像（前に重なっているものから探す）。無ければ空 */
  QString hitOverlay(QPointF pos) const;
  /** 重ねるトラックの映像の枠（基準解像度の座標）。今の時間に出ていなければ false */
  bool overlayQuad(const OverlayItem& o, double t, QPolygonF* quad) const;
  /** 選んでいるのが映像（メイン・重ねるトラック）なら、辺のつまみ（幅・高さ）を出す */
  bool selectionStretchable() const;
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
  /** 辺のつまみのドラッグ: 始めたときの伸縮と、中心からの（回転を戻した）距離 */
  double origStretchX_ = 1;
  double origStretchY_ = 1;
  double stretchStart_ = 1;
  bool guideV_ = false, guideH_ = false;
  bool dropHover_ = false;
  bool fullScreen_ = false;
  /** 被写体を選ぶ: カーソルの位置（ウィジェット座標）と、押しているキー（Shift で足す・Alt / Ctrl で外す） */
  QPointF maskCursor_;
  bool maskCursorIn_ = false;
  Qt::KeyboardModifiers maskMods_;
  /** カーソルを合わせた所のマスク（色を付けたもの）と、作ったときの元（同じなら作り直さない） */
  QImage maskTint_;
  QString maskTintKey_;

  // 文字の直接編集の状態
  QString editId_;
  QString editMerge_;
  QString editOriginal_;
  int caret_ = 0;
  int anchor_ = 0;
  bool upstream_ = false;
  /** 上下の矢印キーで動かすときに保つ x（基準座標。-1 は未設定） */
  double wantX_ = -1;
  QString preedit_;
  int preeditCursor_ = 0;
  QList<QInputMethodEvent::Attribute> preeditAttrs_;
  std::vector<EditState> undo_;
  std::vector<EditState> redo_;
  EditKind lastKind_ = EditKind::None;
  bool applying_ = false;
  bool blinkOn_ = true;
  QTimer* blink_ = nullptr;
};

/** 再生ボタン・時刻・比率の切り替え */
class TransportBar : public QWidget {
  Q_OBJECT
 public:
  explicit TransportBar(QWidget* parent = nullptr);

 signals:
  /** 全画面ボタンが押された */
  void fullScreenRequested();

 private:
  void refresh();
  QLabel* time_;
  QToolButton* play_;
  /** ループ再生のオン・オフ */
  QToolButton* loop_;
  /** ガイド（セーフマージン・三分割のグリッド）のメニュー */
  QToolButton* guides_;
  QAction* safe_;
  QAction* grid_;
  /** オブジェクトマスク（被写体をクリックして選ぶ）のボタン */
  QToolButton* objMask_;
  int objMaskIcon_ = -1;
  /** 今出しているアイコンの状態（再生中は何度も refresh するので、変わったときだけ描き直す。-1 はまだ） */
  int playIcon_ = -1;
  int loopIcon_ = -1;
  int guidesIcon_ = -1;
  Segmented* aspect_;
};

class PreviewPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PreviewPanel(PreviewEngine* engine, QWidget* parent = nullptr);
  ~PreviewPanel() override;
  PreviewStage* stage() const { return stage_; }

  /** 全画面表示中か */
  bool isFullScreen() const { return fs_ != nullptr; }
  /**
   * プレビューだけを別のウィンドウに移して全画面で出す（編集画面は隠すだけで、レイアウトや選択などはそのまま）
   * 終了すると、プレビューを元の場所へ戻して編集画面を出す
   */
  void enterFullScreen();
  void exitFullScreen();
  void toggleFullScreen();

 private:
  void endFullScreen(bool showMain);

  PreviewStage* stage_;
  QVBoxLayout* layout_;
  FullScreenWindow* fs_ = nullptr;
  QPointer<QWidget> mainWin_;
};

}  // namespace kv
