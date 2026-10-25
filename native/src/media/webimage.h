#pragma once
// ブラウザーからドラッグした Web の画像を、メディアの一覧に取り込む。
// - 画像のデータそのもの（ブラウザーが渡す画像・data: の URL）は、確かめてからそのまま保存する（通信しない）。
// - 画像の URL は、安全を確かめてからダウンロードする: http / https だけ。この PC・家や会社のネットワークなど、内側のアドレスへは
//   行かない（名前引きした結果と、転送（リダイレクト）先も 1 回ずつ確かめる）。大きさの上限（20 MB）・時間の上限（30 秒）あり。
// - 受け取ったデータは、対応している画像（PNG・JPEG・GIF・WebP・BMP・TIFF）として読めるときだけ保存する。拡張子は中身から決め、
//   ファイルとして開いたり実行したりはしない。
// 保存先は、アプリの設定と同じ場所（AppLocalData）の web-images。同じ中身の画像は同じファイルにするので、二重に登録されない。

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>
#include <optional>

class QHostAddress;
class QMimeData;
class QNetworkAccessManager;
class QNetworkReply;

namespace kv {

class WebImageImporter : public QObject {
  Q_OBJECT
 public:
  static WebImageImporter& instance();

  static constexpr qint64 kMaxBytes = 20 * 1024 * 1024;  // ダウンロード・画像のデータの上限
  static constexpr int kTimeoutMs = 30000;               // ダウンロードの時間の上限
  static constexpr int kMaxRedirects = 5;                // 転送（リダイレクト）の回数の上限
  static constexpr int kMaxSide = 16384;                 // 画像の幅・高さの上限（px）

  /** ブラウザーからドラッグしているものに、取り込める Web の画像（画像のデータか、画像の URL）があるか。ローカルのファイルがあれば false */
  static bool canAccept(const QMimeData* m);
  /** ドロップされたものから Web の画像を取り込む（URL ならダウンロードしてから）。取り込みを始めたら true（結果は通知で知らせる） */
  bool importFromMime(const QMimeData* m);
  /** ダウンロード中の数 */
  int pending() const { return int(inFlight_.size()); }

  // ---- 確かめる決まり（テストからも使う） ----
  /** ダウンロードしてよい URL か。だめなら理由（空なら OK）。名前引きはしない（IP アドレスで書いた URL・localhost などだけを見る） */
  static QString checkUrl(const QUrl& url);
  /** 行ってはいけないアドレス（この PC・家や会社のネットワーク・リンクローカル・マルチキャストなど、インターネットの外のもの）か */
  static bool isBlockedAddress(const QHostAddress& a);
  /** データが、対応している画像として読めるか。読めれば保存に使う拡張子（"png" など）、読めなければ空にして error に理由 */
  static QString imageFormatOf(const QByteArray& data, QString* error);
  /** 確かめた画像を保存して、そのパスを返す（同じ中身なら、前に保存したファイル）。保存できなければ空 */
  static QString saveImage(const QByteArray& data, const QString& ext, const QString& baseName, QString* error);
  /** data: の URL（data:image/...）の中身。画像の data: でなければ nullopt */
  static std::optional<QByteArray> decodeDataUrl(const QString& url, QString* error);
  static QString storeDir();

 signals:
  /** 取り込みを 1 つ終えた（ok: 一覧に入れた。path: 保存した画像。message: 失敗の理由） */
  void finished(bool ok, const QString& path, const QString& message);

 private:
  WebImageImporter();
  struct Job {
    QUrl url;
    QString baseName;
    int redirects = 0;
    /** ダウンロードできなかったときに代わりに使う、ブラウザーが一緒に渡した画像（PNG。無ければ空） */
    QByteArray fallback;
  };
  /** URL を確かめて（名前引きも）、だいじょうぶならダウンロードを始める */
  void start(const Job& job);
  /** 確かめたあとの URL へ、実際にリクエストを送る */
  void startRequest(const Job& job);
  /** 画像のデータを確かめて保存し、メディアの一覧に入れる */
  bool accept(const QByteArray& data, const QString& baseName, QString* error);
  void fail(const Job& job, const QString& reason);
  QNetworkAccessManager* nam_;
  QSet<QString> inFlight_;  // ダウンロード中の URL（同じ画像を続けてドロップしても、1 回だけ）
};

inline WebImageImporter& webImages() {
  return WebImageImporter::instance();
}

}  // namespace kv
