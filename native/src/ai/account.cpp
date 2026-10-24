#include "ai/account.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <windows.h>
#include <wincrypt.h>

#include "ai/aiconfig.h"
#include "ai/servercall.h"

namespace kv::ai {

/** つながりを確かめる間隔と、返事を待つ時間（1 回あたり。すぐに切られたときは、つなぎ直す） */
static constexpr int kPollMs = 30000;
static constexpr int kHealthTimeoutMs = 5000;

// ---- ログイン中の印の保存（DPAPI: この PC のこのユーザーだけが元に戻せる） ----------------------

static QString sessionPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("session.dat"));
}

static QByteArray protect(const QByteArray& plain) {
  DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"kantan-video-editor", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
  QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
  LocalFree(out.pbData);
  return result;
}

static QByteArray unprotect(const QByteArray& data) {
  DATA_BLOB in{DWORD(data.size()), reinterpret_cast<BYTE*>(const_cast<char*>(data.constData()))};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
  QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return result;
}

// ---- 読み取り -------------------------------------------------------------------------

static std::optional<qint64> optionalNumber(const QJsonValue& v) {
  if (!v.isDouble()) return std::nullopt;
  return qint64(v.toDouble());
}

AccountUsage parseUsage(const QJsonObject& u) {
  AccountUsage out;
  out.month = u.value(QStringLiteral("month")).toString();
  out.used = qint64(u.value(QStringLiteral("used")).toDouble());
  out.allowance = optionalNumber(u.value(QStringLiteral("allowance")));
  out.remaining = optionalNumber(u.value(QStringLiteral("remaining")));
  return out;
}

QString quotaSummary(const AccountInfo& info, bool* warn) {
  // トークンの数は 3 桁ごとにカンマ
  const auto n = [](qint64 v) { return QLocale(QLocale::Japanese, QLocale::Japan).toString(v); };
  bool w = false;
  QString s;
  const AccountUsage& u = info.usage;
  if (!info.canUseAi) {
    s = QStringLiteral("このアカウントでは AI を使えません");
    w = true;
  } else if (!u.allowance) {
    s = QStringLiteral("今月 %1 トークン使用（上限なし）").arg(n(u.used));
  } else if (u.remaining.value_or(0) <= 0) {
    s = QStringLiteral("今月のトークンを使い切りました（%1）").arg(n(*u.allowance));
    w = true;
  } else {
    s = QStringLiteral("今月の残り %1 / %2 トークン").arg(n(*u.remaining), n(*u.allowance));
  }
  if (warn) *warn = w;
  return s;
}

QString accountLabel(const AccountInfo& info) {
  const QString name = info.displayName.isEmpty() ? info.email : info.displayName;
  return info.role.isEmpty() ? name : QStringLiteral("%1（%2）").arg(name, info.role);
}

/** つながらなかった理由を、次にどうすればよいか分かる文にする */
/** gaveUpAfter: 毎回すぐに切られて、つなぎ直すのをあきらめたときの、つなぎ直した回数（そうでなければ 0） */
static QString networkProblem(QNetworkReply::NetworkError error, const QString& detail, int gaveUpAfter) {
  if (gaveUpAfter > 0) {
    return QStringLiteral("AI サーバーにつないでも、すぐに切られてしまいます（%1 回つなぎ直しました）。少し待ってから、もう一度試してください").arg(gaveUpAfter);
  }
  switch (error) {
    case QNetworkReply::ConnectionRefusedError:
      return QStringLiteral("AI サーバーが起動していないか、ポート番号が違います");
    case QNetworkReply::HostNotFoundError:
      return QStringLiteral("サーバーが見つかりません。アドレスを確かめてください");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
      return QStringLiteral("サーバーから返事がありません。サーバーの PC が起動しているか、同じネットワークにつながっているか確かめてください");
    case QNetworkReply::SslHandshakeFailedError:
    case QNetworkReply::TooManyRedirectsError:
    case QNetworkReply::InsecureRedirectError:
      return QStringLiteral("HTTPS でつなげませんでした（%1）").arg(detail);
    default:
      return QStringLiteral("AI サーバーにつながりません（%1）").arg(detail);
  }
}

