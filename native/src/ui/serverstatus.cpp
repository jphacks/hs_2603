#include "ui/serverstatus.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QUrl>

#include "ai/account.h"
#include "ai/aiconfig.h"
#include "core/editor.h"
#include "ui/accountdialog.h"
#include "ui/theme.h"

namespace kv {

LinkLook currentLinkLook() {
  using Link = ai::Account::Link;
  const ai::Account& a = ai::account();
  switch (a.link()) {
    case Link::None:
      return {theme::dim, QStringLiteral("未設定")};
    case Link::Checking:
      return {theme::dim, QStringLiteral("確認中…")};
    case Link::Offline:
      return {theme::danger, QStringLiteral("つながりません")};
    case Link::Wrong:
      return {theme::danger, QStringLiteral("アドレスを確かめてください")};
    case Link::NoDatabase:
      return {theme::warn, QStringLiteral("データベースにつながりません")};
    case Link::Online:
      break;
  }
  if (!a.loggedIn()) return {theme::warn, QStringLiteral("ログインしていません")};
  if (a.info().mustChangePassword) return {theme::warn, QStringLiteral("パスワードを変えてください")};
  const QString name = a.info().displayName.isEmpty() ? a.info().email : a.info().displayName;
  // サーバーに Bedrock のキーが無いと、ログインはできても AI は動かない
  return {a.aiReady() ? theme::ok : theme::warn, QStringLiteral("接続中（%1）").arg(name)};
}

QPixmap linkDot(const QColor& color, qreal dpr) {
  const int size = 8;
  QPixmap pm(QSize(size, size) * dpr);
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(color);
  p.drawEllipse(QRectF(0.5, 0.5, size - 1, size - 1));
  return pm;
}

/** アドレスを書いてもらう .env の場所 */
static QString envHintPath() {
  const QString used = ai::envFile();
  return used.isEmpty() ? ai::suggestedEnvPath() : used;
}

ServerStatus::ServerStatus(QWidget* parent) : QToolButton(parent), menu_(new QMenu(this)) {
  setObjectName(QStringLiteral("ServerStatus"));
  setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  setAutoRaise(true);
  setFocusPolicy(Qt::TabFocus);
  setCursor(Qt::PointingHandCursor);
  setIconSize(QSize(8, 8));
  connect(this, &QToolButton::clicked, this, &ServerStatus::onClicked);
  connect(&ai::account(), &ai::Account::linkChanged, this, &ServerStatus::refresh);
  connect(&ai::account(), &ai::Account::changed, this, &ServerStatus::refresh);
  connect(&ai::account(), &ai::Account::linkChecked, this, &ServerStatus::announce);
  refresh();
}

void ServerStatus::refresh() {
  const ai::Account& a = ai::account();
  const LinkLook look = currentLinkLook();
  setIcon(QIcon(linkDot(look.color, devicePixelRatioF())));
  setText(QStringLiteral("AI サーバー: %1").arg(look.text));
  setAccessibleName(QStringLiteral("AI サーバー"));
  setAccessibleDescription(look.text);
  // サーバーのアドレスは出さない（.env に書くだけで、画面では見せない）
  QStringList tip;
  if (a.server().isEmpty()) {
    tip << QStringLiteral("AI サーバーのアドレスが決まっていません")
        << QStringLiteral("%1 に KANTAN_AI_SERVER=（AI サーバーのアドレス）を書くと、AI アシスタントを使えます").arg(envHintPath());
  } else if (!a.linkDetail().isEmpty()) {
    tip << a.linkDetail();
    // アドレスの書き方の問題なら、どのファイルを直せばよいかだけ出す
    if (a.link() == ai::Account::Link::Wrong) tip << QStringLiteral("設定のファイル: %1").arg(ai::envFile());
  }
  if (a.loggedIn()) tip << ai::accountLabel(a.info()) << ai::quotaSummary(a.info());
  tip << (a.server().isEmpty() ? QStringLiteral("押すと、.env の場所が出ます")
          : a.loggedIn()       ? QStringLiteral("押すと、アカウントのメニューが出ます")
                               : QStringLiteral("押すと、ログインの画面が出ます"));
  setToolTip(tip.join(QLatin1Char('\n')));
}

void ServerStatus::announce() {
  // 「確かめ直す」「読み直す」を押したときだけ、答えを知らせる（30 秒ごとの確認では出さない）
  if (!announce_) return;
  announce_ = false;
  const ai::Account& a = ai::account();
  if (a.server().isEmpty()) {
    editor().toast(QStringLiteral("%1 に KANTAN_AI_SERVER が見つかりません").arg(envHintPath()), ToastKind::Error);
  } else if (a.link() == ai::Account::Link::Online) {
    editor().toast(QStringLiteral("AI サーバーにつながっています"), ToastKind::Success);
  } else if (a.link() != ai::Account::Link::Checking) {
    editor().toast(a.linkDetail().isEmpty() ? QStringLiteral("AI サーバー: %1").arg(currentLinkLook().text) : a.linkDetail(), ToastKind::Error);
  }
}

void ServerStatus::popupMenu() {
  // 画面の下にあるので、ボタンの上に右をそろえて出す
  const QSize hint = menu_->sizeHint();
  menu_->popup(mapToGlobal(QPoint(width() - hint.width(), -hint.height())));
}

void ServerStatus::onClicked() {
  ai::Account& a = ai::account();
  // .env を書き換えたばかりかもしれないので、読み直してから決める
  a.reloadServer();
  menu_->clear();
  if (a.server().isEmpty()) {
    // アドレスが決まっていない: どこに書けばよいかを出す
    const QString path = envHintPath();
    menu_->addAction(QStringLiteral("AI サーバーのアドレスが決まっていません"))->setEnabled(false);
    menu_->addAction(QStringLiteral("%1 に").arg(path))->setEnabled(false);
    menu_->addAction(QStringLiteral("KANTAN_AI_SERVER=（AI サーバーのアドレス）を書いてください"))->setEnabled(false);
    menu_->addSeparator();
    menu_->addAction(QStringLiteral(".env のフォルダーを開く"), this, [path] {
      const QString dir = QFileInfo(path).absolutePath();
      QDir().mkpath(dir);
      QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    menu_->addAction(QStringLiteral(".env を読み直す"), this, [this] {
      announce_ = true;
      ai::account().reloadServer();
      ai::account().checkLink();
    });
    popupMenu();
    return;
  }
  // ログインしていなければ、ログインの画面
  if (!a.loggedIn()) {
    a.checkLink();
    AccountDialog::run(window(), AccountDialog::Page::Login);
    return;
  }
  menu_->addAction(ai::accountLabel(a.info()))->setEnabled(false);
  menu_->addAction(ai::quotaSummary(a.info()))->setEnabled(false);
  menu_->addSeparator();
  menu_->addAction(QStringLiteral("つながりを確かめ直す"), this, [this] {
    announce_ = true;
    ai::account().checkLink();
    ai::account().refresh();
  });
  menu_->addAction(QStringLiteral("パスワードを変える..."), this, [this] { AccountDialog::run(window(), AccountDialog::Page::Password); });
  menu_->addSeparator();
  menu_->addAction(QStringLiteral("ログアウト"), this, [] { ai::account().logout(); });
  popupMenu();
}

}  // namespace kv
