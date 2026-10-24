#pragma once
// AI サーバーのアカウント（メールアドレスとパスワードでログインする）と、AI サーバーとのつながり。
// ログイン中の印は、Windows の DPAPI で暗号化して %LOCALAPPDATA%\JPHACKS\kantan-video-editor\session.dat に置く
// （この PC のこのユーザーだけが読める）。印は、ログインしたサーバーにしか送らない。
// つながっているかは、AI サーバーの /api/health を 30 秒ごと（とアプリに戻ってきたとき）に見て確かめる。
// 通信は ServerCall を通す（つないだ直後に切られたときは、自動でつなぎ直す）。

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <functional>
#include <optional>

namespace kv::ai {

class ServerCall;

/** その月に使ったトークンと、使える量（null は無制限） */
struct AccountUsage {
  QString month;
  qint64 used = 0;
  std::optional<qint64> allowance;
  std::optional<qint64> remaining;
};

struct AccountInfo {
  QString email;
  QString displayName;
  QString role;
  /** サーバーで使っているモデル（深さを変えられるかの判断に使う） */
  QString model;
  bool mustChangePassword = false;
  bool canUseAi = true;
  /** 選べる深さの上限（low〜max）と、はじめに選ぶ深さ（auto など） */
  QString maxEffort = QStringLiteral("max");
  QString defaultEffort = QStringLiteral("auto");
  AccountUsage usage;
};

class Account : public QObject {
  Q_OBJECT
 public:
  /** AI サーバーとのつながり */
  enum class Link {
    /** アドレスが決まっていない */
    None,
    /** 確かめている途中（まだ一度も分かっていない） */
    Checking,
    Online,
    /** サーバーは動いているが、データベースにつながっていない */
    NoDatabase,
    /** つながらない */
    Offline,
    /** つながったが、AI サーバーではない（アドレスやポート番号の間違い） */
    Wrong,
  };
  /** 前回のログインを読み直した結果 */
  enum class Refresh { LoggedIn, Expired, Unreachable };
  /** error が空なら成功 */
  using Done = std::function<void(const QString& error)>;

  static Account& instance();

  /** ログインしていて、自分の情報を読めているか */
  bool loggedIn() const { return !token_.isEmpty() && loaded_; }
  /** ログイン中の印を持っているか（サーバーにつながらず、情報はまだ読めていないこともある） */
  bool hasToken() const { return !token_.isEmpty(); }
  QString token() const { return token_; }
  const AccountInfo& info() const { return info_; }
  /** 今の AI サーバーのアドレス（.env の KANTAN_AI_SERVER か、exe に組み込んだもの。末尾の / なし。決まっていなければ空） */
  QString server() const { return server_; }
  /**
   * .env を読み直す（アプリに戻ってきたときにも自動で行う）。アドレスが変わっていたら、そのサーバーを使う
   * （前のサーバーのログインは使わない）
   */
  void reloadServer();
  /** ログイン・登録・パスワードの変更を送っている途中 */
  bool busy() const { return !reply_.isNull(); }
  /** ログインしてから、まだ深さを選んでいない（はじめの深さを使う） */
  bool freshLogin() const { return freshLogin_; }
  void clearFreshLogin() { freshLogin_ = false; }

  // ---- つながり ----
  Link link() const { return link_; }
  /** つながらない理由など（画面にそのまま出せる文。無ければ空） */
  QString linkDetail() const { return linkDetail_; }
  /** 新規登録の受け付け方（off / approval / open。分からなければ空） */
  QString registration() const { return registration_; }
  /**
   * ワークショップ（テキストのテンプレートの公開）の使い方（off / approval / open）。
   * まだ分からない・古いサーバーで使えないときは空。つながらない間は、前に分かったもののまま
   */
  QString workshop() const { return workshop_; }
  /** サーバーに Bedrock の API キーがあるか（無いと AI は動かない） */
  bool aiReady() const { return aiReady_; }
  /** 今のサーバーにつながるか、すぐに確かめる */
  void checkLink();

  // ---- ログイン ----
  /** 前回のログインを読み込む（今のサーバーのものがあれば true）。サーバーで確かめるのは refresh */
  bool loadSaved();
  void login(const QString& email, const QString& password, QObject* ctx, Done done);
  /** 新規登録。すぐに使えるサーバーなら、そのままログインする。承認待ちになったときは pendingMessage に説明が入る */
  void registerAccount(const QString& email, const QString& password, const QString& displayName, QObject* ctx,
                       std::function<void(const QString& error, const QString& pendingMessage)> done);
  void logout();
  /** 自分の情報（今月の残りなど）を読み直す。done を渡さなければ、失敗は failed で知らせる */
  void refresh(QObject* ctx = nullptr, std::function<void(Refresh)> done = {});
  void changePassword(const QString& current, const QString& next, QObject* ctx, Done done);
  /** AI の返事に付いてきた、今月の使用量 */
  void updateUsage(const QJsonObject& usage);
  /** ログインの期限が切れた（印を捨てて、ログインしていない状態にする） */
  void expire(const QString& message);

 signals:
  /** ログインした・ログアウトした・自分の情報が変わった・サーバーを変えた */
  void changed();
  /** つながり（と新規登録の受け付け方など）が変わった */
  void linkChanged();
  /** つながるかを確かめ終わった（変わっていなくても出す。「確かめる」を押したときの答えに使う） */
  void linkChecked();
  void busyChanged(bool busy);
  /** 読み直しの失敗など（画面にそのまま出せる文） */
  void failed(const QString& message);

 private:
  Account();
  /** status: HTTP の状態（つながらなければ 0、送れなければ -1）。json の error に、画面に出せる文が入ることがある */
  void request(const QString& method, const QString& path, const QJsonObject& body, bool withToken,
               std::function<void(int status, const QJsonObject& json)> done);
  void apply(const QJsonObject& me);
  /** ログインできた（印を受け取った） */
  void signedIn(const QJsonObject& json);
  void save();
  void forget();
  void setLink(Link link, const QString& detail);

  /** 送っている途中のログイン・登録・パスワードの変更と、つながりの確認（すぐに切られたときは、自動でつなぎ直す） */
  QPointer<ServerCall> reply_;
  QPointer<ServerCall> health_;
  QString server_;
  QString token_;
  /** 印を受け取ったサーバー（ほかのサーバーには印を送らない） */
  QString tokenServer_;
  AccountInfo info_;
  bool loaded_ = false;
  bool freshLogin_ = false;
  Link link_ = Link::None;
  QString linkDetail_;
  QString registration_;
  QString workshop_;
  bool aiReady_ = true;
  QTimer poll_;
  QElapsedTimer lastCheck_;
};

inline Account& account() {
  return Account::instance();
}

/** AI サーバーの使用量の JSON（usage）を読む */
AccountUsage parseUsage(const QJsonObject& usage);
/** 今月の残りの文（例: 今月の残り 1,965,192 / 2,000,000 トークン）。warn には、使い切った・AI を使えないときに true */
QString quotaSummary(const AccountInfo& info, bool* warn = nullptr);
/** 名前（ロール）。名前が無ければメールアドレス */
QString accountLabel(const AccountInfo& info);

}  // namespace kv::ai
