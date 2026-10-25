#include "ai/imagegen.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include "ai/account.h"
#include "ai/aiconfig.h"
#include "ai/servercall.h"
#include "core/model.h"

namespace kv::ai {

QString generatedImageDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("generated"));
}

bool isGeneratedImage(const QString& path) {
  if (path.isEmpty()) return false;
  const QString root = QDir::cleanPath(QFileInfo(generatedImageDir()).absoluteFilePath()) + QLatin1Char('/');
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).startsWith(root, Qt::CaseInsensitive);
}

void generateImage(const QString& prompt, const QString& aspect, QObject* ctx, std::function<void(const QString& error, const QString& file)> done) {
  QPointer<QObject> guard(ctx);
  const bool hasContext = ctx != nullptr;
  auto finish = [guard, hasContext, done](const QString& error, const QString& file) {
    if (hasContext && !guard) return;
    if (done) done(error, file);
  };
  Account& a = account();
  const QString server = a.server();
  QString problem = serverProblem(server);
  if (problem.isEmpty() && !a.hasToken()) problem = QStringLiteral("画像を作るには、ログインしてください");
  if (!problem.isEmpty()) {
    QMetaObject::invokeMethod(QCoreApplication::instance(), [finish, problem] { finish(problem, QString()); }, Qt::QueuedConnection);
    return;
  }
  QNetworkRequest req(QUrl(server + QStringLiteral("/api/ai/image")));
  req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
  req.setRawHeader("Accept", "application/json");
  // ログインの印は、今のサーバーで受け取ったもの（サーバーを変えると、Account が印を捨てる）
  req.setRawHeader("Authorization", "Bearer " + a.token().toUtf8());
  // 1 枚は数秒〜十数秒。混んでいるときのために長めに待つ
  req.setTransferTimeout(150000);
  const QByteArray body =
      QJsonDocument(QJsonObject{{QStringLiteral("prompt"), prompt}, {QStringLiteral("aspect"), aspect}}).toJson(QJsonDocument::Compact);
  ServerCall* c = ServerCall::post(req, body, ctx ? ctx : QCoreApplication::instance());
  QObject::connect(c, &ServerCall::finished, c, [finish, server](QNetworkReply* r) {
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject json = QJsonDocument::fromJson(r->readAll()).object();
    if (status == 0) {
      // Qt の説明にアドレスが入っていることがあるので伏せる（アドレスは画面に出さない）
      account().checkLink();
      finish(QStringLiteral("AI サーバーにつながりませんでした（%1）").arg(hideServer(r->errorString(), server)), QString());
      return;
    }
    const QString message = json.value(QStringLiteral("error")).toString();
    if (status == 401) account().expire(message.isEmpty() ? QStringLiteral("ログインの期限が切れました。もう一度ログインしてください") : message);
    if (status == 404 && json.value(QStringLiteral("code")).toString() == u"not_found") {
      // 画像を作れるようになる前の版のサーバー（この API が無い）
      finish(QStringLiteral("この AI サーバーは古いので、画像を作れません。サーバーを管理している人に、新しくしてもらってください"), QString());
      return;
    }
    if (status != 200) {
      finish(message.isEmpty() ? QStringLiteral("画像を作れませんでした（エラー %1）").arg(status) : message, QString());
      return;
    }
    account().updateUsage(json.value(QStringLiteral("usage")).toObject());
    const QImage image = QImage::fromData(QByteArray::fromBase64(json.value(QStringLiteral("image")).toString().toLatin1()));
    if (image.isNull()) {
      finish(QStringLiteral("作った画像を読めませんでした"), QString());
      return;
    }
    // JPEG にして置く（PNG のままだと 1 枚 2〜3MB になる。透明な所は無い）
    const QString dir = generatedImageDir();
    QDir().mkpath(dir);
    const QString file = QDir(dir).filePath(newId() + QStringLiteral(".jpg"));
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || !image.convertToFormat(QImage::Format_RGB32).save(&out, "JPG", 92) || !out.commit()) {
      finish(QStringLiteral("作った画像を保存できませんでした（%1）").arg(QDir::toNativeSeparators(dir)), QString());
      return;
    }
    finish(QString(), QFileInfo(file).absoluteFilePath());
  });
}

}  // namespace kv::ai
