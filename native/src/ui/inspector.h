#pragma once

#include <QScrollArea>

class QFrame;
class QLabel;
class QPushButton;

namespace kv {

class ProjectPanel;
class ClipPanel;
class TextPanel;
class AudioPanel;

class Inspector : public QScrollArea {
  Q_OBJECT
 public:
  explicit Inspector(QWidget* parent = nullptr);
  /** テキストの入力欄にカーソルを移す */
  void focusText();

 protected:
  /** ほかのタブの裏にいた間・閉じていた間に変わった分（再生位置の音量など）を出し直す */
  void showEvent(QShowEvent* e) override;

 private:
  void refreshCurrent();
  void onSelection();
  /** いくつかを一緒に選んでいるときの案内（グループ化のボタン） */
  void refreshMulti();
  QFrame* multi_;
  QLabel* multiText_;
  QPushButton* group_;
  QPushButton* ungroup_;
  ProjectPanel* project_;
  ClipPanel* clip_;
  TextPanel* text_;
  AudioPanel* audio_;
};

}  // namespace kv
