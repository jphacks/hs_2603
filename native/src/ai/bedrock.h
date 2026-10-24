#pragma once
// AI サーバー（server フォルダー）を通して、Amazon Bedrock の Converse API を呼ぶ。
// サーバーがモデル・返事の長さの上限・考える深さを決めて Bedrock に送り、使ったトークンを記録する。

#include <QJsonObject>
#include <QObject>
#include <QPointer>

#include "ai/aiconfig.h"

namespace kv::ai {

class ServerCall;

class BedrockClient : public QObject {
  Q_OBJECT
 public:
  explicit BedrockClient(QObject* parent = nullptr);
  ~BedrockClient() override;

  /** body は messages・system・toolConfig を含む Converse API のリクエスト。前の呼び出しが残っていれば取り消す */
  void converse(const Config& config, const QJsonObject& body);
  /** 取り消した呼び出しの結果は出さない */
  void cancel();
  bool busy() const { return !call_.isNull(); }

 signals:
  void finished(const QJsonObject& response);
  /** 画面にそのまま出せる説明 */
  void failed(const QString& message);
  /** サーバーが実際に使った深さ（モデルの標準なら空）と、今月の使用量 */
  void serverMeta(const QString& effort, const QJsonObject& usage);
  /** ログインの期限が切れた・アカウントが止められた */
  void sessionExpired(const QString& message);
  /** 上限・AI を使えるか・パスワードの変更など、アカウントの状態が変わったかもしれない（読み直す合図） */
  void accountChanged();
  /** AI サーバーにつながらなかった（つながりを確かめ直す合図） */
  void unreachable();

 private:
  /** 送っている途中の依頼（つながりの確認・ログインと同じ接続を使い、すぐに切られたときは自動でつなぎ直す） */
  QPointer<ServerCall> call_;
};

}  // namespace kv::ai
