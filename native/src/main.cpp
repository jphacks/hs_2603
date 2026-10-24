#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include "core/editor.h"
#include "media/mediamanager.h"
#include "ui/mainwindow.h"
#include "ui/theme.h"
#include "uitest.h"

using namespace kv;

static QStringList openableFiles(const QStringList& args) {
  static const QStringList exts = {"mp4", "mov", "m4v", "avi", "mkv", "webm", "wmv", "mts", "m2ts", "ts", "flv", "3gp", "mpg", "mpeg",
                                   "jpg", "jpeg", "png", "webp", "bmp", "gif", "tif", "tiff", "heic", "heif", "avif",
                                   "mp3", "wav", "m4a", "aac", "ogg", "flac", "wma", "opus", "kvproj"};
  QStringList out;
  for (int i = 1; i < args.size(); ++i) {
    const QFileInfo fi(args[i]);
    if (args[i].startsWith(QLatin1Char('-')) || !fi.isFile()) continue;
    if (exts.contains(fi.suffix().toLower())) out << fi.absoluteFilePath();
  }
  return out;
}

int main(int argc, char** argv) {
  QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("kantan-video-editor"));
  QApplication::setApplicationDisplayName(QStringLiteral("かんたん動画エディタ"));
  QApplication::setOrganizationName(QStringLiteral("JPHACKS"));
  QApplication::setApplicationVersion(QStringLiteral(KV_VERSION));

  // Qt の標準の文言（右クリックメニューなど）を日本語にする
  QTranslator qtTr;
  for (const QString& dir : {QApplication::applicationDirPath() + QStringLiteral("/translations"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)}) {
    if (qtTr.load(QLocale(QLocale::Japanese), QStringLiteral("qtbase"), QStringLiteral("_"), dir)) {
      app.installTranslator(&qtTr);
      break;
    }
  }
  theme::apply(app);

  const QStringList args = app.arguments();

  // 2 つ目に起動されたときは、ファイルを最初のウィンドウへ渡して終わる
  const QString serverName = QStringLiteral("kantan-video-editor-") + qEnvironmentVariable("USERNAME");
  QLocalServer server;
  {
    QLocalSocket probe;
    probe.connectToServer(serverName);
    if (probe.waitForConnected(300)) {
      probe.write(openableFiles(args).join(QLatin1Char('\n')).toUtf8());
      probe.waitForBytesWritten(1000);
      probe.disconnectFromServer();
      return 0;
    }
    QLocalServer::removeServer(serverName);
    server.listen(serverName);
  }

  // 一時的: 動作確認
  const int uiTest = int(args.indexOf(QStringLiteral("--ui-test")));
  if (uiTest >= 0) prepareUiTest(args.value(uiTest + 1), args.value(uiTest + 2));
  MainWindow window;
  QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
    QLocalSocket* s = server.nextPendingConnection();
    QObject::connect(s, &QLocalSocket::disconnected, s, [s, &window] {
      const QStringList files = QString::fromUtf8(s->readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
      if (!files.isEmpty()) media().importPaths(files, true);
      window.setWindowState((window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
      window.raise();
      window.activateWindow();
      s->deleteLater();
    });
  });
  window.show();
  if (uiTest >= 0) runUiTest(&window, args.value(uiTest + 1), args.value(uiTest + 2));

  const QStringList files = openableFiles(args);
  QTimer::singleShot(0, &window, [&window, files] {
    // 先に AI サーバーへのログイン（前回のログインが残っていれば、そのまま使う）
    window.startupLogin();
    if (!files.isEmpty()) media().importPaths(files, true);
    else window.offerRestore();
  });
  return app.exec();
}
