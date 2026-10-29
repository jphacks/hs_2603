#pragma once
// AI の「操作の案内」（show_guide）で示せる、画面の場所の一覧。
// AI に見せる説明（道具の説明に入れる）と、画面のどこを示すか（ui/guide.cpp が使う）を、ここ 1 か所で決める

#include <QString>
#include <functional>
#include <vector>

namespace kv::ai {

/** 場所の種類（画面のどこを探すか） */
enum class GuideKind {
  /** メニューの項目。ref は keymap の操作の ID か、QAction の objectName。メニューを順に開く案内から出す */
  Action,
  /** ウィンドウの中の部品。ref は objectName（いくつかあれば、見えている最初のもの） */
  Widget,
  /** 左右の欄のタブ。ref はビューの ID（media・audio・text・effects・properties） */
  View,
  /** タイムラインの中の場所。ref は TimelineView::guideRect の名前 */
  Timeline,
  /** 右の「プロパティ」の中。ref はまとまりの見出しの名前か、「#」に続けて部品の objectName */
  Property,
  /** ダイアログの中の部品。ref は objectName。ダイアログが開いていなければ、via の場所から案内する */
  Dialog,
};

/** その場所を使う前に要る状態（無ければ、先にその準備を案内する） */
enum class GuideNeed {
  None,
  /** 映像（動画・写真。メインのビデオか重ねる段のもの）を選んでいる */
  Visual,
  /** 動画（写真ではないもの）を選んでいる */
  Video,
  /** 写真（静止画のクリップ）を選んでいる */
  Photo,
  /** テキストを選んでいる */
  Text,
  /** オーディオを選んでいる */
  Audio,
  /** トランジションを選んでいる */
  Transition,
  /** フェードを付けられるもの（映像・オーディオ）を選んでいる */
  Fadable,
  /** 何か（トランジションのほか）を選んでいる */
  Item,
  /** 何も選んでいない */
  Nothing,
  /** タイムラインにクリップがある */
  Clips,
  /** ビデオにクリップが 2 つ以上ある（切れ目がある） */
  TwoClips,
  /** 切れ目にトランジションが付いている */
  HasTransition,
  /** メディアの一覧に動画・写真がある */
  Media,
  /** オーディオの一覧に音がある */
  AudioMedia,
  /** マーカーがある */
  Marker,
};

struct GuideTarget {
  QString id;
  /** AI に見せる短い説明（グループの説明に続けて読む） */
  QString label;
  /** 画面のカードと、道具の結果に出す場所の名前（例: 上のメニュー「クリップ」→「逆再生」） */
  QString place;
  GuideKind kind = GuideKind::Widget;
  QString ref;
  /** その部品があるビュー（空ならビューの外） */
  QString view;
  GuideNeed need = GuideNeed::None;
  /** Dialog のとき: ダイアログを開く場所（ほかの場所の ID） */
  QString via;
  /** ドラッグして使うときの行き先（タイムラインの場所の名前。空なら押すだけ） */
  QString drop;
};

/** 場所の ID の始まりごとの説明（AI に見せる一覧を短くするため、まとめて書く） */
struct GuideGroup {
  QString prefix;
  QString label;
};

const std::vector<GuideTarget>& guideTargets();
const std::vector<GuideGroup>& guideGroups();
/** ID の場所（無ければ nullptr） */
const GuideTarget* guideTarget(const QString& id);

/** 画面から見た、場所の今の様子（道具の結果で AI に知らせる） */
struct GuideInfo {
  /** その操作のショートカットキー（無ければ空） */
  QString keys;
  /** 今は案内できない理由（使えるなら空） */
  QString problem;
};
/** 画面の側（ui）が決める。決めていなければ、キー・使えない理由は知らせない */
void setGuideInfoProvider(std::function<GuideInfo(const QString& id)> provider);
GuideInfo guideInfo(const QString& id);

}  // namespace kv::ai
