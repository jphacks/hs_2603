#include "ui/plaintextinput.h"

#include <QFocusEvent>
#include <QInputMethodEvent>

namespace kv {

PlainTextInput::PlainTextInput(QWidget* parent) : QPlainTextEdit(parent) {}

void PlainTextInput::inputMethodEvent(QInputMethodEvent* e) {
  const bool composing = !e->preeditString().isEmpty();
  // 変換が始まったら、文字が出るのと同時にプレースホルダーを消す（変換中の文字を描く前に隠す）
  if (composing && !composing_) {
    composing_ = true;
    hiddenPlaceholder_ = placeholderText();
    setPlaceholderText(QString());
  }
  QPlainTextEdit::inputMethodEvent(e);
  // 確定した・取り消した（Esc で消した）ときは戻す。確定した文字があれば、文書が空でないので出ない
  if (!composing && composing_) endComposing();
}

void PlainTextInput::focusOutEvent(QFocusEvent* e) {
  QPlainTextEdit::focusOutEvent(e);
  // ほかの所を押したときは、日本語入力が変換を終える。終わりの知らせが来なくても、案内の文字が消えたままにならないようにする
  if (composing_) endComposing();
}

void PlainTextInput::endComposing() {
  composing_ = false;
  setPlaceholderText(hiddenPlaceholder_);
  hiddenPlaceholder_.clear();
}

}  // namespace kv
