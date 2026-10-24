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

#include "ai/workshop.h"
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
  /**
   * propose_audio の候補のときの、ワークショップのオーディオ（ChoiceSet::audio のときだけ）。
   * label はその名前（ほかの人が付けたものなので、画面に出すだけで、AI への知らせには入れない）
   */
  WorkshopTemplate audio;
};

/** propose_choices・propose_audio で見せる、選ぶ案のまとまり */
struct ChoiceSet {
  QString id;
  /** ユーザーへの問いかけ（空なら決まり文句） */
  QString question;
  /** プレビューの時間（秒）。どの案も同じ時間の画面を見せる */
  std::vector<double> times;
  std::vector<Choice> choices;
  /** 案を作った編集場所（ほかのタブには当てはめない） */
  int workspaceUid = -1;
  /**
   * ワークショップのオーディオの候補（propose_audio）。プレビューの画像の代わりに試聴して選ぶ。
   * 選ぶと、ファイルを手元に置いてメディア一覧に入れてから、案の編集（add_workshop_audio）を当てはめる
   */
  bool audio = false;
};

/** 案を、今の編集場所に当てはめた結果 */
struct ChoiceApplied {
  bool ok = false;
  /** 当てはめられなかった理由 */
  QString error;
  /** 何をしたか（1 行ずつ） */
  QStringList log;
  /** 変えたテキスト（なければクリップ、オーディオ）の ID。選んで見せるのに使う */
  QString focusText;
  QString focusClip;
  QString focusAudio;
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
  /** propose_choices・propose_audio の案 */
  bool hasChoices = false;
  ChoiceSet choiceSet;
  /** この編集で使ったワークショップのもの（ID。本当に当てはめたときに、使った人の数を増やす） */
  QStringList workshopUsed;
};

/** Converse API の toolConfig.tools */
const QJsonArray& toolSpecs();
/** 今の編集場所の状態（AI に渡す） */
QJsonObject describeProject();
/**
 * AI に渡す JSON の文字列。文字の中の < > は \u003c \u003e にする（JSON としては同じ値）。
 * 素材のファイル名やテキストの文字が </project> などの区切りに見えて、AI への指示のように読まれるのを防ぐ
 */
QString promptJson(const QJsonObject& o);
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
/** AI に見せるワークショップのものの ID（"workshop:12"） */
QString workshopRef(const QString& id);
/** ワークショップのオーディオ id を手元に置いたファイルが、今の編集場所のメディア一覧にあれば、その素材の ID（無ければ空） */
QString workshopAudioAsset(const QString& id);

}  // namespace kv::ai
