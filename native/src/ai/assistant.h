#pragma once
// AI アシスタントとの会話。依頼を AI サーバー経由で Bedrock に送り、AI が道具（tools.h）を使うたびに実行して結果を返す。
// 1 回の依頼で行った変更は、1 回の「元に戻す」でまとめて戻せる。

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <map>
#include <utility>
#include <vector>

#include "ai/aiconfig.h"
#include "ai/tools.h"

namespace kv::ai {

class BedrockClient;

/** 使ったトークン（Bedrock の返事の usage を足したもの。出力には、考えた分も入る） */
struct Usage {
  /** 入力（キャッシュから読んだ分・キャッシュに書いた分も含む） */
  qint64 input = 0;
  /** 入力のうち、キャッシュから読んだ・キャッシュに書いた分（GPT は自動でキャッシュする） */
  qint64 cached = 0;
  qint64 output = 0;
  /** AI に送った回数（1 回の依頼でも、道具を使うたびに増える） */
  int calls = 0;
  void add(const Usage& u) {
    input += u.input;
    cached += u.cached;
    output += u.output;
    calls += u.calls;
  }
};

class Assistant : public QObject {
  Q_OBJECT
 public:
  explicit Assistant(QObject* parent = nullptr);

  /** 考える深さ（auto / low / medium / high / max）。次の依頼から使う */
  void setEffort(const QString& choice);
  QString effort() const { return effort_; }
  /** auto のときに、依頼の文から決める深さ（low か medium） */
  static QString autoEffort(const QString& request);
  /** この会話で使ったトークン（「新しい会話」で 0 に戻る） */
  const Usage& sessionUsage() const { return session_; }

  /**
   * 1 回の依頼の文の長さの上限（文字数。プロジェクトの状態や道具の結果は数えない）。編集の依頼には十分な長さにして、
   * 動画と関係のない長い文書を貼り付けて頼む（要約・翻訳 など）のを、アプリの中で止める。
   * アプリの中だけの上限で、AI サーバーでは数えない（API を直接呼ぶ人には、AI サーバーの決まりとトークンの上限が効く）
   */
  static constexpr int kMaxRequestChars = 2000;
  /** 送れない依頼なら、その理由（長すぎる など）。送れるなら空 */
  static QString requestProblem(const QString& text);

  bool busy() const { return busy_; }
  /** 依頼を送る。送れないとき（返事を待っている、ログインしていない、長すぎる）は false */
  bool send(const QString& text);
  /** 返事を待つのをやめる。それまでに行った変更は残る（元に戻すで戻せる） */
  void cancel();
  /** 会話を最初からにする */
  void reset();
  /**
   * 見比べて選ぶ案（setId のまとまり）から、ユーザーが choice（例: A「ポップに」）を選んだことを AI に知らせる。
   * 案を出した道具の結果に書き足し、次の依頼のはじめにも書く。選び直したときは、最後に選んだものだけを残す
   */
  void noteChoice(const QString& setId, const QString& choice);

 signals:
  void busyChanged(bool busy);
  /** 今していること（空なら何もしていない） */
  void statusChanged(const QString& status);
  void replied(const QString& text);
  /** AI が行った変更（1 行） */
  void acted(const QString& summary);
  void proposed(const QString& kind, const std::vector<kv::ai::TextIdea>& ideas);
  /** 見比べて選ぶ案（プレビューつき）を出した */
  void choicesProposed(const kv::ai::ChoiceSet& set);
  /** 操作の案内（show_guide）を出した。画面で、手順を順に示す */
  void guideProposed(const std::vector<kv::ai::GuideStep>& steps);
  void failed(const QString& message);
  /** この会話で使ったトークンが変わった（AI の返事を受け取るたび。「新しい会話」でも） */
  void usageChanged(const kv::ai::Usage& session);
  /**
   * 1 回の依頼が終わった（返事・失敗・中止）。その依頼で使ったトークンと、使った深さ
   * （effort は low など。モデルが深さに対応していなければ空。automatic は「自動」で決めたか）
   */
  void turnFinished(const kv::ai::Usage& turn, const QString& effort, bool automatic);

 private:
  void request();
  void onResponse(const QJsonObject& response);
  void onFailed(const QString& message);
  void runNextTool();
  void endTurn();
  /** この回の依頼を、会話の記録から取り除く（失敗・中止のとき。記録が AI に受け付けられない形で残らないように） */
  void rollbackTurn();
  QJsonArray requestMessages() const;
  void setStatus(const QString& status);

  BedrockClient* client_;
  Config config_;
  QJsonArray messages_;
  int turnStart_ = 0;
  QString merge_;
  int rounds_ = 0;
  int workspaceUid_ = -1;
  bool busy_ = false;
  int generation_ = 0;
  QJsonArray pendingUses_;
  int pendingIndex_ = 0;
  QJsonArray pendingResults_;
  /** 道具の結果の後ろに付ける画像など（toolResult の外に置く） */
  QJsonArray pendingAttachments_;
  /** 案のまとまりの ID → その案を出した道具の呼び出しの ID（選ばれたことを、その道具の結果に書き足すため） */
  std::map<QString, QString> choiceToolUses_;
  /** まだ AI に知らせていない「選んだ案」（まとまりの ID と、知らせる文） */
  std::vector<std::pair<QString, QString>> choiceNotes_;
  /** この回の依頼で知らせた「選んだ案」（依頼が失敗・中止したら、次の依頼で知らせ直す） */
  std::vector<std::pair<QString, QString>> sentNotes_;
  /** 選んでいる考える深さ（auto など） */
  QString effort_ = QStringLiteral("auto");
  /** この回の依頼で使う深さ（モデルが対応していなければ空）と、「自動」で決めたか */
  QString turnEffort_;
  bool turnAuto_ = false;
  Usage turnUsage_;
  Usage session_;
};

}  // namespace kv::ai
