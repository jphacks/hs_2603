#include "core/editor.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <set>

#include "core/animtemplates.h"
#include "core/effects.h"
#include "core/filters.h"
#include "core/textstyles.h"
#include "core/transitions.h"

namespace kv {

static constexpr size_t kHistoryLimit = 100;

/** アプリの設定（AI の設定・キーの割り当てと同じ settings.ini。プロジェクトには保存しない） */
static QString editorSettingsPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("settings.ini"));
}

ProjectData emptyProject() {
  ProjectData p;
  p.name = QStringLiteral("無題のプロジェクト");
  p.settings.aspect = Aspect::R16_9;
  p.settings.fps = 30;
  syncTextTrackIds(p);  // テキストの段（はじめは 1 段）の ID
  syncAudioTrackNos(p);
  return p;
}

Editor::Editor() {
  ws_.push_back(makeWorkspace());
  // スポイト: ほかのものを選んだ・再生を始めた・編集場所を切り替えた・その映像が無くなったら、やめる
  connect(this, &Editor::selectionChanged, this, [this] {
    if (!chromaPick_.isEmpty() && selection().id != chromaPick_) setChromaPick({});
  });
  connect(this, &Editor::playingChanged, this, [this] {
    if (playing_) setChromaPick({});
  });
  connect(this, &Editor::workspaceAboutToChange, this, [this] { setChromaPick({}); });
  connect(this, &Editor::projectChanged, this, [this] {
    if (!chromaPick_.isEmpty() && !clip(chromaPick_)) setChromaPick({});
  });
  // オブジェクトマスクの被写体を選ぶとき: スポイトと同じ決まりでやめる
  connect(this, &Editor::selectionChanged, this, [this] {
    if (!maskPick_.isEmpty() && selection().id != maskPick_) setMaskPick({});
  });
  connect(this, &Editor::playingChanged, this, [this] {
    if (playing_) setMaskPick({});
  });
  connect(this, &Editor::workspaceAboutToChange, this, [this] { setMaskPick({}); });
  connect(this, &Editor::projectChanged, this, [this] {
    if (!maskPick_.isEmpty() && !clip(maskPick_)) setMaskPick({});
  });
  {
    QSettings s(editorSettingsPath(), QSettings::IniFormat);
    loop_ = s.value(QStringLiteral("playback/loop"), false).toBool();
    safeMargins_ = s.value(QStringLiteral("preview/safeMargins"), false).toBool();
    grid_ = s.value(QStringLiteral("preview/grid"), false).toBool();
  }
}

Editor& Editor::instance() {
  static Editor* e = new Editor();
  return *e;
}

FitMode defaultFit(const Asset& a, Aspect aspect) {
  if (a.width <= 0 || a.height <= 0) return FitMode::Contain;
  const double ratio = double(a.width) / a.height;
  return std::abs(std::log(ratio / aspectValue(aspect))) > 0.12 ? FitMode::Blur : FitMode::Contain;
}

static VideoClip makeClip(const Asset& a, Aspect aspect) {
  VideoClip c;
  c.id = newId();
  c.assetId = a.id;
  c.isImage = a.kind == AssetKind::Image;
  c.in = 0;
  c.out = c.isImage ? kDefaultImageDuration : std::max(kMinClipDuration, a.duration);
  c.fit = defaultFit(a, aspect);
  c.color = ColorAdjust{};
  return c;
}

/** 重ねるトラックに置いたときの、はじめの大きさ（画面の半分） */
static constexpr double kOverlayScale = 0.5;

/** 重ねるトラックに置く映像・画像。はじめは画面の半分の大きさで、中央に置く（重なっているのが分かるように） */
static OverlayItem makeOverlay(const Asset& a, double start) {
  OverlayItem o;
  static_cast<VideoClip&>(o) = makeClip(a, Aspect::R16_9);
  o.fit = FitMode::Contain;
  o.scale = kOverlayScale;
  o.start = std::max(0.0, start);
  return o;
}

/** 位置・大きさ・回転を変えていない（それらのキーフレームも無い）か。大きさは scale のまま */
static bool layoutUntouched(const VideoClip& c, double scale) {
  for (const AnimProp prop : {AnimProp::X, AnimProp::Y, AnimProp::Scale, AnimProp::Rotation}) {
    if (!c.anim.keys(prop).empty()) return false;
  }
  return std::abs(c.x - 0.5) < 1e-9 && std::abs(c.y - 0.5) < 1e-9 && std::abs(c.scale - scale) < 1e-9 && std::abs(c.rotation) < 1e-9 &&
         !hasStretch(c);
}

static AudioItem makeAudioItem(const Asset& a, double start) {
  AudioItem item;
  item.id = newId();
  item.assetId = a.id;
  item.start = std::max(0.0, start);
  item.in = 0;
  item.out = a.duration;
  item.volume = kMaxVolume;  // 初期音量は 100%
  return item;
}

template <typename T>
static T* findById(std::vector<T>& list, const QString& id) {
  for (auto& x : list) {
    if (x.id == id) return &x;
  }
  return nullptr;
}

template <typename T>
static const T* findById(const std::vector<T>& list, const QString& id) {
  for (const auto& x : list) {
    if (x.id == id) return &x;
  }
  return nullptr;
}

/** メインのビデオクリップか、重ねるトラックの映像・画像（映像の設定は同じ形） */
static VideoClip* findClipAny(ProjectData& p, const QString& id) {
  if (VideoClip* c = findById(p.clips, id)) return c;
  if (OverlayItem* o = findById(p.overlays, id)) return o;
  return nullptr;
}

/** そのものの開始と長さ（テキスト・オーディオ・重ねるトラック）。見つからなければ false */
static bool itemSpan(const ProjectData& p, const Selection& s, double* start, double* duration) {
  if (s.type == Selection::Text) {
    if (const TextItem* t = findById(p.texts, s.id)) {
      *start = t->start;
      *duration = t->duration;
      return true;
    }
  } else if (s.type == Selection::Audio) {
    if (const AudioItem* a = findById(p.audios, s.id)) {
      *start = a->start;
      *duration = audioDuration(*a);
      return true;
    }
  } else if (s.type == Selection::Overlay) {
    if (const OverlayItem* o = findById(p.overlays, s.id)) {
      *start = o->start;
      *duration = overlayDuration(*o, p.settings.fps);
      return true;
    }
  }
  return false;
}

static QString* groupField(ProjectData& p, const Selection& s) {
  if (s.type == Selection::Text) {
    if (TextItem* t = findById(p.texts, s.id)) return &t->group;
  } else if (s.type == Selection::Audio) {
    if (AudioItem* a = findById(p.audios, s.id)) return &a->group;
  } else if (s.type == Selection::Overlay) {
    if (OverlayItem* o = findById(p.overlays, s.id)) return &o->group;
  }
  return nullptr;
}

static QString groupValue(const ProjectData& p, const Selection& s) {
  if (s.type == Selection::Text) {
    if (const TextItem* t = findById(p.texts, s.id)) return t->group;
  } else if (s.type == Selection::Audio) {
    if (const AudioItem* a = findById(p.audios, s.id)) return a->group;
  } else if (s.type == Selection::Overlay) {
    if (const OverlayItem* o = findById(p.overlays, s.id)) return o->group;
  }
  return {};
}

const TimelineLayout& Editor::layout() const {
  if (ws().layoutRevision != ws().revision) {
    ws().layout = layoutClips(ws().project.clips, ws().project.settings.fps);
    ws().layoutRevision = ws().revision;
  }
  return ws().layout;
}

double Editor::contentEnd() const {
  double end = layout().total;
  for (const auto& o : ws().project.overlays) end = std::max(end, o.start + overlayDuration(o, ws().project.settings.fps));
  for (const auto& t : ws().project.texts) end = std::max(end, t.start + t.duration);
  for (const auto& a : ws().project.audios) end = std::max(end, a.start + audioDuration(a));
  return end;
}

double Editor::playEnd() const {
  const double total = layout().total;
  return total > 0 ? total : contentEnd();
}

Selection Editor::validSelection(const Selection& s) const {
  if (s.type == Selection::Clip && findById(ws().project.clips, s.id)) return s;
  if (s.type == Selection::Text && findById(ws().project.texts, s.id)) return s;
  if (s.type == Selection::Audio && findById(ws().project.audios, s.id)) return s;
  if (s.type == Selection::Overlay && findById(ws().project.overlays, s.id)) return s;
  if (s.type == Selection::Transition) {
    // 切れ目の後ろのクリップ（先頭ではないもの）に、トランジションが付いているときだけ
    const auto& clips = ws().project.clips;
    for (size_t i = 1; i < clips.size(); ++i) {
      if (clips[i].id == s.id && clips[i].transIn.type != TransitionType::None) return s;
    }
  }
  return {};
}

void Editor::setDirty(bool d) {
  if (ws().dirty == d) return;
  ws().dirty = d;
  emit dirtyChanged();
  emit workspacesChanged();
}

void Editor::afterProjectChange() {
  ++ws().revision;
  Selection sel = validSelection(ws().selection);
  // 一緒に選んでいたもののうち、無くなったものは外す（主に選んでいたものが無くなったら、残りの最後のものを主にする）
  std::vector<Selection> extra;
  for (const auto& e : ws().extra) {
    if (validSelection(e) && !(e == sel)) extra.push_back(e);
  }
  if (!sel && !extra.empty()) {
    sel = extra.back();
    extra.pop_back();
  }
  const bool selChanged = !(sel == ws().selection) || extra.size() != ws().extra.size();
  ws().selection = sel;
  ws().extra = std::move(extra);
  setDirty(true);
  emit projectChanged();
  if (selChanged) emit selectionChanged();
}

