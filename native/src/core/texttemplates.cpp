#include "core/texttemplates.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <optional>

#include "core/projectio.h"
#include "core/timeline.h"

namespace kv {

static QString templatesPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("text-templates.json"));
}

/** テンプレートとして残す形にする（文字と、置いた場所・時間に関わるものは外す。長さと位置は、追加するときに使う） */
static TextItem templateStyle(const TextItem& item) {
  TextItem t = item;
  t.id.clear();
  t.text.clear();
  t.start = 0;
  t.track = 0;
  t.fontSpans.clear();
  t.group.clear();
  t.duration = std::max(kMinTextDuration, item.duration);
  return t;
}

TextTemplates& TextTemplates::instance() {
  static TextTemplates* t = new TextTemplates();
  return *t;
}

TextTemplates::TextTemplates() {
  load();
}

QString TextTemplates::cleanName(const QString& name) {
  const QString s = name.simplified().left(30);
  return s.isEmpty() ? QStringLiteral("マイテンプレート") : s;
}

const TextPreset* TextTemplates::find(const QString& id) const {
  if (!isUserId(id)) return nullptr;
  for (const auto& p : list_) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

void TextTemplates::load() {
  list_.clear();
  info_.clear();
  QFile f(templatesPath());
  if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
  for (const auto& v : root.value(QStringLiteral("templates")).toArray()) {
    const QJsonObject o = v.toObject();
    const QString id = o.value(QStringLiteral("id")).toString();
    if (!isUserId(id) || find(id)) continue;
    TextPreset p;
    p.id = id;
    p.label = cleanName(o.value(QStringLiteral("name")).toString());
    p.style = templateStyle(textFromJson(o.value(QStringLiteral("style")).toObject()));
    list_.push_back(p);
    // ワークショップとの関わり（以前の版で保存したファイルには無い）
    const QJsonObject w = o.value(QStringLiteral("workshop")).toObject();
    TemplateWorkshopInfo info;
    info.sourceId = w.value(QStringLiteral("sourceId")).toString();
    info.author = w.value(QStringLiteral("author")).toString().simplified().left(40);
    info.publishedId = w.value(QStringLiteral("publishedId")).toString();
    if (!info.empty()) info_[id] = info;
  }
}

bool TextTemplates::save() const {
  QJsonArray arr;
  for (const auto& p : list_) {
    QJsonObject o{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.label}, {QStringLiteral("style"), textToJson(p.style)}};
    if (const auto it = info_.find(p.id); it != info_.end()) {
      QJsonObject w;
      if (!it->second.sourceId.isEmpty()) w[QStringLiteral("sourceId")] = it->second.sourceId;
      if (!it->second.author.isEmpty()) w[QStringLiteral("author")] = it->second.author;
      if (!it->second.publishedId.isEmpty()) w[QStringLiteral("publishedId")] = it->second.publishedId;
      o[QStringLiteral("workshop")] = w;
    }
    arr.append(o);
  }
  const QJsonObject root{{QStringLiteral("app"), QStringLiteral("kantan-video-editor")}, {QStringLiteral("version"), 1}, {QStringLiteral("templates"), arr}};
  const QString path = templatesPath();
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  return f.commit();
}

QString TextTemplates::add(const QString& name, const TextItem& item) {
  TextPreset p;
  p.id = QStringLiteral("user:") + newId();
  p.label = cleanName(name);
  p.style = templateStyle(item);
  list_.push_back(p);
  if (!save()) {
    list_.pop_back();
    return {};
  }
  emit changed();
  return p.id;
}

bool TextTemplates::rename(const QString& id, const QString& name) {
  for (auto& p : list_) {
    if (p.id != id) continue;
    const QString before = p.label;
    p.label = cleanName(name);
    if (p.label == before) return true;
    if (!save()) {
      p.label = before;
      return false;
    }
    emit changed();
    return true;
  }
  return false;
}

bool TextTemplates::remove(const QString& id) {
  auto it = std::find_if(list_.begin(), list_.end(), [&](const TextPreset& p) { return p.id == id; });
  if (it == list_.end()) return false;
  const TextPreset removed = *it;
  const auto pos = it - list_.begin();
  list_.erase(it);
  const auto info = info_.find(id);
  std::optional<TemplateWorkshopInfo> removedInfo;
  if (info != info_.end()) {
    removedInfo = info->second;
    info_.erase(info);
  }
  if (!save()) {
    list_.insert(list_.begin() + pos, removed);
    if (removedInfo) info_[id] = *removedInfo;
    return false;
  }
  emit changed();
  return true;
}

// ---- ワークショップ ------------------------------------------------------------------------

TemplateWorkshopInfo TextTemplates::workshopInfo(const QString& id) const {
  const auto it = info_.find(id);
  return it == info_.end() ? TemplateWorkshopInfo() : it->second;
}

QString TextTemplates::findBySource(const QString& sourceId) const {
  if (sourceId.isEmpty()) return {};
  for (const auto& [id, info] : info_) {
    if (info.sourceId == sourceId && find(id)) return id;
  }
  return {};
}

QString TextTemplates::addFromWorkshop(const QString& name, const TextItem& style, const QString& sourceId, const QString& author) {
  if (const QString existing = findBySource(sourceId); !existing.isEmpty()) return existing;
  const QString id = add(name, style);
  if (id.isEmpty()) return {};
  TemplateWorkshopInfo info;
  info.sourceId = sourceId;
  info.author = author.simplified().left(40);
  info_[id] = info;
  if (!save()) {
    // 追加はできたので、関わりだけ覚えられなかった（次に追加すると、もう 1 つ増える）
    info_.erase(id);
  }
  emit changed();
  return id;
}

bool TextTemplates::setPublished(const QString& id, const QString& publishedId) {
  if (!find(id)) return false;
  TemplateWorkshopInfo info = workshopInfo(id);
  const TemplateWorkshopInfo before = info;
  info.publishedId = publishedId;
  if (info.publishedId == before.publishedId) return true;
  if (info.empty()) info_.erase(id);
  else info_[id] = info;
  if (!save()) {
    if (before.empty()) info_.erase(id);
    else info_[id] = before;
    return false;
  }
  emit changed();
  return true;
}

void TextTemplates::clearPublished(const QString& publishedId) {
  if (publishedId.isEmpty()) return;
  for (const auto& [id, info] : std::map<QString, TemplateWorkshopInfo>(info_)) {
    if (info.publishedId == publishedId) setPublished(id, QString());
  }
}

QJsonObject TextTemplates::styleToJson(const TextItem& item) {
  QJsonObject o = textToJson(templateStyle(item));
  // 文字と、置いた場所に関わるものは送らない（追加したときの位置 x, y と長さ duration は、テンプレートの一部として送る）
  for (const char* key : {"id", "text", "start", "track", "group", "fontSpans"}) o.remove(QLatin1String(key));
  return o;
}

TextItem TextTemplates::styleFromJson(const QJsonObject& o) {
  TextItem t = templateStyle(textFromJson(o));
  // 読み込みで大きさ・色などは範囲に収まる。位置・長さ・キーフレームの数も、ここで収める
  t.x = clampProp(AnimProp::X, t.x);
  t.y = clampProp(AnimProp::Y, t.y);
  t.duration = std::clamp(t.duration, kMinTextDuration, 600.0);
  t.fontFamily = t.fontFamily.simplified().left(100);
  constexpr size_t kMaxKeys = 100;
  for (auto& keys : t.anim.tracks) {
    if (keys.size() > kMaxKeys) keys.resize(kMaxKeys);
  }
  return t;
}

}  // namespace kv
