#pragma once
// 自分で保存したテキストのアニメーションのテンプレート（キーフレームと、登場のアニメーション）。
// 文字そのもの・フォントや色などの見た目は保存しない（それはテキストのテンプレート）。アプリの設定と同じ場所（AppLocalData）の
// anim-templates.json に書き、どのプロジェクトのどのテキストにも使える。

#include <QObject>
#include <QString>
#include <vector>

#include "core/model.h"

namespace kv {

struct AnimTemplate {
  /** "anim:..." */
  QString id;
  QString name;
  /** キーフレーム（位置・拡大率・回転・不透明度・字間と、それぞれの補間方式）。時間はテキストの始まりから。値は保存したテキストのまま */
  Animation anim;
  /** 保存したテキストの位置。位置のキーフレームは、使うテキストの位置との差だけずらして当てはめる（使うテキストの位置を基準に動く） */
  double baseX = 0.5;
  double baseY = 0.82;
  /** 保存したテキストの長さ（長さの違うテキストに使うとき、キーフレームの時間を合わせるのに使う） */
  double duration = 3;
  /** 登場のアニメーション（テキストの設定の「登場」） */
  TextAnimation entrance = TextAnimation::Fade;
};

class AnimTemplates : public QObject {
  Q_OBJECT
 public:
  static AnimTemplates& instance();

  /** 保存した順 */
  const std::vector<AnimTemplate>& list() const { return list_; }
  const AnimTemplate* find(const QString& id) const;
  static bool isAnimId(const QString& id) { return id.startsWith(QStringLiteral("anim:")); }
  /** テキストのアニメーションを、name という名前で保存する。ID を返す（保存できなければ空） */
  QString add(const QString& name, const TextItem& item);
  bool rename(const QString& id, const QString& name);
  bool remove(const QString& id);
  /** 名前を整える（前後の空白を除き、長すぎる名前は切る。空なら「マイアニメーション」） */
  static QString cleanName(const QString& name);

  /** テンプレートに保存できるアニメーション（キーフレーム）があるか */
  static bool hasAnimation(const TextItem& item) { return !item.anim.empty(); }
  /** テキストのアニメーションを、テンプレートの形にする（名前と ID は空） */
  static AnimTemplate capture(const TextItem& item);
  /**
   * テンプレートのアニメーションを、テキストに当てはめる（キーフレームと登場のアニメーションを置き換える。文字・見た目は変えない）。
   * 位置は、このテキストの位置を基準にする。長さが違うときは、保存したテキストより長ければ、前半のキーフレームは始まりから、
   * 後半は終わりからの時間をそのままにする（入り・出の速さが変わらない）。短ければ、全体を縮める
   */
  static void applyTo(const AnimTemplate& t, TextItem& item);

 signals:
  void changed();

 private:
  AnimTemplates();
  void load();
  bool save() const;
  std::vector<AnimTemplate> list_;
};

inline AnimTemplates& animTemplates() {
  return AnimTemplates::instance();
}

}  // namespace kv
