#include "ai/workshop.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

#include "ai/account.h"
#include "ai/aiconfig.h"
#include "ai/servercall.h"
#include "core/texttemplates.h"

namespace kv::ai {

bool workshopVisible() {
  const Account& a = account();
  return !a.server().isEmpty() && a.workshop() != u"off";
}

QString workshopProblem() {
  const Account& a = account();
  if (a.server().isEmpty()) return QStringLiteral("AI サーバーのアドレスが決まっていないので、ワークショップは使えません");
  if (a.workshop() == u"off") return QStringLiteral("このサーバーでは、ワークショップを使えません");
  switch (a.link()) {
    case Account::Link::Offline:
    case Account::Link::Wrong:
    case Account::Link::NoDatabase:
      return a.linkDetail().isEmpty() ? QStringLiteral("AI サーバーにつながりません") : a.linkDetail();
    case Account::Link::Checking:
      return QStringLiteral("AI サーバーにつながるか確かめています。少し待ってください");
    default:
      break;
  }
  if (a.workshop().isEmpty()) return QStringLiteral("AI サーバーが古いので、ワークショップを使えません。サーバーを管理している人に、新しくしてもらってください");
  if (!a.loggedIn()) return QStringLiteral("ワークショップを使うには、ログインしてください");
  if (a.info().mustChangePassword) return QStringLiteral("ワークショップを使う前に、パスワードを変えてください");
  return {};
}

// ---- オーディオのファイル -----------------------------------------------------------------

const QStringList& workshopAudioExtensions() {
  // サーバーが受け付ける形式（中身の先頭で見分ける）と同じ
  static const QStringList exts = {QStringLiteral("mp3"), QStringLiteral("wav"), QStringLiteral("m4a"),  QStringLiteral("aac"),
                                   QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("flac"), QStringLiteral("wma")};
  return exts;
}

QString workshopAudioDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("workshop/audio"));
}

/** ID はサーバーの数字だけ（フォルダーの名前に使うので、ほかの文字は受け付けない） */
static bool safeId(const QString& id) {
  static const QRegularExpression re(QStringLiteral("^[0-9]{1,18}$"));
  return re.match(id).hasMatch();
}

QString workshopAudioFile(const QString& id) {
  if (!safeId(id)) return {};
  const QDir dir(QDir(workshopAudioDir()).filePath(id));
  const QFileInfoList files = dir.entryInfoList(QDir::Files, QDir::Name);
  for (const QFileInfo& f : files) {
    if (workshopAudioExtensions().contains(f.suffix().toLower()) && f.size() > 0) return f.absoluteFilePath();
  }
  return {};
}

bool isWorkshopAudio(const QString& path) {
  const QString root = QDir::cleanPath(QFileInfo(workshopAudioDir()).absoluteFilePath()) + QLatin1Char('/');
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).startsWith(root, Qt::CaseInsensitive);
}

// ---- 写真のファイル -----------------------------------------------------------------------

/** 写真の形式（サーバーが受け付けるもの） */
static const QStringList& workshopImageTypes() {
  static const QStringList types = {QStringLiteral("jpg"), QStringLiteral("png")};
  return types;
}

QSize workshopImageSize(const QSize& size) {
  if (size.isEmpty()) return size;
  const QSize bound = size.width() >= size.height() ? QSize(1920, 1080) : QSize(1080, 1920);
  if (size.width() <= bound.width() && size.height() <= bound.height()) return size;
  return size.scaled(bound, Qt::KeepAspectRatio);
}

QString workshopImageDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("workshop/image"));
}

QString workshopImageFile(const QString& id) {
  static const QRegularExpression re(QStringLiteral("^[0-9]{1,18}$"));
  if (!re.match(id).hasMatch()) return {};
  const QDir dir(QDir(workshopImageDir()).filePath(id));
  for (const QFileInfo& f : dir.entryInfoList(QDir::Files, QDir::Name)) {
    if (workshopImageTypes().contains(f.suffix().toLower()) && f.size() > 0) return f.absoluteFilePath();
  }
  return {};
}

bool isWorkshopImage(const QString& path) {
  const QString root = QDir::cleanPath(QFileInfo(workshopImageDir()).absoluteFilePath()) + QLatin1Char('/');
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).startsWith(root, Qt::CaseInsensitive);
}

