#include "core/fontlibrary.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QStandardPaths>
#include <algorithm>
#include <map>

namespace kv {

namespace {
/** 同梱のフォント（ファイル名の始まり → 一覧に出す名前）。一覧はこの順に並べる */
struct BundledName {
  const char* filePrefix;
  const char* label;
};
const BundledName kBundled[] = {
    {"NotoSansJP", "Noto Sans JP（ゴシック）"},
    {"MPLUSRounded1c", "M PLUS Rounded 1c（丸ゴシック）"},
    {"ZenMaruGothic", "Zen丸ゴシック"},
    {"KosugiMaru", "小杉丸ゴシック"},
    {"DelaGothicOne", "デラゴシック（極太）"},
    {"RocknRollOne", "ロックンロール"},
    {"MochiyPopOne", "モッチーポップ"},
    {"ReggaeOne", "レゲエ"},
    {"RampartOne", "ランパート（立体）"},
    {"YuseiMagic", "油性マジック"},
};

const QStringList kFontSuffixes = {QStringLiteral("ttf"), QStringLiteral("otf"), QStringLiteral("ttc"), QStringLiteral("otc")};

QStringList fontFilesIn(const QString& dir) {
  QStringList out;
  const QFileInfoList list = QDir(dir).entryInfoList(QDir::Files, QDir::Name);
  for (const QFileInfo& fi : list) {
    if (kFontSuffixes.contains(fi.suffix().toLower())) out << fi.absoluteFilePath();
  }
  return out;
}

int bundledOrder(const QString& fileName) {
  for (int i = 0; i < int(std::size(kBundled)); ++i) {
    if (fileName.startsWith(QLatin1String(kBundled[i].filePrefix), Qt::CaseInsensitive)) return i;
  }
  return int(std::size(kBundled));
}
}  // namespace

FontLibrary& FontLibrary::instance() {
  static FontLibrary* lib = new FontLibrary();
  return *lib;
}

QString FontLibrary::importDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("fonts"));
}

QString FontLibrary::fileFilter() {
  return QStringLiteral("フォント (*.ttf *.otf *.ttc *.otc);;すべてのファイル (*)");
}

bool FontLibrary::available(const QString& family) {
  return !family.isEmpty() && QFontDatabase::hasFamily(family);
}

QString FontLibrary::boldFamily(const QString& family) const {
  auto it = boldAlias_.find(family);
  return it == boldAlias_.end() ? QString() : it->second;
}

QString FontLibrary::labelOf(const QString& family) const {
  for (const auto& e : bundled_) {
    if (e.family == family) return e.label;
  }
  for (const auto& e : imported_) {
    if (e.family == family) return e.label;
  }
  return family;
}

void FontLibrary::loadAll() {
  if (loaded_) return;
  loaded_ = true;
  // 同梱のフォント: アプリのフォルダーの fonts（開発中の確認用に、環境変数 KANTAN_FONT_DIR でも指定できる）
  QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/fonts");
  if (const QString env = qEnvironmentVariable("KANTAN_FONT_DIR"); !env.isEmpty() && QFileInfo(env).isDir()) dir = env;
  QStringList files = fontFilesIn(dir);
  std::stable_sort(files.begin(), files.end(), [](const QString& a, const QString& b) {
    return bundledOrder(QFileInfo(a).fileName()) < bundledOrder(QFileInfo(b).fileName());
  });
  // 同じフォントの太さ違いのファイル（-Regular と -Bold）は、一覧には 1 つだけ出す。太字のファイルが別のファミリー名で
  // 登録される（古い名前の付け方のフォント）ときは、太字で描くときにそのファミリーを使う（boldFamily）
  std::map<int, QString> primary;    // 同梱の表の番号 → 一覧に出すファミリー
  std::map<int, QString> boldNames;  // 同梱の表の番号 → 太字のファイルのファミリー
  for (const QString& f : files) {
    const int id = QFontDatabase::addApplicationFont(f);
    if (id < 0) continue;
    const QString name = QFileInfo(f).fileName();
    const int order = bundledOrder(name);
    const QStringList fams = QFontDatabase::applicationFontFamilies(id);
    if (fams.isEmpty()) continue;
    if (order >= int(std::size(kBundled))) {
      // 表に無い同梱のフォント: ファミリー名のまま出す
      for (const QString& fam : fams) {
        if (std::none_of(bundled_.begin(), bundled_.end(), [&](const FontEntry& e) { return e.family == fam; })) bundled_.push_back({fam, fam, true, QString()});
      }
      continue;
    }
    if (name.contains(QStringLiteral("-Bold"), Qt::CaseInsensitive)) boldNames[order] = fams.front();
    if (name.contains(QStringLiteral("-Regular"), Qt::CaseInsensitive) || !primary.count(order)) primary[order] = fams.front();
  }
  for (const auto& [order, fam] : primary) {
    const QString label = QString::fromUtf8(kBundled[order].label);
    bundled_.push_back({fam, label, true, QString()});
    if (auto b = boldNames.find(order); b != boldNames.end() && b->second != fam) boldAlias_[fam] = b->second;
  }
  // 表の順に並べる（表に無いものは後ろ）
  auto rank = [&](const FontEntry& e) {
    for (const auto& [order, fam] : primary) {
      if (fam == e.family) return order;
    }
    return int(std::size(kBundled));
  };
  std::stable_sort(bundled_.begin(), bundled_.end(), [&](const FontEntry& a, const FontEntry& b) { return rank(a) < rank(b); });
  // 自分で読み込んだフォント
  for (const QString& f : fontFilesIn(importDir())) {
    const int id = QFontDatabase::addApplicationFont(f);
    if (id < 0) continue;
    importedFiles_.push_back({id, f, QFontDatabase::applicationFontFamilies(id)});
  }
  rebuildImported();
}