/** どの操作で変えても、音量は 0〜100% の範囲に収める */
static void clampVolumes(ProjectData& p) {
  auto fixClip = [](VideoClip& c) {
    c.volume = std::clamp(c.volume, 0.0, kMaxVolume);
    for (auto& pt : c.volumePoints) pt.volume = std::clamp(pt.volume, 0.0, kMaxVolume);
    c.pan = std::clamp(std::isfinite(c.pan) ? c.pan : 0.0, -kMaxPan, kMaxPan);
    c.x = clampProp(AnimProp::X, c.x);
    c.y = clampProp(AnimProp::Y, c.y);
    c.scale = clampProp(AnimProp::Scale, c.scale);
    c.rotation = clampProp(AnimProp::Rotation, c.rotation);
    c.opacity = clampProp(AnimProp::Opacity, c.opacity);
    clampCropStretch(c);  // 切り抜き・伸縮・クロマキーの値も範囲に収める
    for (auto& e : c.effects) clampEffect(e);
    // オブジェクトマスク: 広げる・ぼかしの範囲。点の無いマスクは持たない
    if (!c.mask.has()) {
      c.mask = ObjectMask{};
    } else {
      c.mask.expand = std::clamp(std::isfinite(c.mask.expand) ? c.mask.expand : 0.0, -kMaxMaskExpand, kMaxMaskExpand);
      c.mask.feather = std::clamp(std::isfinite(c.mask.feather) ? c.mask.feather : 0.0, 0.0, kMaxMaskFeather);
    }
  };
  for (auto& c : p.clips) {
    fixClip(c);
    c.transIn.duration = std::clamp(std::isfinite(c.transIn.duration) ? c.transIn.duration : kDefaultTransitionDuration, kMinTransitionDuration,
                                    kMaxTransitionDuration);
  }
  // 先頭のクリップの前には切れ目が無いので、トランジションは使わない
  if (!p.clips.empty()) p.clips.front().transIn = Transition{};
  // マーカー: 時間の順に、0 より前には置かない
  for (auto& m : p.markers) {
    m.t = std::max(0.0, std::isfinite(m.t) ? m.t : 0.0);
    if (m.id.isEmpty()) m.id = newId();
    if (!m.color.isValid()) m.color = Marker{}.color;
  }
  std::stable_sort(p.markers.begin(), p.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
  for (auto& o : p.overlays) {
    fixClip(o);
    o.transIn = Transition{};  // 重ねるトラックには切れ目が無い
    o.fit = FitMode::Contain;  // 重ねるトラックは、いつも全体表示（余白は透明）
    o.start = std::max(0.0, o.start);
    o.track = std::max(0, o.track);
    // エフェクトレイヤーは素材を使わない（長さは写真と同じ数え方。音・速度・クロマキー・切り抜きは使わない）
    if (o.isEffectLayer()) {
      o.assetId.clear();
      o.isImage = true;
      o.speed = 1;
      o.chroma.enabled = false;
      o.cropL = o.cropT = o.cropR = o.cropB = 0;
      o.flipX = o.flipY = false;
      o.volumePoints.clear();
      o.mask = ObjectMask{};  // 素材が無いので、被写体のマスクも使わない
      o.region.feather = std::clamp(o.region.feather, 0.0, kMaxRegionFeather);
    }
  }
  for (auto& t : p.texts) {
    t.x = clampProp(AnimProp::X, t.x);
    t.y = clampProp(AnimProp::Y, t.y);
    t.scale = clampProp(AnimProp::Scale, t.scale);
    t.rotation = clampProp(AnimProp::Rotation, t.rotation);
    t.opacity = clampProp(AnimProp::Opacity, t.opacity);
    t.letterSpacing = clampProp(AnimProp::LetterSpacing, t.letterSpacing);
  }
  for (auto& a : p.audios) {
    a.volume = std::clamp(a.volume, 0.0, kMaxVolume);
    for (auto& pt : a.volumePoints) pt.volume = std::clamp(pt.volume, 0.0, kMaxVolume);
    a.pan = std::clamp(std::isfinite(a.pan) ? a.pan : 0.0, -kMaxPan, kMaxPan);
  }
}


void Editor::commit(const std::function<bool(ProjectData&)>& fn, const QString& merge) {
  ProjectData next = ws().project;
  if (!fn(next)) return;
  clampVolumes(next);
  // テキストは、同じ段の中で時間が重ならないようにする（時間・段を変えたテキストが重なったら、空いている近くの段へ。
  // ほかのテキストは動かさない）。段の ID は、段の数にそろえる
  resolveTextOverlaps(next, ws().project);
  resolveAudioOverlaps(next, ws().project);  // オーディオも同じ決まり（同じ段の中で重ねない）
  syncTextTrackIds(next);
  syncAudioTrackNos(next);  // オーディオの段の番号（見出しの数字）も、段の数にそろえる
  // 文字を書き換えたテキストは、一部だけ別のフォントにした範囲を、書き換えた所に合わせて付け直す
  // （どこから書き換えても同じ。範囲そのものを変えた操作は、そのまま）
  for (auto& t : next.texts) {
    const TextItem* before = findById(ws().project.texts, t.id);
    if (before && before->text != t.text && before->fontSpans == t.fontSpans) adjustFontSpans(before->text, t.text, t.fontSpans);
    normalizeFontSpans(t);
  }
  normalizeTrackFlags(next);
  // 結果が今と同じ（値を同じに設定し直した・範囲の端で止まったなど）なら、履歴に残さず、保存していない印も付けない
  if (next == ws().project) return;
  const bool mergeable = !merge.isEmpty() && merge == ws().lastMerge;
  if (!mergeable) {
    ws().past.push_back(ws().project);
    if (ws().past.size() > kHistoryLimit) ws().past.erase(ws().past.begin());
  }
  ws().future.clear();
  ws().lastMerge = merge;
  ws().project = std::move(next);
  afterProjectChange();
}

ProjectData Editor::tryChanges(const std::function<void()>& fn) {
  const QSignalBlocker block(this);
  Workspace& live = ws();
  // 元に戻す履歴は試すのに要らないので、写す前に脇へ置く
  std::vector<ProjectData> past = std::move(live.past);
  std::vector<ProjectData> future = std::move(live.future);
  Workspace trial = live;
  live.past = std::move(past);
  live.future = std::move(future);
  // 本物は中身を動かさずに脇へ置き、写しを今の編集場所として試す（vector は移すだけなので、
  // 画面の部品が持っている要素への参照は、戻したあともそのまま使える）
  std::swap(live, trial);
  const QString reject = lastRejectMerge_;
  const int seekToken = seekToken_;
  const bool playing = playing_;
  fn();
  ProjectData result = std::move(ws().project);
  std::swap(ws(), trial);
  lastRejectMerge_ = reject;
  seekToken_ = seekToken;
  playing_ = playing;
  return result;
}

void Editor::beginEditSession() {
  endMerge();  // これまでの変更とは別の操作にする
  EditSession s;
  s.active = true;
  s.uid = ws().uid;
  s.revision = ws().revision;
  s.dirty = ws().dirty;
  s.project = ws().project;
  s.pastSize = ws().past.size();
  if (ws().past.size() >= kHistoryLimit) s.droppedFront = ws().past.front();
  s.future = ws().future;
  editSession_ = std::move(s);
}

void Editor::cancelEditSession() {
  if (!editSession_.active) return;
  EditSession s = std::move(editSession_);
  editSession_ = EditSession();
  if (ws().uid != s.uid || ws().revision == s.revision) return;  // 別の編集場所になった / 何も変えていない
  ws().project = s.project;
  // 始めたあとに足された履歴を取り除く（いっぱいで押し出された先頭は戻す）
  if (s.droppedFront) {
    if (!ws().past.empty()) ws().past.pop_back();
    ws().past.insert(ws().past.begin(), *s.droppedFront);
  }
  while (ws().past.size() > s.pastSize) ws().past.pop_back();
  ws().future = std::move(s.future);
  ws().lastMerge.clear();
  afterProjectChange();
  setDirty(s.dirty);
}

void Editor::endEditSession() {
  if (!editSession_.active) return;
  const EditSession s = std::move(editSession_);
  editSession_ = EditSession();
  // 何かの拍子に変更のまとまりが途切れて、履歴が 2 つ以上に分かれていたら、始めてからの変更を 1 つにまとめる
  // （最初に足された履歴が、始める前の状態。履歴がいっぱいで押し出された場合は、位置がずれるので何もしない）
  if (ws().uid == s.uid && !s.droppedFront && ws().past.size() > s.pastSize + 1) {
    ws().past.erase(ws().past.begin() + std::ptrdiff_t(s.pastSize) + 1, ws().past.end());
  }
  endMerge();
}

void Editor::undo() {
  if (ws().past.empty()) return;
  ws().future.insert(ws().future.begin(), ws().project);
  if (ws().future.size() > kHistoryLimit) ws().future.pop_back();
  ws().project = ws().past.back();
  ws().past.pop_back();
  ws().lastMerge.clear();
  afterProjectChange();
}

void Editor::redo() {
  if (ws().future.empty()) return;
  ws().past.push_back(ws().project);
  if (ws().past.size() > kHistoryLimit) ws().past.erase(ws().past.begin());
  ws().project = ws().future.front();
  ws().future.erase(ws().future.begin());
  ws().lastMerge.clear();
  afterProjectChange();
}

void Editor::select(const Selection& sel) {
  ws().lastMerge.clear();
  if (sel == ws().selection && ws().extra.empty()) return;
  ws().selection = sel;
  ws().extra.clear();
  emit selectionChanged();
}

void Editor::toggleSelect(const Selection& sel) {
  ws().lastMerge.clear();
  Workspace& w = ws();
  if (!groupable(sel.type) || !validSelection(sel)) {
    select(sel);
    return;
  }
  if (!groupable(w.selection.type)) {
    // メインのビデオクリップは一緒に選べないので、選び直す
    select(sel);
    return;
  }
  auto it = std::find(w.extra.begin(), w.extra.end(), sel);
  if (sel == w.selection) {
    // 主に選んでいたものを外す: 一緒に選んでいた最後のものを、主にする
    if (w.extra.empty()) {
      w.selection = {};
    } else {
      w.selection = w.extra.back();
      w.extra.pop_back();
    }
  } else if (it != w.extra.end()) {
    w.extra.erase(it);
  } else {
    // 加える: 今まで主に選んでいたものは一緒に選ぶものへ回し、押したものを主にする（設定欄に出す）
    if (w.selection) w.extra.push_back(w.selection);
    w.selection = sel;
  }
  emit selectionChanged();
}

void Editor::focusInSelection(const Selection& sel) {
  Workspace& w = ws();
  if (sel == w.selection) return;
  auto it = std::find(w.extra.begin(), w.extra.end(), sel);
  if (it == w.extra.end()) {
    // 一緒に選んでいたものでなければ（同じグループのものなど）、そのまま主にする（一緒に選んでいたものは残す）
    if (!inSelectionSet(sel)) {
      select(sel);
      return;
    }
    if (w.selection) w.extra.push_back(w.selection);
    w.selection = sel;
  } else {
    *it = w.selection;
    w.selection = sel;
  }
  ws().lastMerge.clear();
  emit selectionChanged();
}

std::vector<Selection> Editor::selectionSet() const {
  const Workspace& w = ws();
  std::vector<Selection> out;
  if (!w.selection) return out;
  out.push_back(w.selection);
  if (!groupable(w.selection.type)) return out;  // メインのビデオクリップは 1 つずつ
  for (const auto& e : w.extra) {
    if (std::find(out.begin(), out.end(), e) == out.end()) out.push_back(e);
  }
  // 選んだものと同じグループのもの
  std::set<QString> groups;
  for (const auto& s : out) {
    const QString g = groupValue(w.project, s);
    if (!g.isEmpty()) groups.insert(g);
  }
  if (!groups.empty()) {
    auto addIf = [&](Selection::Type type, const QString& id, const QString& g) {
      if (g.isEmpty() || !groups.count(g)) return;
      const Selection s{type, id};
      if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    };
    for (const auto& t : w.project.texts) addIf(Selection::Text, t.id, t.group);
    for (const auto& o : w.project.overlays) addIf(Selection::Overlay, o.id, o.group);
    for (const auto& a : w.project.audios) addIf(Selection::Audio, a.id, a.group);
  }
  return out;
}

bool Editor::inSelectionSet(const Selection& sel) const {
  if (!sel) return false;
  const std::vector<Selection> set = selectionSet();
  return std::find(set.begin(), set.end(), sel) != set.end();
}

QString Editor::groupOf(const Selection& sel) const {
  return groupValue(ws().project, sel);
}

bool Editor::canGroup() const {
  const std::vector<Selection> set = selectionSet();
  if (set.size() < 2) return false;
  // すでに全部が 1 つのグループなら、まとめ直すことはない
  const QString g = groupOf(set.front());
  if (g.isEmpty()) return true;
  for (const auto& s : set) {
    if (groupOf(s) != g) return true;
  }
  return false;
}

bool Editor::canUngroup() const {
  for (const auto& s : selectionSet()) {
    if (!groupOf(s).isEmpty()) return true;
  }
  return false;
}

bool Editor::groupSelection() {
  if (!canGroup()) return false;
  const std::vector<Selection> set = selectionSet();
  const QString g = newId();
  commit([&](ProjectData& p) {
    bool changed = false;
    for (const auto& s : set) {
      if (QString* f = groupField(p, s)) {
        *f = g;
        changed = true;
      }
    }
    return changed;
  });
  toast(QStringLiteral("%1 個をグループにしました。一緒に動かす・削除・複製できます").arg(set.size()), ToastKind::Success);
  emit selectionChanged();  // 選んでいるものの見た目（グループの印）を描き直す
  return true;
}

bool Editor::ungroupSelection() {
  if (!canUngroup()) return false;
  const std::vector<Selection> set = selectionSet();
  commit([&](ProjectData& p) {
    bool changed = false;
    for (const auto& s : set) {
      if (QString* f = groupField(p, s); f && !f->isEmpty()) {
        f->clear();
        changed = true;
      }
    }
    return changed;
  });
  // グループを解いたあとは、主に選んでいたものだけを選ぶ
  select(ws().selection);
  toast(QStringLiteral("グループを解きました"));
  return true;
}

void Editor::seek(double t) {
  const double end = contentEnd();
  ws().currentTime = std::clamp(t, 0.0, std::max(0.0, end));
  ++seekToken_;
  emit timeChanged();
}

// ---- 書き出す範囲 ----------------------------------------------------------------

std::optional<Editor::TimeRange> Editor::exportRange() const {
  if (!ws().range) return std::nullopt;
  // 書き出せるのはビデオの終わりまで。範囲を決めたあとで短くなっていたら、そこまでに収める
  const double total = layout().total;
  const double frame = 1.0 / std::max(1, ws().project.settings.fps);
  TimeRange r = *ws().range;
  r.start = std::clamp(r.start, 0.0, total);
  r.end = std::clamp(r.end, 0.0, total);
  if (r.end - r.start < frame - 1e-9) return std::nullopt;
  return r;
}

void Editor::setRange(double a, double b) {
  const int fps = ws().project.settings.fps;
  const double total = layout().total;
  double s = frameRound(std::clamp(std::min(a, b), 0.0, total), fps);
  double e = frameRound(std::clamp(std::max(a, b), 0.0, total), fps);
  // 1 フレームより短い範囲は作らない
  if (e - s < 1.0 / fps - 1e-9) {
    toast(QStringLiteral("範囲が短すぎます。開始と終了を 1 フレーム以上離してください"));
    return;
  }
  ws().range = TimeRange{s, e};
  emit rangeChanged();
}

void Editor::setRangeStart(double t) {
  const std::optional<TimeRange> r = exportRange();
  const double total = layout().total;
  if (r && t < r->end) setRange(t, r->end);
  else setRange(t, total);
}

void Editor::setRangeEnd(double t) {
  const std::optional<TimeRange> r = exportRange();
  if (r && t > r->start) setRange(r->start, t);
  else setRange(0, t);
}

void Editor::clearRange() {
  if (!ws().range) return;
  ws().range.reset();
  emit rangeChanged();
}

void Editor::setTimeFromEngine(double t) {
  if (t == ws().currentTime) return;
  ws().currentTime = t;
  emit timeChanged();
}

void Editor::play() {
  const double end = playEnd();
  if (end <= 0 || playing_) return;
  if (loop_) {
    // ループ再生: 再生位置がくり返す範囲の外（終わりも含む）なら、範囲の始めから
    const TimeRange r = loopRange();
    if (r.end - r.start >= kMinLoopLength && (ws().currentTime < r.start - 1e-3 || ws().currentTime >= r.end - 1e-3)) {
      ws().currentTime = r.start;
      ++seekToken_;
      emit timeChanged();
    }
  } else if (ws().currentTime >= end - 1e-3) {
    ws().currentTime = 0;
    ++seekToken_;
    emit timeChanged();
  }
  playing_ = true;
  emit playingChanged();
}

void Editor::pause() {
  if (!playing_) return;
  playing_ = false;
  emit playingChanged();
}

void Editor::togglePlay() {
  if (playing_) pause();
  else play();
}

void Editor::setPxPerSec(double v) {
  const double c = std::clamp(v, 4.0, 400.0);
  if (c == ws().pxPerSec) return;
  ws().pxPerSec = c;
  emit zoomChanged();
}

// ---- メディア ---------------------------------------------------------------

bool Editor::hasAsset(const QString& id) const {
  for (const auto& w : ws_) {
    if (w.assets.count(id)) return true;
  }
  return false;
}

const Asset* Editor::asset(const QString& id) const {
  auto it = ws().assets.find(id);
  return it == ws().assets.end() ? nullptr : &it->second;
}

Editor::Workspace* Editor::workspaceOfAsset(const QString& id) {
  // 今の編集場所を先に見る
  if (ws().assets.count(id)) return &ws();
  for (auto& w : ws_) {
    if (w.assets.count(id)) return &w;
  }
  return nullptr;
}

void Editor::addAsset(const Asset& a, int workspaceUid) {
  const int index = workspaceUid < 0 ? active_ : indexOfWorkspaceUid(workspaceUid);
  if (index < 0) return;  // 読み込みの途中で、その編集場所が閉じられた
  Workspace& w = ws_[size_t(index)];
  w.assets[a.id] = a;
  if (std::find(w.assetOrder.begin(), w.assetOrder.end(), a.id) == w.assetOrder.end()) w.assetOrder.push_back(a.id);
  if (index == active_) emit assetsChanged();
}

void Editor::updateAsset(const QString& id, const std::function<void(Asset&)>& fn) {
  Workspace* w = workspaceOfAsset(id);
  if (!w) return;
  fn(w->assets.find(id)->second);
  // 見えていない編集場所のメディアは、切り替えたときにまとめて表示を更新する
  if (w == &ws()) emit assetsChanged();
}

void Editor::relinkAsset(const QString& id, const std::function<void(Asset&)>& fn) {
  Workspace* w = workspaceOfAsset(id);
  if (!w) return;
  fn(w->assets.find(id)->second);
  if (w != &ws()) {
    // 見えていない編集場所: 保存していない印だけ付ける（表示は、切り替えたときにまとめて出し直す）
    if (!w->dirty) {
      w->dirty = true;
      emit workspacesChanged();
    }
    return;
  }
  setDirty(true);
  emit assetsChanged();
  // プレビュー・タイムライン・設定欄・自動保存は、編集データが変わったときと同じように出し直す
  emit projectChanged();
}

static bool usesAsset(const ProjectData& p, const QString& id) {
  // 逆再生しているクリップの元の動画も、使っているものとして数える（逆再生をやめると、元の動画に戻すので）
  for (const auto& c : p.clips) {
    if (c.assetId == id || c.reversedFrom == id) return true;
  }
  for (const auto& o : p.overlays) {
    if (o.assetId == id || o.reversedFrom == id) return true;
  }
  for (const auto& a : p.audios) {
    if (a.assetId == id) return true;
  }
  return false;
}

bool Editor::removeAsset(const QString& id) {
  if (usesAsset(ws().project, id)) {
    // タイムラインで使っている素材は、一覧からも外さない（外すと、タイムラインのクリップが素材を失うため）
    int uses = 0;
    for (const auto& c : ws().project.clips) uses += c.assetId == id || c.reversedFrom == id;
    for (const auto& o : ws().project.overlays) uses += o.assetId == id || o.reversedFrom == id;
    for (const auto& a : ws().project.audios) uses += a.assetId == id;
    const Asset* a = asset(id);
    toast(QStringLiteral("「%1」はタイムラインで %2 か所使っているため、一覧から削除できません。先にタイムラインから外してください")
              .arg(a ? a->name : QStringLiteral("この素材")).arg(uses),
          ToastKind::Error);
    return false;
  }
  bool inHistory = false;
  for (const auto& p : ws().past) inHistory = inHistory || usesAsset(p, id);
  for (const auto& p : ws().future) inHistory = inHistory || usesAsset(p, id);
  // 「元に戻す」で復活する可能性があるときは情報だけ残しておく
  if (!inHistory) ws().assets.erase(id);
  ws().assetOrder.erase(std::remove(ws().assetOrder.begin(), ws().assetOrder.end(), id), ws().assetOrder.end());
  emit assetsChanged();
  return true;
}

const std::vector<QImage>* Editor::thumbs(const QString& assetId) const {
  auto it = thumbs_.find(assetId);
  return it == thumbs_.end() ? nullptr : &it->second;
}

void Editor::setThumbs(const QString& assetId, std::vector<QImage> images) {
  // どの編集場所にも無いメディア（閉じた編集場所のもの）は、メモリを増やさないよう捨てる
  Workspace* w = workspaceOfAsset(assetId);
  if (!w) return;
  thumbs_[assetId] = std::move(images);
  if (w == &ws()) emit thumbsChanged(assetId);
}

const Waveform* Editor::waveform(const QString& assetId) const {
  auto it = waveforms_.find(assetId);
  return it == waveforms_.end() ? nullptr : &it->second;
}

void Editor::setWaveform(const QString& assetId, Waveform w) {
  Workspace* owner = workspaceOfAsset(assetId);
  if (!owner) return;
  waveforms_[assetId] = std::move(w);
  if (owner == &ws()) emit waveformChanged(assetId);
}

// ---- 編集操作 ---------------------------------------------------------------

void Editor::addToTimeline(const std::vector<QString>& assetIds, std::optional<int> index, std::optional<double> at) {
  std::vector<Asset> visual;
  std::vector<Asset> audio;
  for (const auto& id : assetIds) {
    const Asset* a = asset(id);
    if (!a || a->status == AssetStatus::Error) continue;
    (a->kind == AssetKind::Audio ? audio : visual).push_back(*a);
  }
  if (visual.empty() && audio.empty()) return;
  const bool wasEmpty = ws().project.clips.empty();
  Selection first;
  commit([&](ProjectData& p) {
    if (!visual.empty()) {
      std::vector<VideoClip> created;
      for (const auto& a : visual) created.push_back(makeClip(a, p.settings.aspect));
      const int idx = std::clamp(index.value_or(int(p.clips.size())), 0, int(p.clips.size()));
      p.clips.insert(p.clips.begin() + idx, created.begin(), created.end());
      first = {Selection::Clip, created.front().id};
    }
    if (!audio.empty()) {
      double pos = at.value_or(p.audios.empty() ? 0.0 : ws().currentTime);
      QString firstAudio;
      for (const auto& a : audio) {
        AudioItem item = makeAudioItem(a, pos);
        // 新しいオーディオの段は、ここで一度だけ決める（その時間が空いている一番上の段）。ほかのオーディオの段は変えない
        item.track = freeAudioTrack(p.audios, item.start, audioDuration(item));
        pos += audioDuration(item);
        if (firstAudio.isEmpty()) firstAudio = item.id;
        p.audios.push_back(item);
      }
      if (!first) first = {Selection::Audio, firstAudio};
    }
    return true;
  });
  if (first) select(first);
  if (wasEmpty && !visual.empty()) requestFit();
}

void Editor::updateClip(const QString& id, const std::function<void(VideoClip&)>& fn, const QString& merge) {
  commit([&](ProjectData& p) {
    // 重ねるトラックの映像・画像も、映像の設定は同じ形なので、同じ操作で変える
    VideoClip* c = findClipAny(p, id);
    if (!c) return false;
    fn(*c);
    // 重ねるトラック: 速度・長さを変えて同じ段の次のものに重なったら、上の空いている段へ
    if (findById(p.overlays, id)) liftOverlayToFreeLane(p.overlays, id, p.settings.fps);
    return true;
  }, merge);
}

void Editor::moveClip(const QString& id, int toIndex) {
  commit([&](ProjectData& p) {
    auto it = std::find_if(p.clips.begin(), p.clips.end(), [&](const VideoClip& c) { return c.id == id; });
    if (it == p.clips.end()) return false;
    const int from = int(it - p.clips.begin());
    VideoClip c = *it;
    p.clips.erase(it);
    const int idx = std::clamp(toIndex, 0, int(p.clips.size()));
    if (idx == from) return false;
    p.clips.insert(p.clips.begin() + idx, c);
    return true;
  });
}

int Editor::textTrackFor(double start, double duration) const {
  return freeTextTrack(ws().project.texts, start, duration);
}

void Editor::moveTextTrack(const QString& id, int delta) {
  commit([&](ProjectData& p) {
    const TextItem* t = findById(p.texts, id);
    if (!t || delta == 0) return false;
    const int from = t->track;
    int lastOther = -1;
    for (const auto& x : p.texts) {
      if (x.id != id) lastOther = std::max(lastOther, x.track);
    }
    using K = TextTrackTarget::Kind;
    TextTrackTarget target;
    if (delta < 0) {
      if (from <= 0) return false;
      // 1 つ上の段。その時間にほかのテキストがあるときは、重ならないよう、そのテキストの上に新しい段を作って入れる（手前に出る）
      const int k = from - 1;
      target = textTrackFree(p.texts, k, t->start, t->duration, id) ? TextTrackTarget{K::Lane, k} : TextTrackTarget{K::Insert, k};
    } else {
      // 一番下の段のさらに下へ 1 段だけ増やせる（自分だけの段をどんどん下へ送ることはできない）
      if (from > lastOther) return false;
      const int k = from + 1;
      // 1 つ下の段。その時間にほかのテキストがあるときは、そのテキストの下に新しい段を作って入れる（後ろに回る）
      target = k > lastOther || textTrackFree(p.texts, k, t->start, t->duration, id) ? TextTrackTarget{K::Lane, k} : TextTrackTarget{K::Insert, k + 1};
    }
    return moveTextToTrack(p, id, target);
  });
}

bool Editor::swapTracks(Selection::Type kind, int a, int b) {
  bool done = false;
  commit([&](ProjectData& p) {
    done = kind == Selection::Text ? swapTextTracks(p, a, b) : kind == Selection::Audio ? swapAudioTracks(p, a, b) : false;
    return done;
  });
  return done;
}

bool Editor::placeTextTrack(const QString& id, const TextTrackTarget& target, const QString& merge) {
  bool moved = false;
  commit([&](ProjectData& p) {
    moved = moveTextToTrack(p, id, target);
    return moved;
  }, merge);
  return moved;
}

double Editor::newTextDuration(double start, double wanted) const {
  const double total = layout().total;
  double duration = std::max(kMinTextDuration, wanted);
  // 動画の終わりをまたぐときだけ、終わりまでに縮める（終わりより後ろに置いたときは、そのままの長さ）
  if (total > 1 && start < total && start + duration > total) duration = std::max(kMinTextDuration, total - start);
  return duration;
}

QString Editor::addTextAt(const QString& presetId, double start) {
  TextItem item = makeText(presetId, frameRound(std::max(0.0, start), ws().project.settings.fps));
  // 長さは、テンプレートなら保存したときの長さ、組み込みのスタイルなら決まった長さ（動画の終わりをまたぐときは終わりまで）
  item.duration = newTextDuration(item.start, item.duration);
  commit([&](ProjectData& p) {
    // 新しいテキストの段は、ここで一度だけ決める。ほかのテキストの段は変えない
    item.track = freeTextTrack(p.texts, item.start, item.duration);
    p.texts.push_back(item);
    return true;
  });
  select({Selection::Text, item.id});
  return item.id;
}

QString Editor::addText(const QString& presetId, std::optional<double> at) {
  // ボタン・キー・ダブルクリックで追加するときは、動画の終わりぎわなら少し手前にずらして、見える位置に置く
  const double total = layout().total;
  double start = at.value_or(ws().currentTime);
  if (total > 0 && start >= total - 0.3) start = std::max(0.0, total - presetDuration(presetId));
  return addTextAt(presetId, start);
}

QString Editor::addOverlays(const std::vector<QString>& assetIds, std::optional<double> at, std::optional<int> track) {
  std::vector<Asset> visual;
  for (const auto& id : assetIds) {
    const Asset* a = asset(id);
    if (!a || a->status == AssetStatus::Error || a->kind == AssetKind::Audio) continue;
    visual.push_back(*a);
  }
  if (visual.empty()) return {};
  const int fps = ws().project.settings.fps;
  QString first;
  commit([&](ProjectData& p) {
    double pos = frameRound(std::max(0.0, at.value_or(ws().currentTime)), fps);
    for (const auto& a : visual) {
      OverlayItem o = makeOverlay(a, pos);
      const double dur = overlayDuration(o, fps);
      // 指定の段が空いていればそこへ。ふさがっていれば（指定が無ければ）、重ならない段へ（ほかのものの段は変えない）
      if (track && *track >= 0 && overlayTrackFree(p.overlays, *track, o.start, dur, fps)) o.track = *track;
      else o.track = freeOverlayTrack(p.overlays, o.start, dur, fps);
      if (first.isEmpty()) first = o.id;
      p.overlays.push_back(o);
      pos += dur;
    }
    return true;
  });
  if (!first.isEmpty()) select({Selection::Overlay, first});
  return first;
}

QString Editor::addEffectLayer(const QString& presetId, std::optional<double> at, std::optional<int> track) {
  const int fps = ws().project.settings.fps;
  QString id;
  commit([&](ProjectData& p) {
    OverlayItem o = makeEffectLayer(presetId, p.settings.aspect);
    o.id = newId();
    o.start = frameRound(std::max(0.0, at.value_or(ws().currentTime)), fps);
    const double dur = overlayDuration(o, fps);
    if (track && *track >= 0 && overlayTrackFree(p.overlays, *track, o.start, dur, fps)) {
      o.track = *track;
    } else {
      // その時間に重ねている映像・レイヤーより上の段（下にあるものすべてに効果が掛かる）。ふさがっていれば、さらに上へ
      int lane = 0;
      for (const auto& x : p.overlays) {
        if (x.start < o.start + dur - 1e-6 && o.start < x.start + overlayDuration(x, fps) - 1e-6) lane = std::max(lane, x.track + 1);
      }
      while (!overlayTrackFree(p.overlays, lane, o.start, dur, fps)) ++lane;
      o.track = lane;
    }
    id = o.id;
    p.overlays.push_back(o);
    return true;
  });
  if (!id.isEmpty()) select({Selection::Overlay, id});
  return id;
}

void Editor::updateOverlay(const QString& id, const std::function<void(OverlayItem&)>& fn, const QString& merge) {
  commit([&](ProjectData& p) {
    OverlayItem* o = findById(p.overlays, id);
    if (!o) return false;
    fn(*o);
    return true;
  }, merge);
}

void Editor::moveOverlayTrack(const QString& id, int delta) {
  commit([&](ProjectData& p) {
    OverlayItem* o = findById(p.overlays, id);
    if (!o || delta == 0) return false;
    // 一番上の段のさらに上へ 1 段だけ増やせる（自分だけの段をどんどん上へ送ることはできない）
    int lastOther = -1;
    for (const auto& x : p.overlays) {
      if (x.id != id) lastOther = std::max(lastOther, x.track);
    }
    const int next = std::clamp(o->track + delta, 0, lastOther + 1);
    if (next == o->track) return false;
    o->track = next;
    return true;
  });
}

bool Editor::moveClipToOverlay(const QString& id, double start, int track) {
  bool moved = false;
  commit([&](ProjectData& p) {
    auto it = std::find_if(p.clips.begin(), p.clips.end(), [&](const VideoClip& c) { return c.id == id; });
    if (it == p.clips.end()) return false;
    OverlayItem o;
    static_cast<VideoClip&>(o) = *it;  // ID も映像の設定も、そのまま
    o.transIn = Transition{};          // 重ねるトラックには切れ目が無い
    o.start = std::max(0.0, start);
    // 一番上の段のさらに上へ 1 段だけ増やせる
    int last = -1;
    for (const auto& x : p.overlays) last = std::max(last, x.track);
    o.track = std::clamp(track, 0, last + 1);
    o.fit = FitMode::Contain;  // 重ねるトラックは、いつも全体表示（余白は透明）
    // 画面いっぱいのままのクリップは、重ねるトラックに追加したときと同じ大きさにする（下の映像が隠れてしまわないように）
    if (layoutUntouched(o, 1.0)) o.scale = kOverlayScale;
    p.clips.erase(it);
    p.overlays.push_back(o);
    moved = true;
    return true;
  });
  if (moved) select({Selection::Overlay, id});
  return moved;
}

bool Editor::moveOverlayToMain(const QString& id, int index, const QString& merge) {
  bool moved = false;
  commit([&](ProjectData& p) {
    auto it = std::find_if(p.overlays.begin(), p.overlays.end(), [&](const OverlayItem& o) { return o.id == id; });
    // エフェクトレイヤーは素材が無いので、メインのビデオには置けない
    if (it == p.overlays.end() || it->isEffectLayer()) return false;
    VideoClip c = *it;  // ID も映像の設定も、そのまま（重ねるトラックの開始・段・グループは持っていかない）
    // 重ねるトラックに追加したときの大きさのままなら、メインのビデオのいつもの大きさ（画面いっぱい）に戻す
    if (layoutUntouched(c, kOverlayScale)) c.scale = 1;
    // 表示方法は、メインのビデオに追加したときと同じ（画面と縦横比が違う素材は「ぼかし背景」）
    if (const Asset* a = asset(c.assetId)) c.fit = defaultFit(*a, p.settings.aspect);
    p.overlays.erase(it);
    p.clips.insert(p.clips.begin() + std::clamp(index, 0, int(p.clips.size())), c);
    moved = true;
    return true;
  }, merge);
  if (moved) select({Selection::Clip, id});
  return moved;
}

void Editor::updateText(const QString& id, const std::function<void(TextItem&)>& fn, const QString& merge) {
  commit([&](ProjectData& p) {
    TextItem* t = findById(p.texts, id);
    if (!t) return false;
    fn(*t);
    return true;
  }, merge);
}

/** 音量キーフレームの持ち主（オーディオ、または音声つきの映像）。ID で探す */
struct VolumeTarget {
  std::vector<VolumePoint>* points = nullptr;
  double volume = kMaxVolume;
  double* base = nullptr;  // キーフレームが無いときの音量
  double start = 0;        // タイムライン上の開始
  double duration = 0;
};

static VolumeTarget volumeTarget(ProjectData& p, const QString& id) {
  VolumeTarget t;
  if (AudioItem* a = findById(p.audios, id)) {
    t.points = &a->volumePoints;
    t.volume = a->volume;
    t.base = &a->volume;
    t.start = a->start;
    t.duration = audioDuration(*a);
    return t;
  }
  if (VideoClip* c = findById(p.clips, id)) {
    t.points = &c->volumePoints;
    t.volume = c->volume;
    t.base = &c->volume;
    for (const auto& l : layoutClips(p.clips, p.settings.fps).items) {
      if (l.clip.id == id) {
        t.start = l.start;
        t.duration = l.duration;
      }
    }
  } else if (OverlayItem* o = findById(p.overlays, id)) {
    t.points = &o->volumePoints;
    t.volume = o->volume;
    t.base = &o->volume;
    t.start = o->start;
    t.duration = overlayDuration(*o, p.settings.fps);
  }
  return t;
}

bool Editor::setVolumeValue(const QString& id, double value, const QString& merge) {
  const double now = ws().currentTime;
  const bool ak = autoKey_;
  bool accepted = false;
  value = std::clamp(value, 0.0, kMaxVolume);
  commit([&](ProjectData& p) {
    const VolumeTarget tg = volumeTarget(p, id);
    if (!tg.points) return false;
    auto& pts = *tg.points;
    if (pts.empty()) {
      // キーフレームが無いときは、全体の音量を変える
      accepted = true;
      if (std::abs(*tg.base - value) < 1e-9) return false;
      *tg.base = value;
      return true;
    }
    const double local = std::clamp(now - tg.start, 0.0, tg.duration);
    for (auto& pt : pts) {
      if (std::abs(pt.t - local) < kMinVolumePointGap * 0.99) {
        // 再生位置にキーフレームがある: その音量を変える
        accepted = true;
        if (std::abs(pt.volume - value) < 1e-9) return false;
        pt.volume = value;
        return true;
      }
    }
    if (!ak) return false;  // 自動記録がOFF: 意図せずキーフレームを増やさない
    accepted = true;
    auto it = std::lower_bound(pts.begin(), pts.end(), local, [](const VolumePoint& pt, double t) { return pt.t < t; });
    pts.insert(it, VolumePoint{local, value});
    return true;
  }, merge);
  if (!accepted && merge != lastRejectMerge_) {
    lastRejectMerge_ = merge;
    toast(QStringLiteral("この音量にはキーフレームがあります。◆を押すか、自動記録をオンにすると、再生位置に記録できます"));
  } else if (accepted) {
    lastRejectMerge_.clear();
  }
  return accepted;
}

int Editor::addVolumePoint(const QString& audioId, double local) {
  int result = -1;
  commit([&](ProjectData& p) {
    const VolumeTarget tg = volumeTarget(p, audioId);
    if (!tg.points) return false;
    local = std::clamp(local, 0.0, tg.duration);
    auto& pts = *tg.points;
    // 近すぎる場所には増やさず、そこにあるキーフレームを返す（音量が急に変わってクリックノイズになるのを防ぐ）
    for (size_t i = 0; i < pts.size(); ++i) {
      if (std::abs(pts[i].t - local) < kMinVolumePointGap) {
        result = int(i);
        return false;
      }
    }
    // 追加した場所の今の音量で作るので、追加しただけでは音は変わらない
    const VolumePoint np{local, volumeAt(pts, tg.volume, local)};
    auto it = std::lower_bound(pts.begin(), pts.end(), local, [](const VolumePoint& pt, double t) { return pt.t < t; });
    result = int(it - pts.begin());
    pts.insert(it, np);
    return true;
  });
  return result;
}

void Editor::moveVolumePoint(const QString& audioId, int index, double local, double volume, const QString& merge) {
  commit([&](ProjectData& p) {
    const VolumeTarget tg = volumeTarget(p, audioId);
    if (!tg.points || index < 0 || index >= int(tg.points->size())) return false;
    auto& pts = *tg.points;
    // 隣のキーフレームを追い越さない・近づきすぎない範囲で動かす。クリップの外には出さない
    const double lo = index > 0 ? pts[size_t(index) - 1].t + kMinVolumePointGap : 0.0;
    const double hi = index + 1 < int(pts.size()) ? pts[size_t(index) + 1].t - kMinVolumePointGap : tg.duration;
    VolumePoint np{lo <= hi ? std::clamp(local, lo, hi) : pts[size_t(index)].t, std::clamp(volume, 0.0, kMaxVolume)};
    if (std::abs(np.t - pts[size_t(index)].t) < 1e-9 && std::abs(np.volume - pts[size_t(index)].volume) < 1e-9) return false;
    pts[size_t(index)] = np;
    return true;
  }, merge);
}

void Editor::removeVolumePoint(const QString& audioId, int index) {
  commit([&](ProjectData& p) {
    const VolumeTarget tg = volumeTarget(p, audioId);
    if (!tg.points || index < 0 || index >= int(tg.points->size())) return false;
    tg.points->erase(tg.points->begin() + index);
    return true;
  });
}

void Editor::clearVolumePoints(const QString& audioId) {
  commit([&](ProjectData& p) {
    const VolumeTarget tg = volumeTarget(p, audioId);
    if (!tg.points || tg.points->empty()) return false;
    tg.points->clear();
    return true;
  });
}

void Editor::updateAudio(const QString& id, const std::function<void(AudioItem&)>& fn, const QString& merge) {
  commit([&](ProjectData& p) {
    AudioItem* a = findById(p.audios, id);
    if (!a) return false;
    fn(*a);
    return true;
  }, merge);
}

void Editor::deleteSelection() {
  // トランジションを選んでいるとき: そのトランジションだけを外す（クリップはそのまま）
  if (ws().selection.type == Selection::Transition) {
    removeTransition(ws().selection.id);
    return;
  }
  // 一緒に選んでいるものと、同じグループのものも、まとめて消す（メインのビデオクリップは 1 つずつ）
  const std::vector<Selection> set = selectionSet();
  if (set.empty()) return;
  commit([&](ProjectData& p) {
    bool changed = false;
    auto erase = [&](auto& list, Selection::Type type) {
      const auto before = list.size();
      list.erase(std::remove_if(list.begin(), list.end(),
                                [&](const auto& x) { return std::find(set.begin(), set.end(), Selection{type, x.id}) != set.end(); }),
                 list.end());
      changed |= list.size() != before;
    };
    erase(p.clips, Selection::Clip);
    erase(p.overlays, Selection::Overlay);
    erase(p.texts, Selection::Text);
    erase(p.audios, Selection::Audio);
    return changed;
  });
  select({});
  // 再生位置が終わりより後ろになったら戻す
  if (ws().currentTime > contentEnd()) seek(contentEnd());
}

void Editor::duplicateSelection() {
  const Selection sel = ws().selection;
  if (!sel || sel.type == Selection::Transition) return;
  const std::vector<Selection> set = selectionSet();
  const int fps = ws().project.settings.fps;
  if (set.size() > 1) {
    // まとめて複製: 全体の終わりのすぐ後ろへ、並びと間隔をそのままに写す。グループは、写したもの同士の新しいグループにする
    double first = 1e18;
    double last = 0;
    for (const auto& s : set) {
      double st = 0, du = 0;
      if (itemSpan(ws().project, s, &st, &du)) {
        first = std::min(first, st);
        last = std::max(last, st + du);
      }
    }
    if (first > last) return;
    const double shift = frameRound(last - first, fps);
    std::map<QString, QString> groups;  // 元のグループ → 写したもののグループ
    auto mapGroup = [&](const QString& g) {
      if (g.isEmpty()) return QString();
      auto it = groups.find(g);
      if (it == groups.end()) it = groups.insert({g, newId()}).first;
      return it->second;
    };
    std::vector<Selection> copies;
    commit([&](ProjectData& p) {
      for (const auto& s : set) {
        const QString nid = newId();
        if (s.type == Selection::Text) {
          const TextItem* t = findById(p.texts, s.id);
          if (!t) continue;
          TextItem copy = *t;
          copy.id = nid;
          copy.start = t->start + shift;
          copy.group = mapGroup(t->group);
          if (!textTrackFree(p.texts, copy.track, copy.start, copy.duration)) copy.track = freeTextTrack(p.texts, copy.start, copy.duration);
          p.texts.push_back(copy);
        } else if (s.type == Selection::Audio) {
          const AudioItem* a = findById(p.audios, s.id);
          if (!a) continue;
          AudioItem copy = *a;
          copy.id = nid;
          copy.start = a->start + shift;
          copy.group = mapGroup(a->group);
          if (!audioTrackFree(p.audios, copy.track, copy.start, audioDuration(copy))) copy.track = freeAudioTrack(p.audios, copy.start, audioDuration(copy));
          p.audios.push_back(copy);
        } else if (s.type == Selection::Overlay) {
          const OverlayItem* o = findById(p.overlays, s.id);
          if (!o) continue;
          OverlayItem copy = *o;
          copy.id = nid;
          copy.start = o->start + shift;
          copy.group = mapGroup(o->group);
          const double dur = overlayDuration(copy, fps);
          if (!overlayTrackFree(p.overlays, copy.track, copy.start, dur, fps)) copy.track = freeOverlayTrack(p.overlays, copy.start, dur, fps);
          p.overlays.push_back(copy);
        } else {
          continue;
        }
        copies.push_back({s.type, nid});
      }
      return !copies.empty();
    });
    if (copies.empty()) return;
    // 写したものを選ぶ（主に選んでいたものの写しを主に）
    ws().selection = copies.front();
    ws().extra.assign(copies.begin() + 1, copies.end());
    ws().lastMerge.clear();
    emit selectionChanged();
    return;
  }
  const QString newIdValue = newId();
  bool done = false;
  commit([&](ProjectData& p) {
    if (sel.type == Selection::Clip) {
      auto it = std::find_if(p.clips.begin(), p.clips.end(), [&](const VideoClip& c) { return c.id == sel.id; });
      if (it == p.clips.end()) return false;
      VideoClip copy = *it;
      copy.id = newIdValue;
      copy.transIn = Transition{};  // 元のクリップのすぐ後ろ（間はそのままつながる）
      p.clips.insert(it + 1, copy);
    } else if (sel.type == Selection::Text) {
      const TextItem* t = findById(p.texts, sel.id);
      if (!t) return false;
      TextItem copy = *t;
      copy.id = newIdValue;
      copy.start = t->start + t->duration;
      copy.group.clear();  // 1 つだけ写したものは、元のグループに入れない
      // 元と同じ段が空いていればそこへ。ふさがっていれば、重ならない段へ（元のテキストの段は変えない）
      if (!textTrackFree(p.texts, t->track, copy.start, copy.duration)) copy.track = freeTextTrack(p.texts, copy.start, copy.duration);
      p.texts.push_back(copy);
    } else if (sel.type == Selection::Overlay) {
      const OverlayItem* o = findById(p.overlays, sel.id);
      if (!o) return false;
      OverlayItem copy = *o;
      copy.id = newIdValue;
      copy.start = o->start + overlayDuration(*o, fps);
      copy.group.clear();
      const double dur = overlayDuration(copy, fps);
      if (!overlayTrackFree(p.overlays, copy.track, copy.start, dur, fps)) copy.track = freeOverlayTrack(p.overlays, copy.start, dur, fps);
      p.overlays.push_back(copy);
    } else {
      const AudioItem* a = findById(p.audios, sel.id);
      if (!a) return false;
      AudioItem copy = *a;
      copy.id = newIdValue;
      copy.start = a->start + audioDuration(*a);
      copy.group.clear();
      // 元と同じ段が空いていればそこへ。ふさがっていれば、重ならない段へ（元のオーディオの段は変えない）
      if (!audioTrackFree(p.audios, copy.track, copy.start, audioDuration(copy))) copy.track = freeAudioTrack(p.audios, copy.start, audioDuration(copy));
      p.audios.push_back(copy);
    }
    done = true;
    return true;
  });
  if (done) select({sel.type, newIdValue});
}

int Editor::copySelection() {
  const std::vector<Selection> set = selectionSet();
  if (set.empty()) return 0;
  const ProjectData& p = ws().project;
  ItemClipboard c;
  std::vector<QString> assetIds;
  auto useAsset = [&](const QString& id) {
    if (std::find(assetIds.begin(), assetIds.end(), id) == assetIds.end()) assetIds.push_back(id);
  };
  double origin = 1e18;
  for (const auto& s : set) {
    if (s.type == Selection::Clip) {
      if (const VideoClip* v = findById(p.clips, s.id)) {
        c.clips.push_back(*v);
        useAsset(v->assetId);
      }
    } else if (s.type == Selection::Text) {
      if (const TextItem* t = findById(p.texts, s.id)) {
        c.texts.push_back(*t);
        origin = std::min(origin, t->start);
      }
    } else if (s.type == Selection::Audio) {
      if (const AudioItem* a = findById(p.audios, s.id)) {
        c.audios.push_back(*a);
        useAsset(a->assetId);
        origin = std::min(origin, a->start);
      }
    } else if (s.type == Selection::Overlay) {
      if (const OverlayItem* o = findById(p.overlays, s.id)) {
        c.overlays.push_back(*o);
        useAsset(o->assetId);
        origin = std::min(origin, o->start);
      }
    }
  }
  if (c.empty()) return 0;
  c.origin = origin < 1e17 ? origin : 0;
  for (const QString& id : assetIds) {
    if (const Asset* a = asset(id)) c.assets.push_back(*a);
  }
  itemClip_ = std::move(c);
  return itemClip_.count();
}

int Editor::pasteClipboard(std::vector<Asset>* added, std::optional<double> atTime, std::optional<int> clipIndex) {
  if (itemClip_.empty()) return 0;
  const ItemClipboard c = itemClip_;  // 貼り付けの途中で写し直されても困らないように
  // メディア: 同じファイル（同じ種類）が今の一覧にあればそれを使い、無ければ写しを足す（ID は新しくする）
  std::map<QString, QString> assetMap;
  bool assetsAdded = false;
  for (const Asset& a : c.assets) {
    QString found;
    const QString path = QFileInfo(a.path).absoluteFilePath();
    for (const QString& id : ws().assetOrder) {
      const Asset* b = asset(id);
      if (b && b->kind == a.kind && QFileInfo(b->path).absoluteFilePath().compare(path, Qt::CaseInsensitive) == 0) {
        found = id;
        break;
      }
    }
    if (found.isEmpty()) {
      Asset copy = a;
      copy.id = newId();
      // サムネイル・波形は、作り直すまでの間も同じものを出す
      if (auto it = thumbs_.find(a.id); it != thumbs_.end()) thumbs_[copy.id] = it->second;
      if (auto it = waveforms_.find(a.id); it != waveforms_.end()) waveforms_[copy.id] = it->second;
      ws().assets[copy.id] = copy;
      ws().assetOrder.push_back(copy.id);
      if (added) added->push_back(copy);
      assetsAdded = true;
      found = copy.id;
    }
    assetMap[a.id] = found;
  }
  if (assetsAdded) emit assetsChanged();
  auto mapAsset = [&](QString& id) {
    if (auto it = assetMap.find(id); it != assetMap.end()) id = it->second;
  };
  // グループは、貼り付けたもの同士の新しいグループにする（1 つだけなら、グループに入れない）
  std::map<QString, QString> groups;
  const bool single = c.count() == 1;
  auto mapGroup = [&](const QString& g) {
    if (g.isEmpty() || single) return QString();
    auto it = groups.find(g);
    if (it == groups.end()) it = groups.insert({g, newId()}).first;
    return it->second;
  };
  const double at = std::max(0.0, atTime.value_or(ws().currentTime));
  std::vector<Selection> pasted;
  commit([&](ProjectData& p) {
    const int fps = p.settings.fps;
    const double shift = at - c.origin;
    if (!c.clips.empty()) {
      // メインのビデオクリップ: 決めた位置か、その時間にあるクリップのうしろ（クリップの頭なら、その前）に入れる
      const TimelineLayout layout = layoutClips(p.clips, fps);
      int index = int(p.clips.size());
      if (clipIndex) {
        index = std::clamp(*clipIndex, 0, int(p.clips.size()));
      } else {
        for (size_t i = 0; i < layout.items.size(); ++i) {
          const auto& l = layout.items[i];
          if (at < l.end - 1e-6) {
            index = at <= l.start + 1e-6 ? int(i) : int(i) + 1;
            break;
          }
        }
      }
      for (const VideoClip& v : c.clips) {
        VideoClip copy = v;
        copy.id = newId();
        copy.transIn = Transition{};  // 切れ目のトランジションは写さない（Premiere と同じく、切れ目に付くもの）
        mapAsset(copy.assetId);
        p.clips.insert(p.clips.begin() + std::clamp(index, 0, int(p.clips.size())), copy);
        ++index;
        pasted.push_back({Selection::Clip, copy.id});
      }
    }
    for (const TextItem& t : c.texts) {
      TextItem copy = t;
      copy.id = newId();
      copy.start = frameRound(std::max(0.0, t.start + shift), fps);
      copy.group = mapGroup(t.group);
      // 元と同じ段が空いていればそこへ。ふさがっていれば、重ならない段へ
      if (!textTrackFree(p.texts, copy.track, copy.start, copy.duration)) copy.track = freeTextTrack(p.texts, copy.start, copy.duration);
      p.texts.push_back(copy);
      pasted.push_back({Selection::Text, copy.id});
    }
    for (const AudioItem& a : c.audios) {
      AudioItem copy = a;
      copy.id = newId();
      mapAsset(copy.assetId);
      copy.start = frameRound(std::max(0.0, a.start + shift), fps);
      copy.group = mapGroup(a.group);
      if (!audioTrackFree(p.audios, copy.track, copy.start, audioDuration(copy))) copy.track = freeAudioTrack(p.audios, copy.start, audioDuration(copy));
      p.audios.push_back(copy);
      pasted.push_back({Selection::Audio, copy.id});
    }
    for (const OverlayItem& o : c.overlays) {
      OverlayItem copy = o;
      copy.id = newId();
      mapAsset(copy.assetId);
      copy.start = frameRound(std::max(0.0, o.start + shift), fps);
      copy.group = mapGroup(o.group);
      const double dur = overlayDuration(copy, fps);
      if (!overlayTrackFree(p.overlays, copy.track, copy.start, dur, fps)) copy.track = freeOverlayTrack(p.overlays, copy.start, dur, fps);
      p.overlays.push_back(copy);
      pasted.push_back({Selection::Overlay, copy.id});
    }
    return !pasted.empty();
  });
  if (pasted.empty()) return 0;
  // 貼り付けたものを選ぶ（一緒に選べるのはテキスト・オーディオ・重ねるものだけ）
  ws().selection = pasted.front();
  ws().extra.clear();
  for (size_t i = 1; i < pasted.size(); ++i) {
    if (groupable(pasted[i].type) && groupable(pasted.front().type)) ws().extra.push_back(pasted[i]);
  }
  ws().lastMerge.clear();
  emit selectionChanged();
  return int(pasted.size());
}

void Editor::splitAtPlayhead() {
  const int fps = ws().project.settings.fps;
  const double t = frameRound(ws().currentTime, fps);
  const Selection sel = ws().selection;
  const QString newIdValue = newId();

  if (sel.type == Selection::Text) {
    const TextItem* item = text(sel.id);
    if (item && t > item->start + kMinTextDuration && t < item->start + item->duration - kMinTextDuration) {
      const TextItem orig = *item;
      commit([&](ProjectData& p) {
        for (size_t i = 0; i < p.texts.size(); ++i) {
          if (p.texts[i].id != orig.id) continue;
          TextItem second = orig;
          second.id = newIdValue;
          second.start = t;
          second.duration = orig.start + orig.duration - t;
          p.texts[i].duration = t - orig.start;
          // アニメーションのキーフレームは前半・後半に振り分ける（分け目の値は、どちらにも残す）
          splitAnimation(orig.anim, t - orig.start, p.texts[i].anim, second.anim);
          p.texts.insert(p.texts.begin() + i + 1, second);
          return true;
        }
        return false;
      });
      select({Selection::Text, newIdValue});
      return;
    }
  }

  if (sel.type == Selection::Audio) {
    const AudioItem* item = audio(sel.id);
    if (item && t > item->start + kMinClipDuration && t < item->start + audioDuration(*item) - kMinClipDuration) {
      const AudioItem orig = *item;
      const double cut = orig.in + (t - orig.start);
      commit([&](ProjectData& p) {
        for (size_t i = 0; i < p.audios.size(); ++i) {
          if (p.audios[i].id != orig.id) continue;
          AudioItem second = orig;
          second.id = newIdValue;
          second.start = t;
          second.in = cut;
          second.fadeIn = 0;
          p.audios[i].out = cut;
          p.audios[i].fadeOut = 0;
          // 音量キーフレームは、前半・後半に振り分ける（分け目で音量が途切れないよう、同じ音量の点を両側に置く）
          splitVolumePoints(orig, t - orig.start, p.audios[i].volumePoints, second.volumePoints);
          p.audios.insert(p.audios.begin() + i + 1, second);
          return true;
        }
        return false;
      });
      select({Selection::Audio, newIdValue});
      return;
    }
  }

  if (sel.type == Selection::Overlay) {
    const OverlayItem* item = overlay(sel.id);
    if (!item) return;
    const double dur = overlayDuration(*item, fps);
    const int localFrames = int(std::floor((t - item->start) * fps + 0.5));
    const int totalFrames = int(std::floor(dur * fps + 0.5));
    if (localFrames < 1 || localFrames > totalFrames - 1) {
      toast(QStringLiteral("再生ヘッドの位置が、選んでいる映像の中にありません"));
      return;
    }
    const double local = double(localFrames) / fps;
    const OverlayItem orig = *item;
    const double cut = orig.isImage ? orig.in + local : orig.in + local * orig.speed;
    commit([&](ProjectData& p) {
      for (size_t i = 0; i < p.overlays.size(); ++i) {
        if (p.overlays[i].id != orig.id) continue;
        OverlayItem second = orig;
        second.id = newIdValue;
        second.start = orig.start + local;
        second.in = cut;
        second.fadeIn = 0;
        p.overlays[i].out = cut;
        p.overlays[i].fadeOut = 0;
        // キーフレームは前半・後半に振り分ける（分け目の値は、どちらにも残す）
        splitAnimation(orig.anim, local, p.overlays[i].anim, second.anim);
        splitVolumePointList(orig.volumePoints, orig.volume, local, p.overlays[i].volumePoints, second.volumePoints);
        p.overlays.insert(p.overlays.begin() + i + 1, second);
        return true;
      }
      return false;
    });
    select({Selection::Overlay, newIdValue});
    return;
  }

  const TimelineLayout& lay = layout();
  const ClipLayout* hit = nullptr;
  if (sel.type == Selection::Clip) {
    for (const auto& l : lay.items) {
      if (l.clip.id == sel.id && t > l.start && t < l.end) hit = &l;
    }
  }
  if (!hit) hit = findClipAt(lay, t);
  if (!hit || t <= hit->start || t >= hit->end) {
    toast(QStringLiteral("再生ヘッドの位置にクリップがありません"));
    return;
  }
  const int localFrames = int(std::floor((t - hit->start) * fps + 0.5));
  const int totalFrames = int(std::floor(hit->duration * fps + 0.5));
  if (localFrames < 1 || localFrames > totalFrames - 1) {
    toast(QStringLiteral("クリップの端では分割できません"));
    return;
  }
  const double local = double(localFrames) / fps;
  const VideoClip c = hit->clip;
  const double cut = c.isImage ? c.in + local : c.in + local * c.speed;
  commit([&](ProjectData& p) {
    for (size_t i = 0; i < p.clips.size(); ++i) {
      if (p.clips[i].id != c.id) continue;
      VideoClip second = c;
      second.id = newIdValue;
      second.in = cut;
      second.fadeIn = 0;
      second.transIn = Transition{};  // 分けた所は、そのままつながる（トランジションは前半の頭に残す）
      p.clips[i].out = cut;
      p.clips[i].fadeOut = 0;
      // キーフレームは前半・後半に振り分ける。時間はクリップの先頭からのタイムライン上の秒数
      splitAnimation(c.anim, local, p.clips[i].anim, second.anim);
      splitVolumePointList(c.volumePoints, c.volume, local, p.clips[i].volumePoints, second.volumePoints);
      p.clips.insert(p.clips.begin() + i + 1, second);
      return true;
    }
    return false;
  });
  select({Selection::Clip, newIdValue});
}

// ---- Premiere と同じ編集の近道 -------------------------------------------------------------------

namespace {

/** フェードを、長さ duration に収め直す（イン・アウトが重ならないように） */
template <typename T>
void refitFades(T& x, double duration) {
  x.fadeIn = fitFade(x.fadeIn, duration, 0);
  x.fadeOut = fitFade(x.fadeOut, duration, x.fadeIn);
}

/**
 * リップル: 時刻 at より後ろ（at ちょうども含む）に始まるテキスト・オーディオ・重ねるもの・マーカーを、delta 秒ずらす（負なら前へ詰める）。
 * 詰めるとき（delta < 0）は、取り除いた時間 [at + delta, at) にかかっていたものからも、その時間を取り除く
 * （中に収まっていたものは消す。途中から始まるものは頭を、途中で終わるものは終わりを切り落とす。前から（ちょうど始まりから）
 * 続いていたものは、始まりはそのままで、その分だけ短くする）。
 * 取り除いた時間の中にあったマーカーは、詰めた所へ寄せる。
 * 重ねるものは、ずらした先の段がふさがっていれば、上の空いている段へ（下の段へは落とさない。エフェクトレイヤーが、
 * 効果を掛けていたものの下に回らないように。テキスト・オーディオは commit で同じように直る）
 */
void rippleFreeItems(ProjectData& p, double at, double delta) {
  const int fps = p.settings.fps;
  constexpr double eps = 1e-6;
  const double removed = delta < 0 ? -delta : 0;
  const double cutA = at - removed;  // 取り除く時間の始まり（詰めるときだけ。ずらすだけなら at と同じ）
  const double cutB = at;
  auto shifted = [&](double start) { return frameRound(std::max(0.0, start + delta), fps); };
  struct Cut {
    bool keep = true;
    bool touched = false;  // 取り除く時間にかかっていた（長さ・頭を変えた）
    double start = 0;
    double head = 0;  // 頭から切り落とした秒数
    double length = 0;
  };
  auto cutSpan = [&](double s, double len, double minLen) {
    Cut c;
    c.start = s;
    c.length = len;
    if (s >= cutB - eps) {
      c.start = shifted(s);  // 後ろ: そのままずらす
      return c;
    }
    const double e = s + len;
    if (removed <= 0 || e <= cutA + eps) return c;  // 前（またはずらすだけ）: そのまま
    c.touched = true;
    const double keepHead = std::max(0.0, cutA - s);  // 取り除く時間より前の部分
    const double keepTail = std::max(0.0, e - cutB);  // 取り除く時間より後ろの部分
    if (s > cutA + eps) {
      c.head = cutB - s;  // 途中から始まる: 頭を切り落として、詰めた所から始める（後ろの映像と、ずれないように）
      c.start = frameRound(cutA, fps);
      c.length = keepTail;
    } else {
      // 前（または取り除く時間のちょうど始まり）から始まる: 始まりはそのままで、終わりを切り落とす / 後ろまで続いていたら、
      // 取り除く分だけ短くする（最初から流している BGM などが、途中から始まらないように）
      c.length = keepHead + keepTail;
    }
    c.length = frameRound(c.length, fps);
    c.keep = c.length >= minLen - eps;
    return c;
  };
  std::set<QString> dropped;
  for (auto& t : p.texts) {
    const Cut c = cutSpan(t.start, t.duration, kMinTextDuration);
    if (!c.keep) {
      dropped.insert(t.id);
      continue;
    }
    if (c.head > eps) t.anim = trimAnimationStart(t.anim, c.head);
    t.start = c.start;
    t.duration = c.length;
  }
  for (auto& a : p.audios) {
    const Cut c = cutSpan(a.start, audioDuration(a), kMinClipDuration);
    if (!c.keep) {
      dropped.insert(a.id);
      continue;
    }
    if (c.touched) {
      if (c.head > eps) {
        a.volumePoints = trimVolumePointsStart(a.volumePoints, a.volume, c.head);
        a.in += c.head;
      }
      a.out = a.in + c.length;
      refitFades(a, audioDuration(a));
    }
    a.start = c.start;
  }
  std::vector<QString> movedOverlays;
  for (auto& o : p.overlays) {
    const Cut c = cutSpan(o.start, overlayDuration(o, fps), kMinClipDuration);
    if (!c.keep) {
      dropped.insert(o.id);
      continue;
    }
    if (c.touched) {
      if (c.head > eps) {
        o.anim = trimAnimationStart(o.anim, c.head);
        o.volumePoints = trimVolumePointsStart(o.volumePoints, o.volume, c.head);
        if (o.isImage) o.out -= c.head;
        else o.in += c.head * o.speed;
      }
      o.out = o.isImage ? o.in + c.length : o.in + c.length * o.speed;
      refitFades(o, overlayDuration(o, fps));
    }
    if (std::abs(c.start - o.start) > eps) movedOverlays.push_back(o.id);
    o.start = c.start;
  }
  if (!dropped.empty()) {
    std::erase_if(p.texts, [&](const TextItem& x) { return dropped.count(x.id) > 0; });
    std::erase_if(p.audios, [&](const AudioItem& x) { return dropped.count(x.id) > 0; });
    std::erase_if(p.overlays, [&](const OverlayItem& x) { return dropped.count(x.id) > 0; });
  }
  for (const QString& id : movedOverlays) {
    OverlayItem* o = findById(p.overlays, id);
    if (!o) continue;
    const double dur = overlayDuration(*o, fps);
    int lane = o->track;
    while (!overlayTrackFree(p.overlays, lane, o->start, dur, fps, o->id)) ++lane;
    o->track = lane;
  }
  for (auto& m : p.markers) {
    if (m.t >= cutB - eps) m.t = shifted(m.t);
    else if (removed > 0 && m.t > cutA + eps) m.t = frameRound(cutA, fps);
  }
}

/** メインのビデオのクリップ c を、先頭から local 秒（フレームにそろえたもの）で 2 つに分ける（splitAtPlayhead と同じ分け方） */
void splitClipValue(const VideoClip& c, double local, const QString& secondId, VideoClip& first, VideoClip& second) {
  const double cut = c.isImage ? c.in + local : c.in + local * c.speed;
  first = c;
  second = c;
  second.id = secondId;
  second.in = cut;
  second.fadeIn = 0;
  second.transIn = Transition{};
  first.out = cut;
  first.fadeOut = 0;
  splitAnimation(c.anim, local, first.anim, second.anim);
  splitVolumePointList(c.volumePoints, c.volume, local, first.volumePoints, second.volumePoints);
}

}  // namespace

bool Editor::rippleTrimToPlayhead(bool start) {
  const int fps = ws().project.settings.fps;
  const double t = frameRound(ws().currentTime, fps);
  const Selection sel = ws().selection;
  // 選んでいるテキスト・オーディオ・重ねるものに再生位置がかかっていれば、その端を再生位置まで詰める（ほかは動かさない）
  if (sel.type == Selection::Text || sel.type == Selection::Audio || sel.type == Selection::Overlay) {
    double s = 0, d = 0;
    if (itemSpan(ws().project, sel, &s, &d) && t > s + 1e-6 && t < s + d - 1e-6) {
      const double minDur = sel.type == Selection::Text ? kMinTextDuration : kMinClipDuration;
      const double keep = start ? s + d - t : t - s;
      if (keep < minDur - 1e-6) {
        toast(QStringLiteral("残りが短すぎるので詰められません"));
        return false;
      }
      const double cut = start ? t - s : s + d - t;
      commit([&](ProjectData& p) {
        if (sel.type == Selection::Text) {
          TextItem* x = findById(p.texts, sel.id);
          if (!x) return false;
          if (start) {
            x->anim = trimAnimationStart(x->anim, cut);
            x->start = t;
          }
          x->duration = keep;
          return true;
        }
        if (sel.type == Selection::Audio) {
          AudioItem* x = findById(p.audios, sel.id);
          if (!x) return false;
          if (start) {
            x->volumePoints = trimVolumePointsStart(x->volumePoints, x->volume, cut);
            x->in += cut;
            x->start = t;
          } else {
            x->out = x->in + keep;
          }
          refitFades(*x, audioDuration(*x));
          return true;
        }
        OverlayItem* x = findById(p.overlays, sel.id);
        if (!x) return false;
        if (start) {
          x->anim = trimAnimationStart(x->anim, cut);
          x->volumePoints = trimVolumePointsStart(x->volumePoints, x->volume, cut);
          if (x->isImage) x->out -= cut;
          else x->in += cut * x->speed;
          x->start = t;
        } else {
          x->out = x->isImage ? x->in + keep : x->in + keep * x->speed;
        }
        refitFades(*x, overlayDuration(*x, fps));
        return true;
      });
      return true;
    }
  }
  // メインのビデオ: 再生位置のクリップを詰めて、後ろ（ほかの段のものも）を前へ詰める
  const TimelineLayout& lay = layout();
  const ClipLayout* hit = findClipAt(lay, t);
  if (!hit || t <= hit->start + 1e-6 || t >= hit->end - 1e-6) {
    toast(QStringLiteral("再生位置にクリップがありません（詰めたいクリップの途中に、再生位置を置いてください）"));
    return false;
  }
  const int localFrames = int(std::floor((t - hit->start) * fps + 0.5));
  const int totalFrames = int(std::floor(hit->duration * fps + 0.5));
  if (localFrames < 1 || localFrames > totalFrames - 1) {
    toast(QStringLiteral("クリップの端では詰められません"));
    return false;
  }
  const VideoClip c = hit->clip;
  const double clipStart = hit->start;
  const double clipEnd = hit->end;
  const double local = double(localFrames) / fps;
  commit([&](ProjectData& p) {
    VideoClip* x = findById(p.clips, c.id);
    if (!x) return false;
    if (start) {
      // 先頭から再生位置までを取り除く
      if (x->isImage) x->out -= local;
      else x->in += local * x->speed;
      x->anim = trimAnimationStart(c.anim, local);
      x->volumePoints = trimVolumePointsStart(c.volumePoints, c.volume, local);
      rippleFreeItems(p, clipStart + local, -local);
    } else {
      // 再生位置から終わりまでを取り除く
      const double removed = (clipEnd - clipStart) - local;
      x->out = x->isImage ? x->in + local : x->in + local * x->speed;
      rippleFreeItems(p, clipEnd, -removed);
    }
    refitFades(*x, clipDuration(*x, fps));
    return true;
  });
  // Q のときは、詰めた所（クリップの新しい始まり）へ再生位置を動かす
  if (start) seek(clipStart);
  return true;
}

int Editor::splitAllAtPlayhead() {
  const int fps = ws().project.settings.fps;
  const double t = frameRound(ws().currentTime, fps);
  int count = 0;
  QString firstSecond;
  commit([&](ProjectData& p) {
    count = 0;
    // メインのビデオ
    const TimelineLayout lay = layoutClips(p.clips, fps);
    if (const ClipLayout* hit = findClipAt(lay, t); hit && t > hit->start && t < hit->end) {
      const int lf = int(std::floor((t - hit->start) * fps + 0.5));
      const int tf = int(std::floor(hit->duration * fps + 0.5));
      if (lf >= 1 && lf <= tf - 1) {
        const size_t i = size_t(hit->index);
        VideoClip first, second;
        splitClipValue(p.clips[i], double(lf) / fps, newId(), first, second);
        p.clips[i] = first;
        p.clips.insert(p.clips.begin() + long(i) + 1, second);
        if (firstSecond.isEmpty()) firstSecond = second.id;
        ++count;
      }
    }
    // 重ねるトラック
    for (size_t i = 0; i < p.overlays.size(); ++i) {
      const OverlayItem o = p.overlays[i];
      const int lf = int(std::floor((t - o.start) * fps + 0.5));
      const int tf = int(std::floor(overlayDuration(o, fps) * fps + 0.5));
      if (lf < 1 || lf > tf - 1) continue;
      const double local = double(lf) / fps;
      const double cut = o.isImage ? o.in + local : o.in + local * o.speed;
      OverlayItem second = o;
      second.id = newId();
      second.start = o.start + local;
      second.in = cut;
      second.fadeIn = 0;
      p.overlays[i].out = cut;
      p.overlays[i].fadeOut = 0;
      splitAnimation(o.anim, local, p.overlays[i].anim, second.anim);
      splitVolumePointList(o.volumePoints, o.volume, local, p.overlays[i].volumePoints, second.volumePoints);
      p.overlays.insert(p.overlays.begin() + long(i) + 1, second);
      ++i;
      ++count;
    }
    // テキスト
    for (size_t i = 0; i < p.texts.size(); ++i) {
      const TextItem orig = p.texts[i];
      if (!(t > orig.start + kMinTextDuration && t < orig.start + orig.duration - kMinTextDuration)) continue;
      TextItem second = orig;
      second.id = newId();
      second.start = t;
      second.duration = orig.start + orig.duration - t;
      p.texts[i].duration = t - orig.start;
      splitAnimation(orig.anim, t - orig.start, p.texts[i].anim, second.anim);
      p.texts.insert(p.texts.begin() + long(i) + 1, second);
      ++i;
      ++count;
    }
    // オーディオ
    for (size_t i = 0; i < p.audios.size(); ++i) {
      const AudioItem orig = p.audios[i];
      if (!(t > orig.start + kMinClipDuration && t < orig.start + audioDuration(orig) - kMinClipDuration)) continue;
      const double cut = orig.in + (t - orig.start);
      AudioItem second = orig;
      second.id = newId();
      second.start = t;
      second.in = cut;
      second.fadeIn = 0;
      p.audios[i].out = cut;
      p.audios[i].fadeOut = 0;
      splitVolumePoints(orig, t - orig.start, p.audios[i].volumePoints, second.volumePoints);
      p.audios.insert(p.audios.begin() + long(i) + 1, second);
      ++i;
      ++count;
    }
    return count > 0;
  });
  if (count == 0) {
    toast(QStringLiteral("再生位置にかかっているものがありません"));
  } else {
    if (!firstSecond.isEmpty()) select({Selection::Clip, firstSecond});
    toast(QStringLiteral("再生位置で %1 個を分割しました").arg(count));
  }
  return count;
}

bool Editor::rippleDeleteSelection() {
  const Selection sel = ws().selection;
  if (!sel) return false;
  if (sel.type == Selection::Transition) return removeTransition(sel.id);
  if (sel.type == Selection::Clip) {
    double s = 0, d = 0;
    if (!clipSpan(sel.id, &s, &d)) return false;
    const QString id = sel.id;
    commit([&](ProjectData& p) {
      auto it = std::find_if(p.clips.begin(), p.clips.end(), [&](const VideoClip& c) { return c.id == id; });
      if (it == p.clips.end()) return false;
      p.clips.erase(it);
      // メインのビデオは自然に詰まる。後ろのテキスト・オーディオ・重ねるものも一緒に詰めて、ずれないようにする
      rippleFreeItems(p, s + d, -d);
      return true;
    });
    select({});
    if (ws().currentTime > contentEnd()) seek(contentEnd());
    return true;
  }
  // テキスト・オーディオ・重ねるもの（一緒に選んだもの・同じグループのものも）: 消して、同じ段の後ろを詰める
  const std::vector<Selection> set = selectionSet();
  bool done = false;
  commit([&](ProjectData& p) {
    const int fps = p.settings.fps;
    struct Gap {
      Selection::Type kind;
      int track;
      double end;
      double duration;
    };
    std::vector<Gap> gaps;
    auto inSet = [&](Selection::Type type, const QString& id) { return std::find(set.begin(), set.end(), Selection{type, id}) != set.end(); };
    for (const auto& t : p.texts) {
      if (inSet(Selection::Text, t.id)) gaps.push_back({Selection::Text, t.track, t.start + t.duration, t.duration});
    }
    for (const auto& a : p.audios) {
      if (inSet(Selection::Audio, a.id)) gaps.push_back({Selection::Audio, a.track, a.start + audioDuration(a), audioDuration(a)});
    }
    for (const auto& o : p.overlays) {
      if (inSet(Selection::Overlay, o.id)) gaps.push_back({Selection::Overlay, o.track, o.start + overlayDuration(o, fps), overlayDuration(o, fps)});
    }
    if (gaps.empty()) return false;
    std::erase_if(p.texts, [&](const TextItem& x) { return inSet(Selection::Text, x.id); });
    std::erase_if(p.audios, [&](const AudioItem& x) { return inSet(Selection::Audio, x.id); });
    std::erase_if(p.overlays, [&](const OverlayItem& x) { return inSet(Selection::Overlay, x.id); });
    auto shiftOf = [&](Selection::Type kind, int track, double start) {
      double s = 0;
      for (const Gap& g : gaps) {
        if (g.kind == kind && g.track == track && g.end <= start + 1e-6) s += g.duration;
      }
      return s;
    };
    for (auto& t : p.texts) t.start = frameRound(std::max(0.0, t.start - shiftOf(Selection::Text, t.track, t.start)), fps);
    for (auto& a : p.audios) a.start = frameRound(std::max(0.0, a.start - shiftOf(Selection::Audio, a.track, a.start)), fps);
    for (auto& o : p.overlays) o.start = frameRound(std::max(0.0, o.start - shiftOf(Selection::Overlay, o.track, o.start)), fps);
    done = true;
    return true;
  });
  if (done) select({});
  return done;
}

bool Editor::gapAt(Selection::Type kind, int track, double t, double* start, double* end) const {
  const ProjectData& p = ws().project;
  const int fps = p.settings.fps;
  double g0 = 0;
  double g1 = 1e18;
  auto consider = [&](int tr, double s, double d) {
    if (tr != track) return false;
    if (t >= s - 1e-6 && t < s + d - 1e-6) return true;  // その上にある
    if (s + d <= t + 1e-6) g0 = std::max(g0, s + d);
    if (s > t + 1e-6) g1 = std::min(g1, s);
    return false;
  };
  if (kind == Selection::Text) {
    for (const auto& x : p.texts) {
      if (consider(x.track, x.start, x.duration)) return false;
    }
  } else if (kind == Selection::Audio) {
    for (const auto& x : p.audios) {
      if (consider(x.track, x.start, audioDuration(x))) return false;
    }
  } else if (kind == Selection::Overlay) {
    for (const auto& x : p.overlays) {
      if (consider(x.track, x.start, overlayDuration(x, fps))) return false;
    }
  } else {
    return false;
  }
  if (g1 > 1e17 || g1 - g0 < 0.5 / fps) return false;
  if (start) *start = g0;
  if (end) *end = g1;
  return true;
}

bool Editor::closeGap(Selection::Type kind, int track, double t) {
  double g0 = 0, g1 = 0;
  if (!gapAt(kind, track, t, &g0, &g1)) return false;
  const double d = g1 - g0;
  commit([&](ProjectData& p) {
    const int fps = p.settings.fps;
    auto moveIt = [&](double& start, int tr) {
      if (tr == track && start >= g1 - 1e-6) start = frameRound(std::max(0.0, start - d), fps);
    };
    if (kind == Selection::Text) {
      for (auto& x : p.texts) moveIt(x.start, x.track);
    } else if (kind == Selection::Audio) {
      for (auto& x : p.audios) moveIt(x.start, x.track);
    } else {
      for (auto& x : p.overlays) moveIt(x.start, x.track);
    }
    return true;
  });
  return true;
}

int Editor::importSubtitles(const std::vector<SubtitleCue>& cues, const QString& presetId) {
  if (cues.empty()) return 0;
  const int fps = ws().project.settings.fps;
  int count = 0;
  commit([&](ProjectData& p) {
    std::vector<TextItem> items;
    for (const SubtitleCue& c : cues) {
      const double s = frameRound(std::max(0.0, c.start), fps);
      const double e = std::max(frameRound(c.end, fps), s + std::ceil(kMinTextDuration * fps - 1e-9) / fps);  // 終わりもフレームにそろえる
      TextItem t = makeText(presetId, s, c.text);
      t.duration = e - s;
      items.push_back(t);
    }
    std::stable_sort(items.begin(), items.end(), [](const TextItem& a, const TextItem& b) { return a.start < b.start; });
    // 字幕の時間に、ほかのテキストが無い段（無ければ一番下に新しい段）
    const double spanStart = items.front().start;
    double spanEnd = 0;
    for (const TextItem& t : items) spanEnd = std::max(spanEnd, t.start + t.duration);
    const int existing = p.texts.empty() ? 0 : textTrackCount(p.texts);
    int base = existing;
    for (int k = 0; k < existing; ++k) {
      const bool busy = std::any_of(p.texts.begin(), p.texts.end(), [&](const TextItem& x) {
        return x.track == k && x.start < spanEnd - 1e-6 && spanStart < x.start + x.duration - 1e-6;
      });
      if (!busy) {
        base = k;
        break;
      }
    }
    TextItem* prev = nullptr;
    for (size_t i = 0; i < items.size(); ++i) {
      TextItem& t = items[i];
      t.track = base;
      // 前の字幕と重なるとき: 前の字幕の終わりぎわに重なっただけなら、前の字幕をこの字幕の始まりで切る。
      // 前の字幕の中に収まる（長い字幕の途中に出る短い字幕）・切ると短くなりすぎるなら、前の字幕は切らずに、この字幕を空いている段へ
      if (prev && prev->start + prev->duration > t.start + 1e-6) {
        const bool inside = prev->start + prev->duration >= t.start + t.duration - 1e-6;
        if (!inside && t.start - prev->start >= kMinTextDuration - 1e-9) {
          prev->duration = t.start - prev->start;
        } else {
          std::vector<TextItem> all = p.texts;
          all.insert(all.end(), items.begin(), items.begin() + long(i));  // ここまでに置いた字幕
          t.track = freeTextTrack(all, t.start, t.duration);
        }
      }
      if (t.track == base) prev = &t;
    }
    for (const TextItem& t : items) p.texts.push_back(t);
    count = int(items.size());
    return true;
  });
  return count;
}

bool Editor::seekToEdit(bool forward) {
  const ProjectData& p = ws().project;
  const int fps = p.settings.fps;
  std::vector<double> pts{0};
  for (const auto& l : layout().items) {
    pts.push_back(l.start);
    pts.push_back(l.end);
  }
  for (const auto& o : p.overlays) {
    pts.push_back(o.start);
    pts.push_back(o.start + overlayDuration(o, fps));
  }
  for (const auto& t : p.texts) {
    pts.push_back(t.start);
    pts.push_back(t.start + t.duration);
  }
  for (const auto& a : p.audios) {
    pts.push_back(a.start);
    pts.push_back(a.start + audioDuration(a));
  }
  const double now = ws().currentTime;
  const double eps = 0.5 / fps;
  std::optional<double> best;
  for (double x : pts) {
    if (forward ? x > now + eps : x < now - eps) {
      if (!best || (forward ? x < *best : x > *best)) best = x;
    }
  }
  if (!best) return false;
  pause();
  seek(*best);
  return true;
}

bool Editor::nudgeSelection(int frames) {
  const std::vector<Selection> set = selectionSet();
  if (set.empty() || frames == 0) return false;
  if (std::any_of(set.begin(), set.end(), [](const Selection& s) { return !groupable(s.type); })) {
    toast(QStringLiteral("1 フレームずつ動かせるのは、テキスト・オーディオ・重ねるトラックのものです（ビデオのクリップは、ドラッグで並べ替えます）"));
    return false;
  }
  bool blocked = false;
  // 続けて押したときは 1 つの操作にまとめる（同じものを動かしているときだけ。間に再生位置を動かしたら、別の操作にする）
  QStringList keyIds;
  for (const Selection& s : set) keyIds << QString::number(int(s.type)) + QLatin1Char(':') + s.id;
  keyIds.sort();
  const QString merge = QStringLiteral("nudge|") + keyIds.join(QLatin1Char('|'));
  if (ws().lastMerge == merge && nudgeSeekToken_ != seekToken_) endMerge();
  commit([&](ProjectData& p) {
    const int fps = p.settings.fps;
    const double delta = double(frames) / fps;
    auto inSet = [&](Selection::Type type, const QString& id) { return std::find(set.begin(), set.end(), Selection{type, id}) != set.end(); };
    // 先にフレームにそろえてから、重なりを調べる
    for (auto& t : p.texts) {
      if (inSet(Selection::Text, t.id)) t.start = frameRound(t.start + delta, fps);
    }
    for (auto& a : p.audios) {
      if (inSet(Selection::Audio, a.id)) a.start = frameRound(a.start + delta, fps);
    }
    for (auto& o : p.overlays) {
      if (inSet(Selection::Overlay, o.id)) o.start = frameRound(o.start + delta, fps);
    }
    // 0:00 より前・同じ段のほかのもの（一緒に動かさないもの）に重なるなら、動かさない
    auto overlaps = [](double s1, double d1, double s2, double d2) { return s1 < s2 + d2 - 1e-6 && s2 < s1 + d1 - 1e-6; };
    for (const auto& t : p.texts) {
      if (!inSet(Selection::Text, t.id)) continue;
      if (t.start < -1e-6) blocked = true;
      for (const auto& x : p.texts) {
        if (!inSet(Selection::Text, x.id) && x.track == t.track && overlaps(t.start, t.duration, x.start, x.duration)) blocked = true;
      }
    }
    for (const auto& a : p.audios) {
      if (!inSet(Selection::Audio, a.id)) continue;
      if (a.start < -1e-6) blocked = true;
      for (const auto& x : p.audios) {
        if (!inSet(Selection::Audio, x.id) && x.track == a.track && overlaps(a.start, audioDuration(a), x.start, audioDuration(x))) blocked = true;
      }
    }
    for (const auto& o : p.overlays) {
      if (!inSet(Selection::Overlay, o.id)) continue;
      if (o.start < -1e-6) blocked = true;
      for (const auto& x : p.overlays) {
        if (!inSet(Selection::Overlay, x.id) && x.track == o.track && overlaps(o.start, overlayDuration(o, fps), x.start, overlayDuration(x, fps))) blocked = true;
      }
    }
    return !blocked;
  }, merge);
  nudgeSeekToken_ = seekToken_;
  if (blocked) toast(QStringLiteral("隣のもの（または 0:00）に当たるので、それ以上は動かせません"));
  return !blocked;
}

QString Editor::detachAudio(const QString& clipId) {
  const VideoClip* c = clip(clipId);
  if (!c) return {};
  const Asset* a = asset(c->assetId);
  if (c->isImage || !a || !a->hasAudio) {
    toast(QStringLiteral("音声の無い映像です"));
    return {};
  }
  if (c->muted) {
    toast(QStringLiteral("音を消している映像です（「音を消す」を外してから切り離してください）"));
    return {};
  }
  if (std::abs(c->speed - 1) > 1e-6) {
    toast(QStringLiteral("速度を変えた映像の音は切り離せません（速度を 1x に戻してから切り離してください）"));
    return {};
  }
  double start = 0, dur = 0;
  if (!clipSpan(clipId, &start, &dur)) return {};
  AudioItem item;
  item.id = newId();
  item.assetId = c->assetId;
  item.start = start;
  item.in = c->in;
  item.out = c->in + dur;
  item.volume = c->volume;
  item.volumePoints = c->volumePoints;
  item.fadeIn = c->fadeIn;
  item.fadeOut = c->fadeOut;
  item.pan = c->pan;
  commit([&](ProjectData& p) {
    VideoClip* x = findClipAny(p, clipId);
    if (!x) return false;
    x->muted = true;
    item.track = freeAudioTrack(p.audios, item.start, audioDuration(item));
    p.audios.push_back(item);
    return true;
  });
  select({Selection::Audio, item.id});
  toast(QStringLiteral("音声を切り離しました（映像の音は消しました。オーディオの段で、音だけを動かせます）"), ToastKind::Success);
  return item.id;
}

bool Editor::insertFrameHold(const QString& clipId, double local, const QString& imageAssetId, double seconds,
                             const std::optional<ObjectMask>& holdMask) {
  const Asset* img = asset(imageAssetId);
  if (!img || img->kind != AssetKind::Image) return false;
  const int fps = ws().project.settings.fps;
  QString holdId;
  commit([&](ProjectData& p) {
    auto it = std::find_if(p.clips.begin(), p.clips.end(), [&](const VideoClip& c) { return c.id == clipId; });
    if (it == p.clips.end()) return false;
    const size_t i = size_t(it - p.clips.begin());
    const VideoClip c = *it;
    double clipStart = 0;
    for (const auto& l : layoutClips(p.clips, fps).items) {
      if (l.clip.id == clipId) clipStart = l.start;
    }
    const int tf = clipFrames(c, fps);
    const int lf = std::clamp(int(std::floor(local * fps + 0.5)), 0, tf);
    const double at = double(lf) / fps;
    // 写真のクリップ: 元のクリップの見た目（表示方法・色・エフェクト・反転・切り抜き・クロマキー）のまま、動きはその時間の値で止める
    VideoClip hold = c;
    hold.id = newId();
    hold.assetId = imageAssetId;
    hold.isImage = true;
    hold.reversedFrom.clear();
    hold.in = 0;
    hold.out = std::max(1.0 / fps, frameRound(std::clamp(seconds, kMinClipDuration, 600.0), fps));
    hold.speed = 1;
    hold.muted = false;
    hold.volumePoints.clear();
    hold.fadeIn = hold.fadeOut = 0;
    hold.transIn = Transition{};
    const LayerState ls = layerStateAt(c, std::min(at, std::max(0.0, double(tf - 1) / fps)));
    hold.x = ls.x;
    hold.y = ls.y;
    hold.scale = ls.scale;
    hold.rotation = ls.rotation;
    hold.opacity = ls.opacity;
    hold.anim = Animation();
    // オブジェクトマスク: 止めたフレームのマスク（呼ぶ側が、その 1 フレームのマスクを作って渡す）。無ければ外す
    hold.mask = holdMask ? *holdMask : ObjectMask{};
    holdId = hold.id;
    if (lf >= 1 && lf <= tf - 1) {
      VideoClip first, second;
      splitClipValue(c, at, newId(), first, second);
      p.clips[i] = first;
      p.clips.insert(p.clips.begin() + long(i) + 1, hold);
      p.clips.insert(p.clips.begin() + long(i) + 2, second);
    } else if (lf <= 0) {
      // クリップの頭: その前に入れる（トランジションは、写真のクリップの頭に移す）
      hold.transIn = c.transIn;
      p.clips[i].transIn = Transition{};
      p.clips.insert(p.clips.begin() + long(i), hold);
    } else {
      p.clips.insert(p.clips.begin() + long(i) + 1, hold);
    }
    // 後ろのテキスト・オーディオ・重ねるもの・マーカーも、入れた長さだけ後ろへずらす（映像とずれないように）
    rippleFreeItems(p, clipStart + at, hold.out);
    return true;
  });
  if (holdId.isEmpty()) return false;
  select({Selection::Clip, holdId});
  return true;
}

bool Editor::setClipReversed(const QString& clipId, const QString& reversedAssetId) {
  const VideoClip* c = clip(clipId);
  if (!c || c->isImage) return false;
  const bool on = !reversedAssetId.isEmpty();
  if (on == !c->reversedFrom.isEmpty()) return false;  // もうその向き
  // 逆向きの動画の長さ D で、素材の [in, out] を [D - out, D - in] に直す（やめるときも同じ D で戻すので、元の値にぴったり戻る）
  const Asset* rev = asset(on ? reversedAssetId : c->assetId);
  const QString target = on ? reversedAssetId : c->reversedFrom;
  if (!rev || rev->duration <= 0 || !asset(target)) return false;
  const double D = rev->duration;
  bool done = false;
  commit([&](ProjectData& p) {
    VideoClip* x = findClipAny(p, clipId);
    if (!x) return false;
    const double in = std::clamp(D - x->out, 0.0, D);
    const double out = std::clamp(D - x->in, in, D);
    x->reversedFrom = on ? x->assetId : QString();
    x->assetId = target;
    x->in = in;
    x->out = std::max(out, in + 0.01);
    // オブジェクトマスク: 素材の向きが変わるので、作ったマスクは使えない（クリックした所は逆向きの時刻に直して残す。「トラック」で作り直せる）
    if (x->mask.has()) {
      x->mask.file.clear();
      for (MaskPrompt& pr : x->mask.prompts) pr.t = std::clamp(D - pr.t, 0.0, D);
    }
    done = true;
    return true;
  });
  return done;
}

bool Editor::copyAttributes() {
  const Selection s = ws().selection;
  if (s.type != Selection::Clip && s.type != Selection::Overlay) return false;
  const VideoClip* c = clip(s.id);
  if (!c) return false;
  attrClip_ = *c;
  const OverlayItem* o = overlay(s.id);
  attrFromLayer_ = o && o->isEffectLayer();
  emit attributesChanged();
  return true;
}

int Editor::pasteAttributes(int parts) {
  if (!attrClip_ || parts == 0) return 0;
  const VideoClip src = *attrClip_;
  const bool srcLayer = attrFromLayer_;
  std::vector<QString> ids;
  for (const Selection& s : selectionSet()) {
    if (s.type == Selection::Clip || s.type == Selection::Overlay) ids.push_back(s.id);
  }
  if (ids.empty()) return 0;
  int count = 0;
  commit([&](ProjectData& p) {
    const int fps = p.settings.fps;
    count = 0;
    for (const QString& id : ids) {
      VideoClip* x = findClipAny(p, id);
      if (!x) continue;
      OverlayItem* ov = findById(p.overlays, id);
      const bool layer = ov && ov->isEffectLayer();
      // 映像とエフェクトレイヤーの間では、片方にしか無い設定（切り抜き・クロマキー・反転）は写さない
      const bool sameKind = layer == srcLayer;
      // 速度は先に変える（この映像がもとから持っているキーフレームを、同じ映像の位置に合わせて伸び縮みさせる。
      // 位置・音量を一緒に貼るときは、そのあとで、写したキーフレームに置き換える）
      if ((parts & AttrSpeed) && !x->isImage && !src.isImage && std::abs(x->speed - src.speed) > 1e-9) {
        const double f = x->speed / src.speed;
        x->anim = scaleAnimationTime(x->anim, f);
        for (auto& pt : x->volumePoints) pt.t *= f;
        x->speed = src.speed;
      }
      if (parts & AttrColor) x->color = src.color;
      if (parts & AttrEffects) {
        // 貼る先で使えるエフェクトだけ（クリップ用・エフェクトレイヤー用）
        x->effects.clear();
        for (Effect e : src.effects) {
          const EffectInfo& info = effectInfo(e.type);
          if (layer ? !info.layer : !info.clip) continue;
          e.id = newId();
          x->effects.push_back(e);
        }
        if (!layer && sameKind) {
          x->flipX = src.flipX;
          x->flipY = src.flipY;
        }
      }
      if (parts & AttrMotion) {
        x->x = src.x;
        x->y = src.y;
        x->scale = src.scale;
        x->rotation = src.rotation;
        x->opacity = src.opacity;
        x->anim = src.anim;
      }
      if ((parts & AttrCrop) && !layer && sameKind) {
        x->cropL = src.cropL;
        x->cropT = src.cropT;
        x->cropR = src.cropR;
        x->cropB = src.cropB;
        x->stretchX = src.stretchX;
        x->stretchY = src.stretchY;
      }
      if ((parts & AttrChroma) && !layer && sameKind) x->chroma = src.chroma;
      if ((parts & AttrMask) && !layer && sameKind && src.mask.has()) {
        // 同じ素材（同じ向き）なら、マスクごと写す（同じ目印になるので、トラックし直すと両方に入る）。
        // 違う素材なら、被写体の形は使えないので、マスクの使い方（モード・反転・広げる・ぼかし）だけを写す
        const Asset* sa = asset(src.assetId);
        const Asset* xa = asset(x->assetId);
        const bool sameSource = x->assetId == src.assetId ||
                                (sa && xa && QFileInfo(sa->path).absoluteFilePath().compare(QFileInfo(xa->path).absoluteFilePath(), Qt::CaseInsensitive) == 0);
        if (sameSource && x->isImage == src.isImage) {
          x->mask = src.mask;
        } else if (x->mask.has()) {
          x->mask.enabled = src.mask.enabled;
          x->mask.mode = src.mask.mode;
          x->mask.invert = src.mask.invert;
          x->mask.expand = src.mask.expand;
          x->mask.feather = src.mask.feather;
        }
      }
      if ((parts & AttrVolume) && !layer && !srcLayer) {
        x->volume = src.volume;
        x->muted = src.muted;
        x->pan = src.pan;
        x->volumePoints = src.volumePoints;
      }
      if (parts & AttrFade) {
        x->fadeIn = src.fadeIn;
        x->fadeOut = src.fadeOut;
      }
      refitFades(*x, clipDuration(*x, fps));
      // 重ねるトラック: 速度で長くなって同じ段の次のものに重なったら、上の空いている段へ
      if (ov) liftOverlayToFreeLane(p.overlays, ov->id, fps);
      ++count;
    }
    return count > 0;
  });
  return count;
}

// ---- トランジション -----------------------------------------------------------------------------

TransitionType Editor::defaultTransitionType() {
  TransitionType t = TransitionType::Dissolve;
  const QString s = QSettings(editorSettingsPath(), QSettings::IniFormat).value(QStringLiteral("transition/type")).toString();
  if (!transitionFromString(s, &t)) t = TransitionType::Dissolve;
  return t;
}

double Editor::defaultTransitionDuration() {
  const double d = QSettings(editorSettingsPath(), QSettings::IniFormat).value(QStringLiteral("transition/duration"), kDefaultTransitionDuration).toDouble();
  return std::clamp(std::isfinite(d) ? d : kDefaultTransitionDuration, kMinTransitionDuration, kMaxTransitionDuration);
}

void Editor::setDefaultTransition(TransitionType type, double duration) {
  if (type == TransitionType::None) return;
  QSettings s(editorSettingsPath(), QSettings::IniFormat);
  s.setValue(QStringLiteral("transition/type"), transitionToString(type));
  s.setValue(QStringLiteral("transition/duration"), std::clamp(duration, kMinTransitionDuration, kMaxTransitionDuration));
}

bool Editor::setTransition(const QString& clipId, TransitionType type, std::optional<double> duration, const QString& merge) {
  if (type == TransitionType::None) return removeTransition(clipId);
  bool found = false;
  bool changed = false;
  double used = kDefaultTransitionDuration;
  commit([&](ProjectData& p) {
    for (size_t i = 1; i < p.clips.size(); ++i) {
      if (p.clips[i].id != clipId) continue;
      Transition& tr = p.clips[i].transIn;
      const Transition before = tr;
      const bool had = tr.type != TransitionType::None;
      if (!had) tr = Transition{};  // 新しく付ける: 音のクロスフェードなどは、新しいトランジションの既定のまま
      tr.type = type;
      tr.duration = std::clamp(duration.value_or(had ? tr.duration : defaultTransitionDuration()), kMinTransitionDuration, kMaxTransitionDuration);
      used = tr.duration;
      found = true;
      changed = !(tr == before);
      return changed;  // 同じ種類・長さなら、何も変えない（履歴にも残さない）
    }
    return false;
  }, merge);
  if (!found) {
    const bool first = !ws().project.clips.empty() && ws().project.clips.front().id == clipId;
    if (first) toast(QStringLiteral("先頭のクリップの前には、トランジションを付けられません"));
    return false;
  }
  if (!changed) {
    if (validSelection({Selection::Transition, clipId})) select({Selection::Transition, clipId});
    return false;
  }
  setDefaultTransition(type, used);
  select({Selection::Transition, clipId});
  // クリップが短すぎて入れられないときは知らせる（付けたまま。クリップを長くすれば効く）
  for (const auto& l : layout().items) {
    if (l.clip.id == clipId && transitionEffectiveDuration(layout(), l.index, ws().project.settings.fps) <= 0) {
      toast(QStringLiteral("前後のクリップが短いので、トランジションを入れられません（クリップを長くすると効きます）"));
    }
  }
  return true;
}

bool Editor::setTransitionDuration(const QString& clipId, double seconds, const QString& merge) {
  bool changed = false;
  commit([&](ProjectData& p) {
    for (size_t i = 1; i < p.clips.size(); ++i) {
      if (p.clips[i].id != clipId || p.clips[i].transIn.type == TransitionType::None) continue;
      const double d = std::clamp(seconds, kMinTransitionDuration, kMaxTransitionDuration);
      changed = std::abs(p.clips[i].transIn.duration - d) > 1e-9;
      p.clips[i].transIn.duration = d;
      return changed;
    }
    return false;
  }, merge);
  return changed;
}

bool Editor::setTransitionAudioCrossfade(const QString& clipId, bool on) {
  bool changed = false;
  commit([&](ProjectData& p) {
    for (size_t i = 1; i < p.clips.size(); ++i) {
      if (p.clips[i].id != clipId || p.clips[i].transIn.type == TransitionType::None) continue;
      changed = p.clips[i].transIn.audioCrossfade != on;
      p.clips[i].transIn.audioCrossfade = on;
      return changed;
    }
    return false;
  });
  return changed;
}

void Editor::rememberTransitionDefault(const QString& clipId) {
  if (const VideoClip* c = clip(clipId); c && c->transIn.type != TransitionType::None) setDefaultTransition(c->transIn.type, c->transIn.duration);
}

bool Editor::removeTransition(const QString& clipId) {
  bool done = false;
  commit([&](ProjectData& p) {
    for (auto& c : p.clips) {
      if (c.id != clipId || c.transIn.type == TransitionType::None) continue;
      c.transIn = Transition{};
      done = true;
      return true;
    }
    return false;
  });
  if (done && ws().selection.type == Selection::Transition && ws().selection.id == clipId) select({});
  return done;
}

int Editor::applyTransitionToAll(TransitionType type, double duration, bool audioCrossfade) {
  if (type == TransitionType::None) return removeAllTransitions();
  int count = 0;
  commit([&](ProjectData& p) {
    count = 0;
    for (size_t i = 1; i < p.clips.size(); ++i) {
      p.clips[i].transIn = {type, std::clamp(duration, kMinTransitionDuration, kMaxTransitionDuration), audioCrossfade};
      ++count;
    }
    return count > 0;
  });
  if (count > 0) setDefaultTransition(type, duration);
  return count;
}

int Editor::removeAllTransitions() {
  int count = 0;
  commit([&](ProjectData& p) {
    count = 0;
    for (auto& c : p.clips) {
      if (c.transIn.type == TransitionType::None) continue;
      c.transIn = Transition{};
      ++count;
    }
    return count > 0;
  });
  return count;
}

QString Editor::nearestCut(double t, double maxDistance) const {
  const auto& items = layout().items;
  QString best;
  double bestD = maxDistance;
  for (size_t i = 1; i < items.size(); ++i) {
    const double d = std::abs(items[i].start - t);
    if (d <= bestD) {
      bestD = d;
      best = items[i].clip.id;
    }
  }
  return best;
}

// ---- トラックの非表示・ミュート ---------------------------------------------------------------

namespace {
template <typename T>
bool hasValue(const std::vector<T>& v, const T& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}
template <typename T>
void setValue(std::vector<T>& v, const T& x, bool on) {
  if (on && !hasValue(v, x)) v.push_back(x);
  if (!on) std::erase(v, x);
}
}  // namespace

bool Editor::trackFlag(TrackFlag flag, int lane) const {
  const ProjectData& p = ws().project;
  switch (flag) {
    case TrackFlag::MainVideoHidden: return p.mainVideoHidden;
    case TrackFlag::MainAudioMuted: return p.mainAudioMuted;
    case TrackFlag::TextHidden: return lane >= 0 && lane < int(p.textTrackIds.size()) && hasValue(p.hiddenTextTracks, p.textTrackIds[size_t(lane)]);
    case TrackFlag::OverlayHidden: return hasValue(p.hiddenOverlayTracks, lane);
    case TrackFlag::OverlayMuted: return hasValue(p.mutedOverlayTracks, lane);
    case TrackFlag::AudioMuted: return lane >= 0 && lane < int(p.audioTrackNos.size()) && hasValue(p.mutedAudioTracks, p.audioTrackNos[size_t(lane)]);
  }
  return false;
}

bool Editor::setTrackFlag(TrackFlag flag, int lane, bool on) {
  if (trackFlag(flag, lane) == on) return false;
  bool changed = false;
  commit([&](ProjectData& p) {
    switch (flag) {
      case TrackFlag::MainVideoHidden: p.mainVideoHidden = on; break;
      case TrackFlag::MainAudioMuted: p.mainAudioMuted = on; break;
      case TrackFlag::TextHidden:
        if (lane < 0 || lane >= int(p.textTrackIds.size())) return false;
        setValue(p.hiddenTextTracks, p.textTrackIds[size_t(lane)], on);
        break;
      case TrackFlag::OverlayHidden:
        if (lane < 0) return false;
        setValue(p.hiddenOverlayTracks, lane, on);
        break;
      case TrackFlag::OverlayMuted:
        if (lane < 0) return false;
        setValue(p.mutedOverlayTracks, lane, on);
        break;
      case TrackFlag::AudioMuted:
        if (lane < 0 || lane >= int(p.audioTrackNos.size())) return false;
        setValue(p.mutedAudioTracks, p.audioTrackNos[size_t(lane)], on);
        break;
    }
    changed = true;
    return true;
  });
  return changed && trackFlag(flag, lane) == on;
}

bool Editor::anyTrackFlag() const {
  const ProjectData& p = ws().project;
  return p.mainVideoHidden || p.mainAudioMuted || !p.hiddenTextTracks.empty() || !p.hiddenOverlayTracks.empty() || !p.mutedOverlayTracks.empty() ||
         !p.mutedAudioTracks.empty();
}

void Editor::clearTrackFlags() {
  commit([](ProjectData& p) {
    p.mainVideoHidden = false;
    p.mainAudioMuted = false;
    p.hiddenTextTracks.clear();
    p.hiddenOverlayTracks.clear();
    p.mutedOverlayTracks.clear();
    p.mutedAudioTracks.clear();
    return true;
  });
}

// ---- マーカー ---------------------------------------------------------------------------------

QString Editor::addMarker(double t) {
  const int fps = ws().project.settings.fps;
  const double at = frameRound(std::max(0.0, t), fps);
  for (const Marker& m : ws().project.markers) {
    if (std::abs(m.t - at) < 0.5 / fps) return m.id;
  }
  Marker m;
  m.id = newId();
  m.t = at;
  commit([&](ProjectData& p) {
    p.markers.push_back(m);
    return true;
  });
  return m.id;
}

const Marker* Editor::marker(const QString& id) const {
  return findById(ws().project.markers, id);
}

void Editor::updateMarker(const QString& id, const std::function<void(Marker&)>& fn, const QString& merge) {
  commit([&](ProjectData& p) {
    Marker* m = findById(p.markers, id);
    if (!m) return false;
    fn(*m);
    m->t = frameRound(std::max(0.0, m->t), p.settings.fps);
    return true;
  }, merge);
}

void Editor::removeMarker(const QString& id) {
  commit([&](ProjectData& p) { return std::erase_if(p.markers, [&](const Marker& m) { return m.id == id; }) > 0; });
}

void Editor::clearMarkers() {
  commit([&](ProjectData& p) {
    if (p.markers.empty()) return false;
    p.markers.clear();
    return true;
  });
}

bool Editor::seekToMarker(bool forward) {
  const double now = ws().currentTime;
  const double eps = 0.5 / ws().project.settings.fps;
  const double end = contentEnd();
  std::optional<double> best;
  for (const Marker& m : ws().project.markers) {
    if (m.t > end + eps) continue;  // 動画の終わりより後ろのマーカーへは行けない（再生位置は終わりまで）
    if (forward ? m.t > now + eps : m.t < now - eps) {
      if (!best || (forward ? m.t < *best : m.t > *best)) best = m.t;
    }
  }
  if (!best) return false;
  pause();
  seek(*best);
  return true;
}

// ---- ループ再生 ---------------------------------------------------------------------------------

void Editor::setLoopPlayback(bool on) {
  if (loop_ == on) return;
  loop_ = on;
  QSettings(editorSettingsPath(), QSettings::IniFormat).setValue(QStringLiteral("playback/loop"), on);
  emit loopChanged();
}

void Editor::setSafeMarginsVisible(bool on) {
  if (safeMargins_ == on) return;
  safeMargins_ = on;
  QSettings(editorSettingsPath(), QSettings::IniFormat).setValue(QStringLiteral("preview/safeMargins"), on);
  emit guidesChanged();
}

void Editor::setGridVisible(bool on) {
  if (grid_ == on) return;
  grid_ = on;
  QSettings(editorSettingsPath(), QSettings::IniFormat).setValue(QStringLiteral("preview/grid"), on);
  emit guidesChanged();
}

Editor::TimeRange Editor::loopRange() const {
  if (const auto r = exportRange()) return *r;
  return {0, playEnd()};
}

void Editor::setAspect(Aspect aspect) {
  commit([&](ProjectData& p) {
    if (p.settings.aspect == aspect) return false;
    // 自動で決めた表示方法のままのクリップは、新しい比率に合わせて選び直す
    for (auto& c : p.clips) {
      const Asset* a = asset(c.assetId);
      if (!a || c.fit != defaultFit(*a, p.settings.aspect)) continue;
      c.fit = defaultFit(*a, aspect);
    }
    p.settings.aspect = aspect;
    return true;
  });
}

void Editor::setFps(int fps) {
  commit([&](ProjectData& p) {
    if (p.settings.fps == fps || fps <= 0) return false;
    p.settings.fps = fps;
    // 始まり・終わり・マーカーを、新しいフレームにそろえる（隣り合っていたものは、同じ所にそろうので重ならない）
    for (auto& t : p.texts) {
      const double s = frameRound(t.start, fps);
      const double e = frameRound(t.start + t.duration, fps);
      t.start = s;
      t.duration = std::max(e - s, std::ceil(kMinTextDuration * fps - 1e-9) / fps);
    }
    for (auto& a : p.audios) {
      const double s = frameRound(a.start, fps);
      const double e = frameRound(a.start + audioDuration(a), fps);
      a.start = s;
      if (e - s >= kMinClipDuration - 1e-9) a.out = a.in + (e - s);
      refitFades(a, audioDuration(a));
    }
    for (auto& o : p.overlays) {
      o.start = frameRound(o.start, fps);
      if (o.isImage) o.out = o.in + std::max(1.0 / fps, frameRound(o.out - o.in, fps));
    }
    for (auto& c : p.clips) {
      if (c.isImage) c.out = c.in + std::max(1.0 / fps, frameRound(c.out - c.in, fps));
    }
    for (auto& m : p.markers) m.t = frameRound(m.t, fps);
    return true;
  });
}

void Editor::setProjectSpeed(double speed) {
  const double s = std::clamp(speed, kMinProjectSpeed, kMaxProjectSpeed);
  commit([&](ProjectData& p) {
    if (std::abs(p.settings.speed - s) < 1e-9) return false;
    p.settings.speed = s;
    return true;
  });
}

const VideoClip* Editor::clip(const QString& id) const {
  if (const VideoClip* c = findById(ws().project.clips, id)) return c;
  return findById(ws().project.overlays, id);
}

const OverlayItem* Editor::overlay(const QString& id) const {
  return findById(ws().project.overlays, id);
}

bool Editor::clipSpan(const QString& id, double* start, double* duration) const {
  for (const auto& l : layout().items) {
    if (l.clip.id != id) continue;
    *start = l.start;
    *duration = l.duration;
    return true;
  }
  if (const OverlayItem* o = overlay(id)) {
    *start = o->start;
    *duration = overlayDuration(*o, ws().project.settings.fps);
    return true;
  }
  return false;
}

const TextItem* Editor::text(const QString& id) const {
  return findById(ws().project.texts, id);
}

const AudioItem* Editor::audio(const QString& id) const {
  return findById(ws().project.audios, id);
}

// ---- キーフレームアニメーション -----------------------------------------------------

namespace {
/** 選んでいる映像・文字のアニメーションと、その持ち主の開始・長さ（編集用） */
struct AnimTarget {
  Animation* anim = nullptr;
  VideoClip* clip = nullptr;
  TextItem* text = nullptr;
  double start = 0;
  double duration = 0;
  double base(AnimProp p) const { return clip ? baseProp(*clip, p) : baseProp(*text, p); }
  void setBase(AnimProp p, double v) const {
    if (clip) setBaseProp(*clip, p, v);
    else setBaseProp(*text, p, v);
  }
  /** 今の値（キーフレームがあれば、その補間した値） */
  double valueAt(AnimProp p, double local) const { return animValueAt(anim->keys(p), base(p), local); }
  /** この項目をアニメーションできるか（字間は文字だけ） */
  bool supports(AnimProp p) const { return p != AnimProp::LetterSpacing || text != nullptr; }
};
}  // namespace

/** 選んでいるものに、その項目があるか（字間は文字だけ） */
static bool propSupported(const Selection& sel, AnimProp p) {
  return p != AnimProp::LetterSpacing || sel.type == Selection::Text;
}

static AnimTarget animTarget(ProjectData& p, const Selection& sel) {
  AnimTarget t;
  if (sel.type == Selection::Clip) {
    t.clip = findById(p.clips, sel.id);
    if (!t.clip) return {};
    t.anim = &t.clip->anim;
    for (const auto& l : layoutClips(p.clips, p.settings.fps).items) {
      if (l.clip.id == sel.id) {
        t.start = l.start;
        t.duration = l.duration;
      }
    }
  } else if (sel.type == Selection::Overlay) {
    OverlayItem* o = findById(p.overlays, sel.id);
    if (!o) return {};
    t.clip = o;
    t.anim = &o->anim;
    t.start = o->start;
    t.duration = overlayDuration(*o, p.settings.fps);
  } else if (sel.type == Selection::Text) {
    t.text = findById(p.texts, sel.id);
    if (!t.text) return {};
    t.anim = &t.text->anim;
    t.start = t.text->start;
    t.duration = t.text->duration;
  }
  return t;
}

bool Editor::animatable() const {
  const Selection& s = ws().selection;
  return s.type == Selection::Clip || s.type == Selection::Text || s.type == Selection::Overlay;
}

const Animation* Editor::animation() const {
  const Selection& s = ws().selection;
  if (s.type == Selection::Clip || s.type == Selection::Overlay) {
    const VideoClip* c = clip(s.id);
    return c ? &c->anim : nullptr;
  }
  if (s.type == Selection::Text) {
    const TextItem* t = text(s.id);
    return t ? &t->anim : nullptr;
  }
  return nullptr;
}

double Editor::objectStart() const {
  const Selection& s = ws().selection;
  if (s.type == Selection::Clip || s.type == Selection::Overlay) {
    double start = 0, duration = 0;
    if (clipSpan(s.id, &start, &duration)) return start;
  } else if (s.type == Selection::Text) {
    if (const TextItem* t = text(s.id)) return t->start;
  }
  return 0;
}

double Editor::objectDuration() const {
  const Selection& s = ws().selection;
  if (s.type == Selection::Clip || s.type == Selection::Overlay) {
    double start = 0, duration = 0;
    if (clipSpan(s.id, &start, &duration)) return duration;
  } else if (s.type == Selection::Text) {
    if (const TextItem* t = text(s.id)) return t->duration;
  }
  return 0;
}

LayerState Editor::currentLayerState() const {
  const Selection& s = ws().selection;
  const double local = std::clamp(currentTime() - objectStart(), 0.0, std::max(0.0, objectDuration()));
  if (s.type == Selection::Clip || s.type == Selection::Overlay) {
    if (const VideoClip* c = clip(s.id)) return layerStateAt(*c, local);
  } else if (s.type == Selection::Text) {
    if (const TextItem* t = text(s.id)) return layerStateAt(*t, local);
  }
  return {};
}

void Editor::setAutoKey(bool on) {
  if (autoKey_ == on) return;
  autoKey_ = on;
  emit autoKeyChanged();
}

void Editor::setChromaPick(const QString& clipId) {
  if (chromaPick_ == clipId) return;
  if (!clipId.isEmpty()) {
    pause();  // 止めた画面から選ぶ
    setMaskPick({});
  }
  chromaPick_ = clipId;
  emit chromaPickChanged();
}

void Editor::setMaskPick(const QString& clipId) {
  if (maskPick_ == clipId) return;
  if (!clipId.isEmpty()) {
    pause();  // 止めた画面から選ぶ
    setChromaPick({});
  }
  maskPick_ = clipId;
  emit maskPickChanged();
}

int Editor::updateMasks(const QString& maskId, const std::function<void(ObjectMask&)>& fn) {
  if (maskId.isEmpty()) return 0;
  int count = 0;
  commit([&](ProjectData& p) {
    count = 0;
    auto apply = [&](VideoClip& c) {
      if (c.mask.id != maskId) return;
      fn(c.mask);
      ++count;
    };
    for (auto& c : p.clips) apply(c);
    for (auto& o : p.overlays) apply(o);
    return count > 0;
  });
  return count;
}

bool Editor::setPropValue(AnimProp prop, double value, const QString& merge) {
  const Selection sel = ws().selection;
  if (!animatable() || !propSupported(sel, prop)) return false;
  const double now = ws().currentTime;
  const bool ak = autoKey_;
  bool accepted = false;
  value = clampProp(prop, value);
  commit([&](ProjectData& p) {
    AnimTarget tg = animTarget(p, sel);
    if (!tg.anim) return false;
    auto& keys = tg.anim->keys(prop);
    if (keys.empty()) {
      // キーフレームが無い項目は、静的な値を変える（従来どおり）
      accepted = true;
      if (std::abs(tg.base(prop) - value) < 1e-9) return false;
      tg.setBase(prop, value);
      return true;
    }
    const double local = std::clamp(now - tg.start, 0.0, tg.duration);
    const int idx = findKey(keys, local);
    if (idx >= 0) {
      // 今の再生位置にキーフレームがある: その値を変える
      accepted = true;
      if (std::abs(keys[size_t(idx)].value - value) < 1e-9) return false;
      keys[size_t(idx)].value = value;
      return true;
    }
    if (!ak) return false;  // 自動記録が OFF: 意図せずキーフレームを増やさない
    accepted = true;
    insertKey(keys, {local, value, Easing::Linear});
    return true;
  }, merge);
  if (!accepted && merge != lastRejectMerge_) {
    lastRejectMerge_ = merge;
    toast(QStringLiteral("この項目にはキーフレームがあります。◆を押すか、自動記録をオンにすると、再生位置に記録できます"));
  } else if (accepted) {
    lastRejectMerge_.clear();
  }
  return accepted;
}

bool Editor::toggleKey(AnimProp prop) {
  const Selection sel = ws().selection;
  if (!animatable() || !propSupported(sel, prop)) return false;
  const double now = ws().currentTime;
  bool added = false;
  bool outside = false;
  commit([&](ProjectData& p) {
    AnimTarget tg = animTarget(p, sel);
    if (!tg.anim) return false;
    const double local = now - tg.start;
    if (local < -1e-6 || local > tg.duration + 1e-6) {
      outside = true;
      return false;
    }
    auto& keys = tg.anim->keys(prop);
    const int idx = findKey(keys, local);
    if (idx >= 0) {
      // 最後の 1 つを取り除くときは、今の値を静的な値として残す（見た目が急に変わらないように）
      if (keys.size() == 1) tg.setBase(prop, keys.front().value);
      keys.erase(keys.begin() + idx);
      return true;
    }
    const double v = clampProp(prop, tg.valueAt(prop, local));
    insertKey(keys, {std::clamp(local, 0.0, tg.duration), v, Easing::Linear});
    added = true;
    return true;
  });
  if (outside) toast(QStringLiteral("再生位置が、選んでいるものの外にあります。範囲の中へ動かしてください"));
  return added;
}

bool Editor::seekToKey(AnimProp prop, bool forward) {
  const Animation* a = animation();
  if (!a) return false;
  const double start = objectStart();
  const double local = currentTime() - start;
  const auto& keys = a->keys(prop);
  const double eps = 1e-3;
  if (forward) {
    for (const auto& k : keys) {
      if (k.t > local + eps) {
        seek(start + k.t);
        return true;
      }
    }
  } else {
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
      if (it->t < local - eps) {
        seek(start + it->t);
        return true;
      }
    }
  }
  return false;
}

