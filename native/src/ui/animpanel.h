#pragma once
// アニメーション（キーフレーム）の設定欄。映像・文字を選んでいるときに、インスペクターに出す。
// 位置・拡大率・回転・不透明度を、項目ごとに ◆ ボタンでキーフレームにできる。

#include <QAbstractButton>
#include <QWidget>
#include <optional>
#include <vector>

#include "core/editor.h"
#include "ui/widgets.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QToolButton;

namespace kv {

class SliderRow;

/** ◆（キーフレーム）と ◀ ▶（前後のキーフレームへ）のボタン。QPainter で描く */
class KeyButton : public QAbstractButton {
  Q_OBJECT
 public:
  enum class Kind { Diamond, Prev, Next };
  explicit KeyButton(Kind kind, QWidget* parent = nullptr);
  /** ◆: 今の再生位置にキーフレームがあるとき true（強調して表示する） */
  void setActive(bool on);
  bool active() const { return active_; }
  QSize sizeHint() const override { return {24, 24}; }

 protected:
  void paintEvent(QPaintEvent*) override;
  void enterEvent(QEnterEvent*) override;
  void leaveEvent(QEvent*) override;

 private:
  Kind kind_;
  bool active_ = false;
  bool hover_ = false;
};

/** 音量キーフレームの ◀ ◆ ▶（映像の音声・オーディオ用）。ID でどちらかを探し、再生位置に追加・削除する */
class VolumeKeyControls : public QWidget {
  Q_OBJECT
 public:
  explicit VolumeKeyControls(QWidget* parent = nullptr);
  void setTarget(const QString& id) { id_ = id; }
  /** ◆ の強調を更新する */
  void refresh();

 private:
  /** 対象の、タイムライン上の開始時間とキーフレーム（なければ nullptr） */
  const std::vector<VolumePoint>* points(double* start) const;
  QString id_;
  KeyButton* prev_;
  KeyButton* diamond_;
  KeyButton* next_;
};

class AnimPanel : public QWidget {
  Q_OBJECT
 public:
  explicit AnimPanel(QWidget* parent = nullptr);
  /** 選んでいる映像・文字の今の値に合わせて表示を更新する */
  void refresh();

 private:
  struct Row {
    AnimProp prop;
    QLabel* label = nullptr;
    SliderRow* slider = nullptr;
    KeyButton* prev = nullptr;
    KeyButton* diamond = nullptr;
    KeyButton* next = nullptr;
  };
  void setOpen(bool open);
  /** 選んでいる項目の、再生位置にあるキーフレーム（無ければ空） */
  std::optional<Editor::KeyRef> activeKey() const;
  /** 再生位置にある、すべての項目のキーフレーム（コピーの対象） */
  std::vector<Editor::KeyRef> keysAtPlayhead() const;

  bool open_ = true;
  /** 最後に触った項目（時間・値・補間方式の入力欄は、この項目のキーフレームを編集する） */
  AnimProp active_ = AnimProp::X;
  /** キーフレームの時間を動かしている途中（表示を更新しない） */
  bool busy_ = false;
  QToolButton* toggle_;
  QCheckBox* autoKey_;
  QWidget* body_;
  QLabel* hint_;
  std::vector<Row> rows_;
  QWidget* keyEdit_;
  QLabel* keyTitle_;
  NumberSpin* keyTime_;
  NumberSpin* keyValue_;
  QComboBox* easing_;
  QPushButton* copy_;
  QPushButton* paste_;
  QPushButton* clear_;
};

}  // namespace kv
