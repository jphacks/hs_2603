#include "media/webimage.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QImage>
#include <QImageReader>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <memory>

#include "core/editor.h"
#include "media/mediamanager.h"

namespace kv {

static const QString kInternalAddress = QStringLiteral("この PC や、家・会社のネットワークの中のアドレスの画像は、安全のため読み込みません");
/** メディアの一覧に入れたときに添える、著作権の注意 */
static const QString kCopyrightNote = QStringLiteral("Web の画像は、著作権や利用規約を確かめてから使ってください");

WebImageImporter& WebImageImporter::instance() {
  static WebImageImporter* w = new WebImageImporter();
  return *w;
}

WebImageImporter::WebImageImporter() {
  // 会社などでプロキシを使っている PC でもつながるよう、Windows のプロキシ設定に従う（AI サーバーとの通信と同じ）
  QNetworkProxyFactory::setUseSystemConfiguration(true);
  nam_ = new QNetworkAccessManager(this);
}

QString WebImageImporter::storeDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("web-images"));
}

// ---- 確かめる決まり -------------------------------------------------------------------------

bool WebImageImporter::isBlockedAddress(const QHostAddress& a) {
  if (a.isNull()) return true;
  bool v4 = false;
  quint32 ip = a.toIPv4Address(&v4);  // IPv4 と、IPv6 に埋め込んだ IPv4（::ffff:a.b.c.d）
  if (!v4 && a.protocol() == QAbstractSocket::IPv6Protocol) {
    // NAT64（64:ff9b::/96）に埋め込んだ IPv4 も、中の IPv4 で確かめる
    const Q_IPV6ADDR b = a.toIPv6Address();
    static const quint8 nat64[12] = {0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0};
    if (std::equal(nat64, nat64 + 12, b.c)) {
      ip = (quint32(b.c[12]) << 24) | (quint32(b.c[13]) << 16) | (quint32(b.c[14]) << 8) | quint32(b.c[15]);
      v4 = true;
    }
  }
  if (v4) {
    const quint32 b0 = ip >> 24;
    const quint32 b1 = (ip >> 16) & 0xff;
    const quint32 b2 = (ip >> 8) & 0xff;
    return b0 == 0                                  // 0.0.0.0/8（この機械）
           || b0 == 10                              // 10.0.0.0/8（家・会社のネットワーク）
           || b0 == 127                             // 127.0.0.0/8（この PC）
           || (b0 == 169 && b1 == 254)              // 169.254.0.0/16（リンクローカル・クラウドの設定情報）
           || (b0 == 172 && (b1 & 0xf0) == 16)      // 172.16.0.0/12
           || (b0 == 192 && b1 == 168)              // 192.168.0.0/16
           || (b0 == 100 && (b1 & 0xc0) == 64)      // 100.64.0.0/10（キャリアの中・Tailscale など）
           || (b0 == 192 && b1 == 0 && b2 == 0)     // 192.0.0.0/24
           || (b0 == 192 && b1 == 0 && b2 == 2)     // 192.0.2.0/24（文書用）
           || (b0 == 198 && (b1 & 0xfe) == 18)      // 198.18.0.0/15（試験用）
           || (b0 == 198 && b1 == 51 && b2 == 100)  // 198.51.100.0/24（文書用）
           || (b0 == 203 && b1 == 0 && b2 == 113)   // 203.0.113.0/24（文書用）
           || b0 >= 224;                            // マルチキャスト・予約・ブロードキャスト
  }
  if (a.protocol() != QAbstractSocket::IPv6Protocol) return true;
  if (a.isLoopback() || a.isLinkLocal() || a.isSiteLocal() || a.isUniqueLocalUnicast() || a.isMulticast()) return true;
  return !a.isGlobal();
}

QString WebImageImporter::checkUrl(const QUrl& url) {
  if (!url.isValid() || url.isRelative()) return QStringLiteral("URL の形が正しくありません");
  const QString scheme = url.scheme().toLower();
  if (scheme != u"http" && scheme != u"https") return QStringLiteral("http・https 以外の URL（%1:）の画像は読み込めません").arg(scheme);
  if (!url.userName().isEmpty() || !url.password().isEmpty()) return QStringLiteral("ユーザー名・パスワードを含む URL の画像は、安全のため読み込みません");
  const QString host = url.host(QUrl::FullyEncoded).toLower();
  if (host.isEmpty()) return QStringLiteral("URL にサーバーの名前がありません");
  QHostAddress literal;
  if (literal.setAddress(host)) return isBlockedAddress(literal) ? kInternalAddress : QString();
  // 名前で書いたもの: この PC・家のネットワークの中でだけ使う名前（localhost・.local・点の無い名前など）は行かない
  static const QStringList internalSuffixes = {QStringLiteral(".localhost"), QStringLiteral(".local"), QStringLiteral(".internal"),
                                               QStringLiteral(".lan"), QStringLiteral(".home"), QStringLiteral(".home.arpa"), QStringLiteral(".intranet")};
  if (host == u"localhost" || !host.contains(QLatin1Char('.'))) return kInternalAddress;
  for (const QString& s : internalSuffixes) {
    if (host.endsWith(s)) return kInternalAddress;
  }
  return {};
}

