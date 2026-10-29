#include "media/audioengine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/timeline.h"
#include "media/ffutil.h"
#include "miniaudio.h"

namespace kv {

static constexpr int kBlock = 1024;
static constexpr size_t kRingFrames = kAudioRate / 4;  // 0.25 秒

struct AudioEngine::Device {
  ma_device device;
};

struct AudioEngine::Reader {
  AudioDecoder dec;
  bool opened = false;
  double nextTimeline = -1;  // 次に読む位置（タイムライン）
  double tempo = 1;
  int idleBlocks = 0;
};

static void dataCallback(ma_device* dev, void* out, const void*, ma_uint32 frames) {
  static_cast<AudioEngine*>(dev->pUserData)->render(static_cast<float*>(out), frames);
}

AudioEngine::AudioEngine() : device_(std::make_unique<Device>()) {
  ring_.assign(kRingFrames * 2, 0.0f);
  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate = kAudioRate;
  cfg.periodSizeInMilliseconds = 10;
  cfg.dataCallback = dataCallback;
  cfg.pUserData = this;
  if (ma_device_init(nullptr, &cfg, &device_->device) == MA_SUCCESS) {
    if (ma_device_start(&device_->device) == MA_SUCCESS) {
      ok_ = true;
      const auto& pb = device_->device.playback;
      latency_ = double(pb.internalPeriodSizeInFrames) * std::max(1u, pb.internalPeriods) / std::max(1u, pb.internalSampleRate) * 0.5;
    } else {
      ma_device_uninit(&device_->device);
    }
  }
  mixer_ = std::thread([this] { mixerLoop(); });
}

AudioEngine::~AudioEngine() {
  quit_ = true;
  if (mixer_.joinable()) mixer_.join();
  if (ok_) ma_device_uninit(&device_->device);
}

void AudioEngine::setSnapshot(std::shared_ptr<const MixSnapshot> snap) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_ = std::move(snap);
}

void AudioEngine::start(double t0) {
  playing_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    t0_ = t0;
  }
  framesPlayed_ = 0;
  ++generation_;
  active_ = true;
}

void AudioEngine::stop() {
  active_ = false;
  playing_ = false;
}

double AudioEngine::clock() const {
  const double played = double(framesPlayed_.load()) / kAudioRate;
  return t0_ + std::max(0.0, played - latency_);
}

void AudioEngine::render(float* out, unsigned frames) {
  ++inCallback_;
  if (!playing_.load()) {
    std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
    --inCallback_;
    return;
  }
  size_t r = readPos_.load(std::memory_order_acquire);
  const size_t w = writePos_.load(std::memory_order_acquire);
  const size_t avail = w - r;
  const size_t n = std::min<size_t>(frames, avail);
  for (size_t i = 0; i < n; ++i) {
    const size_t idx = ((r + i) % kRingFrames) * 2;
    out[i * 2] = ring_[idx];
    out[i * 2 + 1] = ring_[idx + 1];
  }
  if (n < frames) std::memset(out + n * 2, 0, (frames - n) * 2 * sizeof(float));
  readPos_.store(r + n, std::memory_order_release);
  framesPlayed_ += frames;
  --inCallback_;
}

AudioEngine::Reader* AudioEngine::reader(const MixSource& s) {
  auto& slot = readers_[s.key + QLatin1Char('|') + s.path];
  if (!slot) {
    slot = std::make_unique<Reader>();
    slot->opened = slot->dec.open(s.path);
  }
  slot->idleBlocks = 0;
  return slot->opened ? slot.get() : nullptr;
}

double mixSourceGain(const MixSource& s, double local) {
  // 音量・キーフレーム・フェードは、クリップの中の時刻で決める（クロスフェードで前後に足した所は、端の値のまま。書き出しと同じ）
  const double inner = std::clamp(local, 0.0, std::max(0.0, s.duration));
  double g = volumeAt(s.points, s.volume, inner) * fadeFactor(inner, s.duration, s.fadeIn, s.fadeOut);
  constexpr double kHalfPi = 1.5707963267948966;
  // 前のクリップからのクロスフェード: 大きくしていく（qsin: sin(x·π/2)）
  if (s.headXf > 0) {
    const double x = (local + s.headExt) / s.headXf;
    if (x < 1) g *= std::sin(std::clamp(x, 0.0, 1.0) * kHalfPi);
  }
  // 次のクリップへのクロスフェード: 小さくしていく（acrossfade の 1 つ目と同じく、1 サンプル先の位置の qsin を逆向きに）
  if (s.tailXf > 0) {
    const double x = (local - (s.duration + s.tailExt - s.tailXf)) / s.tailXf;
    if (x > 0 - 1.0 / (s.tailXf * kAudioRate)) g *= std::sin(std::clamp(1 - x - 1.0 / (s.tailXf * kAudioRate), 0.0, 1.0) * kHalfPi);
  }
  return g;
}

