#include "ai/aiconfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <cstdint>
#include <map>

// 配る exe に組み込む AI サーバーのアドレス（build.ps1 が native\release.env から、ビルドのフォルダーの generated に作る）。
// IDE などで build.ps1 を通さずにビルドしたときは無いので、組み込まない
#if defined(__has_include)
#if __has_include("kv_embedded_server.h")
#include "kv_embedded_server.h"
#define KV_HAS_EMBEDDED_SERVER 1
#endif
#endif

namespace kv::ai {

// ---- 設定ファイル（考える深さ） -------------------------------------------------------------

static QString settingsPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("settings.ini"));
}

// ---- 考える深さ ----------------------------------------------------------------------
// Claude と OpenAI の GPT の両方が受け付ける深さだけにする（Claude Sonnet 4.6 は xhigh を受け付けない）

static const QStringList& effortLevels() {
  static const QStringList levels = {QStringLiteral("low"), QStringLiteral("medium"), QStringLiteral("high"), QStringLiteral("max")};
  return levels;
}

/** Claude の世代（4.6 なら 4 と 6）。読めなければ false */
static bool claudeVersion(const QString& model, int& major, int& minor) {
  // 例: jp.anthropic.claude-sonnet-4-6 / us.anthropic.claude-opus-4-6-v1 / anthropic.claude-sonnet-5 /
  //     jp.anthropic.claude-sonnet-4-5-20250929-v1:0（日付は世代ではない）。古い claude-3-7-sonnet は読めない
  static const QRegularExpression re(QStringLiteral("claude-[a-z]+-(\\d+)(?:-(\\d{1,2}))?(?!\\d)"));
  const QRegularExpressionMatch m = re.match(model);
  if (!m.hasMatch()) return false;
  major = m.captured(1).toInt();
  minor = m.captured(2).isEmpty() ? 0 : m.captured(2).toInt();
  return true;
}

bool Config::supportsEffort() const {
  // モデルは推論プロファイルの ID（jp.anthropic.… / global.openai.… など）で見分ける（サーバーと同じ決まり）
  if (model.contains(QStringLiteral("anthropic."))) {
    int major = 0;
    int minor = 0;
    return claudeVersion(model, major, minor) && (major > 4 || (major == 4 && minor >= 6));
  }
  return model.contains(QStringLiteral("openai.gpt-")) && !model.contains(QStringLiteral("gpt-oss"));
}

int Config::timeoutMs() const {
  // 画像を見ながら考えると時間がかかるので、もともと長めに待つ。深く考えるときは、さらに長く
  // （サーバーが Bedrock を待つ時間と同じ。サーバーを通る分は、送るときに足す）
  const bool deep = supportsEffort() && (effort == u"high" || effort == u"max");
  if (!deep) return 180000;
  return effort == u"high" ? 300000 : 600000;
}

const QStringList& effortChoices() {
  static const QStringList choices = QStringList{QStringLiteral("auto")} + effortLevels();
  return choices;
}

QString clampEffort(const QString& level, const QString& max) {
  const qsizetype i = effortLevels().indexOf(level);
  const qsizetype m = effortLevels().indexOf(max);
  if (i < 0 || m < 0) return level;
  return i > m ? max : level;
}

QString savedEffort() {
  const QString v = QSettings(settingsPath(), QSettings::IniFormat).value(QStringLiteral("ai/effort")).toString();
  return effortChoices().contains(v) ? v : QString();
}

void saveEffort(const QString& choice) {
  if (!effortChoices().contains(choice)) return;
  QDir().mkpath(QFileInfo(settingsPath()).absolutePath());
  QSettings(settingsPath(), QSettings::IniFormat).setValue(QStringLiteral("ai/effort"), choice);
}

// ---- .env ---------------------------------------------------------------------------

/** リポジトリの中で動かしているとき（開発中）は、その一番上のフォルダー。そうでなければ空 */
static QString repoRoot() {
  QDir d(QCoreApplication::applicationDirPath());
  // native\dist\kantan-video-editor の 3 つ上がリポジトリの一番上
  for (int i = 0; i < 4; ++i) {
    if (QFileInfo::exists(d.filePath(QStringLiteral(".env.example"))) || QFileInfo::exists(d.filePath(QStringLiteral(".git")))) {
      return d.absolutePath();
    }
    if (!d.cdUp()) break;
  }
  return {};
}