/** ファイルの名前に使える形にする（名前は、ほかの人が付けたものなので、フォルダーの外を指したり、使えない文字が入ったりしないように） */
static QString safeFileName(const QString& name, const QString& fallback = QStringLiteral("オーディオ")) {
  QString s = name;
  static const QRegularExpression bad(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
  s.replace(bad, QStringLiteral("_"));
  s = s.simplified().left(60);
  while (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' '))) s.chop(1);
  while (s.startsWith(QLatin1Char('.'))) s.remove(0, 1);
  if (s.isEmpty()) s = fallback;
  // Windows で使えない名前（CON・NUL・COM1 など）
  static const QRegularExpression reserved(QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])$"), QRegularExpression::CaseInsensitiveOption);
  if (reserved.match(s).hasMatch()) s += QLatin1Char('_');
  return s;
}

// ---- 通信 ---------------------------------------------------------------------------------

Workshop& Workshop::instance() {
  static Workshop* w = new Workshop();
  return *w;
}

WorkshopTemplate parseWorkshopTemplate(const QJsonObject& o) {
  WorkshopTemplate t;
  const QJsonValue id = o.value(QStringLiteral("id"));
  t.id = id.isString() ? id.toString() : QString::number(id.toInteger());
  const QString kind = o.value(QStringLiteral("kind")).toString();
  t.kind = kind == u"audio" ? WorkshopKind::Audio : kind == u"image" ? WorkshopKind::Image : WorkshopKind::Text;
  t.name = TextTemplates::cleanName(o.value(QStringLiteral("name")).toString());
  t.description = o.value(QStringLiteral("description")).toString().simplified().left(200);
  t.author = o.value(QStringLiteral("author")).toString().simplified().left(40);
  if (t.author.isEmpty()) t.author = QStringLiteral("名前なし");
  t.status = o.value(QStringLiteral("status")).toString(QStringLiteral("published"));
  t.uses = std::max(0, o.value(QStringLiteral("uses")).toInt());
  t.mine = o.value(QStringLiteral("mine")).toBool();
  t.used = o.value(QStringLiteral("used")).toBool();
  t.createdAt = QDateTime::fromString(o.value(QStringLiteral("createdAt")).toString(), Qt::ISODateWithMs);
  if (t.kind == WorkshopKind::Text) {
    t.style = TextTemplates::styleFromJson(o.value(QStringLiteral("style")).toObject());
  } else if (t.kind == WorkshopKind::Image) {
    const QJsonObject f = o.value(QStringLiteral("file")).toObject();
    const QString type = f.value(QStringLiteral("type")).toString().toLower();
    t.fileType = workshopImageTypes().contains(type) ? type : QString();
    t.fileSize = std::max<qint64>(0, f.value(QStringLiteral("size")).toInteger());
    t.width = std::clamp(f.value(QStringLiteral("width")).toInt(), 0, 10000);
    t.height = std::clamp(f.value(QStringLiteral("height")).toInt(), 0, 10000);
    // 見本はほかの人が作ったものなので、大きすぎるものは使わない（サーバーでも 200KB までにしている）
    const QByteArray thumb = QByteArray::fromBase64(o.value(QStringLiteral("thumb")).toString().toLatin1());
    if (thumb.size() <= 300 * 1024) t.thumb = thumb;
  } else {
    const QJsonObject f = o.value(QStringLiteral("file")).toObject();
    const QString type = f.value(QStringLiteral("type")).toString().toLower();
    t.fileType = workshopAudioExtensions().contains(type) ? type : QString();
    t.fileSize = std::max<qint64>(0, f.value(QStringLiteral("size")).toInteger());
    t.duration = std::clamp(f.value(QStringLiteral("duration")).toDouble(), 0.0, kWorkshopAudioMaxSeconds);
  }
  return t;
}

/** サーバーの失敗の文（サーバーが日本語の説明を返していればそれを使う） */
static QString errorText(int status, const QJsonObject& json) {
  const QString message = json.value(QStringLiteral("error")).toString();
  if (!message.isEmpty()) return message;
  if (status == 404) return QStringLiteral("AI サーバーが古いので、ワークショップを使えません。サーバーを管理している人に、新しくしてもらってください");
  if (status == 413) return QStringLiteral("ファイルが大きすぎます");
  return QStringLiteral("AI サーバーでエラーが起きました（%1）").arg(status);
}

