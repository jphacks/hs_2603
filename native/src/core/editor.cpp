#include "core/editor.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>
#include <set>

#include "core/filters.h"
#include "core/textstyles.h"

namespace kv {

static constexpr size_t kHistoryLimit = 100;

ProjectData emptyProject() {
  ProjectData p;
  p.name = QStringLiteral("無題のプロジェクト");
  p.settings.aspect = Aspect::R16_9;
  p.settings.fps = 30;
  return p;
}

Editor::Editor() {
  ws_.push_back(makeWorkspace());
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

const TimelineLayout& Editor::layout() const {
  if (ws().layoutRevision != ws().revision) {
    ws().layout = layoutClips(ws().project.clips, ws().project.settings.fps);
    ws().layoutRevision = ws().revision;
  }
  return ws().layout;
}

double Editor::contentEnd() const {
  double end = layout().total;
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
  const Selection sel = validSelection(ws().selection);
  const bool selChanged = !(sel == ws().selection);
  ws().selection = sel;
  setDirty(true);
  emit projectChanged();
  if (selChanged) emit selectionChanged();
}

/** どの操作で変えても、音量は 0〜100% の範囲に収める */
static void clampVolumes(ProjectData& p) {
  for (auto& c : p.clips) {
    c.volume = std::clamp(c.volume, 0.0, kMaxVolume);
    for (auto& pt : c.volumePoints) pt.volume = std::clamp(pt.volume, 0.0, kMaxVolume);
    c.x = clampProp(AnimProp::X, c.x);
    c.y = clampProp(AnimProp::Y, c.y);
    c.scale = clampProp(AnimProp::Scale, c.scale);
    c.rotation = clampProp(AnimProp::Rotation, c.rotation);
    c.opacity = clampProp(AnimProp::Opacity, c.opacity);
  }
  for (auto& t : p.texts) {
    t.x = clampProp(AnimProp::X, t.x);
    t.y = clampProp(AnimProp::Y, t.y);
    t.scale = clampProp(AnimProp::Scale, t.scale);
    t.rotation = clampProp(AnimProp::Rotation, t.rotation);
    t.opacity = clampProp(AnimProp::Opacity, t.opacity);
  }
  for (auto& a : p.audios) {
    a.volume = std::clamp(a.volume, 0.0, kMaxVolume);
    for (auto& pt : a.volumePoints) pt.volume = std::clamp(pt.volume, 0.0, kMaxVolume);
  }
}

void Editor::commit(const std::function<bool(ProjectData&)>& fn, const QString& merge) {
  ProjectData next = ws().project;
  if (!fn(next)) return;
  clampVolumes(next);
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
  if (sel == ws().selection) return;
  ws().selection = sel;
  emit selectionChanged();
}

void Editor::seek(double t) {
  const double end = contentEnd();
  ws().currentTime = std::clamp(t, 0.0, std::max(0.0, end));
  ++seekToken_;
  emit timeChanged();
}

void Editor::setTimeFromEngine(double t) {
  if (t == ws().currentTime) return;
  ws().currentTime = t;
  emit timeChanged();
}

void Editor::play() {
  const double end = playEnd();
  if (end <= 0 || playing_) return;
  if (ws().currentTime >= end - 1e-3) {
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

static bool usesAsset(const ProjectData& p, const QString& id) {
  for (const auto& c : p.clips) {
    if (c.assetId == id) return true;
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
    for (const auto& c : ws().project.clips) uses += c.assetId == id;
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
    VideoClip* c = findById(p.clips, id);
    if (!c) return false;
    fn(*c);
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
    TextItem* t = findById(p.texts, id);
    if (!t || delta == 0) return false;
    // 一番下の段のさらに下へ 1 段だけ増やせる（自分だけの段をどんどん下へ送ることはできない）
    int lastOther = -1;
    for (const auto& x : p.texts) {
      if (x.id != id) lastOther = std::max(lastOther, x.track);
    }
    const int next = std::clamp(t->track + delta, 0, lastOther + 1);
    if (next == t->track) return false;
    t->track = next;
    return true;
  });
}

double Editor::newTextDuration(double start) const {
  const double total = layout().total;
  double duration = kDefaultTextDuration;
  // 動画の終わりをまたぐときだけ、終わりまでに縮める（終わりより後ろに置いたときは、そのままの長さ）
  if (total > 1 && start < total && start + duration > total) duration = std::max(kMinTextDuration, total - start);
  return duration;
}

QString Editor::addTextAt(const QString& presetId, double start) {
  TextItem item = makeText(presetId, frameRound(std::max(0.0, start), ws().project.settings.fps));
  item.duration = newTextDuration(item.start);
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
  if (total > 0 && start >= total - 0.3) start = std::max(0.0, total - kDefaultTextDuration);
  return addTextAt(presetId, start);
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
  const Selection sel = ws().selection;
  if (!sel) return;
  commit([&](ProjectData& p) {
    auto erase = [&](auto& list) {
      const auto before = list.size();
      list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& x) { return x.id == sel.id; }), list.end());
      return list.size() != before;
    };
    if (sel.type == Selection::Clip) return erase(p.clips);
    if (sel.type == Selection::Text) return erase(p.texts);
    return erase(p.audios);
  });
  select({});
  // 再生位置が終わりより後ろになったら戻す
  if (ws().currentTime > contentEnd()) seek(contentEnd());
}

