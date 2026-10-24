// 一時的: 自動でつなぎ直す動きの確認（確認が終わったら消す）
#include "uitest.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <functional>
#include <memory>
#include <vector>

#include "ai/account.h"
#include "ai/aiconfig.h"
#include "ai/assistant.h"
#include "ai/servercall.h"
#include "core/projectio.h"
#include "ui/accountdialog.h"
#include "ui/aipanel.h"
#include "ui/mainwindow.h"

namespace kv {

namespace {

QString g_out;
QStringList g_netLog;

bool testCopy() {
  return QCoreApplication::applicationDirPath().contains(QStringLiteral("kv-ui-app"));
}
QString exeEnv() {
  return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral(".env"));
}
void writeFile(const QString& path, const QByteArray& bytes) {
  QFile f(path);
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(bytes);
}
QJsonObject stats() {
  // サーバーが書き直している途中に読むと、空や途中までになるので、読めるまで読み直す
  for (int i = 0; i < 20; ++i) {
    QFile f(g_out + QStringLiteral("/stats.json"));
    if (f.open(QIODevice::ReadOnly)) {
      const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
      if (o.contains(QStringLiteral("login"))) return o;
    }
    QThread::msleep(30);
  }
  return {};
}
int stat(const char* key) {
  // stats.json は 200 ms ごとに書かれるので、書き直されるのを待ってから読む
  QThread::msleep(450);
  return stats().value(QString::fromLatin1(key)).toInt();
}
/** サーバーに「次の n 回を切る」と頼み、受け取るまで待つ（最初は、イベントループの前に呼ぶ） */
bool queueDrops(const char* kind, int n) {
  writeFile(g_out + QStringLiteral("/%1drop-%2.txt").arg(QString::fromLatin1(kind)).arg(n), "1");
  const QString key = QStringLiteral("%1Drops").arg(QString::fromLatin1(kind));
  for (int i = 0; i < 50; ++i) {
    if (stats().value(key).toInt() == n && !QFile::exists(g_out + QStringLiteral("/%1drop-%2.txt").arg(QString::fromLatin1(kind)).arg(n))) return true;
    QThread::msleep(100);
  }
  return false;
}
void captureNetLog(QtMsgType, const QMessageLogContext& ctx, const QString& msg) {
  if (ctx.category && QByteArray(ctx.category) == "kv.ai.server") g_netLog << msg;
}
int reconnects() {
  int n = 0;
  for (const QString& l : g_netLog) {
    if (l.contains(QStringLiteral("reconnecting"))) ++n;
  }
  return n;
}

}  // namespace

void prepareUiTest(const QString& outDir, const QString& mode) {
  g_out = outDir;
  QStandardPaths::setTestModeEnabled(true);
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  QDir(dir).removeRecursively();
  QDir().mkpath(dir);
  // つなぎ直したことの記録を受け取る
  QLoggingCategory::setFilterRules(QStringLiteral("kv.ai.server.info=true"));
  qInstallMessageHandler(captureNetLog);
  if (!testCopy()) return;
  if (mode == u"tlsdrop") {
    // テスト用の証明書（%TEMP%\kv-tls の CA）を、このアプリの中だけで信用する
    const QList<QSslCertificate> ca = QSslCertificate::fromPath(QDir::tempPath() + QStringLiteral("/kv-tls/ca.pem"));
    QSslConfiguration conf = QSslConfiguration::defaultConfiguration();
    conf.addCaCertificates(ca);
    QSslConfiguration::setDefaultConfiguration(conf);
    writeFile(outDir + QStringLiteral("/tls-debug.txt"),
              QStringLiteral("backend=%1 loaded=%2 defaultCAs=%3 ca0=%4\n")
                  .arg(QSslSocket::activeBackend())
                  .arg(ca.size())
                  .arg(QSslConfiguration::defaultConfiguration().caCertificates().size())
                  .arg(ca.isEmpty() ? QString() : ca.first().subjectDisplayName())
                  .toUtf8());
    // Schannel が追加の CA を使わないときのため、テスト用のこの証明書だけは、問題があっても受け入れる
    const QList<QSslCertificate> leaf = QSslCertificate::fromPath(QDir::tempPath() + QStringLiteral("/kv-tls/cert.pem"));
    QObject::connect(ai::serverNetwork(), &QNetworkAccessManager::sslErrors, qApp, [leaf](QNetworkReply* reply, const QList<QSslError>& errors) {
      bool onlyTestCert = !leaf.isEmpty();
      for (const QSslError& e : errors) onlyTestCert = onlyTestCert && e.certificate() == leaf.first();
      if (onlyTestCert) reply->ignoreSslErrors(errors);
    });
    writeFile(exeEnv(), "KANTAN_AI_SERVER=https://localhost:18794\n");
    queueDrops("tls", 3);  // 起動したときのつながりの確認を、3 回続けて切る
  } else if (mode == u"httpdrop") {
    writeFile(exeEnv(), "KANTAN_AI_SERVER=http://127.0.0.1:18795\n");
    queueDrops("http", 2);  // 依頼を受け取ってから 2 回切る（GET は送り直してよい）
  } else if (mode == u"real") {
    // 本物のサーバーのアドレスは、ソースに書かずにリポジトリの .env から写す
    QFile src(QStringLiteral("c:/Users/yunre/documents/.create/JPHACKS/e-editor/.env"));
    if (src.open(QIODevice::ReadOnly)) writeFile(exeEnv(), src.readAll());
  }
}