void FontLibrary::rebuildImported() {
  imported_.clear();
  for (const auto& l : importedFiles_) {
    for (const QString& fam : l.families) {
      const bool dup = std::any_of(bundled_.begin(), bundled_.end(), [&](const FontEntry& e) { return e.family == fam; }) ||
                       std::any_of(imported_.begin(), imported_.end(), [&](const FontEntry& e) { return e.family == fam; });
      if (!dup) imported_.push_back({fam, fam, false, l.file});
    }
  }
  std::stable_sort(imported_.begin(), imported_.end(), [](const FontEntry& a, const FontEntry& b) {
    return QString::localeAwareCompare(a.label, b.label) < 0;
  });
}

QStringList FontLibrary::importFile(const QString& path, QString* error) {
  loadAll();
  const QFileInfo src(path);
  if (!src.isFile()) {
    if (error) *error = QStringLiteral("ファイルが見つかりません");
    return {};
  }
  if (!kFontSuffixes.contains(src.suffix().toLower())) {
    if (error) *error = QStringLiteral("読み込めるのは .ttf / .otf / .ttc / .otc のフォントです");
    return {};
  }
  const QString dir = importDir();
  // 同じファイル（名前と大きさが同じ）をすでに読み込んでいれば、それを使う
  for (const auto& l : importedFiles_) {
    const QFileInfo have(l.file);
    if (have.fileName().compare(src.fileName(), Qt::CaseInsensitive) == 0 && have.size() == src.size()) return l.families;
  }
  if (!QDir().mkpath(dir)) {
    if (error) *error = QStringLiteral("フォントを入れるフォルダーを作れません");
    return {};
  }
  // 名前が重なるときは、番号を付けて別のファイルにする
  QString target = QDir(dir).filePath(src.fileName());
  for (int n = 2; QFileInfo::exists(target); ++n) {
    target = QDir(dir).filePath(QStringLiteral("%1-%2.%3").arg(src.completeBaseName()).arg(n).arg(src.suffix()));
  }
  if (!QFile::copy(src.absoluteFilePath(), target)) {
    if (error) *error = QStringLiteral("フォントをコピーできませんでした");
    return {};
  }
  QFile::setPermissions(target, QFile::permissions(target) | QFileDevice::WriteOwner);
  const int id = QFontDatabase::addApplicationFont(target);
  const QStringList families = id >= 0 ? QFontDatabase::applicationFontFamilies(id) : QStringList();
  if (families.isEmpty()) {
    if (id >= 0) QFontDatabase::removeApplicationFont(id);
    QFile::remove(target);
    if (error) *error = QStringLiteral("フォントとして読み込めませんでした（壊れているか、対応していない形式です）");
    return {};
  }
  importedFiles_.push_back({id, target, families});
  rebuildImported();
  emit changed();
  return families;
}

bool FontLibrary::removeImported(const QString& family, QString* error) {
  for (auto it = importedFiles_.begin(); it != importedFiles_.end(); ++it) {
    if (!it->families.contains(family)) continue;
    QFontDatabase::removeApplicationFont(it->id);
    if (!QFile::remove(it->file) && QFileInfo::exists(it->file)) {
      // 消せなかったとき（ほかのアプリが開いている等）は、登録だけ戻す
      it->id = QFontDatabase::addApplicationFont(it->file);
      if (error) *error = QStringLiteral("フォントのファイルを消せませんでした");
      return false;
    }
    importedFiles_.erase(it);
    rebuildImported();
    emit changed();
    return true;
  }
  if (error) *error = QStringLiteral("読み込んだフォントではありません");
  return false;
}

}  // namespace kv
