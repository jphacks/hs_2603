#pragma once
// 自分で保存したテキストのテンプレート（フォント・色・縁取りなどの見た目、登場のアニメーション、キーフレーム）。
// 文字そのものは保存しない。アプリの設定と同じ場所（AppLocalData）の text-templates.json に書き、どのプロジェクトでも使える。

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <map>
#include <vector>

#include "core/textstyles.h"

namespace kv {

/** テンプレートとワークショップ（みんなでテンプレートを公開して使う場所）との関わり。どれも無ければ空 */
struct TemplateWorkshopInfo {
  /** ワークショップから追加したときの、元のテンプレートの ID と、作った人の名前 */
  QString sourceId;
  QString author;
  /** 自分がワークショップに公開したときの、公開したテンプレートの ID */
  QString publishedId;
  bool empty() const { return sourceId.isEmpty() && author.isEmpty() && publishedId.isEmpty(); }
};

class TextTemplates : public QObject {
  Q_OBJECT
 public:
  static TextTemplates& instance();

  /** 保存した順。ID は "user:..."、style は保存したときのテキスト（文字・開始・段・一部のフォント・グループは空） */
  const std::vector<TextPreset>& list() const { return list_; }
  const TextPreset* find(const QString& id) const;
  static bool isUserId(const QString& id) { return id.startsWith(QStringLiteral("user:")); }
  /** item の見た目・登場のアニメーション・キーフレーム・長さ・位置を、name という名前で保存する。ID を返す（保存できなければ空） */
  QString add(const QString& name, const TextItem& item);
  bool rename(const QString& id, const QString& name);
  bool remove(const QString& id);
  /** 名前を整える（前後の空白を除き、長すぎる名前は切る。空なら「マイテンプレート」） */
  static QString cleanName(const QString& name);

  // ---- ワークショップ ----
  TemplateWorkshopInfo workshopInfo(const QString& id) const;
  /**
   * ワークショップのテンプレートを、マイテンプレートに追加する。同じテンプレートを追加済みなら、追加せずにその ID を返す。
   * 保存できなければ空
   */
  QString addFromWorkshop(const QString& name, const TextItem& style, const QString& sourceId, const QString& author);
  /** ワークショップの sourceId のテンプレートを追加済みなら、その ID（無ければ空） */
  QString findBySource(const QString& sourceId) const;
  /** 自分が公開したことを覚える（publishedId が空なら、公開の印を外す） */
  bool setPublished(const QString& id, const QString& publishedId);
  /** ワークショップの publishedId を公開していたテンプレートから、公開の印を外す（公開をやめたとき） */
  void clearPublished(const QString& publishedId);
  /** 見た目を、ワークショップに送る形の JSON にする（文字・開始・段・グループ・一部のフォントは入れない） */
  static QJsonObject styleToJson(const TextItem& item);
  /** ワークショップから受け取った見た目（ほかの人が作ったものなので、値の範囲やキーフレームの数を確かめる） */
  static TextItem styleFromJson(const QJsonObject& o);

 signals:
  void changed();

 private:
  TextTemplates();
  void load();
  bool save() const;
  std::vector<TextPreset> list_;
  /** テンプレートの ID → ワークショップとの関わり（関わりの無いものは入れない） */
  std::map<QString, TemplateWorkshopInfo> info_;
};

inline TextTemplates& textTemplates() {
  return TextTemplates::instance();
}

}  // namespace kv