namespace {

class Runner : public QObject {
 public:
  Runner(MainWindow* w, QString mode) : window_(w), mode_(std::move(mode)) {}

  void start() {
    clock_.start();
    if (!testCopy()) {
      check(false, QStringLiteral("running from the %TEMP% copy (kv-ui-app)"));
      return finish();
    }
    connect(panel()->assistant(), &ai::Assistant::replied, this, [this](const QString& t) { replies_ << t; });
    connect(panel()->assistant(), &ai::Assistant::failed, this, [this](const QString& m) { log(QStringLiteral("    AI FAILED: ") + m); });
    if (mode_ == u"tlsdrop") plan_ = {[this] { tls1(); }, [this] { tls2(); }, [this] { tls3(); }, [this] { tls4(); }};
    else if (mode_ == u"httpdrop") plan_ = {[this] { http1(); }, [this] { http2(); }, [this] { http3(); }};
    else plan_ = {[this] { real1(); }};
    next();
  }

 private:
  void log(const QString& line) {
    QFile f(g_out + QStringLiteral("/report-%1.txt").arg(mode_));
    if (f.open(QIODevice::Append)) f.write((line + QLatin1Char('\n')).toUtf8());
  }
  void check(bool ok, const QString& name, const QString& detail = QString()) {
    log(QStringLiteral("%1  %2%3").arg(ok ? QStringLiteral("OK") : QStringLiteral("NG"), name, detail.isEmpty() ? QString() : QStringLiteral(" :: ") + detail));
    if (!ok) ++failed_;
  }
  void next() {
    if (index_ >= plan_.size()) return finish();
    auto s = plan_[index_++];
    QTimer::singleShot(300, this, s);
  }
  void finish() {
    if (QWidget* m = QApplication::activeModalWidget()) m->close();
    QApplication::processEvents();
    for (const QString& l : g_netLog) log(QStringLiteral("    net: ") + l);
    log(QStringLiteral("DONE failures=%1 total=%2 s").arg(failed_).arg(clock_.elapsed() / 1000));
    clearAutosave();
    QFile::remove(exeEnv());
    QTimer::singleShot(300, qApp, [] { QCoreApplication::exit(0); });
  }
  void waitFor(std::function<bool()> pred, int timeoutMs, std::function<void(bool)> then) {
    auto* timer = new QTimer(this);
    auto started = std::make_shared<QElapsedTimer>();
    started->start();
    timer->setInterval(50);
    connect(timer, &QTimer::timeout, this, [timer, started, pred, timeoutMs, then] {
      const bool ok = pred();
      if (!ok && started->elapsed() < timeoutMs) return;
      timer->stop();
      timer->deleteLater();
      then(ok);
    });
    timer->start();
  }
  AccountDialog* dialog() const { return qobject_cast<AccountDialog*>(QApplication::activeModalWidget()); }
  template <typename T>
  T* inDialog(const char* name) const {
    AccountDialog* d = dialog();
    return d ? d->findChild<T*>(QString::fromLatin1(name)) : nullptr;
  }
  QString dlgText(const char* name) const {
    if (auto* l = inDialog<QLabel>(name)) return l->isVisible() ? l->text() : QString();
    return QStringLiteral("(none)");
  }
  void type(const char* name, const QString& text) {
    if (auto* e = inDialog<QLineEdit>(name)) e->setText(text);
    else check(false, QStringLiteral("field %1 exists").arg(QString::fromLatin1(name)));
  }
  void clickDialog(const char* name) {
    if (auto* b = inDialog<QAbstractButton>(name)) QTimer::singleShot(0, b, [b] { b->click(); });
    else check(false, QStringLiteral("button %1 exists").arg(QString::fromLatin1(name)));
  }
  QToolButton* status() const { return window_->findChild<QToolButton*>(QStringLiteral("ServerStatus")); }
  QString statusText() const { return status() ? status()->text() : QStringLiteral("(no status)"); }
  QString statusTip() const { return status() ? status()->toolTip().replace(QLatin1Char('\n'), QStringLiteral(" / ")) : QString(); }
  AiPanel* panel() const { return window_->findChild<AiPanel*>(); }
  void openAi() {
    if (panel()->isVisible()) return;
    for (QAction* a : window_->findChildren<QAction*>()) {
      if (a->text() == u"AI アシスタント") a->trigger();
    }
    QApplication::processEvents();
  }
  /** 次の通信は、新しくつなぐ（今の接続を使い回さない） */
  void freshConnection() { ai::serverNetwork()->clearConnectionCache(); }
  bool online() const { return ai::account().link() == ai::Account::Link::Online; }

