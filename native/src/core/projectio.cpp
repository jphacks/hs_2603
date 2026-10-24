#include "core/projectio.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <set>

#include "core/editor.h"
#include "core/filters.h"
#include "core/timeline.h"

namespace kv {

static QString colorHex(const QColor& c) {
  return c.name(QColor::HexRgb);
}

static QColor colorFrom(const QJsonValue& v, const QColor& fallback) {
  const QColor c(v.toString());
  return c.isValid() ? c : fallback;
}

static QJsonArray volumePointsToJson(const std::vector<VolumePoint>& pts) {
  QJsonArray points;
  for (const auto& pt : pts) points.append(QJsonObject{{QStringLiteral("t"), pt.t}, {QStringLiteral("volume"), pt.volume}});
  return points;
}

static void volumePointsFromJson(const QJsonArray& arr, std::vector<VolumePoint>& out) {
  for (const auto& pv : arr) {
    const QJsonObject po = pv.toObject();
    if (po.value(QStringLiteral("t")).isDouble()) out.push_back({po.value(QStringLiteral("t")).toDouble(), po.value(QStringLiteral("volume")).toDouble(kMaxVolume)});
  }
  normalizeVolumePoints(out);
}

/** キーフレームアニメーション。キーフレームのある項目だけを書く（無ければ "anim" 自体を書かない） */
static void animToJson(QJsonObject& o, const Animation& a) {
  if (a.empty()) return;
  QJsonObject anim;
  for (int i = 0; i < kAnimPropCount; ++i) {
    const auto& keys = a.tracks[size_t(i)];
    if (keys.empty()) continue;
    QJsonArray arr;
    for (const auto& k : keys) {
      arr.append(QJsonObject{{QStringLiteral("t"), k.t}, {QStringLiteral("value"), k.value}, {QStringLiteral("easing"), easingToString(k.easing)}});
    }
    anim[propToString(static_cast<AnimProp>(i))] = arr;
  }
  o[QStringLiteral("anim")] = anim;
}

/** 古いプロジェクトには無い。その場合はアニメーション無し（従来どおり） */
static Animation animFromJson(const QJsonObject& o) {
  Animation a;
  const QJsonObject anim = o.value(QStringLiteral("anim")).toObject();
  for (int i = 0; i < kAnimPropCount; ++i) {
    const AnimProp prop = static_cast<AnimProp>(i);
    for (const auto& v : anim.value(propToString(prop)).toArray()) {
      const QJsonObject ko = v.toObject();
      if (!ko.value(QStringLiteral("t")).isDouble() || !ko.value(QStringLiteral("value")).isDouble()) continue;
      a.keys(prop).push_back({ko.value(QStringLiteral("t")).toDouble(), ko.value(QStringLiteral("value")).toDouble(),
                              easingFromString(ko.value(QStringLiteral("easing")).toString())});
    }
    normalizeKeys(prop, a.keys(prop));
  }
  return a;
}

QJsonObject projectToJson(const ProjectData& p) {
  QJsonObject proj;
  proj[QStringLiteral("name")] = p.name;
  proj[QStringLiteral("settings")] = QJsonObject{{QStringLiteral("aspect"), aspectToString(p.settings.aspect)},
                                                 {QStringLiteral("fps"), p.settings.fps}};
  QJsonArray clips;
  for (const auto& c : p.clips) {
    QJsonObject co{
        {QStringLiteral("id"), c.id},
        {QStringLiteral("assetId"), c.assetId},
        {QStringLiteral("kind"), c.isImage ? QStringLiteral("image") : QStringLiteral("video")},
        {QStringLiteral("in"), c.in},
        {QStringLiteral("out"), c.out},
        {QStringLiteral("speed"), c.speed},
        {QStringLiteral("volume"), c.volume},
        {QStringLiteral("muted"), c.muted},
        {QStringLiteral("fadeIn"), c.fadeIn},
        {QStringLiteral("fadeOut"), c.fadeOut},
        {QStringLiteral("fit"), fitToString(c.fit)},
        {QStringLiteral("color"), QJsonObject{{QStringLiteral("preset"), presetToString(c.color.preset)},
                                              {QStringLiteral("brightness"), c.color.brightness},
                                              {QStringLiteral("contrast"), c.color.contrast},
                                              {QStringLiteral("saturation"), c.color.saturation},
                                              {QStringLiteral("temperature"), c.color.temperature}}},
        {QStringLiteral("x"), c.x},
        {QStringLiteral("y"), c.y},
        {QStringLiteral("scale"), c.scale},
        {QStringLiteral("rotation"), c.rotation},
        {QStringLiteral("opacity"), c.opacity},
    };
    if (!c.volumePoints.empty()) co[QStringLiteral("volumePoints")] = volumePointsToJson(c.volumePoints);
    animToJson(co, c.anim);
    clips.append(co);
  }
  QJsonArray texts;
  for (const auto& t : p.texts) {
    QJsonObject to{
        {QStringLiteral("id"), t.id},
        {QStringLiteral("text"), t.text},
        {QStringLiteral("start"), t.start},
        {QStringLiteral("duration"), t.duration},
        {QStringLiteral("track"), t.track},
        {QStringLiteral("x"), t.x},
        {QStringLiteral("y"), t.y},
        {QStringLiteral("font"), fontToString(t.font)},
        {QStringLiteral("size"), t.size},
        {QStringLiteral("bold"), t.bold},
        {QStringLiteral("color"), colorHex(t.color)},
        {QStringLiteral("strokeColor"), colorHex(t.strokeColor)},
        {QStringLiteral("strokeWidth"), t.strokeWidth},
        {QStringLiteral("boxColor"), colorHex(t.boxColor)},
        {QStringLiteral("boxOpacity"), t.boxOpacity},
        {QStringLiteral("shadow"), t.shadow},
        {QStringLiteral("align"), alignToString(t.align)},
        {QStringLiteral("letterSpacing"), t.letterSpacing},
        {QStringLiteral("animation"), animationToString(t.animation)},
        {QStringLiteral("scale"), t.scale},
        {QStringLiteral("rotation"), t.rotation},
        {QStringLiteral("opacity"), t.opacity},
    };
    animToJson(to, t.anim);
    texts.append(to);
  }
  QJsonArray audios;
  for (const auto& a : p.audios) {
    audios.append(QJsonObject{
        {QStringLiteral("id"), a.id},
        {QStringLiteral("assetId"), a.assetId},
        {QStringLiteral("start"), a.start},
        {QStringLiteral("in"), a.in},
        {QStringLiteral("out"), a.out},
        {QStringLiteral("volume"), a.volume},
        {QStringLiteral("volumePoints"), volumePointsToJson(a.volumePoints)},
        {QStringLiteral("fadeIn"), a.fadeIn},
        {QStringLiteral("fadeOut"), a.fadeOut},
    });
  }
  proj[QStringLiteral("clips")] = clips;
  proj[QStringLiteral("texts")] = texts;
  proj[QStringLiteral("audios")] = audios;
  return proj;
}

static QJsonArray assetsToJson(const std::vector<Asset>& list) {
  QJsonArray assets;
  for (const auto& a : list) {
    assets.append(QJsonObject{
        {QStringLiteral("id"), a.id},
        {QStringLiteral("path"), QDir::toNativeSeparators(a.path)},
        {QStringLiteral("name"), a.name},
        {QStringLiteral("kind"), assetKindToString(a.kind)},
        {QStringLiteral("duration"), a.duration},
        {QStringLiteral("width"), a.width},
        {QStringLiteral("height"), a.height},
        {QStringLiteral("hasAudio"), a.hasAudio},
    });
  }
  return assets;
}

QByteArray serializeProject() {
  // 今の編集場所 1 つ分。プロジェクトの中身と、メディア一覧（タイムラインで使っているものを含む）
  const WorkspaceData ws = editor().snapshotOf(editor().workspaceIndex());
  QJsonObject root{
      {QStringLiteral("app"), QStringLiteral("kantan-video-editor")},
      {QStringLiteral("version"), 1},
      {QStringLiteral("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {QStringLiteral("project"), projectToJson(ws.project)},
      {QStringLiteral("assets"), assetsToJson(ws.assets)},
  };
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray serializeSession() {
  // 全部の編集場所（タブ）。名前・保存先・未保存かどうかも一緒に残して、そのまま復元できるようにする
  const ProjectFile file = editor().snapshot();
  QJsonArray workspaces;
  for (const auto& ws : file.workspaces) {
    workspaces.append(QJsonObject{
        {QStringLiteral("project"), projectToJson(ws.project)},
        {QStringLiteral("assets"), assetsToJson(ws.assets)},
        {QStringLiteral("name"), ws.name},
        {QStringLiteral("number"), ws.number},
        {QStringLiteral("path"), QDir::toNativeSeparators(ws.path)},
        {QStringLiteral("dirty"), ws.dirty},
    });
  }
  QJsonObject root{
      {QStringLiteral("app"), QStringLiteral("kantan-video-editor")},
      {QStringLiteral("version"), 2},
      {QStringLiteral("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {QStringLiteral("activeWorkspace"), file.active},
      {QStringLiteral("workspaces"), workspaces},
  };
  return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static double numOr(const QJsonObject& o, const char* key, double fallback) {
  const QJsonValue v = o.value(QLatin1String(key));
  return v.isDouble() ? v.toDouble() : fallback;
}

/** 1 つの編集場所分（プロジェクトの中身とメディア一覧）を読む */
static WorkspaceData workspaceFromJson(const QJsonObject& p, const QJsonArray& assetList) {
  WorkspaceData file;
  file.project = emptyProject();
  if (p.value(QStringLiteral("name")).isString()) file.project.name = p.value(QStringLiteral("name")).toString();
  const QJsonObject settings = p.value(QStringLiteral("settings")).toObject();
  file.project.settings.aspect = aspectFromString(settings.value(QStringLiteral("aspect")).toString());
  const int fps = settings.value(QStringLiteral("fps")).toInt(30);
  file.project.settings.fps = (fps == 24 || fps == 30 || fps == 60) ? fps : 30;

  for (const auto& v : p.value(QStringLiteral("clips")).toArray()) {
    const QJsonObject o = v.toObject();
    VideoClip c;
    c.id = o.value(QStringLiteral("id")).toString(newId());
    c.assetId = o.value(QStringLiteral("assetId")).toString();
    c.isImage = o.value(QStringLiteral("kind")).toString() == u"image";
    c.in = std::max(0.0, numOr(o, "in", 0));
    c.out = std::max(c.in + 0.05, numOr(o, "out", c.in + 1));
    c.speed = std::clamp(numOr(o, "speed", 1), 0.1, 16.0);
    // 以前は 200% まで設定できたので、保存されている値が 100% を超えていたら 100% にそろえる
    c.volume = std::clamp(numOr(o, "volume", kMaxVolume), 0.0, kMaxVolume);
    c.muted = o.value(QStringLiteral("muted")).toBool(false);
    c.fadeIn = std::max(0.0, numOr(o, "fadeIn", 0));
    c.fadeOut = std::max(0.0, numOr(o, "fadeOut", 0));
    c.fit = fitFromString(o.value(QStringLiteral("fit")).toString());
    const QJsonObject col = o.value(QStringLiteral("color")).toObject();
    c.color.preset = presetFromString(col.value(QStringLiteral("preset")).toString());
    c.color.brightness = numOr(col, "brightness", 1);
    c.color.contrast = numOr(col, "contrast", 1);
    c.color.saturation = numOr(col, "saturation", 1);
    c.color.temperature = numOr(col, "temperature", 0);
    // 位置・拡大率・回転・不透明度、音量とアニメーションのキーフレーム（以前のプロジェクトには無い。その場合は従来どおり）
    c.x = clampProp(AnimProp::X, numOr(o, "x", 0.5));
    c.y = clampProp(AnimProp::Y, numOr(o, "y", 0.5));
    c.scale = clampProp(AnimProp::Scale, numOr(o, "scale", 1));
    c.rotation = clampProp(AnimProp::Rotation, numOr(o, "rotation", 0));
    c.opacity = clampProp(AnimProp::Opacity, numOr(o, "opacity", 1));
    volumePointsFromJson(o.value(QStringLiteral("volumePoints")).toArray(), c.volumePoints);
    c.anim = animFromJson(o);
    c.anim.keys(AnimProp::LetterSpacing).clear();  // 字間は文字だけの項目（映像に紛れ込んだ分は捨てる）
    file.project.clips.push_back(c);
  }
  bool hasAllTracks = true;
  for (const auto& v : p.value(QStringLiteral("texts")).toArray()) {
    const QJsonObject o = v.toObject();
    TextItem t;
    t.id = o.value(QStringLiteral("id")).toString(newId());
    t.text = o.value(QStringLiteral("text")).toString();
    t.start = std::max(0.0, numOr(o, "start", 0));
    t.duration = std::max(0.1, numOr(o, "duration", 3));
    if (o.value(QStringLiteral("track")).isDouble()) t.track = std::max(0, o.value(QStringLiteral("track")).toInt());
    else hasAllTracks = false;
    t.x = numOr(o, "x", 0.5);
    t.y = numOr(o, "y", 0.82);
    t.font = fontFromString(o.value(QStringLiteral("font")).toString());
    t.size = std::clamp(numOr(o, "size", 0.075), 0.01, 0.5);
    t.bold = o.value(QStringLiteral("bold")).toBool(true);
    t.color = colorFrom(o.value(QStringLiteral("color")), QColor(255, 255, 255));
    t.strokeColor = colorFrom(o.value(QStringLiteral("strokeColor")), QColor(0, 0, 0));
    t.strokeWidth = std::clamp(numOr(o, "strokeWidth", 0.12), 0.0, 0.5);
    t.boxColor = colorFrom(o.value(QStringLiteral("boxColor")), QColor(0, 0, 0));
    t.boxOpacity = std::clamp(numOr(o, "boxOpacity", 0), 0.0, 1.0);
    t.shadow = o.value(QStringLiteral("shadow")).toBool(true);
    t.align = alignFromString(o.value(QStringLiteral("align")).toString());
    // 字間（以前のプロジェクトには無い。その場合は 0 = フォント本来の間隔）
    t.letterSpacing = clampProp(AnimProp::LetterSpacing, numOr(o, "letterSpacing", 0));
    t.animation = animationFromString(o.value(QStringLiteral("animation")).toString());
    t.scale = clampProp(AnimProp::Scale, numOr(o, "scale", 1));
    t.rotation = clampProp(AnimProp::Rotation, numOr(o, "rotation", 0));
    t.opacity = clampProp(AnimProp::Opacity, numOr(o, "opacity", 1));
    t.anim = animFromJson(o);
    file.project.texts.push_back(t);
  }
  // 段を保存していない古いプロジェクトは、以前の自動配置と同じ並びで段を決める（これが最後の自動配置で、以降は保存した段のまま）
  if (!hasAllTracks) autoAssignTextTracks(file.project.texts);
  for (const auto& v : p.value(QStringLiteral("audios")).toArray()) {
    const QJsonObject o = v.toObject();
    AudioItem a;
    a.id = o.value(QStringLiteral("id")).toString(newId());
    a.assetId = o.value(QStringLiteral("assetId")).toString();
    a.start = std::max(0.0, numOr(o, "start", 0));
    a.in = std::max(0.0, numOr(o, "in", 0));
    a.out = std::max(a.in, numOr(o, "out", a.in));
    a.volume = std::clamp(numOr(o, "volume", kMaxVolume), 0.0, kMaxVolume);
    // 音量キーフレーム（以前のプロジェクトには無い。その場合はクリップ全体の音量のまま）
    volumePointsFromJson(o.value(QStringLiteral("volumePoints")).toArray(), a.volumePoints);
    a.fadeIn = std::max(0.0, numOr(o, "fadeIn", 0));
    a.fadeOut = std::max(0.0, numOr(o, "fadeOut", 0));
    file.project.audios.push_back(a);
  }
  for (const auto& v : assetList) {
    const QJsonObject o = v.toObject();
    Asset a;
    a.id = o.value(QStringLiteral("id")).toString();
    a.path = QDir::fromNativeSeparators(o.value(QStringLiteral("path")).toString());
    a.name = o.value(QStringLiteral("name")).toString();
    a.kind = assetKindFromString(o.value(QStringLiteral("kind")).toString());
    a.duration = numOr(o, "duration", 0);
    a.width = o.value(QStringLiteral("width")).toInt();
    a.height = o.value(QStringLiteral("height")).toInt();
    a.hasAudio = o.value(QStringLiteral("hasAudio")).toBool();
    if (!a.id.isEmpty() && !a.path.isEmpty()) file.assets.push_back(a);
  }
  return file;
}

std::optional<ProjectFile> parseProject(const QByteArray& json, QString* error) {
  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
  const QJsonObject root = doc.object();
  const bool isOurs = pe.error == QJsonParseError::NoError && root.value(QStringLiteral("app")).toString() == u"kantan-video-editor";
  const bool isMulti = root.value(QStringLiteral("workspaces")).isArray();
  const bool isLegacy = root.value(QStringLiteral("project")).isObject() && root.value(QStringLiteral("assets")).isArray();
  if (!isOurs || (!isMulti && !isLegacy)) {
    if (error) *error = QStringLiteral("プロジェクトファイルではありません");
    return std::nullopt;
  }
  ProjectFile file;
  if (isMulti) {
    for (const auto& v : root.value(QStringLiteral("workspaces")).toArray()) {
      const QJsonObject o = v.toObject();
      WorkspaceData ws = workspaceFromJson(o.value(QStringLiteral("project")).toObject(), o.value(QStringLiteral("assets")).toArray());
      ws.name = o.value(QStringLiteral("name")).toString().simplified().left(40);
      ws.number = o.value(QStringLiteral("number")).toInt(0);
      ws.path = QDir::fromNativeSeparators(o.value(QStringLiteral("path")).toString());
      ws.dirty = o.value(QStringLiteral("dirty")).toBool(false);
      file.workspaces.push_back(std::move(ws));
    }
    if (file.workspaces.empty()) file.workspaces.push_back(workspaceFromJson({}, {}));
    file.active = std::clamp(root.value(QStringLiteral("activeWorkspace")).toInt(0), 0, int(file.workspaces.size()) - 1);
  } else {
    // 1 つの編集場所だけを保存したファイル（保存で作るのはこの形式）
    file.workspaces.push_back(workspaceFromJson(root.value(QStringLiteral("project")).toObject(), root.value(QStringLiteral("assets")).toArray()));
  }
  return file;
}

bool hasMeaningfulContent(const ProjectData& p) {
  return !p.clips.empty() || !p.texts.empty() || !p.audios.empty();
}

bool hasMeaningfulContent(const ProjectFile& f) {
  for (const auto& ws : f.workspaces) {
    if (hasMeaningfulContent(ws.project)) return true;
  }
  return false;
}

QString autosavePath() {
  const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  QDir().mkpath(dir);
  return dir + QStringLiteral("/autosave.kvproj");
}

std::optional<ProjectFile> readAutosave() {
  QFile f(autosavePath());
  if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
  auto file = parseProject(f.readAll());
  if (!file || !hasMeaningfulContent(*file)) return std::nullopt;
  return file;
}

void writeAutosave() {
  if (!editor().hasAnyContent()) return;
  QSaveFile f(autosavePath());
  if (!f.open(QIODevice::WriteOnly)) return;
  f.write(serializeSession());
  f.commit();
}

void clearAutosave() {
  QFile::remove(autosavePath());
}

}  // namespace kv
