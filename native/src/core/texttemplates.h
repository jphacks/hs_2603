#pragma once
// 自分で保存したテキストのテンプレート（フォント・色・縁取りなどの見た目、登場のアニメーション、キーフレーム）。
// 文字そのものは保存しない。アプリの設定と同じ場所（AppLocalData）の text-templates.json に書き、どのプロジェクトでも使える。

#include <QObject>
#include <QString>
#include <vector>

#include "core/textstyles.h"

namespace kv {

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

 signals:
  void changed();

 private:
  TextTemplates();
  void load();
  bool save() const;
  std::vector<TextPreset> list_;
};

inline TextTemplates& textTemplates() {
  return TextTemplates::instance();
}

}  // namespace kv