void Editor::removeKeys(const std::vector<KeyRef>& refs) {
  const Selection sel = ws().selection;
  if (!animatable() || refs.empty()) return;
  const double now = ws().currentTime;
  commit([&](ProjectData& p) {
    AnimTarget tg = animTarget(p, sel);
    if (!tg.anim) return false;
    bool changed = false;
    for (int pi = 0; pi < kAnimPropCount; ++pi) {
      const AnimProp prop = static_cast<AnimProp>(pi);
      std::vector<int> idx;
      for (const auto& r : refs) {
        if (r.prop == prop) idx.push_back(r.index);
      }
      if (idx.empty()) continue;
      std::sort(idx.begin(), idx.end());
      idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
      auto& keys = tg.anim->keys(prop);
      const double keep = tg.valueAt(prop, std::clamp(now - tg.start, 0.0, tg.duration));
      for (auto it = idx.rbegin(); it != idx.rend(); ++it) {
        if (*it >= 0 && *it < int(keys.size())) {
          keys.erase(keys.begin() + *it);
          changed = true;
        }
      }
      // 全部取り除いたら、取り除く前の今の値を静的な値として残す
      if (keys.empty()) tg.setBase(prop, keep);
    }
    return changed;
  });
}

bool Editor::moveKeys(const std::vector<KeyMove>& moves, const QString& merge) {
  const Selection sel = ws().selection;
  if (!animatable() || moves.empty()) return false;
  bool ok = true;
  bool changed = false;
  commit([&](ProjectData& p) {
    AnimTarget tg = animTarget(p, sel);
    if (!tg.anim) return false;
    Animation next = *tg.anim;
    for (const auto& m : moves) {
      auto& keys = next.keys(m.key.prop);
      if (m.key.index < 0 || m.key.index >= int(keys.size())) {
        ok = false;
        return false;
      }
      keys[size_t(m.key.index)].t = m.t;
    }
    // 動かしたあとも、並び順・最小の間隔・範囲に収まっているときだけ受け付ける
    for (int pi = 0; pi < kAnimPropCount; ++pi) {
      const auto& keys = next.tracks[size_t(pi)];
      for (size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].t < -1e-9 || keys[i].t > tg.duration + 1e-9) ok = false;
        if (i > 0 && keys[i].t - keys[i - 1].t < kMinKeyGap - 1e-9) ok = false;
      }
    }
    if (!ok) return false;
    for (int pi = 0; pi < kAnimPropCount; ++pi) {
      if (next.tracks[size_t(pi)].size() != tg.anim->tracks[size_t(pi)].size()) continue;
      for (size_t i = 0; i < next.tracks[size_t(pi)].size(); ++i) {
        if (std::abs(next.tracks[size_t(pi)][i].t - tg.anim->tracks[size_t(pi)][i].t) > 1e-9) changed = true;
      }
    }
    if (!changed) return false;
    *tg.anim = next;
    return true;
  }, merge);
  return ok && changed;
}