/** サーバーの失敗の文（サーバーが日本語の説明を返していればそれを使う） */
static QString errorText(int status, const QJsonObject& json) {
  const QString message = json.value(QStringLiteral("error")).toString();
  if (!message.isEmpty()) return message;
  if (status == 0) return json.value(QStringLiteral("networkError")).toString(QStringLiteral("AI サーバーにつながりません"));
  return QStringLiteral("AI サーバーでエラーが起きました（%1）").arg(status);
}

/** /api/health の返事を読む */
static Account::Link readHealth(int status, QNetworkReply::NetworkError error, const QString& errorString, int gaveUpAfter, const QJsonObject& json,
                                QString& detail, QString& registration, bool& aiReady) {
  registration.clear();
  aiReady = true;
  if (status == 0) {
    detail = networkProblem(error, errorString, gaveUpAfter);
    return Account::Link::Offline;
  }
  // 前に置いた中継（Cloudflare など）は、AI サーバーの PC が止まっていると 502・530 などを返す。アドレスの間違いではない
  if (status >= 500) {
    detail = QStringLiteral("AI サーバーにつながりません（エラー %1）。AI サーバーが止まっているかもしれません").arg(status);
    return Account::Link::Offline;
  }
  // 古いサーバーは service を返さないが、bedrockConfigured は返す
  const bool kantan = json.value(QStringLiteral("service")).toString() == u"kantan-ai-server" || json.contains(QStringLiteral("bedrockConfigured"));
  if (status != 200 || !kantan) {
    detail = QStringLiteral(".env の KANTAN_AI_SERVER が、AI サーバーではないところを指しているようです。アドレスとポート番号を確かめてください");
    return Account::Link::Wrong;
  }
  registration = json.value(QStringLiteral("registration")).toString(QStringLiteral("off"));
  aiReady = json.value(QStringLiteral("bedrockConfigured")).toBool(true);
  if (!json.value(QStringLiteral("ok")).toBool()) {
    detail = QStringLiteral("AI サーバーは動いていますが、データベース（PostgreSQL）につながっていません。サーバーの管理者に知らせてください");
    return Account::Link::NoDatabase;
  }
  detail = aiReady ? QString() : QStringLiteral("サーバーに Bedrock の API キーが設定されていないので、AI はまだ使えません");
  return Account::Link::Online;
}

/** ctx が消えていたら呼ばない done（ログインの画面を閉じたあとに結果が来たときなど）。ctx が null ならいつも呼ぶ */
template <typename F>
static auto guarded(QObject* ctx, F done) {
  QPointer<QObject> guard(ctx);
  const bool hasContext = ctx != nullptr;
  return [guard, hasContext, done](auto&&... args) {
    if (hasContext && !guard) return;
    if (done) done(args...);
  };
}

// ---- Account -------------------------------------------------------------------------

Account& Account::instance() {
  static Account* a = new Account();
  return *a;
}

Account::Account() {
  server_ = configuredServer();
  link_ = server_.isEmpty() ? Link::None : Link::Checking;
  poll_.setInterval(kPollMs);
  connect(&poll_, &QTimer::timeout, this, &Account::checkLink);
  poll_.start();
  // アプリに戻ってきたら .env を読み直して、つながりを確かめ直す（続けて切り替えたときは、5 秒あける）
  connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState s) {
    if (s != Qt::ApplicationActive) return;
    reloadServer();
    if (!lastCheck_.isValid() || lastCheck_.elapsed() > 5000) checkLink();
  });
  QTimer::singleShot(0, this, &Account::checkLink);
}

