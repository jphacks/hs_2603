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
  }
}

bool TextTemplates::save() const {
  QJsonArray arr;
  for (const auto& p : list_) {
    arr.append(QJsonObject{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.label}, {QStringLiteral("style"), textToJson(p.style)}});
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
  if (!save()) {
    list_.insert(list_.begin() + pos, removed);
    return false;
  }
  emit changed();
  return true;
}

}  // namespace kv