  // ---- tlsdrop: TLS のあいさつで切られる（Cloudflare の前で見られたもの）----
  void tls1() {
    // 起動したときのつながりの確認を 3 回続けて切る → 自動でつなぎ直して、つながる
    waitFor([this] { return dialog() != nullptr; }, 10000, [this](bool shown) {
      check(shown, QStringLiteral("login dialog shown at startup"));
      const bool checking = statusText() == u"AI サーバー: 確認中…";
      check(checking || online(), QStringLiteral("status while reconnecting"), statusText());
      waitFor([this] { return online(); }, 15000, [this](bool ok) {
        QApplication::processEvents();
        check(ok, QStringLiteral("connected after the dropped handshakes"), statusText() + QStringLiteral(" / ") + ai::account().linkDetail());
        check(stat("tlsDropped") == 3, QStringLiteral("server reset 3 handshakes"), QString::number(stat("tlsDropped")));
        check(stat("health") >= 1, QStringLiteral("health request reached the server"), QString::number(stat("health")));
        check(reconnects() == 3, QStringLiteral("app reconnected 3 times"), QString::number(reconnects()));
        check(dlgText("AccountLinkNote").isEmpty(), QStringLiteral("no error line in the dialog"), dlgText("AccountLinkNote"));
        waitFor([this] { return statusText() == u"AI サーバー: ログインしていません"; }, 3000,
                [this](bool s) { check(s, QStringLiteral("status: connected, not logged in"), statusText()); });
        next();
      });
    });
  }
  void tls2() {
    // ログイン（POST）を新しい接続で、TLS のあいさつで 2 回切る → まだ送っていないので、つなぎ直して 1 回だけ届く
    freshConnection();
    const int drops = stat("tlsDropped");
    const int before = reconnects();
    logins_ = stat("login");
    check(queueDrops("tls", 2), QStringLiteral("server ready to reset 2 handshakes"));
    type("AccountLoginEmail", QStringLiteral("hanako@example.com"));
    type("AccountLoginPassword", QStringLiteral("blue-river-2026"));
    clickDialog("AccountLoginButton");
    waitFor([this] { return dialog() == nullptr && ai::account().loggedIn(); }, 15000, [this, drops, before](bool ok) {
      check(ok, QStringLiteral("logged in after the dropped handshakes"), dlgText("AccountLoginError"));
      check(stat("tlsDropped") - drops == 2, QStringLiteral("server reset 2 handshakes"), QString::number(stat("tlsDropped") - drops));
      check(stat("login") - logins_ == 1, QStringLiteral("login reached the server exactly once"), QString::number(stat("login") - logins_));
      check(reconnects() - before == 2, QStringLiteral("app reconnected 2 times for the POST"), QString::number(reconnects() - before));
      next();
    });
  }
  void tls3() {
    // AI への依頼（POST）も同じ: 2 回切られても、つなぎ直して 1 回だけ届く
    openAi();
    freshConnection();
    const int drops = stat("tlsDropped");
    const int calls = stat("converse");
    const int before = reconnects();
    check(queueDrops("tls", 2), QStringLiteral("server ready to reset 2 handshakes"));
    replies_.clear();
    check(panel()->ask(QStringLiteral("こんにちは")), QStringLiteral("AI request sent"));
    waitFor([this] { return !panel()->assistant()->busy(); }, 20000, [this, drops, calls, before](bool) {
      QApplication::processEvents();
      check(replies_.contains(QStringLiteral("テストの返事です。")), QStringLiteral("AI reply after the dropped handshakes"), replies_.join(QStringLiteral(" / ")));
      check(stat("tlsDropped") - drops == 2, QStringLiteral("server reset 2 handshakes"), QString::number(stat("tlsDropped") - drops));
      check(stat("converse") - calls == 1, QStringLiteral("AI request reached the server exactly once"), QString::number(stat("converse") - calls));
      check(reconnects() - before == 2, QStringLiteral("app reconnected 2 times for the AI request"), QString::number(reconnects() - before));
      next();
    });
  }
  void tls4() {
    // 毎回切られる → つなぎ直すのをあきらめて「つながりません」（10 秒ほどで）
    freshConnection();
    const int drops = stat("tlsDropped");
    check(queueDrops("tls", 99), QStringLiteral("server ready to reset every handshake"));
    QElapsedTimer t;
    t.start();
    ai::account().checkLink();
    waitFor([this] { return statusText() == u"AI サーバー: つながりません"; }, 20000, [this, drops, t](bool ok) {
      check(ok, QStringLiteral("status: cannot connect after giving up"), statusText());
      check(t.elapsed() < 14000, QStringLiteral("gave up within 14 s"), QStringLiteral("%1 ms").arg(t.elapsed()));
      check(stat("tlsDropped") - drops >= 8, QStringLiteral("1 try + 7 reconnects"), QString::number(stat("tlsDropped") - drops));
      check(statusTip().contains(QStringLiteral("7 回つなぎ直しました")), QStringLiteral("tooltip says it reconnected 7 times"), statusTip());
      queueDrops("tls", 0);
      next();
    });
  }

