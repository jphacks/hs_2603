#include "media/audiopreview.h"

#include <memory>

#include "core/editor.h"

namespace kv {

AudioPreview& AudioPreview::instance() {
  static AudioPreview* p = new AudioPreview();
  return *p;
}

AudioPreview::AudioPreview() {
  timer_.setInterval(80);
  connect(&timer_, &QTimer::timeout, this, &AudioPreview::tick);
  Editor& ed = editor();
  // タイムラインの再生が始まったら、試聴は止める（2 つの音が重ならないように）
  connect(&ed, &Editor::playingChanged, this, [this] {
    if (editor().playing()) stop();
  });
  // 別のタブに切り替えた・素材を一覧から消したときも止める
  connect(&ed, &Editor::workspaceChanged, this, [this] { stop(); });
  connect(&ed, &Editor::assetsChanged, this, [this] {
    if (asset_ && !assetId_.isEmpty() && !editor().asset(assetId_)) stop();
  });
}

void AudioPreview::toggle(const QString& assetId) {
  if (assetId_ == assetId) {
    stop();
    return;
  }
  const Asset* a = editor().asset(assetId);
  if (!a || a->kind != AssetKind::Audio || a->status == AssetStatus::Missing || a->status == AssetStatus::Error) {
    editor().toast(QStringLiteral("この音声は再生できません（ファイルが見つからないか、読み込めません）"), ToastKind::Error);
    return;
  }
  play(assetId, a->path, a->duration, true);
}

void AudioPreview::toggleFile(const QString& key, const QString& path, double duration) {
  if (assetId_ == key) {
    stop();
    return;
  }
  play(key, path, duration, false);
}

void AudioPreview::play(const QString& key, const QString& path, double duration, bool asset) {
  // 別の素材を試聴中なら止めて、タイムラインの再生も止める
  stop();
  editor().pause();
  if (!engine_) engine_ = std::make_unique<AudioEngine>();
  if (!engine_->ok()) {
    editor().toast(QStringLiteral("音声の出力を開けないため、試聴できません"), ToastKind::Error);
    return;
  }
  auto snap = std::make_shared<MixSnapshot>();
  MixSource s;
  s.key = QStringLiteral("preview");
  s.path = path;
  s.start = 0;
  s.duration = duration;
  s.in = 0;
  s.speed = 1;
  s.volume = 1;
  snap->sources.push_back(s);
  snap->end = duration;
  engine_->setSnapshot(snap);
  engine_->start(0);
  assetId_ = key;
  asset_ = asset;
  duration_ = duration;
  sinceStart_.start();
  timer_.start();
  emit changed();
}

void AudioPreview::stop() {
  if (assetId_.isEmpty()) return;
  timer_.stop();
  if (engine_) engine_->stop();
  assetId_.clear();
  emit changed();
}

void AudioPreview::tick() {
  if (assetId_.isEmpty()) {
    timer_.stop();
    return;
  }
  // 最後まで鳴らしたら、自動で止める。音が出始めないまま長く待っているときも止める
  const bool finished = engine_->started() ? engine_->clock() >= duration_ - 0.02 : sinceStart_.elapsed() > 8000;
  if (finished) stop();
}

}  // namespace kv
