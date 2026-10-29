#include "core/animtemplates.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

#include "core/projectio.h"
#include "core/timeline.h"

namespace kv {

static QString templatesPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("anim-templates.json"));
}

/** 1 つの項目に持てるキーフレームの数（壊れたファイル・手で書き換えたファイルでも、重くならないように） */
static constexpr size_t kMaxKeysPerProp = 200;
static constexpr double kMaxTemplateDuration = 600;

AnimTemplates& AnimTemplates::instance() {
  static AnimTemplates* t = new AnimTemplates();
  return *t;
}

AnimTemplates::AnimTemplates() {
  load();
}

QString AnimTemplates::cleanName(const QString& name) {
  const QString s = name.simplified().left(30);
  return s.isEmpty() ? QStringLiteral("マイアニメーション") : s;
}

const AnimTemplate* AnimTemplates::find(const QString& id) const {
  for (const auto& t : list_) {
    if (t.id == id) return &t;
  }
  return nullptr;
}

AnimTemplate AnimTemplates::capture(const TextItem& item) {
  AnimTemplate t;
  t.anim = item.anim;
  t.baseX = item.x;
  t.baseY = item.y;
  t.duration = std::clamp(item.duration, kMinTextDuration, kMaxTemplateDuration);
  t.entrance = item.animation;
  // テキストの長さより後ろのキーフレームは、当てはめても使われないので残さない
  for (auto& keys : t.anim.tracks) {
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.t < -1e-9 || k.t > t.duration + 1e-9; }), keys.end());
  }
  return t;
}

void AnimTemplates::applyTo(const AnimTemplate& t, TextItem& item) {
  const double D = std::max(kMinTextDuration, item.duration);
  const double S = std::max(kMinTextDuration, t.duration);
  auto timeOf = [&](double k) {
    if (D >= S - 1e-9) return k <= S / 2 + 1e-9 ? k : D - (S - k);  // 長いテキスト: 入りは始まりから、出は終わりから同じ時間
    return k * D / S;                                                // 短いテキスト: 全体を同じ割合で縮める
  };
  Animation a;
  for (int i = 0; i < kAnimPropCount; ++i) {
    const AnimProp prop = static_cast<AnimProp>(i);
    auto& out = a.keys(prop);
    for (const Keyframe& k : t.anim.keys(prop)) {
      Keyframe n = k;
      n.t = std::clamp(timeOf(k.t), 0.0, D);
      if (prop == AnimProp::X) n.value = k.value - t.baseX + item.x;
      else if (prop == AnimProp::Y) n.value = k.value - t.baseY + item.y;
      out.push_back(n);
    }
    normalizeKeys(prop, out);  // 範囲に収め、時間の順に並べ、近すぎるものは省く
  }
  item.anim = a;
  item.animation = t.entrance;
}

void AnimTemplates::load() {
  list_.clear();
  QFile f(templatesPath());
  if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
  for (const auto& v : root.value(QStringLiteral("templates")).toArray()) {
    const QJsonObject o = v.toObject();
    AnimTemplate t;
    t.id = o.value(QStringLiteral("id")).toString();
    if (!isAnimId(t.id) || find(t.id)) continue;
    t.name = cleanName(o.value(QStringLiteral("name")).toString());
    t.duration = std::clamp(o.value(QStringLiteral("duration")).toDouble(3), kMinTextDuration, kMaxTemplateDuration);
    t.baseX = clampProp(AnimProp::X, o.value(QStringLiteral("baseX")).toDouble(0.5));
    t.baseY = clampProp(AnimProp::Y, o.value(QStringLiteral("baseY")).toDouble(0.82));
    t.entrance = animationFromString(o.value(QStringLiteral("entrance")).toString());
    t.anim = animFromJson(o);
    for (auto& keys : t.anim.tracks) {
      if (keys.size() > kMaxKeysPerProp) keys.resize(kMaxKeysPerProp);
    }
    list_.push_back(t);
  }
}

bool AnimTemplates::save() const {
  QJsonArray arr;
  for (const auto& t : list_) {
    QJsonObject o{{QStringLiteral("id"), t.id},
                  {QStringLiteral("name"), t.name},
                  {QStringLiteral("duration"), t.duration},
                  {QStringLiteral("baseX"), t.baseX},
                  {QStringLiteral("baseY"), t.baseY},
                  {QStringLiteral("entrance"), animationToString(t.entrance)}};
    animToJson(o, t.anim);
    arr.append(o);
  }
  const QJsonObject root{{QStringLiteral("app"), QStringLiteral("e-editor")}, {QStringLiteral("version"), 1}, {QStringLiteral("templates"), arr}};
  const QString path = templatesPath();
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) return false;
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  return f.commit();
}

QString AnimTemplates::add(const QString& name, const TextItem& item) {
  AnimTemplate t = capture(item);
  t.id = QStringLiteral("anim:") + newId();
  t.name = cleanName(name);
  list_.push_back(t);
  if (!save()) {
    list_.pop_back();
    return {};
  }
  emit changed();
  return t.id;
}

bool AnimTemplates::rename(const QString& id, const QString& name) {
  for (auto& t : list_) {
    if (t.id != id) continue;
    const QString before = t.name;
    t.name = cleanName(name);
    if (t.name == before) return true;
    if (!save()) {
      t.name = before;
      return false;
    }
    emit changed();
    return true;
  }
  return false;
}

bool AnimTemplates::remove(const QString& id) {
  auto it = std::find_if(list_.begin(), list_.end(), [&](const AnimTemplate& t) { return t.id == id; });
  if (it == list_.end()) return false;
  const AnimTemplate removed = *it;
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