QString WebImageImporter::imageFormatOf(const QByteArray& data, QString* error) {
  if (data.isEmpty()) {
    *error = QStringLiteral("データが空です");
    return {};
  }
  if (data.size() > kMaxBytes) {
    *error = QStringLiteral("画像が大きすぎます（20 MB まで）");
    return {};
  }
  QBuffer buffer;
  buffer.setData(data);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);  // 形式は、名前ではなく中身から決める
  const QByteArray format = reader.format().toLower();
  static const QSet<QByteArray> allowed = {"png", "jpeg", "jpg", "gif", "webp", "bmp", "tiff", "tif"};
  if (format.isEmpty() || !allowed.contains(format)) {
    *error = QStringLiteral("対応している画像（PNG・JPEG・GIF・WebP・BMP・TIFF）ではありません");
    return {};
  }
  const QSize size = reader.size();
  if (size.isValid() && (size.width() > kMaxSide || size.height() > kMaxSide)) {
    *error = QStringLiteral("画像の幅・高さが大きすぎます（%1px まで）").arg(kMaxSide);
    return {};
  }
  // 実際に読めるか（壊れたファイル・画像のふりをしたファイルを入れない）
  const QImage image = reader.read();
  if (image.isNull() || image.width() > kMaxSide || image.height() > kMaxSide) {
    *error = QStringLiteral("画像として読めません（壊れているか、画像ではないファイルです）");
    return {};
  }
  if (format == "jpeg") return QStringLiteral("jpg");
  if (format == "tif") return QStringLiteral("tiff");
  return QString::fromLatin1(format);
}

/** 保存するファイルの名前（Windows で使えない文字・予約された名前を避け、長すぎる名前は切る） */
static QString safeBaseName(const QString& raw) {
  QString s;
  for (const QChar c : raw) {
    if (c.unicode() < 0x20 || QStringLiteral("\\/:*?\"<>|").contains(c)) s += QLatin1Char('_');
    else s += c;
  }
  s = s.simplified().left(40);
  while (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' '))) s.chop(1);
  while (s.startsWith(QLatin1Char('.'))) s.remove(0, 1);
  static const QRegularExpression reserved(QStringLiteral("^(con|prn|aux|nul|com[0-9]|lpt[0-9])$"), QRegularExpression::CaseInsensitiveOption);
  if (reserved.match(s).hasMatch()) s.prepend(QStringLiteral("img-"));
  return s.isEmpty() ? QStringLiteral("web-image") : s;
}

QString WebImageImporter::saveImage(const QByteArray& data, const QString& ext, const QString& baseName, QString* error) {
  const QDir dir(storeDir());
  if (!QDir().mkpath(dir.path())) {
    *error = QStringLiteral("保存先のフォルダーを作れません");
    return {};
  }
  const QString hash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().left(12));
  // 同じ中身の画像を前に保存していたら、そのファイルを使う（同じ画像を二重に登録しない）
  const QStringList same = dir.entryList({QStringLiteral("*-%1.%2").arg(hash, ext)}, QDir::Files);
  if (!same.isEmpty()) return dir.filePath(same.first());
  const QString path = dir.filePath(QStringLiteral("%1-%2.%3").arg(safeBaseName(baseName), hash, ext));
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
    *error = QStringLiteral("画像を保存できませんでした");
    return {};
  }
  return path;
}

