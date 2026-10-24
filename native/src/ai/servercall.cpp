#include "ai/servercall.h"

#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QTimer>
#include <array>

namespace kv::ai {

// つなぎ直したことの記録（ふだんは出さない。QT_LOGGING_RULES="kv.ai.server.info=true" で出る。アドレスは書かない）
Q_LOGGING_CATEGORY(lcServer, "kv.ai.server", QtWarningMsg)

namespace {

/**
 * つなぎ直すまで待つ時間（ミリ秒）。この回数だけつなぎ直す。
 * すぐに切られる失敗は 0.3 秒ほどで分かるので、全部つなぎ直しても 10 秒ほどで終わる
 */
constexpr std::array<int, 7> kRetryDelaysMs = {150, 300, 500, 800, 1000, 1500, 2000};

}  // namespace

/** 接続を切られる失敗か（サーバーが起動していない・名前が無い・時間切れは入れない。つなぎ直しても同じか、依頼が届いているかもしれない） */
static bool droppedConnection(QNetworkReply::NetworkError error) {
  switch (error) {
    case QNetworkReply::RemoteHostClosedError:  // TLS のあいさつの途中で切られた（Cloudflare の前で見られたもの）
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:  // 送る途中で切られた（Qt が分けていない通信の失敗）
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::SslHandshakeFailedError:  // あいさつの途中で切られた（証明書の問題のときは retryable で除く）
      return true;
    default:
      return false;
  }
}

QNetworkAccessManager* serverNetwork() {
  // アプリが終わるまで使う（Account と同じく、終わるときにも消さない）
  static QNetworkAccessManager* const nam = [] {
    // 会社などでプロキシを使っている PC でもつながるよう、Windows のプロキシ設定に従う
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    return new QNetworkAccessManager();
  }();
  return nam;
}

ServerCall* ServerCall::get(const QNetworkRequest& request, QObject* parent) {
  auto* call = new ServerCall(request, QByteArrayLiteral("GET"), {}, parent);
  call->send();
  return call;
}

ServerCall* ServerCall::post(const QNetworkRequest& request, const QByteArray& body, QObject* parent) {
  auto* call = new ServerCall(request, QByteArrayLiteral("POST"), body, parent);
  call->send();
  return call;
}

ServerCall* ServerCall::custom(const QNetworkRequest& request, const QByteArray& method, const QByteArray& body, QObject* parent) {
  auto* call = new ServerCall(request, method.toUpper(), body, parent);
  call->send();
  return call;
}

ServerCall::ServerCall(const QNetworkRequest& request, const QByteArray& method, const QByteArray& body, QObject* parent)
    : QObject(parent), request_(request), method_(method), body_(body), wait_(new QTimer(this)) {
  wait_->setSingleShot(true);
  connect(wait_, &QTimer::timeout, this, &ServerCall::send);
}

ServerCall::~ServerCall() {
  // 送っている途中で消えた（親が消えた）: 通信をやめる（reply は子なので、このあと一緒に消える）
  if (QNetworkReply* r = reply_.data()) {
    r->disconnect(this);
    r->abort();
  }
}

void ServerCall::send() {
  sent_ = false;
  certificateProblem_ = false;
  QNetworkAccessManager* nam = serverNetwork();
  QNetworkReply* r = method_ == "GET"    ? nam->get(request_)
                     : method_ == "POST" ? nam->post(request_, body_)
                                         : nam->sendCustomRequest(request_, method_, body_);
  // 呼び出しが消えたら、通信もやめる
  r->setParent(this);
  reply_ = r;
  // 依頼がサーバーに届いたかもしれない印（届いていたかもしれない POST は、送り直さない）
  connect(r, &QNetworkReply::requestSent, this, [this, r] {
    if (reply_ == r) sent_ = true;
  });
  connect(r, &QNetworkReply::uploadProgress, this, [this, r](qint64 bytesSent, qint64) {
    if (reply_ == r && bytesSent > 0) sent_ = true;
  });
  connect(r, &QNetworkReply::sslErrors, this, [this, r] {
    if (reply_ == r) certificateProblem_ = true;
  });
  connect(r, &QNetworkReply::finished, this, [this, r] {
    if (reply_ == r) onFinished(r);
  });
}

bool ServerCall::retryable(QNetworkReply* r) const {
  // サーバーが答えた（エラーの答えでも）なら、そのまま返す
  if (r->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) return false;
  if (!droppedConnection(r->error())) return false;
  // 証明書が正しくないときは、何度つないでも同じ
  if (r->error() == QNetworkReply::SslHandshakeFailedError && certificateProblem_) return false;
  // POST は、依頼が届いたかもしれないなら送り直さない（ログインや AI への依頼が、二重に届かないように）
  return method_ == "GET" || !sent_;
}

void ServerCall::onFinished(QNetworkReply* r) {
  const bool again = retryable(r);
  // 決めた回数つなぎ直しても切られた
  gaveUp_ = again && retries_ >= int(kRetryDelaysMs.size());
  if (again && !gaveUp_) {
    const int delay = kRetryDelaysMs[size_t(retries_)];
    ++retries_;
    qCInfo(lcServer, "connection dropped (error %d, request %s); reconnecting (%d) in %d ms", int(r->error()),
           sent_ ? "sent" : "not sent", retries_, delay);
    reply_.clear();
    r->deleteLater();
    wait_->start(delay);
    return;
  }
  if (retries_ > 0) {
    qCInfo(lcServer, "done after %d reconnect(s): status %d, error %d", retries_,
           r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), int(r->error()));
  }
  reply_.clear();
  emit finished(r);
  deleteLater();
}

void ServerCall::abort() {
  wait_->stop();
  if (QNetworkReply* r = reply_.data()) {
    reply_.clear();
    r->disconnect(this);
    r->abort();
  }
  deleteLater();
}

}  // namespace kv::ai
