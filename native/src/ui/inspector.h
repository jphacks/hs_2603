#pragma once

#include <QScrollArea>

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

 private:
  void onSelection();
  ProjectPanel* project_;
  ClipPanel* clip_;
  TextPanel* text_;
  AudioPanel* audio_;
};

}  // namespace kv