  // ---- httpdrop: 依頼を受け取ってから切る ----
  void http1() {
    // つながりの確認（GET）は、届いたあとで切られても送り直してよい
    waitFor([this] { return dialog() != nullptr; }, 10000, [this](bool shown) {
      check(shown, QStringLiteral("login dialog shown at startup"));
      waitFor([this] { return online(); }, 15000, [this](bool ok) {
        check(ok, QStringLiteral("GET resent after drops"), statusText() + QStringLiteral(" / ") + ai::account().linkDetail());
        check(stat("httpDropped") == 2, QStringLiteral("server dropped 2 connections after the request"), QString::number(stat("httpDropped")));
        // Qt も HTTP/1.1 では 1 回だけ自分で送り直すので、アプリが送り直すのは残りの分
        check(reconnects() >= 1 && reconnects() <= 2, QStringLiteral("app resent the GET (with Qt's own resend)"), QString::number(reconnects()));
        next();
      });
    });
  }
  void http2() {
    // ログイン（POST）は、届いたかもしれないので、アプリは送り直さない（二重にしない）。
    // Qt が HTTP/1.1 で自分で送り直す分より多く切って、アプリが足していないことを確かめる
    freshConnection();
    const int drops = stat("httpDropped");
    const int passed = stat("httpPassed");
    logins_ = stat("login");
    const int before = reconnects();
    check(queueDrops("http", 6), QStringLiteral("server ready to drop 6 connections after the request"));
    type("AccountLoginEmail", QStringLiteral("hanako@example.com"));
    type("AccountLoginPassword", QStringLiteral("blue-river-2026"));
    clickDialog("AccountLoginButton");
    waitFor([this] { return !dlgText("AccountLoginError").isEmpty(); }, 15000, [this, drops, passed, before](bool shown) {
      check(shown, QStringLiteral("login error shown"), dlgText("AccountLoginError"));
      check(!dlgText("AccountLoginError").contains(QStringLiteral("つなぎ直しました")), QStringLiteral("error does not claim a reconnect"), dlgText("AccountLoginError"));
      log(QStringLiteral("    connections used by this login (incl. Qt's own resends): %1").arg(stat("httpDropped") - drops));
      check(stat("httpDropped") - drops < 6 && stat("httpPassed") == passed, QStringLiteral("app did not resend the sent POST"),
            QStringLiteral("dropped +%1, passed +%2").arg(stat("httpDropped") - drops).arg(stat("httpPassed") - passed));
      check(reconnects() == before, QStringLiteral("no reconnect by the app for the sent POST"), QString::number(reconnects() - before));
      check(stat("login") == logins_, QStringLiteral("login never reached the server"), QString::number(stat("login") - logins_));
      check(!ai::account().loggedIn() && dialog() != nullptr, QStringLiteral("still on the login page"));
      check(queueDrops("http", 0), QStringLiteral("server stops dropping"));
      next();
    });
  }
  void http3() {
    // もう一度押せば、ログインできる
    const int passed = stat("httpPassed");
    const int drops = stat("httpDropped");
    log(QStringLiteral("    before 2nd login: busy=%1 buttonEnabled=%2 password=%3 chars")
            .arg(ai::account().busy())
            .arg(inDialog<QAbstractButton>("AccountLoginButton") && inDialog<QAbstractButton>("AccountLoginButton")->isEnabled())
            .arg(inDialog<QLineEdit>("AccountLoginPassword") ? inDialog<QLineEdit>("AccountLoginPassword")->text().size() : -1));
    clickDialog("AccountLoginButton");
    waitFor([this] { return dialog() == nullptr && ai::account().loggedIn(); }, 15000, [this, passed, drops](bool ok) {
      check(ok, QStringLiteral("second login works"),
            QStringLiteral("error=%1 passed+%2 dropped+%3").arg(dlgText("AccountLoginError")).arg(stat("httpPassed") - passed).arg(stat("httpDropped") - drops));
      check(stat("login") - logins_ == 1, QStringLiteral("login reached the server once"), QString::number(stat("login") - logins_));
      next();
    });
  }

  // ---- real: 本物のサーバー。起動したときに、つながるか ----
  void real1() {
    waitFor([] { return ai::account().link() != ai::Account::Link::Checking; }, 20000, [this](bool) {
      log(QStringLiteral("RESULT online=%1 reconnects=%2 ms=%3 detail=%4")
              .arg(online())
              .arg(reconnects())
              .arg(clock_.elapsed())
              .arg(ai::account().linkDetail()));
      check(online(), QStringLiteral("real server reachable at startup"));
      next();
    });
  }

  MainWindow* window_;
  QString mode_;
  int failed_ = 0;
  int logins_ = 0;
  QElapsedTimer clock_;
  std::vector<std::function<void()>> plan_;
  size_t index_ = 0;
  QStringList replies_;
};

}  // namespace

void runUiTest(MainWindow* window, const QString& outDir, const QString& mode) {
  g_out = outDir;
  auto* r = new Runner(window, mode);
  r->setParent(window);
  QTimer::singleShot(100, r, [r] { r->start(); });
}

}  // namespace kv
