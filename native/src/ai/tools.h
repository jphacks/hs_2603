#pragma once
// AI が使う道具（Bedrock の tool use）。編集はすべて Editor::commit を通すので、元に戻す・やり直しが効く。

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
#include <vector>

#include "core/model.h"

namespace kv::ai {

/** AI が出したテキストの案（propose_texts）。選ばれるまでタイムラインには入れない */
struct TextIdea {
  QString text;
  std::optional<double> start;
  std::optional<double> duration;
  /** スタイルの ID（basic, title …）。空ならスタイルを決めていない */
  QString style;
  /** top / center / bottom。空ならスタイルの標準の位置 */
  QString position;
};

/** propose_choices の案 1 つ。選ばれるまでタイムラインには入れない */
struct Choice {
  /** 案の短い名前（A・B などの記号は付けない。画面ではアプリが付ける） */
  QString label;
  /** どんな案か（1 文。空でもよい） */
  QString description;
  /** この案で行う編集（{"tool": 道具の名前, "input": 入力} の並び）。選ばれたら、その時点の状態にもう一度当てはめる */
  QJsonArray actions;
  /** 案を当てはめた画面の JPEG（ChoiceSet::times と同じ順） */
  std::vector<QByteArray> previews;
  /** 何をするか（1 行ずつ） */
  QStringList log;
};

/** propose_choices で見せる、見比べて選ぶ案のまとまり */
struct ChoiceSet {
  QString id;
  /** ユーザーへの問いかけ（空なら決まり文句） */
  QString question;
  /** プレビューの時間（秒）。どの案も同じ時間の画面を見せる */
  std::vector<double> times;
  std::vector<Choice> choices;
  /** 案を作った編集場所（ほかのタブには当てはめない） */
  int workspaceUid = -1;
};

/** 案を、今の編集場所に当てはめた結果 */
struct ChoiceApplied {
  bool ok = false;
  /** 当てはめられなかった理由 */
  QString error;
  /** 何をしたか（1 行ずつ） */
  QStringList log;
  /** 変えたテキスト（なければクリップ）の ID。選んで見せるのに使う */
  QString focusText;
  QString focusClip;
};

struct ToolResult {
  /** Bedrock に返す toolResult の content（text のブロック） */
  QJsonArray content;
  /**
   * toolResult の外（同じメッセージの、toolResult の後ろ）に付けるブロック。画像はここに入れる。
   * toolResult の中の画像を受け付けないモデル（OpenAI の GPT など）があるため
   */
  QJsonArray attachments;
  bool error = false;
  /** error のときの理由（content にも同じものが入っている） */
  QString message;
  /** 画面に出す「何をしたか」（1 行ずつ） */
  QStringList log;
  /** propose_texts の案 */
  bool hasIdeas = false;
  QString ideasKind;  // title / telop
  std::vector<TextIdea> ideas;
  /** propose_choices の案 */
  bool hasChoices = false;
  ChoiceSet choiceSet;
};

/** Converse API の toolConfig.tools */
const QJsonArray& toolSpecs();
/** 今の編集場所の状態（AI に渡す） */
QJsonObject describeProject();
/** 道具を使う。編集は merge をつけて記録する（1 回の依頼の変更を、まとめて元に戻せる）。done は context のスレッドで呼ぶ */
void runTool(const QString& name, const QJsonObject& input, const QString& merge, QObject* context, std::function<void(ToolResult)> done);
/** 案をタイムラインに追加する（1 回の「元に戻す」で消える）。追加したテキストの ID を、案と同じ順で返す */
QStringList addIdeas(const std::vector<TextIdea>& ideas, const QString& kind);
/**
 * 見比べて選ぶ案を、今の編集場所に当てはめる（merge をつけて 1 回の操作として記録するので、1 回の「元に戻す」で戻せる）。
 * 案を作ったあとにタイムラインが変わっていても、今の状態に当てはめ直す。start を渡すと、今の状態ではなく
 * start に当てはめた結果にする（別の案に切り替えるとき、前の案を当てはめる前の状態を渡す）。
 * 当てはめられないときは何も変えない
 */
ChoiceApplied applyChoice(const Choice& choice, const QString& merge, const ProjectData* start = nullptr);
/** 案の記号（0 → A） */
QString choiceLetter(int index);

}  // namespace kv::ai