void Workshop::call(const QByteArray& method, const QString& path, const QByteArray& body, const QByteArray& contentType, int timeoutMs,
                    QObject* ctx, std::function<void(int, const QJsonObject&, const QByteArray&)> done) {
  QPointer<QObject> guard(ctx);
  const bool hasContext = ctx != nullptr;
  auto finish = [guard, hasContext, done](int status, const QJsonObject& json, const QByteArray& raw) {
    if (hasContext && !guard) return;
    if (done) done(status, json, raw);
  };
  Account& a = account();
  const QString server = a.server();
  QString problem = serverProblem(server);
  if (problem.isEmpty() && !a.hasToken()) problem = QStringLiteral("ワークショップを使うには、ログインしてください");
  if (!problem.isEmpty()) {
    QMetaObject::invokeMethod(
        this, [finish, problem] { finish(-1, QJsonObject{{QStringLiteral("error"), problem}}, QByteArray()); }, Qt::QueuedConnection);
    return;
  }
  QNetworkRequest req(QUrl(server + path));
  if (!contentType.isEmpty()) req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
  req.setRawHeader("Accept", "application/json, audio/*");
  // ログインの印は、今のサーバーで受け取ったもの（サーバーを変えると、Account が印を捨てる）
  req.setRawHeader("Authorization", "Bearer " + a.token().toUtf8());
  req.setTransferTimeout(timeoutMs);
  ServerCall* c = method == "GET" ? ServerCall::get(req, this) : ServerCall::custom(req, method, body, this);
  connect(c, &ServerCall::finished, this, [finish, server](QNetworkReply* r) {
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray raw = r->readAll();
    const bool isJson = r->header(QNetworkRequest::ContentTypeHeader).toString().contains(QStringLiteral("json"));
    QJsonObject json = isJson ? QJsonDocument::fromJson(raw).object() : QJsonObject();
    if (status == 0) {
      // Qt の説明にアドレスが入っていることがあるので伏せる（アドレスは画面に出さない）
      json.insert(QStringLiteral("error"), QStringLiteral("AI サーバーにつながりませんでした（%1）").arg(hideServer(r->errorString(), server)));
      account().checkLink();
    } else if (status == 401) {
      // ログインの期限が切れた・アカウントを止められた
      account().expire(errorText(status, json));
    }
    finish(status, json, isJson ? QByteArray() : raw);
  });
}

void Workshop::callJson(const QByteArray& method, const QString& path, const QJsonObject& body, QObject* ctx,
                        std::function<void(int, const QJsonObject&)> done) {
  const QByteArray payload = method == "GET" ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
  call(method, path, payload, method == "GET" ? QByteArray() : QByteArrayLiteral("application/json"), 30000, ctx,
       [done](int status, const QJsonObject& json, const QByteArray&) {
         if (done) done(status, json);
       });
}

static QString idPath(const QString& id, const char* tail = "") {
  return QStringLiteral("/api/workshop/templates/%1%2").arg(QString::fromLatin1(QUrl::toPercentEncoding(id)), QString::fromLatin1(tail));
}

void Workshop::list(WorkshopKind kind, const QString& query, bool popular, bool mine, int offset, QObject* ctx,
                    std::function<void(const QString&, const WorkshopPage&)> done) {
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("kind"), kind == WorkshopKind::Audio   ? QStringLiteral("audio")
                                         : kind == WorkshopKind::Image ? QStringLiteral("image")
                                                                       : QStringLiteral("text"));
  if (!query.trimmed().isEmpty()) q.addQueryItem(QStringLiteral("q"), query.simplified().left(50));
  if (popular) q.addQueryItem(QStringLiteral("sort"), QStringLiteral("popular"));
  if (mine) q.addQueryItem(QStringLiteral("mine"), QStringLiteral("1"));
  q.addQueryItem(QStringLiteral("offset"), QString::number(std::max(0, offset)));
  q.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
  const QString path = QStringLiteral("/api/workshop/templates?") + q.toString(QUrl::FullyEncoded);
  callJson("GET", path, {}, ctx, [done, kind](int status, const QJsonObject& json) {
    WorkshopPage page;
    if (status != 200) {
      done(errorText(status, json), page);
      return;
    }
    page.total = json.value(QStringLiteral("total")).toInt();
    page.offset = json.value(QStringLiteral("offset")).toInt();
    page.mode = json.value(QStringLiteral("mode")).toString();
    for (const QJsonValue& v : json.value(QStringLiteral("templates")).toArray()) {
      WorkshopTemplate t = parseWorkshopTemplate(v.toObject());
      // 古いサーバーは kind を見ずにテキストのテンプレートを返すので、頼んだ種類のものだけにする
      if (t.kind == kind) page.items.push_back(t);
    }
    done(QString(), page);
  });
}