bool Editor::setKeyValue(AnimProp prop, int index, double value, const QString& merge) {
  const Selection sel = ws().selection;
  if (!animatable() || !propSupported(sel, prop)) return false;
  bool changed = false;
  commit([&](ProjectData& p) {
    AnimTarget tg = animTarget(p, sel);
    if (!tg.anim) return false;
    auto& keys = tg.anim->keys(prop);
    if (index < 0 || index >= int(keys.size())) return false;
    const double v = clampProp(prop, value);
    if (std::abs(keys[size_t(index)].value - v) < 1e-9) return false;
    keys[size_t(index)].value = v;
    changed = true;
    return true;
  }, merge);
  return changed;
}

void Editor::setKeyEasing(const std::vector<KeyRef>& refs, Easing e) {
  const Selection sel = ws().selection;
  if (!animatable() || refs.empty()) return;
  commit([&](ProjectData& p) {
    AnimTarget tg = animTarget(p, sel);
    if (!tg.anim) return false;
    bool changed = false;
    for (const auto& r : refs) {
      auto& keys = tg.anim->keys(r.prop);
      if (r.index < 0 || r.index >= int(keys.size()) || keys[size_t(r.index)].easing == e) continue;
      keys[size_t(r.index)].easing = e;
      changed = true;
    }
    return changed;
  });
}