QStringList envSearchDirs() {
  // 関係のないフォルダーの .env は読まない
  QStringList dirs{QCoreApplication::applicationDirPath()};
  const QString repo = repoRoot();
  if (!repo.isEmpty() && !dirs.contains(repo)) dirs << repo;
  const QString data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (!data.isEmpty() && !dirs.contains(data)) dirs << data;
  return dirs;
}

/** KEY=VALUE の行を読む。# で始まる行と空行は無視し、値の前後の引用符は外す */
static std::map<QString, QString> parseEnv(const QByteArray& bytes) {
  std::map<QString, QString> out;
  QString text;
  if (bytes.startsWith("\xFF\xFE")) {
    // UTF-16（Windows PowerShell 5 の「>」や、メモ帳の「UTF-16 LE」で保存したとき）
    text = QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.constData() + 2), (bytes.size() - 2) / 2);
  } else {
    text = QString::fromUtf8(bytes);
  }
  if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
  for (QString line : text.split(QLatin1Char('\n'))) {
    line = line.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
    if (line.startsWith(QStringLiteral("export "))) line = line.mid(7).trimmed();
    const qsizetype eq = line.indexOf(QLatin1Char('='));
    if (eq <= 0) continue;
    const QString key = line.left(eq).trimmed();
    QString value = line.mid(eq + 1).trimmed();
    if (value.size() >= 2 && ((value.front() == u'"' && value.back() == u'"') || (value.front() == u'\'' && value.back() == u'\''))) {
      value = value.mid(1, value.size() - 2);
    } else {
      const qsizetype hash = value.indexOf(QStringLiteral(" #"));
      if (hash >= 0) value = value.left(hash).trimmed();
    }
    out[key] = value;
  }
  return out;
}

static const QString& serverKey() {
  static const QString key = QStringLiteral("KANTAN_AI_SERVER");
  return key;
}

/** 探す .env の場所（探す順。まだ無いものも入る） */
static QStringList envPaths() {
  QStringList paths;
  for (const QString& dir : envSearchDirs()) paths << QDir(dir).filePath(QStringLiteral(".env"));
  return paths;
}

/**
 * key に値が書いてある最初の .env（場所と値）。どこにも無ければ、場所は空。
 * 「KANTAN_AI_SERVER=」のように値が空なら、書いていないのと同じに扱う（.env.example を写しただけの .env など）
 */
static std::pair<QString, QString> findEnv(const QString& key) {
  for (const QString& path : envPaths()) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) continue;
    const auto env = parseEnv(f.readAll());
    if (const auto it = env.find(key); it != env.end() && !it->second.trimmed().isEmpty()) return {QDir::toNativeSeparators(path), it->second};
  }
  return {};
}

QString envFile() {
  return findEnv(serverKey()).first;
}

QString suggestedEnvPath() {
  // すでに .env があれば、最初に読むものに書き足してもらう
  for (const QString& path : envPaths()) {
    if (QFileInfo(path).isFile()) return QDir::toNativeSeparators(path);
  }
  // 無ければ、開発中はリポジトリの一番上（.gitignore で除外している）。インストールしたアプリでは、データフォルダー
  const QString repo = repoRoot();
  const QString dir = repo.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : repo;
  return QDir::toNativeSeparators(QDir(dir).filePath(QStringLiteral(".env")));
}

// ---- AI サーバーのアドレス ------------------------------------------------------------------

/** 家の中（同じネットワーク・自分の PC・Tailscale）のアドレスか */
static bool privateHost(const QString& host) {
  const QString h = host.toLower();
  if (h == u"localhost" || h.endsWith(QStringLiteral(".local"))) return true;
  QHostAddress a;
  if (!a.setAddress(h)) return false;
  if (a.isLoopback() || a.isLinkLocal() || a.isPrivateUse()) return true;
  // 100.64.0.0/10（Tailscale などが使う）。通り道が暗号化されている
  bool v4 = false;
  const quint32 ip = a.toIPv4Address(&v4);
  return v4 && (ip & 0xFFC00000u) == 0x64400000u;
}

QString normalizeServer(const QString& text) {
  QString s = text.trimmed();
  if (s.isEmpty()) return {};
  if (!s.contains(QStringLiteral("://"))) {
    // 書き始めを省いたとき: 家の中のアドレスは http、インターネットのサーバーは https
    const QString host = QUrl(QStringLiteral("http://") + s).host();
    s.prepend(privateHost(host) ? QStringLiteral("http://") : QStringLiteral("https://"));
  }
  // 大文字・小文字をそろえる（AI.example.com → ai.example.com）。ログインの印を、同じサーバーと見分けられるように
  const QUrl u(s, QUrl::StrictMode);
  if (u.isValid() && !u.host().isEmpty()) s = u.toString(QUrl::StripTrailingSlash);
  while (s.endsWith(QLatin1Char('/'))) s.chop(1);
  return s;
}

