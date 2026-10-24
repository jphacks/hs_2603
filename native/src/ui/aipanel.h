#pragma once
// AI アシスタントの欄（画面の右端）。依頼を入力すると、AI がタイムラインを編集したり、タイトル・テロップの案を出したりする。

#include <QElapsedTimer>
#include <QFrame>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <memory>
#include <vector>

#include "ai/assistant.h"
#include "ai/tools.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace kv {

namespace ai {
class Assistant;
}

/** 見比べて選ぶ案の欄 1 つ分の状態（aipanel.cpp） */
struct AiChoiceCard;

class AiPanel : public QWidget {
  Q_OBJECT
 public:
  explicit AiPanel(QWidget* parent = nullptr);
  void focusInput();
  /** 依頼を送る。shown を指定すると、会話の欄にはその短い文を出す。送れなければ false */
  bool ask(const QString& request, const QString& shown = QString());
  ai::Assistant* assistant() const { return assistant_; }

 signals:
  void closeRequested();

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override;
  void showEvent(QShowEvent* e) override;

 private:
  void sendInput();
  void addItem(QWidget* w);
  void addUser(const QString& text);
  void addReply(const QString& text);
  void addAction(const QString& text);
  void addError(const QString& text);
  void addIdeas(const QString& kind, const std::vector<ai::TextIdea>& ideas);
  /** 見比べて選ぶ案（プレビューつき）を出す */
  void addChoices(const ai::ChoiceSet& set);
  /** 案を大きく横に並べた画面を開く（そこからも選べる） */
  void showComparison(const std::shared_ptr<AiChoiceCard>& card);
  /** 案を選ぶ。ほかの案を選んでいたら、1 回の操作でその案から切り替える */
  void chooseOption(const std::shared_ptr<AiChoiceCard>& card, int index);
  /** 「これにする」ボタンの文字と押せるかを、今の状態に合わせる */
  void refreshChoices();
  /** 元に戻す・やり直しで、選んだ案が外れた・戻ったのを見分ける */
  void trackChoices();
  void setBusy(bool busy);
  /** ログインしていないときに、会話の代わりに出す案内（ログインの画面を開くボタンつき） */
  QWidget* buildGate();
  /** AI サーバーのアカウントに合わせて、案内・アカウントの欄・選べる深さを切り替える */
  void refreshAccount();
  /** 選べる深さ（アカウントの上限まで）と、今の選択 */
  void refreshEffortChoices();
  /** 1 回の依頼で使ったトークンと考える深さを、会話の欄に 1 行出す */
  void addUsage(const ai::Usage& turn, const QString& effort, bool automatic);
  /** この会話で使ったトークンの合計を出す */
  void showUsage(const ai::Usage& session);
  void refreshHints();
  void refreshStatus();
  void clearConversation();

  ai::Assistant* assistant_;
  QScrollArea* scroll_;
  QVBoxLayout* items_;
  QWidget* intro_;
  QLabel* introNote_;
  QLabel* status_;
  QString statusText_;
  QElapsedTimer busyClock_;
  QTimer statusTimer_;
  QPlainTextEdit* input_;
  QPushButton* send_;
  /** 考える深さ（自動・低・中・高・最大） */
  QComboBox* effort_;
  /** 深さの ID と説明（並べ直すときに使う） */
  std::vector<std::pair<QString, QString>> effortTips_;
  /** この会話で使ったトークンの合計 */
  QLabel* usage_;
  // AI サーバーのアカウント
  QWidget* accountBar_;
  QWidget* accountSeparator_;
  QLabel* accountName_;
  QLabel* accountQuota_;
  QWidget* gate_ = nullptr;
  QWidget* chat_;
  QLabel* gateTitle_ = nullptr;
  QLabel* gateText_ = nullptr;
  QPushButton* gateButton_ = nullptr;
  QPushButton* gateRetry_ = nullptr;
  /** 案内のボタンで開くページ（ログイン・パスワードの変更） */
  bool gatePassword_ = false;
  std::vector<QPushButton*> quick_;
  bool hasMessages_ = false;
  /** 出した案の欄（欄が消えると、自然に外れる） */
  std::vector<std::weak_ptr<AiChoiceCard>> choiceCards_;
  /** この回の依頼で出した案の欄。AI が同じ回で案を出し直したら、その場で置き換える */
  QPointer<QFrame> turnChoices_;
  /** 案を当てはめている間（その変更では、選んだ案の見分けをしない） */
  bool choosing_ = false;
};

}  // namespace kv
