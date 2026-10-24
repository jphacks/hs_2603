#include "ai/bedrock.h"

#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include "ai/servercall.h"

namespace kv::ai {

BedrockClient::BedrockClient(QObject* parent) : QObject(parent) {}

BedrockClient::~BedrockClient() {
  cancel();
}

void BedrockClient::converse(const Config& config, const QJsonObject& body) {
  cancel();
  if (!config.hasServer()) {
    QMetaObject::invokeMethod(
        this, [this] { emit failed(QStringLiteral("AI サーバーのアドレスが決まっていません。.env に KANTAN_AI_SERVER=（AI サーバーのアドレス）を書いてください。")); },
        Qt::QueuedConnection);
    return;
  }
  // サーバーがモデル・上限・深さを決めて Bedrock に送る。アプリは、選んだ深さと送る内容だけを渡す
  QNetworkRequest req(QUrl(config.server + QStringLiteral("/api/ai/converse")));
  req.setRawHeader("Authorization", "Bearer " + config.sessionToken.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
  req.setRawHeader("Accept", "application/json");
  // サーバーが Bedrock を待つ時間に、サーバーを通る分を足して待つ（1 回あたり）
  req.setTransferTimeout(config.timeoutMs() + 30000);
  const QJsonObject wrapped{{QStringLiteral("request"), body},
                            {QStringLiteral("effort"), config.effort},
                            {QStringLiteral("effortChoice"), config.effortChoice}};
  // サーバーに届く前に切られたときだけ、自動でつなぎ直して送り直す（届いたかもしれない依頼は、二重にしない）
  ServerCall* call = ServerCall::post(req, QJsonDocument(wrapped).toJson(QJsonDocument::Compact), this);
  call_ = call;
  const QString server = config.server;
  connect(call, &ServerCall::finished, this, [this, call, server](QNetworkReply* r) {
    if (call_.data() != call) return;  // 新しい呼び出しに置き換えた
    call_.clear();
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject obj = QJsonDocument::fromJson(r->readAll()).object();
    if (status == 0) {
      if (r->error() == QNetworkReply::OperationCanceledError || r->error() == QNetworkReply::TimeoutError) {
        emit failed(QStringLiteral("AI から返事がありませんでした。もう一度送ってください。"));
      } else if (call->gaveUp()) {
        emit failed(QStringLiteral("AI サーバーにつないでも、すぐに切られてしまいました（%1 回つなぎ直しました）。少し待ってから、もう一度送ってください。")
                        .arg(call->retries()));
      } else {
        // Qt の説明にアドレスが入っていることがあるので伏せる（アドレスは画面に出さない）
        emit failed(QStringLiteral("AI サーバーにつながりませんでした。サーバーが起動しているか確かめてください。\n（%1）").arg(hideServer(r->errorString(), server)));
      }
      emit unreachable();
      return;
    }
    if (status != 200) {
      const QString code = obj.value(QStringLiteral("code")).toString();
      const QString message = obj.value(QStringLiteral("error")).toString(QStringLiteral("AI サーバーでエラーが起きました（%1）。").arg(status));
      if (status == 401) emit sessionExpired(message);
      if (code == u"quota" || code == u"ai_not_allowed" || code == u"must_change_password") emit accountChanged();
      emit failed(message);
      return;
    }
    const QJsonObject response = obj.value(QStringLiteral("response")).toObject();
    if (!response.contains(QStringLiteral("output"))) {
      emit failed(QStringLiteral("AI の返事を読めませんでした。もう一度送ってください。"));
      return;
    }
    emit serverMeta(obj.value(QStringLiteral("effort")).toString(), obj.value(QStringLiteral("usage")).toObject());
    emit finished(response);
  });
}

void BedrockClient::cancel() {
  // 送っている途中も、つなぎ直すのを待っている途中もやめる（取り消した呼び出しの結果は出さない）
  if (ServerCall* call = call_.data()) {
    call_.clear();
    call->abort();
  }
}

}  // namespace kv::ai
