#include "ui/accountdialog.h"

#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "ai/account.h"
#include "ai/aiconfig.h"
#include "core/editor.h"
#include "ui/widgets.h"

namespace kv {

namespace {

QLabel* wrapped(const QString& text, const char* role, QWidget* parent) {
  QLabel* l = makeLabel(text, role, parent);
  l->setWordWrap(true);
  return l;
}

QLineEdit* passwordField(QWidget* parent, const QString& name) {
  auto* e = new QLineEdit(parent);
  e->setEchoMode(QLineEdit::Password);
  e->setAccessibleName(name);
  return e;
}

/** 見出しと入力欄を縦に並べる（見出しを押すと入力欄に移る） */
void addField(QVBoxLayout* into, const QString& label, QWidget* field, QWidget* parent) {
  QLabel* l = makeLabel(label, "muted", parent);
  l->setBuddy(field);
  into->addWidget(l);
  into->addWidget(field);
}

/** ラベルの役割（色）を変える */
void setRole(QLabel* l, const char* role) {
  l->setProperty("role", QString::fromLatin1(role));
  l->style()->unpolish(l);
  l->style()->polish(l);
}

}  // namespace

bool AccountDialog::run(QWidget* parent, Page page, bool startup) {
  AccountDialog dlg(parent, page, startup);
  dlg.exec();
  const ai::Account& a = ai::account();
  return a.loggedIn() && !a.info().mustChangePassword;
}

AccountDialog::AccountDialog(QWidget* parent, Page page, bool startup) : QDialog(parent), startup_(startup) {
  setObjectName(QStringLiteral("AccountDialog"));
  setWindowTitle(QStringLiteral("ログイン - e-editor"));
  setModal(true);
  // 大きさは中身に合わせて自動で変える: 幅は中身の 392 + 余白 で 440、高さは出ているものがちょうど入るだけ
  // （ページを切り替えたり、失敗の文を出したりするたびに合わせ直す。隠したページの高さは数えない）
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(24, 20, 24, 18);
  outer->setSizeConstraint(QLayout::SetFixedSize);
  auto* content = new QWidget(this);
  content->setFixedWidth(392);
  outer->addWidget(content);
  auto* v = new QVBoxLayout(content);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(8);

  title_ = makeLabel(QString(), "title", this);
  title_->setObjectName(QStringLiteral("AccountTitle"));
  QFont tf = title_->font();
  tf.setPointSizeF(tf.pointSizeF() * 1.25);
  title_->setFont(tf);
  v->addWidget(title_);
  lead_ = wrapped(QString(), "muted", this);
  v->addWidget(lead_);
  // サーバーのアドレスは出さない。つながらないときだけ、その理由を出す（つながっていれば場所も取らない）
  linkNote_ = wrapped(QString(), "error", this);
  linkNote_->setObjectName(QStringLiteral("AccountLinkNote"));
  linkNote_->hide();
  v->addWidget(linkNote_);
  v->addSpacing(4);

  // ログイン・新規登録の切り替え
  auto* seg = new QFrame(this);
  seg->setObjectName(QStringLiteral("Segmented"));
  switcher_ = seg;
  auto* segRow = new QHBoxLayout(seg);
  segRow->setContentsMargins(2, 2, 2, 2);
  segRow->setSpacing(2);
  loginTab_ = new QToolButton(seg);
  loginTab_->setObjectName(QStringLiteral("AccountLoginTab"));
  loginTab_->setText(QStringLiteral("ログイン"));
  registerTab_ = new QToolButton(seg);
  registerTab_->setObjectName(QStringLiteral("AccountRegisterTab"));
  registerTab_->setText(QStringLiteral("新規登録"));
  auto* group = new QButtonGroup(seg);
  for (QToolButton* b : {loginTab_, registerTab_}) {
    b->setCheckable(true);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    group->addButton(b);
    segRow->addWidget(b);
  }
  group->setExclusive(true);
  v->addWidget(seg);
  v->addSpacing(4);

  // ---- ログイン ----（3 つのページを並べて置き、今のページだけを出す）
  auto* login = new QWidget(this);
  loginPage_ = login;
  auto* lv = new QVBoxLayout(login);
  lv->setContentsMargins(0, 0, 0, 0);
  lv->setSpacing(4);
  loginNote_ = wrapped(QString(), "note", login);
  loginNote_->setObjectName(QStringLiteral("AccountLoginNote"));
  loginNote_->hide();
  lv->addWidget(loginNote_);
  loginEmail_ = new QLineEdit(login);
  loginEmail_->setObjectName(QStringLiteral("AccountLoginEmail"));
  loginEmail_->setAccessibleName(QStringLiteral("メールアドレス"));
  loginEmail_->setPlaceholderText(QStringLiteral("you@example.com"));
  addField(lv, QStringLiteral("メールアドレス"), loginEmail_, login);
  loginPasswordEdit_ = passwordField(login, QStringLiteral("パスワード"));
  loginPasswordEdit_->setObjectName(QStringLiteral("AccountLoginPassword"));
  addField(lv, QStringLiteral("パスワード"), loginPasswordEdit_, login);
  loginError_ = wrapped(QString(), "error", login);
  loginError_->setObjectName(QStringLiteral("AccountLoginError"));
  lv->addWidget(loginError_);
  lv->addSpacing(4);
  loginButton_ = new QPushButton(QStringLiteral("ログイン"), login);
  loginButton_->setObjectName(QStringLiteral("AccountLoginButton"));
  loginButton_->setProperty("primary", true);
  lv->addWidget(loginButton_);
  lv->addWidget(wrapped(QStringLiteral("パスワードを忘れたときは、管理者に仮のパスワードを作り直してもらってください。"), "dim", login));
  v->addWidget(login);

  // ---- 新規登録 ----
  auto* reg = new QWidget(this);
  registerPage_ = reg;
  auto* rv = new QVBoxLayout(reg);
  rv->setContentsMargins(0, 0, 0, 0);
  rv->setSpacing(4);
  regName_ = new QLineEdit(reg);
  regName_->setObjectName(QStringLiteral("AccountRegisterName"));
  regName_->setAccessibleName(QStringLiteral("名前"));
  regName_->setMaxLength(100);
  addField(rv, QStringLiteral("名前（空のままでもかまいません）"), regName_, reg);
  regEmail_ = new QLineEdit(reg);
  regEmail_->setObjectName(QStringLiteral("AccountRegisterEmail"));
  regEmail_->setAccessibleName(QStringLiteral("メールアドレス"));
  regEmail_->setPlaceholderText(QStringLiteral("you@example.com"));
  addField(rv, QStringLiteral("メールアドレス（ログインに使います）"), regEmail_, reg);
  regPassword_ = passwordField(reg, QStringLiteral("パスワード"));
  regPassword_->setObjectName(QStringLiteral("AccountRegisterPassword"));
  addField(rv, QStringLiteral("パスワード（10 文字以上）"), regPassword_, reg);
  regAgain_ = passwordField(reg, QStringLiteral("パスワード（もう一度）"));
  regAgain_->setObjectName(QStringLiteral("AccountRegisterAgain"));
  addField(rv, QStringLiteral("パスワード（もう一度）"), regAgain_, reg);
  regNote_ = wrapped(QString(), "dim", reg);
  regNote_->setObjectName(QStringLiteral("AccountRegisterNote"));
  rv->addWidget(regNote_);
  regError_ = wrapped(QString(), "error", reg);
  regError_->setObjectName(QStringLiteral("AccountRegisterError"));
  rv->addWidget(regError_);
  rv->addSpacing(4);
  regButton_ = new QPushButton(QStringLiteral("登録する"), reg);
  regButton_->setObjectName(QStringLiteral("AccountRegisterButton"));
  regButton_->setProperty("primary", true);
  rv->addWidget(regButton_);
  v->addWidget(reg);

  // ---- パスワードの変更 ----
  auto* pw = new QWidget(this);
  passwordPage_ = pw;
  auto* pv = new QVBoxLayout(pw);
  pv->setContentsMargins(0, 0, 0, 0);
  pv->setSpacing(4);
  pwNote_ = wrapped(QString(), "muted", pw);
  pv->addWidget(pwNote_);
  pv->addSpacing(4);
  pwCurrentRow_ = new QWidget(pw);
  auto* cv = new QVBoxLayout(pwCurrentRow_);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(4);
  pwCurrent_ = passwordField(pwCurrentRow_, QStringLiteral("今のパスワード"));
  pwCurrent_->setObjectName(QStringLiteral("AccountPasswordCurrent"));
  addField(cv, QStringLiteral("今のパスワード"), pwCurrent_, pwCurrentRow_);
  pv->addWidget(pwCurrentRow_);
  pwNext_ = passwordField(pw, QStringLiteral("新しいパスワード"));
  pwNext_->setObjectName(QStringLiteral("AccountPasswordNext"));
  addField(pv, QStringLiteral("新しいパスワード（10 文字以上）"), pwNext_, pw);
  pwAgain_ = passwordField(pw, QStringLiteral("新しいパスワード（もう一度）"));
  pwAgain_->setObjectName(QStringLiteral("AccountPasswordAgain"));
  addField(pv, QStringLiteral("新しいパスワード（もう一度）"), pwAgain_, pw);
  pwError_ = wrapped(QString(), "error", pw);
  pwError_->setObjectName(QStringLiteral("AccountPasswordError"));
  pv->addWidget(pwError_);
  pv->addSpacing(4);
  pwButton_ = new QPushButton(QStringLiteral("パスワードを変える"), pw);
  pwButton_->setObjectName(QStringLiteral("AccountPasswordButton"));
  pwButton_->setProperty("primary", true);
  pv->addWidget(pwButton_);
  v->addWidget(pw);

  // 閉じる（起動したときは「ログインしないで使う」）
  v->addSpacing(8);
  auto* bottom = new QHBoxLayout();
  bottom->addStretch(1);
  close_ = new QPushButton(startup ? QStringLiteral("ログインしないで使う") : QStringLiteral("閉じる"), this);
  close_->setObjectName(QStringLiteral("AccountClose"));
  close_->setProperty("flat", true);
  close_->setToolTip(startup ? QStringLiteral("AI アシスタントを使わずに編集します（あとから右下の「AI サーバー」でログインできます）") : QString());
  bottom->addWidget(close_);
  v->addLayout(bottom);

  // ---- 動き ----
  connect(&ai::account(), &ai::Account::linkChanged, this, &AccountDialog::refreshLink);
  connect(&ai::account(), &ai::Account::linkChecked, this, &AccountDialog::refreshLink);
  connect(&ai::account(), &ai::Account::changed, this, &AccountDialog::refreshLink);
  connect(loginTab_, &QToolButton::clicked, this, [this] { showPage(Page::Login); });
  connect(registerTab_, &QToolButton::clicked, this, [this] { showPage(Page::Register); });
  connect(loginButton_, &QPushButton::clicked, this, &AccountDialog::submitLogin);
  connect(loginEmail_, &QLineEdit::returnPressed, loginPasswordEdit_, qOverload<>(&QWidget::setFocus));
  connect(loginPasswordEdit_, &QLineEdit::returnPressed, this, &AccountDialog::submitLogin);
  connect(regButton_, &QPushButton::clicked, this, &AccountDialog::submitRegister);
  connect(regAgain_, &QLineEdit::returnPressed, this, &AccountDialog::submitRegister);
  connect(pwButton_, &QPushButton::clicked, this, &AccountDialog::submitPassword);
  connect(pwAgain_, &QLineEdit::returnPressed, this, &AccountDialog::submitPassword);
  connect(close_, &QPushButton::clicked, this, &AccountDialog::reject);
  // Enter は入力欄ごとに決めた動きだけにする（ダイアログの決まりのボタンを押さない）
  for (QPushButton* b : findChildren<QPushButton*>()) b->setAutoDefault(false);
  // 失敗の文は、出すまで場所を取らないように隠しておく
  for (QLabel* l : {loginError_, regError_, pwError_}) l->hide();

  const ai::Account& a = ai::account();
  if (a.loggedIn()) loginEmail_->setText(a.info().email);
  showPage(page == Page::Password && !a.loggedIn() ? Page::Login : page);
  refreshLink();
  // 開いたときに、つながるかを確かめ直す
  if (page_ != Page::Password) ai::account().checkLink();
}

void AccountDialog::showPage(Page page) {
  page_ = page;
  const ai::Account& a = ai::account();
  const bool password = page == Page::Password;
  const bool forced = password && a.loggedIn() && a.info().mustChangePassword;
  switcher_->setVisible(!password);
  loginTab_->setChecked(page == Page::Login);
  registerTab_->setChecked(page == Page::Register);
  // 今のページだけを出す（隠したページは高さに数えないので、ダイアログは今のページにちょうど合う）
  loginPage_->setVisible(page == Page::Login);
  registerPage_->setVisible(page == Page::Register);
  passwordPage_->setVisible(page == Page::Password);
  switch (page) {
    case Page::Login:
      title_->setText(QStringLiteral("ログイン"));
      lead_->setText(QStringLiteral("AI アシスタントを使うには、アカウントでログインします。アカウントが無いときは「新規登録」から作れます。"));
      break;
    case Page::Register:
      title_->setText(QStringLiteral("新規登録"));
      lead_->setText(QStringLiteral("AI アシスタントを使うための、自分のアカウントを作ります。"));
      break;
    case Page::Password:
      title_->setText(forced ? QStringLiteral("新しいパスワードを決めてください") : QStringLiteral("パスワードを変える"));
      lead_->setText(forced ? QStringLiteral("管理者が作った仮のパスワードでログインしました。AI を使う前に、自分だけのパスワードに変えてください。")
                            : QStringLiteral("ほかの PC でログインしたままのアプリは、ログアウトします。"));
      pwNote_->setText(QStringLiteral("アカウント: %1").arg(ai::accountLabel(a.info())));
      // ログインしたばかりなら、仮のパスワードをもう一度入れなくてよい
      pwCurrentRow_->setVisible(!(forced && !loginPassword_.isEmpty()));
      close_->setText(forced ? QStringLiteral("あとで変える") : QStringLiteral("キャンセル"));
      break;
  }
  refreshRegistration();
  // 最初に入れるところにカーソルを置く
  QLineEdit* first = page == Page::Login      ? (loginEmail_->text().isEmpty() ? loginEmail_ : loginPasswordEdit_)
                     : page == Page::Register ? regEmail_
                                              : (pwCurrentRow_->isHidden() ? pwNext_ : pwCurrent_);
  first->setFocus();
}

void AccountDialog::setError(QLabel* label, const QString& text) {
  // 空のときは場所を取らないように隠す（ダイアログの高さは、レイアウトが自動で合わせ直す）
  label->setText(text);
  label->setVisible(!text.isEmpty());
}

void AccountDialog::refreshRegistration() {
  // 受け付けていないと分かっているときだけ止める（分からないときは送ってみて、サーバーの答えを出す）
  const bool closed = registration_ == u"off";
  registerTab_->setToolTip(closed ? QStringLiteral("今は、新規登録を受け付けていません") : QString());
  regButton_->setEnabled(!closed && !ai::account().busy());
  if (closed) {
    regNote_->setText(QStringLiteral("今は、新しい登録を受け付けていません。管理者に、アカウントを作ってもらってください。"));
    setRole(regNote_, "note");
  } else {
    regNote_->setText(registration_ == u"open" ? QStringLiteral("登録すると、すぐに AI を使えます。")
                                               : QStringLiteral("管理者が登録を承認すると、ログインできるようになります。"));
    setRole(regNote_, "dim");
  }
}

void AccountDialog::refreshLink() {
  using Link = ai::Account::Link;
  const ai::Account& a = ai::account();
  registration_ = a.registration();
  refreshRegistration();
  // つながっている・確かめている途中は何も出さない。つながらないときだけ理由を出す（どの文にもアドレスは入れない）
  QString text;
  const char* role = "error";
  switch (a.link()) {
    case Link::None:
      text = ai::serverProblem(QString());
      break;
    case Link::Checking:
      break;
    case Link::Online:
      // ログインはできるが、サーバーに Bedrock のキーが無く、AI はまだ動かない（それ以外は空）
      text = a.linkDetail();
      role = "note";
      break;
    case Link::NoDatabase:
      text = a.linkDetail();
      role = "note";
      break;
    case Link::Offline:
    case Link::Wrong:
      text = a.linkDetail();
      break;
  }
  setRole(linkNote_, role);
  setError(linkNote_, text);
}

QString AccountDialog::serverProblem() const {
  return ai::serverProblem(ai::account().server());
}

void AccountDialog::setBusy(bool busy) {
  loginButton_->setEnabled(!busy);
  loginButton_->setText(busy && page_ == Page::Login ? QStringLiteral("ログインしています…") : QStringLiteral("ログイン"));
  regButton_->setText(busy && page_ == Page::Register ? QStringLiteral("登録しています…") : QStringLiteral("登録する"));
  pwButton_->setEnabled(!busy);
  pwButton_->setText(busy && page_ == Page::Password ? QStringLiteral("変えています…") : QStringLiteral("パスワードを変える"));
  if (busy) regButton_->setEnabled(false);
  else refreshRegistration();
}

void AccountDialog::submitLogin() {
  if (ai::account().busy()) return;
  setError(loginError_, QString());
  if (const QString problem = serverProblem(); !problem.isEmpty()) {
    setError(loginError_, problem);
    return;
  }
  const QString email = loginEmail_->text().trimmed();
  const QString password = loginPasswordEdit_->text();
  if (email.isEmpty() || password.isEmpty()) {
    setError(loginError_, QStringLiteral("メールアドレスとパスワードを入れてください"));
    return;
  }
  setBusy(true);
  ai::account().login(email, password, this, [this, password](const QString& error) {
    setBusy(false);
    if (!error.isEmpty()) {
      setError(loginError_, error);
      loginPasswordEdit_->selectAll();
      loginPasswordEdit_->setFocus();
      return;
    }
    loginPassword_ = password;
    loginPasswordEdit_->clear();
    afterLogin();
  });
}

void AccountDialog::submitRegister() {
  if (ai::account().busy() || !regButton_->isEnabled()) return;
  setError(regError_, QString());
  if (const QString problem = serverProblem(); !problem.isEmpty()) {
    setError(regError_, problem);
    return;
  }
  const QString email = regEmail_->text().trimmed();
  const QString password = regPassword_->text();
  if (email.isEmpty()) {
    setError(regError_, QStringLiteral("メールアドレスを入れてください"));
    regEmail_->setFocus();
    return;
  }
  if (password.size() < 10) {
    setError(regError_, QStringLiteral("パスワードは 10 文字以上にしてください"));
    regPassword_->setFocus();
    return;
  }
  if (password != regAgain_->text()) {
    setError(regError_, QStringLiteral("パスワードが 2 回とも同じになっていません"));
    regAgain_->selectAll();
    regAgain_->setFocus();
    return;
  }
  setBusy(true);
  ai::account().registerAccount(email, password, regName_->text(), this, [this, email, password](const QString& error, const QString& pending) {
    setBusy(false);
    if (!error.isEmpty()) {
      setError(regError_, error);
      return;
    }
    regPassword_->clear();
    regAgain_->clear();
    if (!pending.isEmpty()) {
      // 承認待ち: ログインの画面に戻して、メールアドレスを入れておく
      loginEmail_->setText(email);
      loginNote_->setText(pending);
      loginNote_->show();
      showPage(Page::Login);
      return;
    }
    loginPassword_ = password;
    afterLogin();
  });
}

void AccountDialog::submitPassword() {
  if (ai::account().busy()) return;
  setError(pwError_, QString());
  const QString current = pwCurrentRow_->isHidden() ? loginPassword_ : pwCurrent_->text();
  const QString next = pwNext_->text();
  if (current.isEmpty()) {
    setError(pwError_, QStringLiteral("今のパスワードを入れてください"));
    pwCurrent_->setFocus();
    return;
  }
  if (next.size() < 10) {
    setError(pwError_, QStringLiteral("新しいパスワードは 10 文字以上にしてください"));
    pwNext_->setFocus();
    return;
  }
  if (next != pwAgain_->text()) {
    setError(pwError_, QStringLiteral("新しいパスワードが 2 回とも同じになっていません"));
    pwAgain_->selectAll();
    pwAgain_->setFocus();
    return;
  }
  setBusy(true);
  ai::account().changePassword(current, next, this, [this](const QString& error) {
    setBusy(false);
    if (!error.isEmpty()) {
      setError(pwError_, error);
      // 期限が切れてログアウトしたときは、ログインの画面に戻す
      if (!ai::account().loggedIn()) showPage(Page::Login);
      return;
    }
    for (QLineEdit* e : {pwCurrent_, pwNext_, pwAgain_}) e->clear();
    loginPassword_.clear();
    editor().toast(QStringLiteral("パスワードを変えました"), ToastKind::Success);
    accept();
  });
}

void AccountDialog::afterLogin() {
  const ai::Account& a = ai::account();
  if (a.info().mustChangePassword) {
    showPage(Page::Password);
    return;
  }
  loginPassword_.clear();
  editor().toast(QStringLiteral("%1でログインしました").arg(ai::accountLabel(a.info())), ToastKind::Success);
  accept();
}

void AccountDialog::reject() {
  loginPassword_.clear();
  QDialog::reject();
}

}  // namespace kv
