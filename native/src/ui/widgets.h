#pragma once
// プロパティ欄などで使う小さな部品

#include <QColor>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <vector>

class QButtonGroup;
class QEnterEvent;
class QFocusEvent;
class QGridLayout;
class QKeyEvent;
class QTimer;
class QWheelEvent;

namespace kv {

struct TextPreset;

/** 複数の中から 1 つを選ぶボタンの列 */
class Segmented : public QFrame {
  Q_OBJECT
 public:
  struct Option {
    QVariant value;
    QString label;
    QString icon;
  };
  explicit Segmented(const std::vector<Option>& options, int columns = 0, QWidget* parent = nullptr);
  /** 合う選択肢が無ければどれも選ばない（シグナルは出さない） */
  void setValue(const QVariant& v);
  QVariant value() const;

 signals:
  void changed(const QVariant& value);

 private:
  QButtonGroup* group_;
  std::vector<Option> options_;
  std::vector<QToolButton*> buttons_;
};

// ---- 数値の増減（ホイール・矢印キー）の共通の決め方 -----------------------------------------
// 数値を扱う部品（スライダー・数値入力欄）は、すべてこの決め方でホイールと矢印キーに答える:
//   通常 = 基本単位、Shift = 10 倍、Alt = 0.1 倍（小数の細かい単位を持つ項目だけ）。
//   結果は単位の格子に丸め、最小値・最大値の範囲に収める。

struct NumberStep {
  /** 基本単位（位置なら 1px、拡大率なら 1% など） */
  double unit = 1;
  /** Alt を押したときの細かい単位。0 なら細かい単位は無い（Alt を押しても基本単位） */
  double fine = 0;
  double min = 0;
  double max = 0;
};
/** cur から direction（+1 / -1）ぶん動かした値 */
double steppedValue(double cur, int direction, Qt::KeyboardModifiers mods, const NumberStep& rule);
/** 整数 % は小数なし、細かい値のときだけ小数 1 桁（例: 100% / 75.5%）。v は 1.0 = 100% */
QString formatPercent(double v);
/** 整数の度は小数なし、細かい値のときだけ小数 1 桁（例: 90° / 12.5°） */
QString formatDegrees(double v);

/**
 * 数値入力欄。直接入力に加えて、ホイール（欄の上で回す）と矢印キー（↑→ で増、↓← で減）で値を変えられる。
 * 変え方は NumberStep の決め方と同じ。連続した操作は、止まって少したったところで stepFinished() を出す
 * （元に戻す履歴を 1 回にまとめる合図）。
 */
class NumberSpin : public QDoubleSpinBox {
  Q_OBJECT
 public:
  explicit NumberSpin(QWidget* parent = nullptr);
  /** 基本単位（singleStep と同じ）と、Alt のときの細かい単位（0 なら無し） */
  void setUnit(double unit, double fine = 0);

 signals:
  /** ホイール・矢印キーの操作が一段落した */
  void stepFinished();

 protected:
  void wheelEvent(QWheelEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void focusInEvent(QFocusEvent*) override;
  void focusOutEvent(QFocusEvent*) override;
  void enterEvent(QEnterEvent*) override;
  void leaveEvent(QEvent*) override;

 private:
  void stepBy(int steps, Qt::KeyboardModifiers mods);
  void restartSettle();
  double fine_ = 0;
  double wheelAccum_ = 0;
  qint64 lastWheelMs_ = 0;
  QTimer* settle_;
};

/**
 * 見出し・値の表示つきのスライダー。ダブルクリックで基準値に戻す。
 * ホイール（スライダーや数値の上で回す）と矢印キー（数値をクリックするか Tab で選んだとき）でも値を変えられる。
 */
class SliderRow : public QWidget {
  Q_OBJECT
 public:
  SliderRow(const QString& label, double min, double max, double step, std::function<QString(double)> format, QWidget* parent = nullptr);
  void setValue(double v);
  double value() const;
  void setDefault(double v) { default_ = v; }
  void setLabelVisible(bool v);
  /** 右上の数値の表示を出す / 隠す（数値を別の入力欄で出すときに隠す） */
  void setValueVisible(bool v);
  /** つまみをドラッグしている間（とホイール・矢印キーで変えている間）だけ、つまみの近くに今の値（format で整えたもの）を出す */
  void setValueBubble(bool on) { bubbleOn_ = on; }
  /**
   * ホイール・矢印キー 1 回ぶんの基本単位。省略すると、スライダーの刻み（step）と同じ。
   * fine は Alt を押したときの細かい単位（0 なら細かい単位は無い）
   */
  void setUnit(double unit, double fine = 0);
  double unit() const { return unit_; }