void Workshop::publish(const QString& name, const QString& description, const TextItem& style, QObject* ctx,
                       std::function<void(const QString&, const WorkshopTemplate&, const QString&)> done) {
  const QJsonObject body{{QStringLiteral("name"), TextTemplates::cleanName(name)},
                         {QStringLiteral("description"), description.simplified().left(200)},
                         {QStringLiteral("style"), TextTemplates::styleToJson(style)}};
  callJson("POST", QStringLiteral("/api/workshop/templates"), body, ctx, [this, done](int status, const QJsonObject& json) {
    if (status != 201 && status != 200) {
      done(errorText(status, json), WorkshopTemplate(), QString());
      return;
    }
    const WorkshopTemplate t = parseWorkshopTemplate(json.value(QStringLiteral("template")).toObject());
    emit published(t);
    done(QString(), t, json.value(QStringLiteral("message")).toString(QStringLiteral("ワークショップに公開しました")));
  });
}

void Workshop::publishAudio(const QString& name, const QString& description, const QString& path, double duration, QObject* ctx,
                            std::function<void(const QString&, const WorkshopTemplate&, const QString&)> done) {
  auto fail = [this, done](const QString& message) {
    QMetaObject::invokeMethod(this, [done, message] { done(message, WorkshopTemplate(), QString()); }, Qt::QueuedConnection);
  };
  const QFileInfo info(path);
  if (!workshopAudioExtensions().contains(info.suffix().toLower())) {
    return fail(QStringLiteral("この形式のファイルは公開できません（MP3・WAV・M4A・AAC・OGG・Opus・FLAC・WMA を公開できます）"));
  }
  if (info.size() > kWorkshopAudioMaxBytes) return fail(QStringLiteral("ファイルが大きすぎます（20MB まで公開できます）"));
  if (duration <= 0 || duration > kWorkshopAudioMaxSeconds) return fail(QStringLiteral("長さが 30 分までのオーディオだけ公開できます"));
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("ファイルを開けませんでした"));
  const QByteArray bytes = f.readAll();
  if (bytes.isEmpty() || bytes.size() > kWorkshopAudioMaxBytes) return fail(QStringLiteral("ファイルを読めませんでした"));
  QUrlQuery q;
  q.addQueryItem(QStringLiteral("name"), TextTemplates::cleanName(name));
  q.addQueryItem(QStringLiteral("description"), description.simplified().left(200));
  q.addQueryItem(QStringLiteral("fileName"), QStringLiteral("audio.") + info.suffix().toLower());
  q.addQueryItem(QStringLiteral("duration"), QString::number(duration, 'f', 3));
  // 大きなファイルは送るのに時間がかかるので、長めに待つ
  call("POST", QStringLiteral("/api/workshop/audio?") + q.toString(QUrl::FullyEncoded), bytes, QByteArrayLiteral("application/octet-stream"), 180000,
       ctx, [this, done](int status, const QJsonObject& json, const QByteArray&) {
         if (status != 201 && status != 200) {
           // オーディオを受け付ける前の版のサーバーには、この API が無い（新しいサーバーは 404 を返さない）
           done(status == 404 ? QStringLiteral("この AI サーバーは古いので、オーディオを公開できません。サーバーを管理している人に、新しくしてもらってください")
                              : errorText(status, json),
                WorkshopTemplate(), QString());
           return;
         }
         const WorkshopTemplate t = parseWorkshopTemplate(json.value(QStringLiteral("template")).toObject());
         emit published(t);
         done(QString(), t, json.value(QStringLiteral("message")).toString(QStringLiteral("ワークショップに公開しました")));
       });
}

