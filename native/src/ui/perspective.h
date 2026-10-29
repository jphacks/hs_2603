#pragma once

#include <QHash>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <utility>
#include <vector>

class QLabel;
class QMenu;
class QSplitter;
class QWidget;

namespace kv {

class ViewArea;

/** ビューの並び（1 つのパースペクティブの中身） */
struct PerspectiveLayout {
  QStringList left;   // 左の欄のタブ（左から順に）
  QStringList right;  // 右の欄のタブ
  QString leftCurrent;
  QString rightCurrent;
  int leftWidth = 264;
  int rightWidth = 316;
  /**
   * この並びを覚えたときにあったビュー（閉じていたものも）。ここに無いビュー（あとの版で増えたビュー）は、
   * 並びを戻すときに、そのビューの欄（home）の最後に開く
   */
  QStringList known;
};

/**
 * Eclipse のパースペクティブのような、左右の欄のビュー（メディア・オーディオ・テキスト・プロパティ）の並び。
 * ビューはタブをドラッグして左右の欄へ動かしたり、並べ替えたり、閉じたりできる。
 * 並びは名前を付けて保存し、切り替えられる。使っている間に変えた並びは、パースペクティブごとに覚えておき
 * （次に起動したときもそのまま）、「リセット」で保存したときの並びに戻せる。
 * 保存先は settings.ini と同じフォルダーの perspectives.json
 */
class PerspectiveManager : public QObject {
  Q_OBJECT
 public:
  enum class Side { Left, Right };

  /** window: ダイアログを出すときの親 */
  explicit PerspectiveManager(QWidget* window);

  /** 左・右の欄（QSplitter に並べる） */
  QWidget* area(Side side) const;
  /** 欄を並べた QSplitter（欄の幅を覚えるため）。center は、残りの幅を使う部品（プレビュー） */
  void attach(QSplitter* splitter, int leftIndex, int centerIndex, int rightIndex);
  /** ビューを登録する。home は「標準」のパースペクティブで置く欄で、閉じたビューを開き直すときもここに置く */
  void addView(const QString& id, const QString& title, QWidget* widget, Side home);

  /** 前回のパースペクティブと並びに戻す（ビューを全部登録してから呼ぶ） */
  void restore();
  /** 今の並びを、すぐに保存する（終了するとき） */
  void storeNow();

  /** ビューを前に出す。閉じていれば、そのビューの欄に開く（左の欄を折りたたんでいれば、開く） */
  void showView(const QString& id);

  /**
   * 左の欄を折りたたむ（左端に細い帯だけを残し、空いた幅はプレビューが使う）/ 開く（折りたたむ前の幅・タブに戻す）。
   * 中のビュー（一覧・選んでいるタブ）はそのまま。次に起動したときも同じ状態にする
   */
  void setLeftCollapsed(bool on);
  bool leftCollapsed() const { return leftCollapsed_; }
  /** 左の欄にビューがあるか（無ければ、欄そのものを出していない） */
  bool leftHasViews() const;

  /** ビューを見せるために押す所（操作の案内で使う） */
  struct ViewReach {
    /** もう前に出ていて、中身が見えている */
    bool visible = false;
    /** 閉じている（「表示」→「ビュー」から開く） */
    bool closed = false;
    /** 折りたたんだ左の欄にある（rect は、細い帯のタブの名前） */
    bool collapsed = false;
    /** 押す所（画面の座標）。visible ならタブそのもの。閉じていれば空 */
    QRect rect;
    /** ビューの名前（メディア など） */
    QString title;
  };
  ViewReach reachView(const QString& id) const;
  /** ビューの中身（無ければ nullptr） */
  QWidget* viewWidget(const QString& id) const;

 signals:
  void leftCollapsedChanged(bool collapsed);
  /** 左の欄のビューが増えた・減った（ビューが無くなると、左の欄は出さない） */
  void leftViewsChanged();

 public:

  /** 表示メニューの「パースペクティブ」「ビュー」の中身を作り直す（メニューを開く直前に呼ぶ） */
  void fillPerspectiveMenu(QMenu* menu);
  void fillViewMenu(QMenu* menu);

  // ---- 欄のタブから使う ----
  void beginDrag(ViewArea* from, int index);
  void dragMove(const QPoint& globalPos);
  void endDrag(const QPoint& globalPos, bool drop);
  void execTabMenu(ViewArea* area, int index, const QPoint& globalPos);
  /** タブの切り替え・欄の幅の変更（少し待ってから保存する） */
  void scheduleSave();

 private:
  struct View {
    QString id;
    QString title;
    QWidget* widget = nullptr;
    Side home = Side::Left;
  };

  const View* view(const QString& id) const;
  QString idOf(const QWidget* w) const;
  ViewArea* areaOf(Side side) const;
  /** ビューが置いてある欄（閉じていれば nullptr） */
  ViewArea* areaContaining(const QString& id) const;
  /** 画面の位置にある欄（無ければ nullptr） */
  ViewArea* areaAt(const QPoint& globalPos) const;
  /** 欄のタブの、ドロップしたときに入る位置 */
  int insertIndexAt(ViewArea* area, const QPoint& globalPos) const;

  void moveView(const QString& id, Side side, int index = -1);
  void closeView(const QString& id);

  PerspectiveLayout capture();
  void apply(const PerspectiveLayout& layout);
  PerspectiveLayout defaultLayout() const;
  /** 保存したときの並び（「標準」は、はじめの並び） */
  PerspectiveLayout definition(const QString& name) const;

  QStringList names() const;
  bool isKnown(const QString& name) const;
  bool isBuiltin(const QString& name) const;
  void switchTo(const QString& name);
  void saveAs();
  void resetCurrent();
  void remove(const QString& name);

  void captureWidths();
  void applySizes();
  /** ビューの無い欄は隠す（ドラッグしている間は、置き場所として出す） */
  void updateAreaVisibility();
  void readStore();
  void writeStore();

  QWidget* window_;
  ViewArea* left_;
  ViewArea* right_;
  QSplitter* splitter_ = nullptr;
  int leftIndex_ = 0;
  int centerIndex_ = 1;
  int rightIndex_ = 2;
  int leftWidth_ = 264;
  int rightWidth_ = 316;
  /** 左の欄を折りたたんでいる（leftWidth_ は、開いたときに戻す幅のまま） */
  bool leftCollapsed_ = false;
  std::vector<View> views_;

  QString current_;
  /** 名前を付けて保存したパースペクティブ（保存した順） */
  std::vector<std::pair<QString, PerspectiveLayout>> saved_;
  /** パースペクティブごとの、使っている間に変えた並び */
  QHash<QString, PerspectiveLayout> state_;
  QTimer saveTimer_;
  bool applying_ = false;

  // ドラッグしている間
  bool dragging_ = false;
  QString dragId_;
  QLabel* dragLabel_ = nullptr;
};

}  // namespace kv
