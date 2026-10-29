#include "core/projectio.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <set>

#include "core/editor.h"
#include "core/effects.h"
#include "core/filters.h"
#include "core/textstyles.h"
#include "core/timeline.h"
#include "core/transitions.h"

namespace kv {

static QString colorHex(const QColor& c) {
  return c.name(QColor::HexRgb);
}

static QColor colorFrom(const QJsonValue& v, const QColor& fallback) {
  const QColor c(v.toString());
  return c.isValid() ? c : fallback;
}

QJsonArray effectsToJson(const std::vector<Effect>& effects) {
  QJsonArray list;
  for (const Effect& e : effects) {
    QJsonObject o{{QStringLiteral("id"), e.id},
                  {QStringLiteral("type"), effectTypeToString(e.type)},
                  {QStringLiteral("enabled"), e.enabled},
                  {QStringLiteral("amount"), e.amount}};
    const EffectInfo& info = effectInfo(e.type);
    if (!info.size.label.isEmpty()) o[QStringLiteral("size")] = e.size;
    if (!info.softness.label.isEmpty()) o[QStringLiteral("softness")] = e.softness;
    if (info.usesColor) o[QStringLiteral("color")] = colorHex(e.color);
    list.append(o);
  }
  return list;
}

std::vector<Effect> effectsFromJson(const QJsonArray& list) {
  std::vector<Effect> out;
  for (const auto& v : list) {
    const QJsonObject o = v.toObject();
    EffectType type;
    // 知らない種類（新しい版で作ったもの）は読み飛ばす
    if (!effectTypeFromString(o.value(QStringLiteral("type")).toString(), &type)) continue;
    Effect e = makeEffect(type);
    const QString id = o.value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) e.id = id;
    e.enabled = o.value(QStringLiteral("enabled")).toBool(true);
    if (o.value(QStringLiteral("amount")).isDouble()) e.amount = o.value(QStringLiteral("amount")).toDouble();
    if (o.value(QStringLiteral("size")).isDouble()) e.size = o.value(QStringLiteral("size")).toDouble();
    if (o.value(QStringLiteral("softness")).isDouble()) e.softness = o.value(QStringLiteral("softness")).toDouble();
    e.color = colorFrom(o.value(QStringLiteral("color")), e.color);
    clampEffect(e);
    // 同じ ID が重なっていたら付け直す（設定欄で見分けられるように）
    for (const Effect& x : out) {
      if (x.id == e.id) e.id = newId();
    }
    out.push_back(e);
  }
  return out;
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
void animToJson(QJsonObject& o, const Animation& a) {
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
Animation animFromJson(const QJsonObject& o) {
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

/** ビデオクリップ（重ねるトラックの映像・画像も同じ形）。切り抜き・伸縮・クロマキーは、使っているときだけ書く */
static QJsonObject clipToJson(const VideoClip& c) {
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
  if (hasCrop(c)) {
    co[QStringLiteral("crop")] = QJsonObject{{QStringLiteral("left"), c.cropL},
                                             {QStringLiteral("top"), c.cropT},
                                             {QStringLiteral("right"), c.cropR},
                                             {QStringLiteral("bottom"), c.cropB}};
  }
  if (hasStretch(c)) co[QStringLiteral("stretch")] = QJsonObject{{QStringLiteral("x"), c.stretchX}, {QStringLiteral("y"), c.stretchY}};
  if (!(c.chroma == ChromaKey{})) {
    co[QStringLiteral("chromaKey")] = QJsonObject{{QStringLiteral("enabled"), c.chroma.enabled},
                                                  {QStringLiteral("color"), colorHex(c.chroma.color)},
                                                  {QStringLiteral("similarity"), c.chroma.similarity},
                                                  {QStringLiteral("blend"), c.chroma.blend},
                                                  {QStringLiteral("despill"), c.chroma.despill}};
  }
  // 反転とエフェクト（使っているときだけ書く。以前の版で開くと、反転もエフェクトも無いクリップになる）
  if (c.flipX || c.flipY) co[QStringLiteral("flip")] = QJsonObject{{QStringLiteral("x"), c.flipX}, {QStringLiteral("y"), c.flipY}};
  if (!c.effects.empty()) co[QStringLiteral("effects")] = effectsToJson(c.effects);
  // 前のクリップからのトランジション（使っているときだけ書く。以前の版で開くと、ふつうの切れ目になる）
  if (c.transIn.type != TransitionType::None) {
    QJsonObject tr{{QStringLiteral("type"), transitionToString(c.transIn.type)}, {QStringLiteral("duration"), c.transIn.duration}};
    if (c.transIn.audioCrossfade) tr[QStringLiteral("audioCrossfade")] = true;  // 無ければ、音は切れ目でそのまま切り替わる
    co[QStringLiteral("transition")] = tr;
  }
  // パン・逆再生（使っているときだけ書く。以前の版で開くと、真ん中の音・逆向きに作り直した動画をふつうに再生する）
  if (std::abs(c.pan) > 1e-9) co[QStringLiteral("pan")] = c.pan;
  if (!c.reversedFrom.isEmpty()) co[QStringLiteral("reversedFrom")] = c.reversedFrom;
  // オブジェクトマスク（使っているときだけ書く。以前の版で開くと、マスクの無い映像になる）
  if (c.mask.has()) {
    const ObjectMask& m = c.mask;
    QJsonArray prompts;
    for (const MaskPrompt& p : m.prompts) {
      QJsonArray points;
      for (const MaskPoint& q : p.points) {
        points.append(QJsonObject{{QStringLiteral("x"), q.x}, {QStringLiteral("y"), q.y}, {QStringLiteral("label"), q.positive ? 1 : 0}});
      }
      prompts.append(QJsonObject{{QStringLiteral("t"), p.t}, {QStringLiteral("points"), points}});
    }
    QJsonObject mo{{QStringLiteral("id"), m.id},
                   {QStringLiteral("prompts"), prompts},
                   {QStringLiteral("enabled"), m.enabled},
                   {QStringLiteral("mode"), m.mode == MaskMode::Effects ? QStringLiteral("effects") : QStringLiteral("cutout")}};
    if (!m.file.isEmpty()) mo[QStringLiteral("file")] = m.file;
    if (m.invert) mo[QStringLiteral("invert")] = true;
    if (std::abs(m.expand) > 1e-9) mo[QStringLiteral("expand")] = m.expand;
    if (m.feather > 1e-9) mo[QStringLiteral("feather")] = m.feather;
    co[QStringLiteral("objectMask")] = mo;
  }
  return co;
}

QJsonObject textToJson(const TextItem& t) {
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
  // 読み込んだフォント・同梱のフォント（無いパソコンでは "font" の種類で描く）と、一部だけ別のフォントにした範囲
  if (!t.fontFamily.isEmpty()) to[QStringLiteral("fontFamily")] = t.fontFamily;
  if (!t.fontSpans.empty()) {
    QJsonArray spans;
    for (const auto& s : t.fontSpans) {
      QJsonObject so{{QStringLiteral("start"), s.start}, {QStringLiteral("length"), s.length}, {QStringLiteral("font"), fontToString(s.font)}};
      if (!s.family.isEmpty()) so[QStringLiteral("fontFamily")] = s.family;
      spans.append(so);
    }
    to[QStringLiteral("fontSpans")] = spans;
  }
  if (!t.group.isEmpty()) to[QStringLiteral("group")] = t.group;
  return to;
}

QJsonObject projectToJson(const ProjectData& p) {
  QJsonObject proj;
  proj[QStringLiteral("name")] = p.name;
  proj[QStringLiteral("settings")] = QJsonObject{{QStringLiteral("aspect"), aspectToString(p.settings.aspect)},
                                                 {QStringLiteral("fps"), p.settings.fps},
                                                 {QStringLiteral("speed"), p.settings.speed}};
  QJsonArray clips;
  for (const auto& c : p.clips) clips.append(clipToJson(c));
  QJsonArray overlays;
  for (const auto& o : p.overlays) {
    QJsonObject oo = clipToJson(o);
    oo[QStringLiteral("start")] = o.start;
    oo[QStringLiteral("track")] = o.track;
    if (!o.group.isEmpty()) oo[QStringLiteral("group")] = o.group;
    // エフェクトレイヤー（素材なし。範囲の形・境目のぼかし・反転）
    if (o.isEffectLayer()) {
      oo[QStringLiteral("layer")] = QStringLiteral("effect");
      const QString shape = o.region.shape == RegionShape::Full   ? QStringLiteral("full")
                            : o.region.shape == RegionShape::Rect ? QStringLiteral("rect")
                                                                  : QStringLiteral("ellipse");
      oo[QStringLiteral("region")] =
          QJsonObject{{QStringLiteral("shape"), shape}, {QStringLiteral("feather"), o.region.feather}, {QStringLiteral("invert"), o.region.invert}};
    }
    overlays.append(oo);
  }
  QJsonArray texts;
  for (const auto& t : p.texts) texts.append(textToJson(t));
  QJsonArray audios;
  for (const auto& a : p.audios) {
    QJsonObject ao{
        {QStringLiteral("id"), a.id},
        {QStringLiteral("assetId"), a.assetId},
        {QStringLiteral("start"), a.start},
        {QStringLiteral("in"), a.in},
        {QStringLiteral("out"), a.out},
        {QStringLiteral("track"), a.track},
        {QStringLiteral("volume"), a.volume},
        {QStringLiteral("volumePoints"), volumePointsToJson(a.volumePoints)},
        {QStringLiteral("fadeIn"), a.fadeIn},
        {QStringLiteral("fadeOut"), a.fadeOut},
    };
    if (!a.group.isEmpty()) ao[QStringLiteral("group")] = a.group;
    if (std::abs(a.pan) > 1e-9) ao[QStringLiteral("pan")] = a.pan;  // 使っているときだけ（以前の版では真ん中の音）
    audios.append(ao);
  }
  proj[QStringLiteral("clips")] = clips;
  // 重ねるトラックは、使っているときだけ書く（以前の版で開いても、ほかの内容はそのまま読める）
  if (!overlays.isEmpty()) proj[QStringLiteral("overlays")] = overlays;
  proj[QStringLiteral("texts")] = texts;
  // テキストの段の ID（上の段から順。テキストの "track" が、この並びの何番目の段か）。以前の版は読み飛ばし、"track" だけで並べる
  if (!p.textTrackIds.empty()) {
    QJsonArray tracks;
    for (const auto& id : p.textTrackIds) tracks.append(id);
    proj[QStringLiteral("textTracks")] = tracks;
  }
  // 段の番号（見出しの「テキスト 1」「オーディオ 2」の数字。上の段から順）。段を入れ替えたときの見出しを、そのまま残す
  auto nosToJson = [](const std::vector<int>& nos) {
    QJsonArray a;
    for (int n : nos) a.append(n);
    return a;
  };
  if (p.textTrackNos.size() > 1) proj[QStringLiteral("textTrackNos")] = nosToJson(p.textTrackNos);
  proj[QStringLiteral("audios")] = audios;
  if (p.audioTrackNos.size() > 1) proj[QStringLiteral("audioTrackNos")] = nosToJson(p.audioTrackNos);
  // マーカー（使っているときだけ書く。以前の版は読み飛ばす）
  if (!p.markers.empty()) {
    QJsonArray markers;
    for (const Marker& m : p.markers) {
      QJsonObject mo{{QStringLiteral("id"), m.id}, {QStringLiteral("t"), m.t}, {QStringLiteral("color"), colorHex(m.color)}};
      if (!m.name.isEmpty()) mo[QStringLiteral("name")] = m.name;
      if (!m.comment.isEmpty()) mo[QStringLiteral("comment")] = m.comment;
      markers.append(mo);
    }
    proj[QStringLiteral("markers")] = markers;
  }
  // トラックの非表示・ミュート（使っているときだけ書く。以前の版で開くと、すべて出す）
  QJsonObject tracks;
  if (p.mainVideoHidden) tracks[QStringLiteral("mainVideoHidden")] = true;
  if (p.mainAudioMuted) tracks[QStringLiteral("mainAudioMuted")] = true;
  if (!p.hiddenTextTracks.empty()) {
    QJsonArray a;
    for (const QString& id : p.hiddenTextTracks) a.append(id);
    tracks[QStringLiteral("hiddenText")] = a;
  }
  if (!p.hiddenOverlayTracks.empty()) tracks[QStringLiteral("hiddenOverlay")] = nosToJson(p.hiddenOverlayTracks);
  if (!p.mutedOverlayTracks.empty()) tracks[QStringLiteral("mutedOverlay")] = nosToJson(p.mutedOverlayTracks);
  if (!p.mutedAudioTracks.empty()) tracks[QStringLiteral("mutedAudio")] = nosToJson(p.mutedAudioTracks);
  if (!tracks.isEmpty()) proj[QStringLiteral("trackStates")] = tracks;
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
      {QStringLiteral("app"), QStringLiteral("e-editor")},
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
      {QStringLiteral("app"), QStringLiteral("e-editor")},
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

static VideoClip clipFromJson(const QJsonObject& o) {
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
  // 切り抜き・伸縮・クロマキー（以前のプロジェクトには無い。その場合は使わない）
  const QJsonObject crop = o.value(QStringLiteral("crop")).toObject();
  c.cropL = numOr(crop, "left", 0);
  c.cropT = numOr(crop, "top", 0);
  c.cropR = numOr(crop, "right", 0);
  c.cropB = numOr(crop, "bottom", 0);
  const QJsonObject stretch = o.value(QStringLiteral("stretch")).toObject();
  c.stretchX = numOr(stretch, "x", 1);
  c.stretchY = numOr(stretch, "y", 1);
  const QJsonObject ck = o.value(QStringLiteral("chromaKey")).toObject();
  if (!ck.isEmpty()) {
    c.chroma.enabled = ck.value(QStringLiteral("enabled")).toBool(false);
    c.chroma.color = colorFrom(ck.value(QStringLiteral("color")), ChromaKey{}.color);
    c.chroma.similarity = numOr(ck, "similarity", ChromaKey{}.similarity);
    c.chroma.blend = numOr(ck, "blend", ChromaKey{}.blend);
    c.chroma.despill = ck.value(QStringLiteral("despill")).toBool(true);
  }
  clampCropStretch(c);
  // 反転とエフェクト（以前のプロジェクトには無い。その場合は使わない）
  const QJsonObject flip = o.value(QStringLiteral("flip")).toObject();
  c.flipX = flip.value(QStringLiteral("x")).toBool(false);
  c.flipY = flip.value(QStringLiteral("y")).toBool(false);
  c.effects = effectsFromJson(o.value(QStringLiteral("effects")).toArray());
  // トランジション（以前のプロジェクトには無い。知らない種類は読み飛ばす）
  const QJsonObject tr = o.value(QStringLiteral("transition")).toObject();
  TransitionType tt = TransitionType::None;
  if (transitionFromString(tr.value(QStringLiteral("type")).toString(), &tt)) {
    c.transIn.type = tt;
    c.transIn.duration = std::clamp(numOr(tr, "duration", kDefaultTransitionDuration), kMinTransitionDuration, kMaxTransitionDuration);
    // 音のクロスフェード（以前のプロジェクトには無い。その場合は、これまでどおり音は切れ目でそのまま切り替える）
    c.transIn.audioCrossfade = tr.value(QStringLiteral("audioCrossfade")).toBool(false);
  }
  // パン・逆再生（以前のプロジェクトには無い。その場合は真ん中・ふつうの向き）
  c.pan = std::clamp(numOr(o, "pan", 0), -kMaxPan, kMaxPan);
  c.reversedFrom = o.value(QStringLiteral("reversedFrom")).toString();
  // オブジェクトマスク（以前のプロジェクトには無い。その場合は使わない）
  const QJsonObject mo = o.value(QStringLiteral("objectMask")).toObject();
  if (!mo.isEmpty()) {
    ObjectMask m;
    m.id = mo.value(QStringLiteral("id")).toString().left(64);
    // マスクのファイルは、generated フォルダーの中の名前だけ（ほかの場所のファイルは読まない）
    const QString file = mo.value(QStringLiteral("file")).toString();
    if (!file.isEmpty() && QFileInfo(file).fileName() == file && file.endsWith(QStringLiteral(".kvmask"))) m.file = file;
    for (const auto& pv : mo.value(QStringLiteral("prompts")).toArray()) {
      const QJsonObject po = pv.toObject();
      MaskPrompt p;
      p.t = std::max(0.0, numOr(po, "t", 0));
      for (const auto& qv : po.value(QStringLiteral("points")).toArray()) {
        const QJsonObject qo = qv.toObject();
        MaskPoint q;
        q.x = std::clamp(numOr(qo, "x", 0.5), 0.0, 1.0);
        q.y = std::clamp(numOr(qo, "y", 0.5), 0.0, 1.0);
        q.positive = qo.value(QStringLiteral("label")).toInt(1) != 0;
        if (p.points.size() < 32) p.points.push_back(q);
      }
      if (!p.points.empty() && m.prompts.size() < 32) m.prompts.push_back(p);
    }
    m.enabled = mo.value(QStringLiteral("enabled")).toBool(true);
    m.mode = mo.value(QStringLiteral("mode")).toString() == u"effects" ? MaskMode::Effects : MaskMode::Cutout;
    m.invert = mo.value(QStringLiteral("invert")).toBool(false);
    m.expand = std::clamp(numOr(mo, "expand", 0), -kMaxMaskExpand, kMaxMaskExpand);
    m.feather = std::clamp(numOr(mo, "feather", 0), 0.0, kMaxMaskFeather);
    if (m.has()) c.mask = m;
  }
  return c;
}

TextItem textFromJson(const QJsonObject& o, bool* hasTrack) {
  TextItem t;
  t.id = o.value(QStringLiteral("id")).toString(newId());
  t.text = o.value(QStringLiteral("text")).toString();
  t.start = std::max(0.0, numOr(o, "start", 0));
  t.duration = std::max(0.1, numOr(o, "duration", 3));
  const bool tracked = o.value(QStringLiteral("track")).isDouble();
  if (tracked) t.track = std::max(0, o.value(QStringLiteral("track")).toInt());
  if (hasTrack) *hasTrack = tracked;
  t.x = numOr(o, "x", 0.5);
  t.y = numOr(o, "y", 0.82);
  t.font = fontFromString(o.value(QStringLiteral("font")).toString());
  t.fontFamily = o.value(QStringLiteral("fontFamily")).toString();
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
  // 一部だけ別のフォントにした範囲（以前のプロジェクトには無い）。文字の範囲に収め、並べ直す
  for (const auto& sv : o.value(QStringLiteral("fontSpans")).toArray()) {
    const QJsonObject so = sv.toObject();
    FontSpan s;
    s.start = so.value(QStringLiteral("start")).toInt(0);
    s.length = so.value(QStringLiteral("length")).toInt(0);
    s.font = fontFromString(so.value(QStringLiteral("font")).toString());
    s.family = so.value(QStringLiteral("fontFamily")).toString();
    t.fontSpans.push_back(s);
  }
  normalizeFontSpans(t);
  t.group = o.value(QStringLiteral("group")).toString();
  return t;
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
  // 動画全体の再生速度（以前のプロジェクトには無い。その場合は元の速さ）
  file.project.settings.speed = std::clamp(settings.value(QStringLiteral("speed")).toDouble(1), kMinProjectSpeed, kMaxProjectSpeed);
  // マーカー（以前のプロジェクトには無い）。時間の順にそろえる
  for (const auto& v : p.value(QStringLiteral("markers")).toArray()) {
    const QJsonObject o = v.toObject();
    Marker m;
    m.id = o.value(QStringLiteral("id")).toString();
    // ID が無い・ほかのマーカーと同じ（手で書き換えたファイルなど）なら、新しく付ける（ID で選ぶ・動かすので）
    const bool dup = std::any_of(file.project.markers.begin(), file.project.markers.end(), [&](const Marker& x) { return x.id == m.id; });
    if (m.id.isEmpty() || dup) m.id = newId();
    m.t = frameRound(std::max(0.0, numOr(o, "t", 0)), file.project.settings.fps);  // フレームにそろえる
    m.name = o.value(QStringLiteral("name")).toString();
    m.comment = o.value(QStringLiteral("comment")).toString();
    m.color = colorFrom(o.value(QStringLiteral("color")), Marker{}.color);
    file.project.markers.push_back(m);
  }
  std::stable_sort(file.project.markers.begin(), file.project.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });

  for (const auto& v : p.value(QStringLiteral("clips")).toArray()) file.project.clips.push_back(clipFromJson(v.toObject()));
  // 重ねるトラック（以前のプロジェクトには無い）
  for (const auto& v : p.value(QStringLiteral("overlays")).toArray()) {
    const QJsonObject o = v.toObject();
    OverlayItem item;
    static_cast<VideoClip&>(item) = clipFromJson(o);
    item.fit = FitMode::Contain;  // 重ねるトラックは、いつも全体表示（余白は透明）
    item.start = std::max(0.0, numOr(o, "start", 0));
    item.track = std::max(0, o.value(QStringLiteral("track")).toInt(0));
    item.group = o.value(QStringLiteral("group")).toString();
    // エフェクトレイヤー（以前のプロジェクトには無い）。素材は使わず、長さは写真と同じ数え方
    if (o.value(QStringLiteral("layer")).toString() == u"effect") {
      item.kind = LayerKind::Effect;
      item.assetId.clear();
      item.isImage = true;
      const QJsonObject r = o.value(QStringLiteral("region")).toObject();
      const QString shape = r.value(QStringLiteral("shape")).toString();
      item.region.shape = shape == u"full" ? RegionShape::Full : shape == u"rect" ? RegionShape::Rect : RegionShape::Ellipse;
      item.region.feather = std::clamp(numOr(r, "feather", EffectRegion{}.feather), 0.0, kMaxRegionFeather);
      item.region.invert = r.value(QStringLiteral("invert")).toBool(false);
    }
    file.project.overlays.push_back(item);
  }
  bool hasAllTracks = true;
  for (const auto& v : p.value(QStringLiteral("texts")).toArray()) {
    bool hasTrack = false;
    file.project.texts.push_back(textFromJson(v.toObject(), &hasTrack));
    if (!hasTrack) hasAllTracks = false;
  }
  // 段を保存していない古いプロジェクトは、以前の自動配置と同じ並びで段を決める（これが最後の自動配置で、以降は保存した段のまま）
  if (!hasAllTracks) autoAssignTextTracks(file.project.texts);
  // テキストの段の ID（以前のプロジェクトには無い。その場合は、ここで新しく付ける）。段の数に合わせる
  file.project.textTrackIds.clear();
  file.project.textTrackNos.clear();
  if (hasAllTracks) {
    for (const auto& v : p.value(QStringLiteral("textTracks")).toArray()) file.project.textTrackIds.push_back(v.toString());
    // 段の番号（以前のプロジェクトには無い。その場合は、上の段から 1, 2, 3 …）
    for (const auto& v : p.value(QStringLiteral("textTrackNos")).toArray()) file.project.textTrackNos.push_back(v.toInt(0));
  }
  syncTextTrackIds(file.project);
  bool audioTracked = true;
  for (const auto& v : p.value(QStringLiteral("audios")).toArray()) {
    const QJsonObject o = v.toObject();
    AudioItem a;
    a.id = o.value(QStringLiteral("id")).toString(newId());
    a.assetId = o.value(QStringLiteral("assetId")).toString();
    a.start = std::max(0.0, numOr(o, "start", 0));
    a.in = std::max(0.0, numOr(o, "in", 0));
    a.out = std::max(a.in, numOr(o, "out", a.in));
    // オーディオの段（以前のプロジェクトには無い。その場合は、以前と同じ自動の並べ方で決める）
    if (o.value(QStringLiteral("track")).isDouble()) a.track = std::max(0, o.value(QStringLiteral("track")).toInt());
    else audioTracked = false;
    a.volume = std::clamp(numOr(o, "volume", kMaxVolume), 0.0, kMaxVolume);
    // 音量キーフレーム（以前のプロジェクトには無い。その場合はクリップ全体の音量のまま）
    volumePointsFromJson(o.value(QStringLiteral("volumePoints")).toArray(), a.volumePoints);
    a.fadeIn = std::max(0.0, numOr(o, "fadeIn", 0));
    a.fadeOut = std::max(0.0, numOr(o, "fadeOut", 0));
    a.group = o.value(QStringLiteral("group")).toString();
    a.pan = std::clamp(numOr(o, "pan", 0), -kMaxPan, kMaxPan);  // 以前のプロジェクトには無い（真ん中）
    file.project.audios.push_back(a);
  }
  if (!audioTracked) autoAssignAudioTracks(file.project.audios);
  file.project.audioTrackNos.clear();
  if (audioTracked) {
    for (const auto& v : p.value(QStringLiteral("audioTrackNos")).toArray()) file.project.audioTrackNos.push_back(v.toInt(0));
  }
  // トラックの非表示・ミュート（以前のプロジェクトには無い。その場合はすべて出す）
  const QJsonObject tracks = p.value(QStringLiteral("trackStates")).toObject();
  file.project.mainVideoHidden = tracks.value(QStringLiteral("mainVideoHidden")).toBool(false);
  file.project.mainAudioMuted = tracks.value(QStringLiteral("mainAudioMuted")).toBool(false);
  for (const auto& v : tracks.value(QStringLiteral("hiddenText")).toArray()) file.project.hiddenTextTracks.push_back(v.toString());
  for (const auto& v : tracks.value(QStringLiteral("hiddenOverlay")).toArray()) file.project.hiddenOverlayTracks.push_back(v.toInt(-1));
  for (const auto& v : tracks.value(QStringLiteral("mutedOverlay")).toArray()) file.project.mutedOverlayTracks.push_back(v.toInt(-1));
  for (const auto& v : tracks.value(QStringLiteral("mutedAudio")).toArray()) file.project.mutedAudioTracks.push_back(v.toInt(-1));
  syncAudioTrackNos(file.project);
  normalizeTrackFlags(file.project);
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
  const bool isOurs = pe.error == QJsonParseError::NoError && root.value(QStringLiteral("app")).toString() == u"e-editor";
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
  return !p.clips.empty() || !p.overlays.empty() || !p.texts.empty() || !p.audios.empty();
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