void Editor::duplicateSelection() {
  const Selection sel = ws().selection;
  if (!sel) return;
  const QString newIdValue = newId();
  bool done = false;
  commit([&](ProjectData& p) {
    if (sel.type == Selection::Clip) {
      auto it = std::find_if(p.clips.begin(), p.clips.end(), [&](const VideoClip& c) { return c.id == sel.id; });
      if (it == p.clips.end()) return false;
      VideoClip copy = *it;
      copy.id = newIdValue;
      p.clips.insert(it + 1, copy);
    } else if (sel.type == Selection::Text) {
      const TextItem* t = findById(p.texts, sel.id);
      if (!t) return false;
      TextItem copy = *t;
      copy.id = newIdValue;
      copy.start = t->start + t->duration;
      // 元と同じ段が空いていればそこへ。ふさがっていれば、重ならない段へ（元のテキストの段は変えない）
      if (!textTrackFree(p.texts, t->track, copy.start, copy.duration)) copy.track = freeTextTrack(p.texts, copy.start, copy.duration);
      p.texts.push_back(copy);
    } else {
      const AudioItem* a = findById(p.audios, sel.id);
      if (!a) return false;
      AudioItem copy = *a;
      copy.id = newIdValue;
      copy.start = a->start + audioDuration(*a);
      p.audios.push_back(copy);
    }
    done = true;
    return true;
  });
  if (done) select({sel.type, newIdValue});
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
    if (p.settings.fps == fps) return false;
    p.settings.fps = fps;
    return true;
  });
}

const VideoClip* Editor::clip(const QString& id) const {
  return findById(ws().project.clips, id);
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
};
}  // namespace

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
  return s.type == Selection::Clip || s.type == Selection::Text;
}

const Animation* Editor::animation() const {
  const Selection& s = ws().selection;
  if (s.type == Selection::Clip) {
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
  if (s.type == Selection::Clip) {
    for (const auto& l : layout().items) {
      if (l.clip.id == s.id) return l.start;
    }
  } else if (s.type == Selection::Text) {
    if (const TextItem* t = text(s.id)) return t->start;
  }
  return 0;
}

double Editor::objectDuration() const {
  const Selection& s = ws().selection;
  if (s.type == Selection::Clip) {
    for (const auto& l : layout().items) {
      if (l.clip.id == s.id) return l.duration;
    }
  } else if (s.type == Selection::Text) {
    if (const TextItem* t = text(s.id)) return t->duration;
  }
  return 0;
}

LayerState Editor::currentLayerState() const {
  const Selection& s = ws().selection;
  const double local = std::clamp(currentTime() - objectStart(), 0.0, std::max(0.0, objectDuration()));
  if (s.type == Selection::Clip) {
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

bool Editor::setPropValue(AnimProp prop, double value, const QString& merge) {
  const Selection sel = ws().selection;
  if (!animatable()) return false;
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
  if (!animatable()) return false;
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
  if (!animatable()) return false;
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
      if (t > tg.duration + 1e-9) continue;
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

bool Editor::hasAnyContent() const {
  for (const auto& w : ws_) {
    const ProjectData& p = w.project;
    if (!p.clips.empty() || !p.texts.empty() || !p.audios.empty()) return true;
  }
  return false;
}

static bool hasContent(const ProjectData& p) {
  return !p.clips.empty() || !p.texts.empty() || !p.audios.empty();
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
