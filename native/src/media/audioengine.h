#pragma once
// プレビューの音声。クリップの音と BGM を混ぜて再生する。再生位置の時計にもなる。

#include <QString>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/model.h"

namespace kv {

struct MixSource {
  QString key;   // 読み出し状態を使い回すための名前
  QString path;
  double start = 0;     // タイムライン上の開始
  double duration = 0;  // タイムライン上の長さ
  double in = 0;        // 素材の開始位置
  double speed = 1;
  double volume = 1;
  double fadeIn = 0;
  double fadeOut = 0;
  /** 音量キーフレーム（クリップの先頭からの秒数）。あるときは volume ではなく、これで音量を変える */
  std::vector<VolumePoint> points;
  /** パン（-1 が左だけ、0 が真ん中、+1 が右だけ） */
  double pan = 0;
  /**
   * トランジションで音もクロスフェードするとき（メインのビデオのクリップ）: 前（headExt 秒）・後ろ（tailExt 秒）にも素材の音を足して、
   * 足した所を含む headXf・tailXf 秒の間で、次のクリップの音と入れ替える（書き出しの acrossfade の qsin と同じ変わり方）。
   * 足した所の音量・フェードは、クリップの端の値のまま
   */
  double headExt = 0;
  double headXf = 0;
  double tailExt = 0;
  double tailXf = 0;
};

/** 素材の音の、タイムライン上のクリップの先頭からの時刻 local（秒）での音量の倍率（音量・キーフレーム・フェード・クロスフェード） */
double mixSourceGain(const MixSource& s, double local);

struct MixSnapshot {
  std::vector<MixSource> sources;
  double end = 0;
};

class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();
  bool ok() const { return ok_; }
  void setSnapshot(std::shared_ptr<const MixSnapshot> snap);
  /** タイムラインの t0 から再生を始める */
  void start(double t0);
  void stop();
  /** 音が出始めたか（準備中は false） */
  bool started() const { return playing_.load(); }
  /** 今聞こえている位置（タイムラインの秒） */
  double clock() const;

  // miniaudio のコールバックから呼ばれる
  void render(float* out, unsigned frames);

  /**
   * 再生とは別に、タイムラインの t からの frames サンプル分（ステレオ）を、プレビューと同じ混ぜ方で作る（確かめる用）。
   * 再生していないときだけ使う
   */
  void mixOffline(float* out, int frames, double t, const MixSnapshot& snap) { mixBlock(out, frames, t, snap); }

 private:
  struct Reader;
  void mixerLoop();
  void mixBlock(float* out, int frames, double t, const MixSnapshot& snap);
  Reader* reader(const MixSource& s);

  struct Device;
  std::unique_ptr<Device> device_;
  bool ok_ = false;
  double latency_ = 0;

  // ミキサー → コールバックのリングバッファ（ステレオ float）
  std::vector<float> ring_;
  std::atomic<size_t> readPos_{0};
  std::atomic<size_t> writePos_{0};
  std::atomic<bool> playing_{false};
  std::atomic<int> inCallback_{0};
  std::atomic<long long> framesPlayed_{0};

  std::thread mixer_;
  std::mutex mutex_;
  std::atomic<bool> quit_{false};
  std::atomic<bool> active_{false};
  std::atomic<unsigned> generation_{0};
  std::atomic<double> t0_{0};
  std::shared_ptr<const MixSnapshot> snapshot_;
  std::map<QString, std::unique_ptr<Reader>> readers_;
};

}  // namespace kv
