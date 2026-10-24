#pragma once
// AI サーバーにログインする画面: ログイン・新規登録・パスワードの変更。
// サーバーのアドレスは .env の KANTAN_AI_SERVER で決め、画面には出さない（つながらないときだけ、その理由を出す）。
// 起動したとき（ログインしていなければ）と、右下の「AI サーバー」・AI の欄から開く。

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace kv {

class AccountDialog : public QDialog {
  Q_OBJECT
 public:
  enum class Page { Login, Register, Password };
  /**
   * 開いて、閉じるまで待つ。ログインできて、AI を使える状態になったら true。
   * startup: アプリを起動したときに出す（閉じるボタンが「ログインしないで使う」になる）
   */
  static bool run(QWidget* parent, Page page = Page::Login, bool startup = false);
  AccountDialog(QWidget* parent, Page page, bool startup);

  void reject() override;

 private:
  void showPage(Page page);
  /** つながらないときの理由と、新規登録を受け付けているかを出す */
  void refreshLink();
  /** 新規登録を受け付けているかに合わせて、「登録する」を押せるようにする */
  void refreshRegistration();
  /** サーバーのアドレスに問題があれば、その文（.env の KANTAN_AI_SERVER を直してもらう） */
  QString serverProblem() const;
  void submitLogin();
  void submitRegister();
  void submitPassword();
  void setBusy(bool busy);
  /** ログインできた（パスワードを変える必要があれば、その画面にする） */
  void afterLogin();
  /** 失敗の文を出す（空なら隠す） */
  void setError(QLabel* label, const QString& text);

  Page page_ = Page::Login;
  bool startup_ = false;
  /** 新規登録の受け付け方（off / approval / open。分からなければ空） */
  QString registration_;
  /** このダイアログでログインしたときのパスワード（仮のパスワードを変えるとき、もう一度入れなくてよいように） */
  QString loginPassword_;

  QLabel* title_;
  QLabel* lead_;
  /** つながらないときだけ出す理由（サーバーのアドレスは書かない） */
  QLabel* linkNote_;
  QWidget* switcher_;
  QToolButton* loginTab_;
  QToolButton* registerTab_;
  /** ページ（今のページだけを出す） */
  QWidget* loginPage_;
  QWidget* registerPage_;
  QWidget* passwordPage_;
  // ログイン
  QLineEdit* loginEmail_;
  QLineEdit* loginPasswordEdit_;
  QLabel* loginNote_;
  QLabel* loginError_;
  QPushButton* loginButton_;
  // 新規登録
  QLineEdit* regName_;
  QLineEdit* regEmail_;
  QLineEdit* regPassword_;
  QLineEdit* regAgain_;
  QLabel* regNote_;
  QLabel* regError_;
  QPushButton* regButton_;
  // パスワードの変更
  QLabel* pwNote_;
  QWidget* pwCurrentRow_;
  QLineEdit* pwCurrent_;
  QLineEdit* pwNext_;
  QLineEdit* pwAgain_;
  QLabel* pwError_;
  QPushButton* pwButton_;
  QPushButton* close_;
};

}  // namespace kv