std::optional<QByteArray> WebImageImporter::decodeDataUrl(const QString& url, QString* error) {
  if (!url.startsWith(QStringLiteral("data:"), Qt::CaseInsensitive)) return std::nullopt;
  const qsizetype comma = url.indexOf(QLatin1Char(','));
  if (comma < 0) {
    *error = QStringLiteral("data: の URL の形が正しくありません");
    return std::nullopt;
  }
  const QString meta = url.mid(5, comma - 5).toLower();
  if (!meta.startsWith(QStringLiteral("image/"))) {
    *error = QStringLiteral("画像の data: の URL ではありません");
    return std::nullopt;
  }
  const QByteArray payload = url.mid(comma + 1).toUtf8();
  // 大きすぎるものは、中身を作る前に断る
  if (payload.size() > kMaxBytes * 3) {
    *error = QStringLiteral("画像が大きすぎます（20 MB まで）");
    return std::nullopt;
  }
  const QByteArray bytes = QByteArray::fromPercentEncoding(payload);
  if (!meta.contains(QStringLiteral(";base64"))) return bytes;
  QByteArray clean;  // 改行・空白は除く
  clean.reserve(bytes.size());
  for (const char ch : bytes) {
    if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') clean += ch;
  }
  const auto decoded = QByteArray::fromBase64Encoding(clean, QByteArray::AbortOnBase64DecodingErrors);
  if (!decoded) {
    *error = QStringLiteral("data: の URL の中身を読めません");
    return std::nullopt;
  }
  return *decoded;
}

// ---- ドロップされたもの ----------------------------------------------------------------------

namespace {

/** ドロップされたものに入っている、Web の画像の候補 */
struct WebCandidates {
  QString dataUrl;     // data:image/...
  QByteArray encoded;  // 画像のファイルの中身（image/png など）
  QImage image;        // ブラウザーが渡した画像（読み込み済みのもの）
  QUrl remote;         // 画像の URL（http / https）
  QString baseName;    // 保存するファイルの名前のもと
  bool any() const { return !dataUrl.isEmpty() || !encoded.isEmpty() || !image.isNull() || !remote.isEmpty(); }
};

bool hasLocalFiles(const QMimeData* m) {
  for (const QUrl& u : m->urls()) {
    if (u.isLocalFile()) return true;
  }
  return false;
}

/** HTML の最初の <img src="..."> */
QString imgSrcOf(const QString& html) {
  static const QRegularExpression re(QStringLiteral(R"re(<img\b[^>]*?\bsrc\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))re"),
                                     QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch m = re.match(html);
  if (!m.hasMatch()) return {};
  QString src = m.captured(1);
  if (src.isEmpty()) src = m.captured(2);
  if (src.isEmpty()) src = m.captured(3);
  src.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
  src.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
  src.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
  return src.trimmed();
}

bool looksLikeImagePath(const QUrl& u) {
  static const QStringList exts = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jfif"), QStringLiteral("gif"),
                                   QStringLiteral("webp"), QStringLiteral("bmp"), QStringLiteral("tif"), QStringLiteral("tiff")};
  return exts.contains(QFileInfo(u.path()).suffix().toLower());
}

WebCandidates collect(const QMimeData* m) {
  WebCandidates c;
  if (!m || hasLocalFiles(m)) return c;  // ローカルのファイルは、これまでどおりの取り込み
  QList<QUrl> urls = m->urls();
  // Firefox の URL（UTF-16 で「URL 改行 題名」）
  if (m->hasFormat(QStringLiteral("text/x-moz-url"))) {
    const QByteArray raw = m->data(QStringLiteral("text/x-moz-url"));
    const QString s = QString::fromUtf16(reinterpret_cast<const char16_t*>(raw.constData()), raw.size() / 2);
    const QString first = s.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (!first.isEmpty()) urls << QUrl(first);
  }
  const QString html = m->hasHtml() ? m->html() : QString();
  const QString src = imgSrcOf(html);
  if (!src.isEmpty()) urls.prepend(QUrl(src));  // <img src> は、画像そのものの URL
  const bool imageDrag = !src.isEmpty() || m->hasImage() || html.contains(QStringLiteral("<img"), Qt::CaseInsensitive);
  for (const QUrl& u : urls) {
    const QString scheme = u.scheme().toLower();
    if (scheme == u"data") {
      const QString s = u.toString();
      if (c.dataUrl.isEmpty() && s.startsWith(QStringLiteral("data:image/"), Qt::CaseInsensitive)) c.dataUrl = s;
      continue;
    }
    // 画像の URL らしいもの（画像としてドラッグしたもの・画像の拡張子の URL）だけ。ふつうのリンクは取り込まない
    if ((scheme == u"http" || scheme == u"https") && c.remote.isEmpty() && (imageDrag || looksLikeImagePath(u))) c.remote = u;
  }
  // 画像のファイルの中身を、そのまま渡すブラウザー・アプリもある
  static const char* const formats[] = {"image/png", "image/jpeg", "image/gif", "image/webp", "image/bmp", "image/tiff",
                                        "application/x-qt-windows-mime;value=\"PNG\"", "application/x-qt-windows-mime;value=\"JFIF\"",
                                        "application/x-qt-windows-mime;value=\"GIF\""};
  for (const char* f : formats) {
    const QString fmt = QString::fromLatin1(f);
    if (!m->hasFormat(fmt)) continue;
    const QByteArray d = m->data(fmt);
    if (!d.isEmpty() && d.size() <= WebImageImporter::kMaxBytes) {
      c.encoded = d;
      break;
    }
  }
  if (m->hasImage()) c.image = qvariant_cast<QImage>(m->imageData());
  if (!c.remote.isEmpty()) c.baseName = QFileInfo(c.remote.path()).completeBaseName();
  return c;
}

QByteArray pngBytes(const QImage& image) {
  QByteArray out;
  QBuffer b(&out);
  b.open(QIODevice::WriteOnly);
  image.save(&b, "PNG");
  return out;
}

QString networkErrorText(QNetworkReply* r) {
  const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  switch (r->error()) {
    case QNetworkReply::HostNotFoundError: return QStringLiteral("サーバーが見つかりません");
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError: return QStringLiteral("時間がかかりすぎたので、やめました（30 秒まで）");
    case QNetworkReply::SslHandshakeFailedError: return QStringLiteral("安全な接続ができませんでした");
    case QNetworkReply::ContentNotFoundError: return QStringLiteral("画像が見つかりません（%1）").arg(status);
    case QNetworkReply::ContentAccessDenied: return QStringLiteral("サーバーが画像の読み込みを許可しませんでした（%1）").arg(status);
    case QNetworkReply::TooManyRedirectsError: return QStringLiteral("転送（リダイレクト）が多すぎます");
    default: break;
  }
  return status > 0 ? QStringLiteral("サーバーが画像を返しませんでした（%1）").arg(status) : r->errorString();
}

}  // namespace

