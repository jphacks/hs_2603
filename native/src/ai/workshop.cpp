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
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <memory>

#include "ai/account.h"
#include "ai/aiconfig.h"
#include "ai/servercall.h"
#include "core/texttemplates.h"
#include "export/exporter.h"
#include "media/ffutil.h"

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

// ---- 動画のファイル -----------------------------------------------------------------------

QString workshopVideoDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("workshop/video"));
}

QString workshopVideoFile(const QString& id) {
  static const QRegularExpression re(QStringLiteral("^[0-9]{1,18}$"));
  if (!re.match(id).hasMatch()) return {};
  const QDir dir(QDir(workshopVideoDir()).filePath(id));
  for (const QFileInfo& f : dir.entryInfoList(QDir::Files, QDir::Name)) {
    if (f.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) == 0 && f.size() > 0) return f.absoluteFilePath();
  }
  return {};
}

bool isWorkshopVideo(const QString& path) {
  const QString root = QDir::cleanPath(QFileInfo(workshopVideoDir()).absoluteFilePath()) + QLatin1Char('/');
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
  t.kind = kind == u"audio"   ? WorkshopKind::Audio
           : kind == u"image" ? WorkshopKind::Image
           : kind == u"video" ? WorkshopKind::Video
                              : WorkshopKind::Text;
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
  } else if (t.kind == WorkshopKind::Video) {
    const QJsonObject f = o.value(QStringLiteral("file")).toObject();
    t.fileType = f.value(QStringLiteral("type")).toString().toLower() == u"mp4" ? QStringLiteral("mp4") : QString();
    t.fileSize = std::max<qint64>(0, f.value(QStringLiteral("size")).toInteger());
    t.duration = std::clamp(f.value(QStringLiteral("duration")).toDouble(), 0.0, kWorkshopVideoMaxSeconds + kWorkshopVideoSlack);
    t.width = std::clamp(f.value(QStringLiteral("width")).toInt(), 0, 10000);
    t.height = std::clamp(f.value(QStringLiteral("height")).toInt(), 0, 10000);
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
                                         : kind == WorkshopKind::Video ? QStringLiteral("video")
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

/** 一覧の見本: 動画の 1 秒あたり（短ければ真ん中）の画面を、長い辺 480px・200KB までの JPEG にする */
static QByteArray videoThumb(const QString& path, double duration) {
  VideoDecoder d;
  if (!d.open(path)) return {};
  const double at = std::min(1.0, std::max(0.0, duration / 2));
  d.seek(at);
  bool got = false;
  for (int i = 0; i < 600 && d.decodeNext(); ++i) {
    got = true;
    if (d.framePts() >= at - d.frameDuration() * 0.5) break;
  }
  if (!got) return {};
  const QImage img = d.frameImage(480, 480);
  if (img.isNull()) return {};
  for (int quality : {80, 60, 40}) {
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.convertToFormat(QImage::Format_RGB32).save(&buf, "JPG", quality);
    if (!bytes.isEmpty() && bytes.size() <= 200 * 1024) return bytes;
  }
  return {};
}

void Workshop::publishVideo(const QString& name, const QString& description, const QString& path, const QSize& size, double duration, bool withAudio,
                            QObject* ctx,
                            std::function<void(const QString&, const WorkshopTemplate&, const QString&)> done,
                            std::function<void(const QString&)> stage) {
  auto fail = [this, done](const QString& message) {
    QMetaObject::invokeMethod(this, [done, message] { done(message, WorkshopTemplate(), QString()); }, Qt::QueuedConnection);
  };
  if (!QFileInfo(path).isFile()) return fail(QStringLiteral("ファイルが見つかりません"));
  const QString ffmpeg = ffmpegPath();
  if (ffmpeg.isEmpty() || !QFileInfo(ffmpeg).isFile()) return fail(QStringLiteral("動画を作り直すための ffmpeg が見つかりません。アプリを入れ直してください"));
  auto tmp = std::make_shared<QTemporaryDir>();
  if (!tmp->isValid()) return fail(QStringLiteral("作業用のフォルダーを作れませんでした"));
  const QString out = tmp->filePath(QStringLiteral("workshop.mp4"));
  // フル HD に収まる大きさ（H.264 の決まりで、幅・高さは偶数にする）
  QSize target = workshopImageSize(size.isEmpty() ? QSize(1280, 720) : size);
  target = QSize(std::max(2, target.width() / 2 * 2), std::max(2, target.height() / 2 * 2));
  const QString wh = QStringLiteral("%1:%2").arg(target.width()).arg(target.height());
  const bool gif = QFileInfo(path).suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0;
  QStringList args = {"-hide_banner", "-nostdin", "-y", "-loglevel", "error", "-i", path, "-t", QString::number(kWorkshopVideoMaxSeconds, 'f', 3)};
  if (gif) {
    // GIF: 透明な所は黒にして、30 コマ/秒の動画にする（GIF はコマごとに出す長さが違うため）。音は無い。
    // 黒い下地を GIF の長さで終わらせる（GIF が先に終わっても最後のコマを出し続けるので、最後のコマも、出す長さどおりに残る）
    const QString end = duration > 0 ? QStringLiteral(":d=%1").arg(std::min(duration, kWorkshopVideoMaxSeconds), 0, 'f', 3) : QString();
    args << "-filter_complex"
         << QStringLiteral("color=c=black:s=%1x%2:r=30%3[bg];[0:v]scale=%4:flags=lanczos,format=rgba[fg];"
                           "[bg][fg]overlay=%5,setsar=1,format=yuv420p[v]")
                .arg(target.width())
                .arg(target.height())
                .arg(end, wh, end.isEmpty() ? QStringLiteral("shortest=1") : QStringLiteral("eof_action=repeat"))
         << "-map" << "[v]" << "-an";
  } else {
    args << "-map" << "0:v:0";
    if (withAudio) args << "-map" << "0:a:0?";
    args << "-vf" << QStringLiteral("scale=%1:flags=lanczos,setsar=1,format=yuv420p").arg(wh);
    if (withAudio) args << "-c:a" << "aac" << "-b:a" << "128k" << "-ac" << "2" << "-ar" << "48000";
    else args << "-an";
  }
  // 撮った場所などの情報・字幕・チャプターは入れない
  args << "-sn" << "-dn" << "-map_metadata" << "-1" << "-map_chapters" << "-1" << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf" << "23"
       << "-pix_fmt" << "yuv420p" << "-movflags" << "+faststart" << "-f" << "mp4" << out;
  if (stage) stage(QStringLiteral("動画を作り直しています…"));
  QPointer<QObject> guard(ctx);
  const bool hasContext = ctx != nullptr;
  auto* proc = new QProcess(this);
  proc->setProcessChannelMode(QProcess::MergedChannels);
  auto upload = [this, tmp, out, name, description, guard, hasContext, done, stage]() {
    if (hasContext && !guard) return;
    QFile f(out);
    if (!f.open(QIODevice::ReadOnly)) {
      done(QStringLiteral("作り直した動画を読めませんでした"), WorkshopTemplate(), QString());
      return;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.isEmpty()) {
      done(QStringLiteral("作り直した動画を読めませんでした"), WorkshopTemplate(), QString());
      return;
    }
    if (bytes.size() > kWorkshopVideoMaxBytes) {
      done(QStringLiteral("動画が大きすぎて公開できません（作り直しても %1MB を超えました）").arg(kWorkshopVideoMaxBytes / 1024 / 1024), WorkshopTemplate(), QString());
      return;
    }
    // 長さは、作り直した動画から読む（サーバーも中身から確かめる）
    ProbeInfo info;
    QString probeError;
    if (!probeMedia(out, info, probeError) || info.duration <= 0) {
      done(QStringLiteral("作り直した動画を読めませんでした（%1）").arg(probeError), WorkshopTemplate(), QString());
      return;
    }
    const QByteArray thumb = videoThumb(out, info.duration);
    if (thumb.isEmpty()) {
      done(QStringLiteral("一覧の見本を作れませんでした"), WorkshopTemplate(), QString());
      return;
    }
    if (stage) stage(QStringLiteral("送っています…"));
    const QJsonObject body{{QStringLiteral("name"), TextTemplates::cleanName(name)},
                           {QStringLiteral("description"), description.simplified().left(200)},
                           {QStringLiteral("video"), QString::fromLatin1(bytes.toBase64())},
                           {QStringLiteral("thumb"), QString::fromLatin1(thumb.toBase64())}};
    call("POST", QStringLiteral("/api/workshop/video"), QJsonDocument(body).toJson(QJsonDocument::Compact), QByteArrayLiteral("application/json"), 300000,
         guard.data(), [this, tmp, done](int status, const QJsonObject& json, const QByteArray&) {
           if (status != 201 && status != 200) {
             // 動画を受け付ける前の版のサーバーには、この API が無い（新しいサーバーは 404 を返さない）
             done(status == 404 ? QStringLiteral("この AI サーバーは古いので、動画を公開できません。サーバーを管理している人に、新しくしてもらってください")
                                : errorText(status, json),
                  WorkshopTemplate(), QString());
             return;
           }
           const WorkshopTemplate t = parseWorkshopTemplate(json.value(QStringLiteral("template")).toObject());
           emit published(t);
           done(QString(), t, json.value(QStringLiteral("message")).toString(QStringLiteral("ワークショップに公開しました")));
         });
  };
  connect(proc, &QProcess::finished, this, [proc, out, guard, hasContext, done, upload](int code, QProcess::ExitStatus status) {
    const QString log = QString::fromLocal8Bit(proc->readAll()).trimmed();
    proc->deleteLater();
    if (hasContext && !guard) return;
    if (status != QProcess::NormalExit || code != 0 || !QFileInfo(out).isFile()) {
      done(QStringLiteral("動画を作り直せませんでした（%1）").arg(log.isEmpty() ? QStringLiteral("ffmpeg が止まりました") : log.right(240)), WorkshopTemplate(), QString());
      return;
    }
    upload();
  });
  connect(proc, &QProcess::errorOccurred, this, [proc, guard, hasContext, done](QProcess::ProcessError error) {
    if (error != QProcess::FailedToStart) return;  // 止まったときは finished で知らせる
    proc->deleteLater();
    if (hasContext && !guard) return;
    done(QStringLiteral("動画を作り直すための ffmpeg を起動できませんでした"), WorkshopTemplate(), QString());
  });
  proc->start(ffmpeg, args);
}

void Workshop::fetchFile(const WorkshopTemplate& t, QObject* ctx, std::function<void(const QString&, const QString&)> done) {
  const bool image = t.kind == WorkshopKind::Image;
  const bool video = t.kind == WorkshopKind::Video;
  if (const QString existing = image ? workshopImageFile(t.id) : video ? workshopVideoFile(t.id) : workshopAudioFile(t.id); !existing.isEmpty()) {
    QMetaObject::invokeMethod(this, [done, existing] { done(QString(), existing); }, Qt::QueuedConnection);
    return;
  }
  if (!safeId(t.id) || t.fileType.isEmpty() || t.kind == WorkshopKind::Text) {
    QMetaObject::invokeMethod(this, [done] { done(QStringLiteral("このファイルは読み込めません"), QString()); }, Qt::QueuedConnection);
    return;
  }
  const QString id = t.id;
  // 置く場所は、ワークショップのフォルダーの ID ごとのフォルダー（名前はほかの人が付けたものなので、使える文字だけにする）
  const QString dir = QDir(image ? workshopImageDir() : video ? workshopVideoDir() : workshopAudioDir()).filePath(id);
  const QString fallback = image ? QStringLiteral("写真") : video ? QStringLiteral("動画") : QStringLiteral("オーディオ");
  const QString file = QDir(dir).filePath(safeFileName(t.name, fallback) + QLatin1Char('.') + t.fileType);
  const qint64 maxBytes = image ? kWorkshopImageMaxBytes : video ? kWorkshopVideoMaxBytes : kWorkshopAudioMaxBytes;
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