void Account::reloadServer() {
  const QString next = configuredServer();
  if (next == server_) return;
  server_ = next;
  // 前のサーバーのログインは使わない（.env を前のアドレスに戻せば、保存してある印をまた使える）
  if (!token_.isEmpty() && tokenServer_ != server_) {
    token_.clear();
    tokenServer_.clear();
    info_ = AccountInfo();
    loaded_ = false;
    freshLogin_ = false;
  }
  // 前のサーバーを確かめている途中（つなぎ直すのを待っている途中も）なら、やめる（その答えは使わない）
  if (ServerCall* old = health_.data()) {
    health_.clear();
    old->abort();
  }
  registration_.clear();
  aiReady_ = true;
  setLink(server_.isEmpty() ? Link::None : Link::Checking, QString());
  // 今のサーバーの印が保存してあれば使う
  if (loadSaved()) refresh();
  emit changed();
  checkLink();
}

void Account::setLink(Link link, const QString& detail) {
  if (link == link_ && detail == linkDetail_) return;
  link_ = link;
  linkDetail_ = detail;
  emit linkChanged();
}

void Account::checkLink() {
  if (server_.isEmpty()) {
    setLink(Link::None, QString());
    emit linkChecked();
    return;
  }
  if (const QString problem = serverProblem(server_); !problem.isEmpty()) {
    setLink(Link::Wrong, problem);
    emit linkChecked();
    return;
  }
  if (health_) return;  // 確かめている途中（つなぎ直すのを待っている途中も）
  lastCheck_.start();
  QNetworkRequest req(QUrl(server_ + QStringLiteral("/api/health")));
  req.setRawHeader("Accept", "application/json");
  req.setTransferTimeout(kHealthTimeoutMs);
  ServerCall* call = ServerCall::get(req, this);
  health_ = call;
  const QString target = server_;
  connect(call, &ServerCall::finished, this, [this, call, target](QNetworkReply* r) {
    if (health_.data() == call) health_.clear();
    if (target != server_) return;  // 途中でサーバーを変えた
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject json = QJsonDocument::fromJson(r->readAll()).object();
    QString detail;
    QString registration;
    bool aiReady = true;
    // Qt の説明にアドレスが入っていることがあるので伏せる（アドレスは画面に出さない）
    const int gaveUpAfter = call->gaveUp() ? call->retries() : 0;
    const Link link = readHealth(status, r->error(), hideServer(r->errorString(), target), gaveUpAfter, json, detail, registration, aiReady);
    // 新規登録の受け付け方や API キーの有無だけが変わったときも、画面を直す
    const bool same = link == link_ && detail == linkDetail_ && registration == registration_ && aiReady == aiReady_;
    link_ = link;
    linkDetail_ = detail;
    registration_ = registration;
    aiReady_ = aiReady;
    if (!same) emit linkChanged();
    emit linkChecked();
    // つながらない間に読めなかった自分の情報を、つながったら読む
    if (link == Link::Online && hasToken() && !loaded_) refresh();
  });
}

void Account::request(const QString& method, const QString& path, const QJsonObject& body, bool withToken,
                      std::function<void(int, const QJsonObject&)> done) {
  if (const QString problem = serverProblem(server_); !problem.isEmpty()) {
    QTimer::singleShot(0, this, [done, problem] { done(-1, QJsonObject{{QStringLiteral("error"), problem}}); });
    return;
  }
  QNetworkRequest req(QUrl(server_ + path));
  req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
  req.setRawHeader("Accept", "application/json");
  // 印は、それを受け取ったサーバーにだけ送る
  if (withToken && !token_.isEmpty() && tokenServer_ == server_) req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setTransferTimeout(30000);
  // すぐに切られたときは自動でつなぎ直す（POST は、サーバーに届く前に切られたときだけ）
  ServerCall* call = method == u"GET" ? ServerCall::get(req, this) : ServerCall::post(req, QJsonDocument(body).toJson(QJsonDocument::Compact), this);
  // ログイン・登録・パスワードの変更のときだけ「送っている途中」にする（二重に送らないように）
  const bool userAction = path != u"/api/me" && path != u"/api/auth/logout";
  if (userAction) {
    reply_ = call;
    emit busyChanged(true);
  }
  const QString target = server_;
  connect(call, &ServerCall::finished, this, [this, call, done, userAction, target](QNetworkReply* r) {
    if (userAction && reply_.data() == call) {
      reply_.clear();
      emit busyChanged(false);
    }
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QJsonObject json = QJsonDocument::fromJson(r->readAll()).object();
    if (status == 0) {
      const QString problem = networkProblem(r->error(), hideServer(r->errorString(), target), call->gaveUp() ? call->retries() : 0);
      json.insert(QStringLiteral("networkError"), problem);
      setLink(Link::Offline, problem);
    } else if (link_ != Link::Online) {
      // 返事が来たので、つながりを確かめ直す
      checkLink();
    }
    done(status, json);
  });
}