bool Editor::copyKeys(const std::vector<KeyRef>& refs) {
  const Animation* a = animation();
  if (!a || refs.empty()) return false;
  std::vector<ClipKey> out;
  double t0 = 1e18;
  for (const auto& r : refs) {
    const auto& keys = a->keys(r.prop);
    if (r.index < 0 || r.index >= int(keys.size())) continue;
    const Keyframe& k = keys[size_t(r.index)];
    out.push_back({r.prop, k.t, k.value, k.easing});
    t0 = std::min(t0, k.t);
  }
  if (out.empty()) return false;
  for (auto& k : out) k.dt -= t0;  // いちばん早いキーフレームからの差にする
  keyClip_ = std::move(out);
  return true;
}

int Editor::pasteKeys(double local) {
  const Selection sel = ws().selection;
  if (!animatable() || keyClip_.empty()) return 0;
  int count = 0;
  commit([&](ProjectData& p) {
    AnimTarget tg = animTarget(p, sel);
    if (!tg.anim) return false;
    local = std::clamp(local, 0.0, tg.duration);
    bool changed = false;
    for (const auto& ck : keyClip_) {
      const double t = local + ck.dt;
      if (t > tg.duration + 1e-9 || !tg.supports(ck.prop)) continue;  // 字間のキーフレームは、映像には貼り付けない
      auto& keys = tg.anim->keys(ck.prop);
      const int near = findKey(keys, t, kMinKeyGap - 1e-9);
      if (near >= 0) {
        // 同じ時間のキーフレームは、貼り付けるもので置き換える（重ねて増やさない）
        keys[size_t(near)].value = clampProp(ck.prop, ck.value);
        keys[size_t(near)].easing = ck.easing;
      } else {
        insertKey(keys, {t, clampProp(ck.prop, ck.value), ck.easing});
      }
      ++count;
      changed = true;
    }
    return changed;
  });
  return count;
}

