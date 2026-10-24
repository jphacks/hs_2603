#pragma once
// 複数行の入力欄（QPlainTextEdit）。日本語入力で変換している途中も、プレースホルダー（薄い案内の文字）を消す。
// QPlainTextEdit は、文書が空のあいだプレースホルダーを出す。変換中の文字（まだ確定していない文字）は文書に入らないので、
// そのままだと、変換している間ずっと案内の文字が入力中の文字に重なって出たままになり、確定してやっと消える。

#include <QPlainTextEdit>
#include <QString>

class QFocusEvent;
class QInputMethodEvent;

namespace kv {

class PlainTextInput : public QPlainTextEdit {
 public:
  explicit PlainTextInput(QWidget* parent = nullptr);
  /** 変換している途中（確定していない文字がある） */
  bool composing() const { return composing_; }

 protected:
  void inputMethodEvent(QInputMethodEvent* e) override;
  void focusOutEvent(QFocusEvent* e) override;

 private:
  /** 変換が終わったら出し直す */
  void endComposing();

  bool composing_ = false;
  /** 変換している間、隠しておくプレースホルダー */
  QString hiddenPlaceholder_;
};

}  // namespace kv
