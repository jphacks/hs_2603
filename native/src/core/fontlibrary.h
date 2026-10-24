#pragma once
// 文字に使えるフォントの一覧（組み込みの種類のほかに、アプリに同梱したフォントと、自分で読み込んだフォント）。
// 同梱のフォントはアプリのフォルダーの fonts、読み込んだフォントは AppLocalData の fonts にコピーして、起動時に登録する。
// 編集データにはファミリー名で保存する（TextItem::fontFamily）。

#include <QObject>
#include <QString>
#include <QStringList>
#include <map>
#include <vector>

namespace kv {

struct FontEntry {
  /** Qt のファミリー名（保存する値） */
  QString family;
  /** 一覧に出す名前 */
  QString label;
  bool bundled = false;
  /** 読み込んだフォントのファイル（AppLocalData の fonts の中）。同梱のフォントは空 */
  QString file;
};

class FontLibrary : public QObject {
  Q_OBJECT
 public:
  static FontLibrary& instance();

  /** 同梱のフォントと、読み込んだフォントを登録する（起動時に 1 回。2 回目からは何もしない） */
  void loadAll();
  const std::vector<FontEntry>& bundled() const { return bundled_; }
  const std::vector<FontEntry>& imported() const { return imported_; }
  /** 一覧に出す名前（同梱・読み込んだフォントでなければ、ファミリー名のまま） */
  QString labelOf(const QString& family) const;
  /** そのファミリーで描けるか（登録したフォント・パソコンに入っているフォント） */
  static bool available(const QString& family);
  /**
   * 太字で描くときに使うファミリー（同梱のフォントの太字のファイルが、別のファミリー名で登録されるとき）。無ければ空
   */
  QString boldFamily(const QString& family) const;

  /**
   * フォントのファイル（.ttf / .otf / .ttc / .otc）を読み込む。AppLocalData の fonts にコピーして登録し、次の起動からも使える。
   * 登録したファミリー名を返す（すでにあったものも含む）。読み込めなければ空で、error に理由を入れる
   */
  QStringList importFile(const QString& path, QString* error = nullptr);
  /** 読み込んだフォントを一覧から外す（コピーしたファイルも消す）。使っている文字は、組み込みの種類のフォントで描かれる */
  bool removeImported(const QString& family, QString* error = nullptr);

  /** 読み込めるフォントのファイルの種類（ファイルを選ぶ画面の絞り込み） */
  static QString fileFilter();

 signals:
  void changed();

 private:
  FontLibrary() = default;
  static QString importDir();
  struct Loaded {
    int id = -1;
    QString file;
    QStringList families;
  };
  std::vector<Loaded> importedFiles_;
  std::vector<FontEntry> bundled_;
  std::vector<FontEntry> imported_;
  std::map<QString, QString> boldAlias_;
  bool loaded_ = false;
  void rebuildImported();
};

inline FontLibrary& fontLibrary() {
  return FontLibrary::instance();
}

}  // namespace kv