bool Editor::applyAnimTemplate(const QString& templateId) {
  const Selection sel = ws().selection;
  const AnimTemplate* found = animTemplates().find(templateId);
  if (!found || sel.type != Selection::Text) return false;
  const AnimTemplate tpl = *found;
  bool done = false;
  commit([&](ProjectData& p) {
    TextItem* item = findById(p.texts, sel.id);
    if (!item) return false;
    const TextItem before = *item;
    AnimTemplates::applyTo(tpl, *item);
    done = true;
    // 同じものを当てはめ直しただけなら、元に戻す履歴には残さない
    bool same = before.animation == item->animation;
    for (int i = 0; same && i < kAnimPropCount; ++i) {
      const auto& a = before.anim.tracks[size_t(i)];
      const auto& b = item->anim.tracks[size_t(i)];
      same = a.size() == b.size();
      for (size_t k = 0; same && k < a.size(); ++k) {
        same = std::abs(a[k].t - b[k].t) < 1e-9 && std::abs(a[k].value - b[k].value) < 1e-9 && a[k].easing == b[k].easing;
      }
    }
    return !same;
  });
  return done;
}

const std::vector<Editor::AnimPreset>& Editor::animPresets() {
  static const std::vector<AnimPreset> list = {
      {QStringLiteral("fadeIn"), QStringLiteral("フェードイン"), QStringLiteral("最初に、だんだん見えてくる")},
      {QStringLiteral("fadeOut"), QStringLiteral("フェードアウト"), QStringLiteral("最後に、だんだん消える")},
      {QStringLiteral("slideLeft"), QStringLiteral("左からスライドイン"), QStringLiteral("画面の左から入ってくる")},
      {QStringLiteral("slideRight"), QStringLiteral("右からスライドイン"), QStringLiteral("画面の右から入ってくる")},
      {QStringLiteral("slideTop"), QStringLiteral("上からスライドイン"), QStringLiteral("画面の上から入ってくる")},
      {QStringLiteral("slideBottom"), QStringLiteral("下からスライドイン"), QStringLiteral("画面の下から入ってくる")},
      {QStringLiteral("zoomIn"), QStringLiteral("ズームイン"), QStringLiteral("小さいところから、大きくなって現れる")},
      {QStringLiteral("zoomOut"), QStringLiteral("ズームアウト"), QStringLiteral("大きいところから、縮んで現れる")},
      {QStringLiteral("spin"), QStringLiteral("回転して登場"), QStringLiteral("回りながら、大きくなって現れる")},
      {QStringLiteral("pop"), QStringLiteral("ポップアップ"), QStringLiteral("ぽんと少し跳ねて現れる")},
  };
  return list;
}