void AudioEngine::mixBlock(float* out, int frames, double t, const MixSnapshot& snap) {
  std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
  const double t1 = t + double(frames) / kAudioRate;
  std::vector<float> tmp;
  for (const auto& s : snap.sources) {
    // 鳴らす範囲（クロスフェードで前後に足した所も含む）。素材の最初より前は無音（読み始めは素材の 0 秒の所から）
    const double from = std::max(s.start - s.headExt, s.start - s.in / std::max(1e-6, s.speed));
    const double end = s.start + s.duration + s.tailExt;
    // 少し先に始まる音は、開いてシークしておく
    if (from >= t1 && from < t1 + 1.5) {
      if (Reader* rd = reader(s)) {
        if (std::abs(rd->nextTimeline - from) > 1e-4 || rd->tempo != s.speed) {
          rd->dec.seek(std::max(0.0, s.in + (from - s.start) * s.speed), s.speed);
          rd->nextTimeline = from;
          rd->tempo = s.speed;
        }
      }
      continue;
    }
    if (end <= t || from >= t1) continue;
    const double a = std::max(t, from);
    const double b = std::min(t1, end);
    const int offset = std::clamp(int(std::lround((a - t) * kAudioRate)), 0, frames);
    const int count = std::clamp(int(std::lround((b - a) * kAudioRate)), 0, frames - offset);
    if (count <= 0) continue;
    Reader* rd = reader(s);
    if (!rd) continue;
    if (std::abs(rd->nextTimeline - a) > 2.0 / kAudioRate || rd->tempo != s.speed) {
      rd->dec.seek(std::max(0.0, s.in + (a - s.start) * s.speed), s.speed);
      rd->tempo = s.speed;
    }
    tmp.resize(size_t(count) * 2);
    rd->dec.read(tmp.data(), count);
    rd->nextTimeline = a + double(count) / kAudioRate;
    const double local0 = a - s.start;
    // パン（反対側を弱める）
    const float gl = float(panGainLeft(s.pan));
    const float gr = float(panGainRight(s.pan));
    for (int i = 0; i < count; ++i) {
      // 1 サンプルごとに音量を決める（キーフレームの間は直線。段差が無いのでクリックノイズが出ない）
      const double local = local0 + double(i) / kAudioRate;
      const float g = float(mixSourceGain(s, local));
      out[size_t(offset + i) * 2] += tmp[size_t(i) * 2] * g * gl;
      out[size_t(offset + i) * 2 + 1] += tmp[size_t(i) * 2 + 1] * g * gr;
    }
  }
  // 音割れ防止（0.97 を超えた分をなだらかに抑える）
  for (int i = 0; i < frames * 2; ++i) {
    const float x = out[i];
    const float ax = std::abs(x);
    if (ax > 0.97f) out[i] = std::copysign(0.97f + 0.03f * std::tanh((ax - 0.97f) / 0.03f), x);
  }
}

void AudioEngine::mixerLoop() {
  unsigned gen = 0;
  double mixTime = 0;
  std::vector<float> block(size_t(kBlock) * 2);
  while (!quit_) {
    if (!active_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    std::shared_ptr<const MixSnapshot> snap;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snap = snapshot_;
      if (gen != generation_.load()) {
        gen = generation_.load();
        mixTime = t0_;
        // コールバックがリングを読んでいないときに空にする
        while (inCallback_.load() > 0) std::this_thread::yield();
        readPos_ = 0;
        writePos_ = 0;
        for (auto& [k, r] : readers_) r->nextTimeline = -1;
      }
    }
    if (!snap) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    const size_t used = writePos_.load() - readPos_.load();
    if (used + kBlock > kRingFrames) {
      if (!playing_ && active_ && gen == generation_.load()) playing_ = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
      continue;
    }
    mixBlock(block.data(), kBlock, mixTime, *snap);
    if (gen != generation_.load()) continue;
    const size_t w = writePos_.load(std::memory_order_acquire);
    for (int i = 0; i < kBlock; ++i) {
      const size_t idx = ((w + size_t(i)) % kRingFrames) * 2;
      ring_[idx] = block[size_t(i) * 2];
      ring_[idx + 1] = block[size_t(i) * 2 + 1];
    }
    writePos_.store(w + kBlock, std::memory_order_release);
    mixTime += double(kBlock) / kAudioRate;
    // 80ms ほど溜まったら音を出し始める
    if (!playing_ && writePos_.load() - readPos_.load() >= size_t(kAudioRate) * 8 / 100) playing_ = true;
    // 使われなくなった読み出し状態を片付ける
    for (auto it = readers_.begin(); it != readers_.end();) {
      if (++it->second->idleBlocks > 500) it = readers_.erase(it);
      else ++it;
    }
  }
}

}  // namespace kv