void Workshop::publishImage(const QString& name, const QString& description, const QImage& image, QObject* ctx,
                            std::function<void(const QString&, const WorkshopTemplate&, const QString&)> done) {
  auto fail = [this, done](const QString& message) {
    QMetaObject::invokeMethod(this, [done, message] { done(message, WorkshopTemplate(), QString()); }, Qt::QueuedConnection);
  };
  if (image.isNull()) return fail(QStringLiteral("写真を読み込めませんでした"));
  // フル HD に収まるように縮めて、JPEG にする（作り直すので、撮った場所などの写真の情報は入らない）
  QImage img = image.convertToFormat(QImage::Format_RGB32);
  const QSize target = workshopImageSize(img.size());
  if (target != img.size()) img = img.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  auto jpeg = [](const QImage& source, int quality) {
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    source.save(&buf, "JPG", quality);
    return bytes;
  };
  QByteArray bytes = jpeg(img, 90);
  if (bytes.size() > kWorkshopImageMaxBytes) bytes = jpeg(img, 75);
  if (bytes.isEmpty() || bytes.size() > kWorkshopImageMaxBytes) return fail(QStringLiteral("写真が大きすぎて公開できません"));
  // 一覧の見本（長い辺 480px まで）
  const QImage small = img.scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation);
  QByteArray thumb = jpeg(small, 80);
  if (thumb.size() > 200 * 1024) thumb = jpeg(small, 60);
  const QJsonObject body{{QStringLiteral("name"), TextTemplates::cleanName(name)},
                         {QStringLiteral("description"), description.simplified().left(200)},
                         {QStringLiteral("image"), QString::fromLatin1(bytes.toBase64())},
                         {QStringLiteral("thumb"), QString::fromLatin1(thumb.toBase64())}};
  call("POST", QStringLiteral("/api/workshop/image"), QJsonDocument(body).toJson(QJsonDocument::Compact), QByteArrayLiteral("application/json"), 180000, ctx,
       [this, done](int status, const QJsonObject& json, const QByteArray&) {
         if (status != 201 && status != 200) {
           // 写真を受け付ける前の版のサーバーには、この API が無い（新しいサーバーは 404 を返さない）
           done(status == 404 ? QStringLiteral("この AI サーバーは古いので、写真を公開できません。サーバーを管理している人に、新しくしてもらってください")
                              : errorText(status, json),
                WorkshopTemplate(), QString());
           return;
         }
         const WorkshopTemplate t = parseWorkshopTemplate(json.value(QStringLiteral("template")).toObject());
         emit published(t);
         done(QString(), t, json.value(QStringLiteral("message")).toString(QStringLiteral("ワークショップに公開しました")));
       });
}

void Workshop::fetchFile(const WorkshopTemplate& t, QObject* ctx, std::function<void(const QString&, const QString&)> done) {
  const bool image = t.kind == WorkshopKind::Image;
  if (const QString existing = image ? workshopImageFile(t.id) : workshopAudioFile(t.id); !existing.isEmpty()) {
    QMetaObject::invokeMethod(this, [done, existing] { done(QString(), existing); }, Qt::QueuedConnection);
    return;
  }
  if (!safeId(t.id) || t.fileType.isEmpty() || t.kind == WorkshopKind::Text) {
    QMetaObject::invokeMethod(this, [done] { done(QStringLiteral("このファイルは読み込めません"), QString()); }, Qt::QueuedConnection);
    return;
  }
  const QString id = t.id;
  // 置く場所は、ワークショップのフォルダーの ID ごとのフォルダー（名前はほかの人が付けたものなので、使える文字だけにする）
  const QString dir = QDir(image ? workshopImageDir() : workshopAudioDir()).filePath(id);
  const QString file = QDir(dir).filePath(safeFileName(t.name, image ? QStringLiteral("写真") : QStringLiteral("オーディオ")) + QLatin1Char('.') + t.fileType);
  const qint64 maxBytes = image ? kWorkshopImageMaxBytes : kWorkshopAudioMaxBytes;
  call("GET", idPath(id, "/file"), {}, {}, 180000, ctx, [done, file, maxBytes](int status, const QJsonObject& json, const QByteArray& raw) {
    if (status != 200) {
      done(errorText(status, json), QString());
      return;
    }
    if (raw.isEmpty() || raw.size() > maxBytes) {
      done(QStringLiteral("ファイルを読み込めませんでした"), QString());
      return;
    }
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) || out.write(raw) != raw.size() || !out.commit()) {
      done(QStringLiteral("ファイルを保存できませんでした（%1）").arg(QDir::toNativeSeparators(QFileInfo(file).absolutePath())), QString());
      return;
    }
    done(QString(), QFileInfo(file).absoluteFilePath());
  });
}

void Workshop::remove(const QString& id, QObject* ctx, Done done) {
  callJson("DELETE", idPath(id), {}, ctx, [this, id, done](int status, const QJsonObject& json) {
    if (status != 200) {
      done(errorText(status, json));
      return;
    }
    emit removed(id);
    done(QString());
  });
}

void Workshop::markUsed(const QString& id) {
  callJson("POST", idPath(id, "/use"), {}, nullptr, {});
}

void Workshop::report(const QString& id, const QString& reason, QObject* ctx, Done done) {
  callJson("POST", idPath(id, "/report"), QJsonObject{{QStringLiteral("reason"), reason.simplified().left(200)}}, ctx,
           [done](int status, const QJsonObject& json) { done(status == 200 ? QString() : errorText(status, json)); });
}

}  // namespace kv::ai