bool Editor::applyAnimPreset(const QString& id) {
  const Selection sel = ws().selection;
  if (!animatable()) return false;
  bool done = false;
  commit([&](ProjectData& p) {
    AnimTarget tg = animTarget(p, sel);
    if (!tg.anim || tg.duration < 0.1) return false;
    const double D = tg.duration;
    const double d = std::min(kDefaultAnimSeconds, D / 2);
    struct Track {
      AnimProp prop;
      std::vector<Keyframe> keys;
    };
    std::vector<Track> tracks;
    // 「入り」は先頭から d 秒、「出」は終わりの d 秒。終わりの状態（いまの見た目）は、その時間の今の値にそろえる
    auto restIn = [&](AnimProp pr) { return tg.valueAt(pr, d); };
    auto restOut = [&](AnimProp pr) { return tg.valueAt(pr, D - d); };
    if (id == u"fadeIn") {
      tracks.push_back({AnimProp::Opacity, {{0, 0, Easing::EaseOut}, {d, restIn(AnimProp::Opacity), Easing::Linear}}});
    } else if (id == u"fadeOut") {
      tracks.push_back({AnimProp::Opacity, {{D - d, restOut(AnimProp::Opacity), Easing::EaseIn}, {D, 0, Easing::Linear}}});
    } else if (id == u"slideLeft" || id == u"slideRight") {
      const double r = restIn(AnimProp::X);
      const double from = id == u"slideLeft" ? r - 1.0 : r + 1.0;
      tracks.push_back({AnimProp::X, {{0, from, Easing::EaseOut}, {d, r, Easing::Linear}}});
    } else if (id == u"slideTop" || id == u"slideBottom") {
      const double r = restIn(AnimProp::Y);
      const double from = id == u"slideTop" ? r - 1.0 : r + 1.0;
      tracks.push_back({AnimProp::Y, {{0, from, Easing::EaseOut}, {d, r, Easing::Linear}}});
    } else if (id == u"zoomIn" || id == u"zoomOut") {
      const double r = restIn(AnimProp::Scale);
      const double from = id == u"zoomIn" ? r * 0.3 : r * 1.7;
      tracks.push_back({AnimProp::Scale, {{0, from, Easing::EaseOut}, {d, r, Easing::Linear}}});
      tracks.push_back({AnimProp::Opacity, {{0, 0, Easing::EaseOut}, {d * 0.6, restIn(AnimProp::Opacity), Easing::Linear}}});
    } else if (id == u"spin") {
      const double rr = restIn(AnimProp::Rotation);
      const double rs = restIn(AnimProp::Scale);
      tracks.push_back({AnimProp::Rotation, {{0, rr - 180, Easing::EaseOut}, {d, rr, Easing::Linear}}});
      tracks.push_back({AnimProp::Scale, {{0, rs * 0.2, Easing::EaseOut}, {d, rs, Easing::Linear}}});
      tracks.push_back({AnimProp::Opacity, {{0, 0, Easing::EaseOut}, {d * 0.5, restIn(AnimProp::Opacity), Easing::Linear}}});
    } else if (id == u"pop") {
      const double rs = restIn(AnimProp::Scale);
      tracks.push_back({AnimProp::Scale, {{0, 0.05, Easing::EaseOut}, {d * 0.6, rs * 1.15, Easing::EaseInOut}, {d, rs, Easing::Linear}}});
      tracks.push_back({AnimProp::Opacity, {{0, 0, Easing::EaseOut}, {d * 0.3, restIn(AnimProp::Opacity), Easing::Linear}}});
    } else {
      return false;
    }
    for (auto& tr : tracks) {
      auto& keys = tg.anim->keys(tr.prop);
      for (auto& k : tr.keys) k.value = clampProp(tr.prop, k.value);
      // その範囲にあった古いキーフレームは取り除いて、プリセットのものに置き換える（範囲の外のキーフレームは残す）
      const double lo = tr.keys.front().t - kMinKeyGap + 1e-9;
      const double hi = tr.keys.back().t + kMinKeyGap - 1e-9;
      keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.t >= lo && k.t <= hi; }), keys.end());
      for (const auto& k : tr.keys) insertKey(keys, k);
    }
    done = true;
    return true;
  });
  return done;
}

