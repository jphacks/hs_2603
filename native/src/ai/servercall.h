#pragma once
// AI サーバーとの通信。アプリから AI サーバーへの通信は、すべてこれを通す。
// - QNetworkAccessManager はアプリで 1 つにして、つないだ接続を、つながりの確認・ログイン・AI への依頼で使い回す
//   （新しくつなぐ回数が減るので、切られにくい）。
// - つないだ直後に接続を切られたとき（依頼をまだ送っていないとき）は、少し待って自動でつなぎ直す。
//   IPv6 の通り道によっては、新しくつなぐたびに TLS のあいさつの途中で切られることがある
//   （Cloudflare の前に置いたサーバーで、半分ほどが切られた）。一度つながれば同じ接続を使い回すので、切られるのはつなぎ始めだけ。
// - 依頼を送ったあとで切られたときは、GET（何度送っても同じ）だけ送り直す。POST（ログイン・登録・パスワードの変更・
//   AI への依頼）は、サーバーに届いていたかもしれないので送り直さない（二重に届かないように）。

#include <QByteArray>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace kv::ai {

/** AI サーバーとの通信に使う、アプリで 1 つの QNetworkAccessManager */
QNetworkAccessManager* serverNetwork();

class ServerCall : public QObject {
  Q_OBJECT
 public:
  /** 送り始める。parent が消えると、通信もやめる */
  static ServerCall* get(const QNetworkRequest& request, QObject* parent);
  static ServerCall* post(const QNetworkRequest& request, const QByteArray& body, QObject* parent);
  /** GET・POST 以外（DELETE など）も送る。送り直しは POST と同じく、サーバーに届く前に切られたときだけ */
  static ServerCall* custom(const QNetworkRequest& request, const QByteArray& method, const QByteArray& body, QObject* parent);
  ~ServerCall() override;

  /** やめる（つなぎ直しも止める）。finished は出さない。このあと消える */
  void abort();
  /** つなぎ直した回数 */
  int retries() const { return retries_; }
  /** 毎回すぐに切られたので、つなぎ直すのをあきらめた（最後の失敗も、切られたもの。画面の文を選ぶのに使う） */
  bool gaveUp() const { return gaveUp_; }

 signals:
  /**
   * 終わった（成功・失敗とも）。reply は最後に送ったもの。受け取った関数から戻ると消えるので、取っておかない。
   * つながらなかったときは、HTTP の状態が無い（reply の HttpStatusCodeAttribute が無効）
   */
  void finished(QNetworkReply* reply);

 private:
  ServerCall(const QNetworkRequest& request, const QByteArray& method, const QByteArray& body, QObject* parent);
  void send();
  void onFinished(QNetworkReply* reply);
  /** 今の失敗が、送り直してよいものか（回数は見ない） */
  bool retryable(QNetworkReply* reply) const;

  QNetworkRequest request_;
  QByteArray method_;
  QByteArray body_;
  QPointer<QNetworkReply> reply_;
  QTimer* wait_;
  int retries_ = 0;
  bool gaveUp_ = false;
  /** 今の 1 回で、依頼をサーバーに送った（届いたかもしれない） */
  bool sent_ = false;
  /** 今の 1 回で、証明書が正しくないと分かった（何度つないでも同じなので、つなぎ直さない） */
  bool certificateProblem_ = false;
};

}  // namespace kv::ai
