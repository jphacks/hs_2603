#pragma once
// FFmpeg (libav*) を使ったメディアの解析・サムネイル・波形

#include <QImage>
#include <QString>
#include <atomic>
#include <memory>
#include <vector>

#include "core/model.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;
struct SwrContext;
struct AVFilterGraph;
struct AVFilterContext;

namespace kv {

struct ProbeInfo {
  double duration = 0;
  int width = 0;
  int height = 0;
  bool hasVideo = false;
  bool hasAudio = false;
  double fps = 0;
  int rotation = 0;
  QString videoCodec;
  QString audioCodec;
  bool isImage = false;
};

QString ffError(int code);
bool isImagePath(const QString& path);

/** 色の規格が書かれていない動画を BT.709 とみなすか（HD 以上なら BT.709、SD なら BT.601） */
bool assumeBt709(int width, int height);
/** 色の規格が書かれていない動画に、プレビューと同じ前提を付ける setparams の値。書かれていれば空 */
QString assumedColorParams(const QString& path);

/** 失敗したときは error にメッセージを入れて false を返す */
bool probeMedia(const QString& path, ProbeInfo& info, QString& error);

/** 動画のサムネイル（高さ 96px） */
std::vector<QImage> videoThumbnails(const QString& path, double duration, int count, const std::atomic<bool>* cancel = nullptr);
/** 動画の最初のフレーム（画像として開けない HEIC などにも使う） */
QImage decodeFirstFrame(const QString& path, int maxSide);
Waveform audioWaveform(const QString& path, double duration);

/** 開いた動画ストリーム（デコードとフレーム変換） */
class VideoDecoder {
 public:
  VideoDecoder() = default;
  ~VideoDecoder();
  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  bool open(const QString& path, QString* error = nullptr);
  void close();
  bool isOpen() const { return fmt_ != nullptr; }
  /** 素材の時刻 t より前のキーフレームへ移動する */
  void seek(double t);
  /** 次のフレームをデコードする。終わりなら false */
  bool decodeNext();
  /** 直前にデコードしたフレームの時刻（秒・先頭が 0） */
  double framePts() const { return pts_; }
  /** 直前のフレームを画像にする（回転・SAR を反映し、maxW x maxH に収める） */
  QImage frameImage(int maxW, int maxH);
  double frameDuration() const { return frameDur_; }
  int displayWidth() const { return dispW_; }
  int displayHeight() const { return dispH_; }
  int rotation() const { return rotation_; }

 private:
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* dec_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVFrame* tmp_ = nullptr;
  SwsContext* sws_ = nullptr;
  int swsSrcW_ = 0, swsSrcH_ = 0, swsSrcFmt_ = -1, swsDstW_ = 0, swsDstH_ = 0;
  int stream_ = -1;
  double timeBase_ = 0;
  long long startTs_ = 0;
  double pts_ = 0;
  double frameDur_ = 1.0 / 30;
  int rotation_ = 0;
  int dispW_ = 0, dispH_ = 0;
  bool flushing_ = false;
  bool eof_ = false;
};

/** 音声を 48kHz・ステレオ・float で読み出す（速度変更は atempo で音程を保つ） */
class AudioDecoder {
 public:
  AudioDecoder() = default;
  ~AudioDecoder();
  AudioDecoder(const AudioDecoder&) = delete;
  AudioDecoder& operator=(const AudioDecoder&) = delete;

  bool open(const QString& path);
  bool isOpen() const { return fmt_ != nullptr; }
  /** 素材の時刻 t から、tempo 倍速で読み出す準備をする */
  void seek(double t, double tempo);
  /** frames フレーム分を書き込む（足りない分は 0）。interleaved stereo */
  void read(float* out, int frames);

 private:
  bool fill();
  bool buildTempo(double tempo);
  void pushSamples(const float* data, int frames);

  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* dec_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwrContext* swr_ = nullptr;
  AVFilterGraph* graph_ = nullptr;
  AVFilterContext* src_ = nullptr;
  AVFilterContext* sink_ = nullptr;
  int stream_ = -1;
  double timeBase_ = 0;
  long long startTs_ = 0;
  double discardUntil_ = 0;
  long long outSamples_ = 0;
  double tempo_ = 1;
  bool eof_ = false;
  std::vector<float> fifo_;
  size_t fifoPos_ = 0;
};

}  // namespace kv