// ---- 編集場所 ---------------------------------------------------------------

static bool hasContent(const ProjectData& p) {
  return !p.clips.empty() || !p.overlays.empty() || !p.texts.empty() || !p.audios.empty();
}

bool Editor::hasAnyContent() const {
  for (const auto& w : ws_) {
    if (hasContent(w.project)) return true;
  }
  return false;
}

WorkspaceData Editor::snapshotOf(int index) const {
  WorkspaceData out;
  if (index < 0 || index >= workspaceCount()) return out;
  const Workspace& w = ws_[size_t(index)];
  out.project = w.project;
  out.name = w.customName;
  out.number = w.number;
  out.path = w.path;
  out.dirty = w.dirty;
  // 一覧にあるメディアと、タイムラインで使っているメディアを保存する
  std::set<QString> seen;
  auto add = [&](const QString& id) {
    if (!seen.insert(id).second) return;
    auto it = w.assets.find(id);
    if (it != w.assets.end()) out.assets.push_back(it->second);
  };
  for (const auto& id : w.assetOrder) add(id);
  for (const auto& c : w.project.clips) add(c.assetId);
  for (const auto& o : w.project.overlays) add(o.assetId);
  for (const auto& a : w.project.audios) add(a.assetId);
  return out;
}

ProjectFile Editor::snapshot() const {
  ProjectFile file;
  file.active = active_;
  for (int i = 0; i < workspaceCount(); ++i) file.workspaces.push_back(snapshotOf(i));
  return file;
}

Editor::Workspace Editor::makeWorkspace() {
  Workspace w;
  w.uid = nextUid_++;
  // まだ使われていない一番小さい番号を「編集N」にする（閉じた番号は次に追加したときに使い回す）
  std::set<int> used;
  for (const auto& x : ws_) used.insert(x.number);
  int n = 1;
  while (used.count(n)) ++n;
  w.number = n;
  return w;
}

QString Editor::workspaceName(int index) const {
  if (index < 0 || index >= workspaceCount()) return {};
  const Workspace& w = ws_[size_t(index)];
  return w.customName.isEmpty() ? QStringLiteral("編集%1").arg(w.number) : w.customName;
}

void Editor::renameWorkspace(int index, const QString& name) {
  if (index < 0 || index >= workspaceCount()) return;
  const QString cleaned = name.simplified().left(40);
  Workspace& w = ws_[size_t(index)];
  if (w.customName == cleaned) return;
  w.customName = cleaned;
  emit workspacesChanged();
}

bool Editor::workspaceDirty(int index) const {
  return index >= 0 && index < workspaceCount() && ws_[size_t(index)].dirty;
}

bool Editor::workspaceHasContent(int index) const {
  return index >= 0 && index < workspaceCount() && hasContent(ws_[size_t(index)].project);
}

QString Editor::workspacePath(int index) const {
  return index >= 0 && index < workspaceCount() ? ws_[size_t(index)].path : QString();
}

int Editor::workspaceUid(int index) const {
  return index >= 0 && index < workspaceCount() ? ws_[size_t(index)].uid : -1;
}

int Editor::indexOfWorkspaceUid(int uid) const {
  for (int i = 0; i < workspaceCount(); ++i) {
    if (ws_[size_t(i)].uid == uid) return i;
  }
  return -1;
}

int Editor::workspaceWithPath(const QString& path) const {
  if (path.isEmpty()) return -1;
  const QString want = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  for (int i = 0; i < workspaceCount(); ++i) {
    const QString& p = ws_[size_t(i)].path;
    if (!p.isEmpty() && QDir::cleanPath(QFileInfo(p).absoluteFilePath()).compare(want, Qt::CaseInsensitive) == 0) return i;
  }
  return -1;
}

QStringList Editor::releaseAssetsOf(const Workspace& w) {
  QStringList ids;
  for (const auto& [id, a] : w.assets) {
    ids << id;
    thumbs_.erase(id);
    waveforms_.erase(id);
  }
  return ids;
}

void Editor::announceActivated() {
  // 再生エンジンが新しい編集場所の再生位置を読み直せるようにする
  ++seekToken_;
  // 先に編集場所が変わったことを知らせ、そのあと表示に必要なものを順に更新させる
  emit workspaceChanged(active_);
  emit rangeChanged();  // 範囲は編集場所ごと
  emit projectChanged();
  emit assetsChanged();
  emit selectionChanged();
  emit zoomChanged();
  emit timeChanged();
  emit dirtyChanged();
  emit workspacesChanged();
}

int Editor::addWorkspace() {
  emit workspaceAboutToChange();
  pause();
  // 離れる編集場所の、すぐ作り直せるキャッシュは手放す
  ws().lastMerge.clear();
  ws().layoutRevision = -1;
  ws().layout = TimelineLayout();
  ws_.push_back(makeWorkspace());
  active_ = workspaceCount() - 1;
  announceActivated();
  return active_;
}

/** 開いた内容の ID をすべて付け直す。同じファイルを複数のタブで開いても、メディア・クリップの ID が重ならないようにする */
static void remapIds(WorkspaceData& d) {
  std::map<QString, QString> assetMap;
  for (auto& a : d.assets) {
    const QString fresh = newId();
    assetMap[a.id] = fresh;
    a.id = fresh;
  }
  auto mapAsset = [&](QString& id) {
    auto it = assetMap.find(id);
    if (it != assetMap.end()) id = it->second;
  };
  for (auto& c : d.project.clips) {
    c.id = newId();
    mapAsset(c.assetId);
  }
  for (auto& o : d.project.overlays) {
    o.id = newId();
    mapAsset(o.assetId);
  }
  for (auto& t : d.project.texts) t.id = newId();
  for (auto& a : d.project.audios) {
    a.id = newId();
    mapAsset(a.assetId);
  }
}

int Editor::openWorkspace(WorkspaceData& data) {
  remapIds(data);
  // 何も入っていない最初のままの編集場所があれば、そこに開く
  const Workspace& cur = ws();
  const bool reuse = !hasContent(cur.project) && cur.assets.empty() && cur.path.isEmpty() && !cur.dirty && cur.past.empty();
  emit workspaceAboutToChange();
  pause();
  Workspace w = makeWorkspace();
  // 空のタブを使い回すときも、番号は引き継ぐが ID は新しくする（そのタブ宛ての読み込み中のメディアが、開いた内容に混ざらないように）
  if (reuse) w.number = cur.number;
  w.project = data.project;
  w.customName = data.name;
  w.path = data.path;
  w.dirty = data.dirty;
  for (const auto& a : data.assets) {
    w.assets[a.id] = a;
    w.assetOrder.push_back(a.id);
  }
  if (reuse) {
    ws() = std::move(w);
  } else {
    ws().lastMerge.clear();
    ws().layoutRevision = -1;
    ws().layout = TimelineLayout();
    ws_.push_back(std::move(w));
    active_ = workspaceCount() - 1;
  }
  announceActivated();
  emit fitRequested();
  return active_;
}

void Editor::closeWorkspace(int index) {
  if (index < 0 || index >= workspaceCount()) return;
  const bool wasActive = index == active_;
  if (wasActive) {
    emit workspaceAboutToChange();
    pause();
  }
  // 閉じる編集場所のサムネイル・波形を手放し、メディアを持ち続ける側にも知らせる
  const QStringList ids = releaseAssetsOf(ws_[size_t(index)]);
  ws_.erase(ws_.begin() + index);
  if (ws_.empty()) {
    // 最後の 1 つを閉じたときは、空の編集場所を表示する
    ws_.push_back(makeWorkspace());
    active_ = 0;
  } else if (index < active_) {
    --active_;
  } else if (wasActive) {
    active_ = std::min(index, workspaceCount() - 1);
  }
  emit workspaceClosed(ids);
  if (wasActive) announceActivated();
  else emit workspacesChanged();
}

void Editor::switchWorkspace(int index) {
  if (index < 0 || index >= workspaceCount() || index == active_) return;
  emit workspaceAboutToChange();
  pause();
  // 離れる編集場所の、すぐ作り直せるキャッシュは手放す（履歴や内容はそのまま）
  ws().lastMerge.clear();
  ws().layoutRevision = -1;
  ws().layout = TimelineLayout();
  active_ = index;
  announceActivated();
}

void Editor::resetSession(const ProjectFile& file) {
  emit workspaceAboutToChange();
  const bool wasPlaying = playing_;
  playing_ = false;

  std::vector<Workspace> list;
  std::set<int> used;
  for (const auto& d : file.workspaces) {
    Workspace w;
    w.uid = nextUid_++;
    w.project = d.project;
    w.customName = d.name;
    w.path = d.path;
    w.dirty = d.dirty;
    w.number = used.insert(d.number).second && d.number > 0 ? d.number : 0;
    for (const auto& a : d.assets) {
      w.assets[a.id] = a;
      w.assetOrder.push_back(a.id);
    }
    list.push_back(std::move(w));
  }
  // 番号が無い・重なっているものには、使われていない一番小さい番号を付ける
  for (auto& w : list) {
    if (w.number > 0) continue;
    int n = 1;
    while (used.count(n)) ++n;
    w.number = n;
    used.insert(n);
  }
  ws_ = std::move(list);
  if (ws_.empty()) ws_.push_back(makeWorkspace());
  active_ = std::clamp(file.active, 0, workspaceCount() - 1);
  thumbs_.clear();
  waveforms_.clear();

  emit projectReset();
  announceActivated();
  if (wasPlaying) emit playingChanged();
  emit fitRequested();
}

void Editor::markSaved(const QString& path) {
  ws().path = path;
  setDirty(false);
  emit dirtyChanged();
  emit workspacesChanged();
}

}  // namespace kv