bool WebImageImporter::canAccept(const QMimeData* m) {
  return collect(m).any();
}

bool WebImageImporter::importFromMime(const QMimeData* m) {
  const WebCandidates c = collect(m);
  if (!c.any()) return false;
  QString error;
  // 1. data: の URL（URL の中に画像が入っている。通信しない）
  if (!c.dataUrl.isEmpty()) {
    if (const auto bytes = decodeDataUrl(c.dataUrl, &error); bytes && accept(*bytes, c.baseName, &error)) return true;
  }
  // 2. 画像のファイルの中身
  if (!c.encoded.isEmpty() && accept(c.encoded, c.baseName, &error)) return true;
  // 3. 画像の URL: 元のファイルをダウンロードする（ダウンロードできなければ、ブラウザーが一緒に渡した画像を使う）
  if (!c.remote.isEmpty()) {
    Job job;
    job.url = c.remote;
    job.baseName = c.baseName;
    if (!c.image.isNull()) job.fallback = pngBytes(c.image);
    start(job);
    return true;
  }
  // 4. ブラウザーが渡した画像だけ
  if (!c.image.isNull() && accept(pngBytes(c.image), c.baseName, &error)) return true;
  editor().toast(QStringLiteral("Web の画像を読み込めませんでした: %1").arg(error), ToastKind::Error);
  emit finished(false, QString(), error);
  return true;
}

bool WebImageImporter::accept(const QByteArray& data, const QString& baseName, QString* error) {
  const QString ext = imageFormatOf(data, error);
  if (ext.isEmpty()) return false;
  const QString path = saveImage(data, ext, baseName, error);
  if (path.isEmpty()) return false;
  // メディアの一覧に入れる（タイムラインには置かない）。同じファイルなら、二重に登録しない
  media().importPaths({path}, false, std::nullopt, std::nullopt, kCopyrightNote);
  emit finished(true, path, QString());
  return true;
}

void WebImageImporter::fail(const Job& job, const QString& reason) {
  // ダウンロードできなかったが、ブラウザーが画像そのものも渡していた: そちらを使う
  QString error;
  if (!job.fallback.isEmpty() && accept(job.fallback, job.baseName, &error)) return;
  editor().toast(QStringLiteral("Web の画像を読み込めませんでした: %1").arg(reason), ToastKind::Error);
  emit finished(false, QString(), reason);
}

void WebImageImporter::start(const Job& job) {
  if (const QString why = checkUrl(job.url); !why.isEmpty()) {
    fail(job, why);
    return;
  }
  const QString key = job.url.toString(QUrl::FullyEncoded);
  if (inFlight_.contains(key)) {
    editor().toast(QStringLiteral("この画像は、いまダウンロードしています"));
    return;
  }
  inFlight_.insert(key);
  editor().toast(QStringLiteral("Web の画像をダウンロードしています…"));
  // 名前を引いて、行ってはいけないアドレスでないかを確かめてから送る
  QHostInfo::lookupHost(job.url.host(), this, [this, job, key](const QHostInfo& info) {
    QString why;
    if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) why = QStringLiteral("サーバーが見つかりません（インターネットにつながっているか確かめてください）");
    for (const QHostAddress& a : info.addresses()) {
      if (isBlockedAddress(a)) why = kInternalAddress;
    }
    if (!why.isEmpty()) {
      inFlight_.remove(key);
      fail(job, why);
      return;
    }
    startRequest(job);
  });
}