void Account::apply(const QJsonObject& me) {
  const QJsonObject user = me.value(QStringLiteral("user")).toObject();
  const QJsonObject policy = me.value(QStringLiteral("policy")).toObject();
  info_.email = user.value(QStringLiteral("email")).toString();
  info_.displayName = user.value(QStringLiteral("displayName")).toString();
  info_.role = user.value(QStringLiteral("role")).toString();
  info_.mustChangePassword = user.value(QStringLiteral("mustChangePassword")).toBool();
  info_.canUseAi = policy.value(QStringLiteral("canUseAi")).toBool(true);
  info_.maxEffort = policy.value(QStringLiteral("maxEffort")).toString(QStringLiteral("max"));
  info_.defaultEffort = policy.value(QStringLiteral("defaultEffort")).toString(QStringLiteral("auto"));
  info_.model = me.value(QStringLiteral("server")).toObject().value(QStringLiteral("model")).toString();
  info_.usage = parseUsage(me.value(QStringLiteral("usage")).toObject());
  loaded_ = true;
}

void Account::signedIn(const QJsonObject& json) {
  token_ = json.value(QStringLiteral("token")).toString();
  tokenServer_ = server_;
  apply(json);
  freshLogin_ = true;
  save();
  emit changed();
}

void Account::save() {
  const QByteArray plain = QJsonDocument(QJsonObject{{QStringLiteral("server"), tokenServer_}, {QStringLiteral("token"), token_}}).toJson(QJsonDocument::Compact);
  const QByteArray data = protect(plain);
  if (data.isEmpty()) return;  // 暗号化できなければ保存しない（次に起動したとき、ログインし直す）
  QDir().mkpath(QFileInfo(sessionPath()).absolutePath());
  QSaveFile f(sessionPath());
  if (f.open(QIODevice::WriteOnly)) {
    f.write(data);
    f.commit();
  }
}

void Account::forget() {
  token_.clear();
  tokenServer_.clear();
  info_ = AccountInfo();
  loaded_ = false;
  freshLogin_ = false;
  QFile::remove(sessionPath());
}

bool Account::loadSaved() {
  if (!token_.isEmpty()) return true;
  QFile f(sessionPath());
  if (server_.isEmpty() || !f.open(QIODevice::ReadOnly)) return false;
  const QJsonObject saved = QJsonDocument::fromJson(unprotect(f.readAll())).object();
  f.close();
  const QString savedServerName = normalizeServer(saved.value(QStringLiteral("server")).toString());
  // 違うサーバーの印は使わない（消さずに残しておき、.env をそのサーバーに戻したら使う）
  if (savedServerName.isEmpty() || savedServerName != server_) return false;
  token_ = saved.value(QStringLiteral("token")).toString();
  tokenServer_ = savedServerName;
  return !token_.isEmpty();
}