/**
 * exe に組み込んだアドレスを戻す（無ければ空）。exe の中には、そのままの文字ではなく、鍵と混ぜたバイトの並びで入っている
 * （exe を文字で眺めても見えないようにするだけ。通信を見れば接続先は分かるので、秘密にはならない）
 */
static QString decodeEmbeddedServer() {
#ifdef KV_HAS_EMBEDDED_SERVER
  QByteArray bytes(embedded::kSize, Qt::Uninitialized);
  // volatile で読み、コンパイラーが戻したあとの文字を exe に入れてしまわないようにする
  const volatile unsigned char* key = embedded::kKey;
  const volatile unsigned char* data = embedded::kData;
  for (int i = 0; i < embedded::kSize; ++i) bytes[i] = char(data[i] ^ key[i] ^ std::uint8_t(i * 0x9D + 0x5B));
  return QString::fromUtf8(bytes);
#else
  return {};
#endif
}

/** exe に組み込んだアドレス（整えたもの。無ければ空） */
static const QString& embeddedServer() {
  static const QString server = normalizeServer(decodeEmbeddedServer());
  return server;
}

bool hasBuiltInServer() {
  return !embeddedServer().isEmpty();
}

ServerSource serverSource() {
  if (!findEnv(serverKey()).first.isEmpty()) return ServerSource::EnvFile;
  return hasBuiltInServer() ? ServerSource::BuiltIn : ServerSource::None;
}

QString configuredServer() {
  // 管理者が別のサーバーで試せるよう、.env を先に見る
  const QString env = normalizeServer(findEnv(serverKey()).second);
  return env.isEmpty() ? embeddedServer() : env;
}

QString wrongServerText() {
  if (serverSource() == ServerSource::BuiltIn) {
    return QStringLiteral("AI サーバーではないところにつながりました。アプリを配った人（管理者）に知らせてください");
  }
  return QStringLiteral(".env の KANTAN_AI_SERVER が、AI サーバーではないところを指しているようです。アドレスとポート番号を確かめてください");
}

QString serverProblem(const QString& url) {
  // 画面に出す文なので、アドレスそのものは入れない（直す人は .env を開けば分かる）
  if (url.isEmpty()) return QStringLiteral("AI サーバーのアドレスが決まっていません。.env に KANTAN_AI_SERVER=（アドレス）を書いてください");
  // 組み込んだアドレスは build.ps1 が確かめているので、ふつうは問題にならない。なっても、使う人には直せない
  const bool builtIn = serverSource() == ServerSource::BuiltIn;
  const QString builtInProblem = QStringLiteral("アプリに組み込まれた AI サーバーのアドレスが正しくありません。アプリを配った人（管理者）に知らせてください");
  const QUrl u(url, QUrl::StrictMode);
  if (!u.isValid() || u.host().isEmpty() || (u.scheme() != u"http" && u.scheme() != u"https")) {
    return builtIn ? builtInProblem : QStringLiteral(".env の KANTAN_AI_SERVER の書き方が正しくありません（例: https://ai.example.com）");
  }
  if (!u.userInfo().isEmpty() || u.hasQuery() || u.hasFragment()) {
    return builtIn ? builtInProblem : QStringLiteral(".env の KANTAN_AI_SERVER には、サーバーのアドレスだけを書いてください（例: https://ai.example.com）");
  }
  if (u.scheme() == u"http" && !privateHost(u.host())) {
    return builtIn ? builtInProblem
                   : QStringLiteral(".env の KANTAN_AI_SERVER が、HTTP で家の外のアドレスになっています。パスワードが読まれないよう、https:// にしてください");
  }
  return {};
}

QString hideServer(QString text, const QString& server) {
  if (server.isEmpty()) return text;
  const QUrl u(server);
  // 長いものから伏せる（http://host:port → host:port → host）
  for (const QString& part : {server, u.authority(), u.host()}) {
    if (part.size() >= 3) text.replace(part, QStringLiteral("…"), Qt::CaseInsensitive);
  }
  return text;
}

Config loadConfig() {
  Config c;
  c.server = configuredServer();
  return c;
}

}  // namespace kv::ai