 signals:
  void changed(double value);
  /** ドラッグを離した / ホイール・矢印キーの操作が一段落した（元に戻す履歴を区切る合図） */
  void released();

 protected:
  void wheelEvent(QWheelEvent*) override;
  bool eventFilter(QObject* obj, QEvent* e) override;

 private:
  friend class NumberSlider;
  void updateText();
  void updateBubble(bool force = false);
  void handleWheel(QWheelEvent* e);
  void handleArrow(QKeyEvent* e);
  /** direction ぶん（回数つき）動かす */
  void stepBy(int steps, Qt::KeyboardModifiers mods);
  void restartSettle();
  void settle();
  void setHot(bool on);
  void refreshValueStyle();
  QLabel* label_;
  QLabel* valueLabel_;
  QSlider* slider_;
  QLabel* bubble_ = nullptr;
  QTimer* settle_;
  bool bubbleOn_ = false;
  bool hot_ = false;
  bool focused_ = false;
  double min_, max_, step_;
  double unit_;
  double fine_ = 0;
  /** 今の値。スライダーの刻みより細かい値（ホイールの細かい単位）も持てる */
  double value_;
  double default_ = 0;
  double wheelAccum_ = 0;
  qint64 lastWheelMs_ = 0;
  std::function<QString(double)> format_;
};

/** 色見本とその他の色 */
class ColorSwatches : public QWidget {
  Q_OBJECT
 public:
  explicit ColorSwatches(const QString& name, QWidget* parent = nullptr);
  void setColor(const QColor& c);
  QColor color() const { return color_; }

 signals:
  void changed(const QColor& c);

 private:
  void refresh();
  QString name_;
  QColor color_;
  std::vector<QToolButton*> buttons_;
  QToolButton* custom_;
};

/** 秒数の入力欄（直接入力・ホイール・矢印キー。基本は 0.1 秒、Shift で 1 秒） */
class SecondsField : public QWidget {
  Q_OBJECT
 public:
  SecondsField(const QString& label, double min, double max, QWidget* parent = nullptr);
  void setValue(double v);
  double value() const { return spin_->value(); }

 signals:
  void changed(double value);
  /** 入力を確定した / ホイール・矢印キーの操作が一段落した（元に戻す履歴を区切る合図） */
  void released();

 private:
  NumberSpin* spin_;
};

/** 見出しつきのまとまり。中身は body() に追加する */
class Section : public QWidget {
 public:
  explicit Section(const QString& title, QWidget* parent = nullptr);
  QVBoxLayout* body() { return body_; }
  void setHeaderWidget(QWidget* w);

 private:
  QVBoxLayout* body_;
  QHBoxLayout* head_;
};

QLabel* makeLabel(const QString& text, const char* role = nullptr, QWidget* parent = nullptr);
QFrame* makeSeparator(QWidget* parent = nullptr);
QToolButton* makeToolButton(const QString& iconName, const QString& tip, QWidget* parent = nullptr, const QString& text = QString());
/** テキストのスタイルを選ぶカード。同じ見本の文字を描いたカードの下にスタイル名を出す */
QToolButton* makeTextPresetButton(const TextPreset& preset, QSize cardSize, QWidget* parent = nullptr);
/**
 * スタイルのカードをドラッグして、タイムラインへ落とせるようにする。カードの半透明の見本がカーソルに付いてくる。
 * クリックしただけではドラッグにならない（ドラッグ後は clicked も出ない）。ドロップ先の処理は受け取る側が行う
 */
void makeTextPresetDraggable(QToolButton* button, const TextPreset& preset);
/** 自分で保存したテキストのテンプレートの右クリックメニュー（名前を変更・削除・ワークショップに公開・公開をやめる） */
void execTemplateMenu(QWidget* parent, const QString& templateId, const QPoint& globalPos);
/** テキスト textId の見た目を、名前を聞いてマイテンプレートに保存する（右の設定欄と、右クリックのメニューから） */
void saveTextAsTemplate(QWidget* parent, const QString& textId);
/** ホイールでは値を変えない。ホイールは親に渡すので、パネルはそのままスクロールできる */
void disableWheel(QWidget* w);

}  // namespace kv