void WebImageImporter::startRequest(const Job& job) {
  const QString key = job.url.toString(QUrl::FullyEncoded);
  inFlight_.insert(key);
  QNetworkRequest req(job.url);
  // 転送（リダイレクト）は、転送先を 1 回ずつ確かめてから進む
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
  req.setMaximumRedirectsAllowed(kMaxRedirects);
  req.setTransferTimeout(kTimeoutMs);
  req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
  req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) e-editor"));
  req.setRawHeader("Accept", "image/webp,image/png,image/jpeg,image/gif,image/*;q=0.8,*/*;q=0.1");
  QNetworkReply* reply = nam_->get(req);
  auto data = std::make_shared<QByteArray>();
  auto reason = std::make_shared<QString>();  // こちらから止めた理由
  auto current = std::make_shared<QUrl>(job.url);
  auto clock = std::make_shared<QElapsedTimer>();
  auto lastToast = std::make_shared<qint64>(0);
  clock->start();
  auto stop = [reply, reason](const QString& why) {
    if (reason->isEmpty()) *reason = why;
    reply->abort();
  };
  // 全体の時間の上限（少しずつ届き続けるときも、ここで止める）
  QTimer::singleShot(kTimeoutMs, reply, [stop] { stop(QStringLiteral("時間がかかりすぎたので、やめました（30 秒まで）")); });
  connect(reply, &QNetworkReply::redirected, this, [reply, stop, current](const QUrl& next) {
    const QUrl target = current->resolved(next);
    if (current->scheme().toLower() == u"https" && target.scheme().toLower() != u"https") {
      stop(QStringLiteral("安全な接続（https）から、安全でない接続（http）への転送は行いません"));
      return;
    }
    if (const QString why = checkUrl(target); !why.isEmpty()) {
      stop(why);
      return;
    }
    QHostInfo::lookupHost(target.host(), reply, [reply, stop, current, target](const QHostInfo& info) {
      if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
        stop(QStringLiteral("転送先のサーバーが見つかりません"));
        return;
      }
      for (const QHostAddress& a : info.addresses()) {
        if (isBlockedAddress(a)) {
          stop(kInternalAddress);
          return;
        }
      }
      *current = target;
      emit reply->redirectAllowed();
    });
  });
  connect(reply, &QNetworkReply::metaDataChanged, this, [reply, stop] {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // 転送の返事・エラーの返事（中身は使わない。エラーは、終わったときに番号で知らせる）
    if (status != 0 && (status < 200 || status >= 300)) return;
    if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > kMaxBytes) {
      stop(QStringLiteral("画像が大きすぎます（20 MB まで）"));
      return;
    }
    const QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (!type.isEmpty() && !type.startsWith(QStringLiteral("image/")) && type != u"application/octet-stream" && type != u"binary/octet-stream") {
      stop(QStringLiteral("画像ではありません（%1）").arg(type));
    }
  });
  connect(reply, &QNetworkReply::readyRead, this, [reply, data, stop] {
    data->append(reply->readAll());
    if (data->size() > kMaxBytes) stop(QStringLiteral("画像が大きすぎます（20 MB まで）"));
  });
  connect(reply, &QNetworkReply::downloadProgress, this, [clock, lastToast](qint64 received, qint64 total) {
    // 時間がかかっているときだけ、進み具合を知らせる
    if (total <= 0 || clock->elapsed() < 700 || clock->elapsed() - *lastToast < 500) return;
    *lastToast = clock->elapsed();
    editor().toast(QStringLiteral("Web の画像をダウンロードしています… %1%").arg(int(received * 100 / total)));
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, data, reason, job, key] {
    reply->deleteLater();
    inFlight_.remove(key);
    if (!reason->isEmpty()) {
      fail(job, *reason);
      return;
    }
    if (reply->error() != QNetworkReply::NoError) {
      fail(job, networkErrorText(reply));
      return;
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 0 && (status < 200 || status >= 300)) {
      fail(job, QStringLiteral("サーバーが画像を返しませんでした（%1）").arg(status));
      return;
    }
    data->append(reply->readAll());
    QString error;
    if (!accept(*data, job.baseName, &error)) fail(job, error);
  });
}

}  // namespace kv