void Account::refresh(QObject* ctx, std::function<void(Refresh)> callback) {
  const bool notify = !callback;
  auto done = guarded(ctx, callback);
  if (token_.isEmpty()) {
    QTimer::singleShot(0, this, [done] { done(Refresh::Expired); });
    return;
  }
  request(QStringLiteral("GET"), QStringLiteral("/api/me"), {}, true, [this, done, notify](int status, const QJsonObject& json) {
    Refresh result = Refresh::Unreachable;
    if (status == 200) {
      apply(json);
      result = Refresh::LoggedIn;
    } else if (status == 401) {
      forget();
      result = Refresh::Expired;
    } else if (notify) {
      // つながらないときは印を残す（つながったら使える）
      emit failed(errorText(status, json));
    }
    emit changed();
    done(result);
  });
}

void Account::login(const QString& email, const QString& password, QObject* ctx, Done callback) {
  if (busy()) return;
  auto done = guarded(ctx, callback);
  const QString target = server_;
  request(QStringLiteral("POST"), QStringLiteral("/api/auth/login"),
          {{QStringLiteral("email"), email.trimmed()}, {QStringLiteral("password"), password}, {QStringLiteral("client"), QStringLiteral("app")}}, false,
          [this, target, done](int status, const QJsonObject& json) {
            if (target != server_) return done(QStringLiteral("途中でサーバーを変えたので、もう一度ログインしてください"));
            if (status != 200 || json.value(QStringLiteral("token")).toString().isEmpty()) return done(errorText(status, json));
            signedIn(json);
            done(QString());
          });
}

void Account::registerAccount(const QString& email, const QString& password, const QString& displayName, QObject* ctx,
                              std::function<void(const QString&, const QString&)> callback) {
  if (busy()) return;
  auto done = guarded(ctx, callback);
  const QString target = server_;
  request(QStringLiteral("POST"), QStringLiteral("/api/auth/register"),
          {{QStringLiteral("email"), email.trimmed()},
           {QStringLiteral("password"), password},
           {QStringLiteral("displayName"), displayName.trimmed()},
           {QStringLiteral("client"), QStringLiteral("app")}},
          false, [this, target, done](int status, const QJsonObject& json) {
            if (target != server_) return done(QStringLiteral("途中でサーバーを変えたので、もう一度登録してください"), QString());
            // 新規登録に対応していない（古い）サーバー
            if (status == 404) return done(QStringLiteral("この AI サーバーは新規登録に対応していません。サーバーを新しくするか、管理者にアカウントを作ってもらってください"), QString());
            if (status != 200 && status != 201) return done(errorText(status, json), QString());
            if (json.value(QStringLiteral("pending")).toBool()) {
              return done(QString(), json.value(QStringLiteral("message")).toString(QStringLiteral("登録しました。管理者が承認すると、ログインできます")));
            }
            if (json.value(QStringLiteral("token")).toString().isEmpty()) return done(QStringLiteral("登録の返事を読めませんでした。ログインしてみてください"), QString());
            signedIn(json);
            done(QString(), QString());
          });
}

void Account::logout() {
  if (!token_.isEmpty() && tokenServer_ == server_) {
    // サーバー側の印も消す（届かなくても、手元の印は消す）
    request(QStringLiteral("POST"), QStringLiteral("/api/auth/logout"), {}, true, [](int, const QJsonObject&) {});
  }
  forget();
  emit changed();
}

void Account::changePassword(const QString& current, const QString& next, QObject* ctx, Done callback) {
  if (busy()) return;
  auto done = guarded(ctx, callback);
  request(QStringLiteral("POST"), QStringLiteral("/api/me/password"), {{QStringLiteral("current"), current}, {QStringLiteral("next"), next}}, true,
          [this, done](int status, const QJsonObject& json) {
            if (status != 200) {
              if (status == 401) {
                forget();
                emit changed();
              }
              return done(errorText(status, json));
            }
            apply(json);
            emit changed();
            done(QString());
          });
}

void Account::updateUsage(const QJsonObject& usage) {
  if (usage.isEmpty()) return;
  info_.usage = parseUsage(usage);
  emit changed();
}

void Account::expire(const QString& message) {
  forget();
  emit failed(message);
  emit changed();
}

}  // namespace kv::ai
