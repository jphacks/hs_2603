#pragma once

#include <QHash>
#include <QObject>
#include <QPoint>
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

  /** ビューを前に出す。閉じていれば、そのビューの欄に開く */
  void showView(const QString& id);

  /** 表示メニューの「パースペクティブ」「ビュー」の中身を作り直す（メニューを開く直前に呼ぶ） */
  void fillPerspectiveMenu(QMenu* menu);
  void fillViewMenu(QMenu* menu);

  // ---- 欄のタブから使う ----
  void beginDrag(ViewArea* from, int index);
  void dragMove(const QPoint& globalPos);
  void endDrag(const QPoint& globalPos, bool drop);
  void execTabMenu(ViewArea* area, int index, const QPoint& globalPos);
  /** タブの × で、そのビューを閉じる（「表示 → ビュー」で開き直せる） */
  void closeTab(ViewArea* area, int index);
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
