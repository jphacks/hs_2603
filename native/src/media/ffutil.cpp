#include "media/ffutil.h"

#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/timeline.h"
#include "core/exportplan.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace kv {

QString ffError(int code) {
  char buf[256] = {0};
  av_strerror(code, buf, sizeof buf);
  return QString::fromUtf8(buf);
}

bool isImagePath(const QString& path) {
  static const QSet<QString> exts = {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"), QStringLiteral("webp"),
                                     QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("tif"), QStringLiteral("tiff"),
                                     QStringLiteral("avif"), QStringLiteral("heic"), QStringLiteral("heif"), QStringLiteral("jfif")};
  return exts.contains(QFileInfo(path).suffix().toLower());
}

bool isAnimatedGif(const QString& path) {
  if (QFileInfo(path).suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) != 0) return false;
  QImageReader reader(path);
  return reader.supportsAnimation() && reader.imageCount() > 1;
}

bool assumeBt709(int width, int height) {
  // 一般的なプレーヤーと同じ判定
  return width >= 1280 || height > 576;
}

QString assumedColorParams(const QString& path) {
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) return {};
  avformat_find_stream_info(fmt, nullptr);
  QStringList parts;
  const int s = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (s >= 0) {
    const AVCodecParameters* par = fmt->streams[s]->codecpar;
    const AVPixelFormat pf = AVPixelFormat(par->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pf);
    // RGB やグレーの素材には付けない
    if (desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->nb_components >= 3) {
      if (par->color_space == AVCOL_SPC_UNSPECIFIED || par->color_space == AVCOL_SPC_RESERVED) {
        parts << (assumeBt709(par->width, par->height) ? QStringLiteral("colorspace=bt709") : QStringLiteral("colorspace=smpte170m"));
      }
      const bool jpegRange = pf == AV_PIX_FMT_YUVJ420P || pf == AV_PIX_FMT_YUVJ422P || pf == AV_PIX_FMT_YUVJ444P;
      if (par->color_range == AVCOL_RANGE_UNSPECIFIED && !jpegRange) parts << QStringLiteral("range=tv");
    }
  }
  avformat_close_input(&fmt);
  return parts.join(u':');
}

static int streamRotation(const AVStream* st) {
  const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                       AV_PKT_DATA_DISPLAYMATRIX);
  if (!sd || sd->size < int(9 * sizeof(int32_t))) return 0;
  double theta = -std::round(av_display_rotation_get(reinterpret_cast<const int32_t*>(sd->data)));
  if (std::isnan(theta)) return 0;
  theta -= 360 * std::floor(theta / 360 + 0.9 / 360);
  const int r = (int(std::lround(theta / 90.0)) * 90) % 360;
  return r < 0 ? r + 360 : r;
}

static double startSeconds(const AVFormatContext* fmt) {
  return fmt->start_time != AV_NOPTS_VALUE ? double(fmt->start_time) / AV_TIME_BASE : 0.0;
}

bool probeMedia(const QString& path, ProbeInfo& info, QString& error) {
  info = {};
  if (!QFileInfo(path).isFile()) {
    error = QStringLiteral("ファイルが見つかりません");
    return false;
  }
  // 動く GIF は、FFmpeg で動画として読む（長さは、コマを出す長さの合計）
  if (isImagePath(path) && !isAnimatedGif(path)) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (size.isValid()) {
      if (reader.transformation() & QImageIOHandler::TransformationRotate90) size.transpose();
    } else {
      const QImage img = decodeFirstFrame(path, 8192);
      if (img.isNull()) {
        error = QStringLiteral("画像を読み込めません");
        return false;
      }
      size = img.size();
    }
    info.isImage = true;
    info.hasVideo = true;
    info.width = size.width();
    info.height = size.height();
    return true;
  }

  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
    error = QStringLiteral("対応していない形式です");
    return false;
  }
  avformat_find_stream_info(fmt, nullptr);
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    const AVStream* st = fmt->streams[i];
    const AVCodecParameters* par = st->codecpar;
    if (par->codec_type == AVMEDIA_TYPE_VIDEO && !info.hasVideo && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      info.hasVideo = true;
      info.videoCodec = QString::fromLatin1(avcodec_get_name(par->codec_id));
      AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : par->sample_aspect_ratio;
      const double s = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
      info.width = int(std::lround(par->width * s));
      info.height = par->height;
      const AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
      info.fps = fr.num && fr.den ? av_q2d(fr) : 0;
      info.rotation = streamRotation(st);
      if (info.rotation == 90 || info.rotation == 270) std::swap(info.width, info.height);
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO && !info.hasAudio) {
      info.hasAudio = true;
      info.audioCodec = QString::fromLatin1(avcodec_get_name(par->codec_id));
    }
  }
  if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
    info.duration = double(fmt->duration) / AV_TIME_BASE;
  } else {
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
      const AVStream* st = fmt->streams[i];
      if (st->duration != AV_NOPTS_VALUE) info.duration = std::max(info.duration, st->duration * av_q2d(st->time_base));
    }
  }
  const QString fmtName = QString::fromLatin1(fmt->iformat->name);
  avformat_close_input(&fmt);
  if (info.hasVideo && !info.hasAudio && (fmtName.contains(QStringLiteral("_pipe")) || fmtName == u"image2")) {
    info.isImage = true;
    info.duration = 0;
  }
  if (!info.hasVideo && !info.hasAudio) {
    error = QStringLiteral("映像・音声が含まれていません");
    return false;
  }
  if (!info.isImage && info.duration <= 0) {
    error = QStringLiteral("長さを取得できません");
    return false;
  }
  return true;
}

// ---- VideoDecoder -----------------------------------------------------------

VideoDecoder::~VideoDecoder() {
  close();
}

void VideoDecoder::close() {
  if (sws_) sws_freeContext(sws_);
  sws_ = nullptr;
  swsSrcFmt_ = -1;
  if (frame_) av_frame_free(&frame_);
  if (tmp_) av_frame_free(&tmp_);
  if (dec_) avcodec_free_context(&dec_);
  if (fmt_) avformat_close_input(&fmt_);
  stream_ = -1;
}

static AVPacket* threadPacket() {
  thread_local AVPacket* pkt = av_packet_alloc();
  return pkt;
}

bool VideoDecoder::open(const QString& path, QString* error) {
  close();
  int r = avformat_open_input(&fmt_, path.toUtf8().constData(), nullptr, nullptr);
  if (r < 0) {
    fmt_ = nullptr;
    if (error) *error = ffError(r);
    return false;
  }
  avformat_find_stream_info(fmt_, nullptr);
  const AVCodec* codec = nullptr;
  stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
  if (stream_ < 0 || !codec) {
    if (error) *error = QStringLiteral("映像がありません");
    close();
    return false;
  }
  AVStream* st = fmt_->streams[stream_];
  dec_ = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(dec_, st->codecpar);
  dec_->pkt_timebase = st->time_base;
  dec_->thread_count = 0;
  dec_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  r = avcodec_open2(dec_, codec, nullptr);
  if (r < 0) {
    if (error) *error = ffError(r);
    close();
    return false;
  }
  frame_ = av_frame_alloc();
  tmp_ = av_frame_alloc();
  timeBase_ = av_q2d(st->time_base);
  startTs_ = 0;
  const AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
  const double fps = fr.num && fr.den ? av_q2d(fr) : 30.0;
  frameDur_ = fps > 1 && fps < 240 ? 1.0 / fps : 1.0 / 30;
  rotation_ = streamRotation(st);
  const AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : st->codecpar->sample_aspect_ratio;
  dispW_ = int(std::lround(st->codecpar->width * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0)));
  dispH_ = st->codecpar->height;
  if (rotation_ == 90 || rotation_ == 270) std::swap(dispW_, dispH_);
  pts_ = -1;
  eof_ = false;
  flushing_ = false;
  return true;
}

void VideoDecoder::seek(double t) {
  if (!fmt_) return;
  const double target = std::max(0.0, t) + startSeconds(fmt_);
  const int64_t ts = int64_t(std::floor(target / timeBase_));
  if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
    av_seek_frame(fmt_, -1, int64_t(target * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
  }
  avcodec_flush_buffers(dec_);
  eof_ = false;
  flushing_ = false;
  pts_ = -1;
}

bool VideoDecoder::decodeNext() {
  if (!dec_ || eof_) return false;
  AVPacket* pkt = threadPacket();
  while (true) {
    // 失敗しても直前のフレームが残るよう、別のフレームに受け取ってから移す
    const int r = avcodec_receive_frame(dec_, tmp_);
    if (r == 0) {
      av_frame_unref(frame_);
      av_frame_move_ref(frame_, tmp_);
      int64_t ts = frame_->best_effort_timestamp;
      if (ts == AV_NOPTS_VALUE) ts = frame_->pts;
      if (ts == AV_NOPTS_VALUE) pts_ = pts_ < 0 ? 0 : pts_ + frameDur_;
      else pts_ = ts * timeBase_ - startSeconds(fmt_);
      return true;
    }
    if (r == AVERROR_EOF || flushing_) {
      eof_ = true;
      return false;
    }
    const int rr = av_read_frame(fmt_, pkt);
    if (rr < 0) {
      avcodec_send_packet(dec_, nullptr);
      flushing_ = true;
      continue;
    }
    if (pkt->stream_index == stream_) avcodec_send_packet(dec_, pkt);
    av_packet_unref(pkt);
  }
}

QImage VideoDecoder::frameImage(int maxW, int maxH) {
  if (!frame_ || frame_->width <= 0 || frame_->height <= 0) return {};
  const int fw = frame_->width;
  const int fh = frame_->height;
  const AVRational sar = frame_->sample_aspect_ratio;
  const double s = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
  const double dw = fw * s;
  const double dh = fh;
  const bool swap = rotation_ == 90 || rotation_ == 270;
  const double outW = swap ? dh : dw;
  const double outH = swap ? dw : dh;
  const double scale = std::min({1.0, maxW / outW, maxH / outH});
  return convertFrame(std::max(2, int(std::lround(dw * scale))), std::max(2, int(std::lround(dh * scale))));
}

QImage VideoDecoder::frameImageStretched(int w, int h) {
  if (!frame_ || frame_->width <= 0 || frame_->height <= 0 || w < 2 || h < 2) return {};
  // 回転する素材は、回転の前の向きで、回転したあとに w x h になる大きさにする
  const bool swap = rotation_ == 90 || rotation_ == 270;
  return convertFrame(swap ? h : w, swap ? w : h);
}

QImage VideoDecoder::convertFrame(int tw, int th) {
  if (!frame_ || frame_->width <= 0 || frame_->height <= 0) return {};
  const int fw = frame_->width;
  const int fh = frame_->height;
  if (!sws_ || swsSrcW_ != fw || swsSrcH_ != fh || swsSrcFmt_ != frame_->format || swsDstW_ != tw || swsDstH_ != th) {
    if (sws_) sws_freeContext(sws_);
    sws_ = sws_getContext(fw, fh, AVPixelFormat(frame_->format), tw, th, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return {};
    swsSrcW_ = fw;
    swsSrcH_ = fh;
    swsSrcFmt_ = frame_->format;
    swsDstW_ = tw;
    swsDstH_ = th;
    int cs;
    switch (frame_->colorspace) {
      case AVCOL_SPC_BT709: cs = SWS_CS_ITU709; break;
      case AVCOL_SPC_BT470BG:
      case AVCOL_SPC_SMPTE170M: cs = SWS_CS_ITU601; break;
      case AVCOL_SPC_BT2020_NCL:
      case AVCOL_SPC_BT2020_CL: cs = SWS_CS_BT2020; break;
      default: cs = assumeBt709(fw, fh) ? SWS_CS_ITU709 : SWS_CS_ITU601; break;
    }
    const AVPixelFormat pf = AVPixelFormat(frame_->format);
    const bool full = frame_->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P || pf == AV_PIX_FMT_YUVJ422P ||
                      pf == AV_PIX_FMT_YUVJ444P;
    int* invTable = nullptr;
    int* table = nullptr;
    int srcRange = 0, dstRange = 0, brightness = 0, contrast = 0, saturation = 0;
    if (sws_getColorspaceDetails(sws_, &invTable, &srcRange, &table, &dstRange, &brightness, &contrast, &saturation) >= 0) {
      sws_setColorspaceDetails(sws_, sws_getCoefficients(cs), full ? 1 : 0, table, 1, brightness, contrast, saturation);
    }
  }
  // swscale は行の終わりを少し越えて書くことがあるので、幅と高さに余裕を持たせた画像に書いてから切り出す
  const int padW = (tw + 31) / 32 * 32;
  QImage buf(padW, th + 2, QImage::Format_RGB32);
  uint8_t* dst[4] = {buf.bits(), nullptr, nullptr, nullptr};
  int dstStride[4] = {int(buf.bytesPerLine()), 0, 0, 0};
  sws_scale(sws_, frame_->data, frame_->linesize, 0, fh, dst, dstStride);
  QImage img = buf.copy(0, 0, tw, th);
  if (rotation_ != 0) img = img.transformed(QTransform().rotate(rotation_));
  return img;
}

std::vector<QImage> videoThumbnails(const QString& path, double duration, int count, const std::atomic<bool>* cancel) {
  std::vector<QImage> out;
  VideoDecoder d;
  if (!d.open(path)) return out;
  const int n = std::clamp(count, 1, 60);
  for (int i = 0; i < n; ++i) {
    if (cancel && cancel->load()) break;
    const double t = duration > 0 ? duration * (i + 0.5) / n : 0;
    d.seek(t);
    bool got = false;
    for (int k = 0; k < 600 && d.decodeNext(); ++k) {
      got = true;
      if (d.framePts() >= t - d.frameDuration() * 0.5) break;
    }
    if (!got) {
      d.seek(0);
      got = d.decodeNext();
    }
    if (got) out.push_back(d.frameImage(4096, 96));
  }
  return out;
}

QImage decodeFirstFrame(const QString& path, int maxSide) {
  VideoDecoder d;
  if (!d.open(path) || !d.decodeNext()) return {};
  return d.frameImage(maxSide, maxSide);
}

Waveform audioWaveform(const QString& path, double duration) {
  Waveform w;
  AVFormatContext* fmt = nullptr;
  if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) return w;
  avformat_find_stream_info(fmt, nullptr);
  const AVCodec* codec = nullptr;
  const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (si < 0 || !codec) {
    avformat_close_input(&fmt);
    return w;
  }
  AVCodecContext* dec = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(dec, fmt->streams[si]->codecpar);
  dec->pkt_timebase = fmt->streams[si]->time_base;
  if (avcodec_open2(dec, codec, nullptr) < 0) {
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return w;
  }
  const int rate = duration > 1200 ? 2000 : 8000;
  w.peaksPerSecond = duration > 1200 ? 10 : 40;
  const int bucket = rate / w.peaksPerSecond;
  SwrContext* swr = nullptr;
  AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
  AVChannelLayout inLayout;
  if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&inLayout, dec->ch_layout.nb_channels);
  else av_channel_layout_copy(&inLayout, &dec->ch_layout);
  swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_FLT, rate, &inLayout, dec->sample_fmt, dec->sample_rate, 0, nullptr);
  av_channel_layout_uninit(&inLayout);
  if (!swr || swr_init(swr) < 0) {
    swr_free(&swr);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return w;
  }
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::vector<float> buf;
  float current = 0;
  int countInBucket = 0;
  auto consume = [&](const float* data, int n) {
    for (int i = 0; i < n; ++i) {
      current = std::max(current, std::abs(data[i]));
      if (++countInBucket >= bucket) {
        w.peaks.push_back(std::min(1.0f, current));
        current = 0;
        countInBucket = 0;
      }
    }
  };
  auto drain = [&](AVFrame* f) {
    const int maxOut = int(swr_get_out_samples(swr, f ? f->nb_samples : 0)) + 32;
    if (maxOut <= 0) return;
    buf.resize(size_t(maxOut));
    uint8_t* outPtr = reinterpret_cast<uint8_t*>(buf.data());
    const int got = swr_convert(swr, &outPtr, maxOut, f ? const_cast<const uint8_t**>(f->extended_data) : nullptr, f ? f->nb_samples : 0);
    if (got > 0) consume(buf.data(), got);
  };
  while (av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0) {
      while (avcodec_receive_frame(dec, frame) == 0) drain(frame);
    }
    av_packet_unref(pkt);
  }
  avcodec_send_packet(dec, nullptr);
  while (avcodec_receive_frame(dec, frame) == 0) drain(frame);
  drain(nullptr);
  if (countInBucket > 0) w.peaks.push_back(std::min(1.0f, current));
  av_frame_free(&frame);
  av_packet_free(&pkt);
  swr_free(&swr);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  return w;
}

// ---- AudioDecoder -----------------------------------------------------------

AudioDecoder::~AudioDecoder() {
  avfilter_graph_free(&graph_);
  swr_free(&swr_);
  if (frame_) av_frame_free(&frame_);
  if (dec_) avcodec_free_context(&dec_);
  if (fmt_) avformat_close_input(&fmt_);
}

bool AudioDecoder::open(const QString& path) {
  if (avformat_open_input(&fmt_, path.toUtf8().constData(), nullptr, nullptr) < 0) {
    fmt_ = nullptr;
    return false;
  }
  avformat_find_stream_info(fmt_, nullptr);
  const AVCodec* codec = nullptr;
  stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (stream_ < 0 || !codec) {
    avformat_close_input(&fmt_);
    return false;
  }
  dec_ = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(dec_, fmt_->streams[stream_]->codecpar);
  dec_->pkt_timebase = fmt_->streams[stream_]->time_base;
  if (avcodec_open2(dec_, codec, nullptr) < 0) {
    avcodec_free_context(&dec_);
    avformat_close_input(&fmt_);
    return false;
  }
  frame_ = av_frame_alloc();
  timeBase_ = av_q2d(fmt_->streams[stream_]->time_base);
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  AVChannelLayout inLayout;
  if (dec_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&inLayout, dec_->ch_layout.nb_channels);
  else av_channel_layout_copy(&inLayout, &dec_->ch_layout);
  swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, kAudioRate, &inLayout, dec_->sample_fmt, dec_->sample_rate, 0, nullptr);
  av_channel_layout_uninit(&inLayout);
  if (!swr_ || swr_init(swr_) < 0) {
    swr_free(&swr_);
    return false;
  }
  return true;
}

bool AudioDecoder::buildTempo(double tempo) {
  avfilter_graph_free(&graph_);
  src_ = sink_ = nullptr;
  if (std::abs(tempo - 1) < 1e-6) return true;
  graph_ = avfilter_graph_alloc();
  const QByteArray srcArgs = QStringLiteral("time_base=1/%1:sample_rate=%1:sample_fmt=flt:channel_layout=stereo").arg(kAudioRate).toUtf8();
  if (avfilter_graph_create_filter(&src_, avfilter_get_by_name("abuffer"), "in", srcArgs.constData(), nullptr, graph_) < 0 ||
      avfilter_graph_create_filter(&sink_, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph_) < 0) {
    avfilter_graph_free(&graph_);
    src_ = sink_ = nullptr;
    return false;
  }
  QStringList chain;
  for (double f : atempoFactors(tempo)) chain << QStringLiteral("atempo=") + num6(f);
  chain << QStringLiteral("aformat=sample_fmts=flt:sample_rates=%1:channel_layouts=stereo").arg(kAudioRate);
  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  outputs->name = av_strdup("in");
  outputs->filter_ctx = src_;
  outputs->pad_idx = 0;
  outputs->next = nullptr;
  inputs->name = av_strdup("out");
  inputs->filter_ctx = sink_;
  inputs->pad_idx = 0;
  inputs->next = nullptr;
  int r = avfilter_graph_parse_ptr(graph_, chain.join(u',').toUtf8().constData(), &inputs, &outputs, nullptr);
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (r >= 0) r = avfilter_graph_config(graph_, nullptr);
  if (r < 0) {
    avfilter_graph_free(&graph_);
    src_ = sink_ = nullptr;
    return false;
  }
  return true;
}

void AudioDecoder::seek(double t, double tempo) {
  if (!fmt_) return;
  // 少し手前から読む（AAC・Opus などは、1 つ前のまとまりを読まないと最初の音が欠けて、小さく始まってしまう）。
  // 手前に何も無い（素材の始まり）ときは、最初のまとまりから読む
  constexpr double kPreroll = 0.1;
  const double target = std::max(0.0, t) - kPreroll + startSeconds(fmt_);
  const int64_t ts = int64_t(std::floor(target / timeBase_));
  if (av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) av_seek_frame(fmt_, stream_, ts, 0);
  avcodec_flush_buffers(dec_);
  swr_close(swr_);
  swr_init(swr_);
  fifo_.clear();
  fifoPos_ = 0;
  eof_ = false;
  // 手前から読んだ分は捨てる（素材の始まりのときも、0 秒より前の音は捨てる）
  discardUntil_ = std::max(1e-9, t);
  outSamples_ = 0;
  tempo_ = tempo;
  buildTempo(tempo);
}

void AudioDecoder::pushSamples(const float* data, int frames) {
  if (frames <= 0) return;
  if (!graph_) {
    fifo_.insert(fifo_.end(), data, data + size_t(frames) * 2);
    return;
  }
  AVFrame* in = av_frame_alloc();
  in->nb_samples = frames;
  in->format = AV_SAMPLE_FMT_FLT;
  in->sample_rate = kAudioRate;
  in->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
  in->pts = outSamples_;
  outSamples_ += frames;
  bool fed = false;
  if (av_frame_get_buffer(in, 0) == 0) {
    std::memcpy(in->data[0], data, size_t(frames) * 2 * sizeof(float));
    fed = av_buffersrc_add_frame(src_, in) >= 0;
  }
  av_frame_free(&in);
  if (!fed) return;
  AVFrame* out = av_frame_alloc();
  while (av_buffersink_get_frame(sink_, out) >= 0) {
    const float* p = reinterpret_cast<const float*>(out->data[0]);
    fifo_.insert(fifo_.end(), p, p + size_t(out->nb_samples) * 2);
    av_frame_unref(out);
  }
  av_frame_free(&out);
}

bool AudioDecoder::fill() {
  if (eof_) return false;
  AVPacket* pkt = threadPacket();
  std::vector<float> buf;
  while (true) {
    const int r = avcodec_receive_frame(dec_, frame_);
    if (r == 0) {
      int64_t ts = frame_->best_effort_timestamp;
      if (ts == AV_NOPTS_VALUE) ts = frame_->pts;
      const double frameStart = ts == AV_NOPTS_VALUE ? discardUntil_ : ts * timeBase_ - startSeconds(fmt_);
      const int maxOut = int(swr_get_out_samples(swr_, frame_->nb_samples)) + 32;
      buf.resize(size_t(maxOut) * 2);
      uint8_t* outPtr = reinterpret_cast<uint8_t*>(buf.data());
      int got = swr_convert(swr_, &outPtr, maxOut, const_cast<const uint8_t**>(frame_->extended_data), frame_->nb_samples);
      if (got <= 0) continue;
      int skip = 0;
      if (discardUntil_ > 0) {
        const double gotDur = double(got) / kAudioRate;
        if (frameStart + gotDur <= discardUntil_) continue;
        if (frameStart < discardUntil_) skip = std::clamp(int(std::lround((discardUntil_ - frameStart) * kAudioRate)), 0, got);
        discardUntil_ = 0;
      }
      pushSamples(buf.data() + size_t(skip) * 2, got - skip);
      return true;
    }
    if (r == AVERROR_EOF) {
      // 残りを出し切る
      const int maxOut = int(swr_get_out_samples(swr_, 0)) + 32;
      buf.resize(size_t(maxOut) * 2);
      uint8_t* outPtr = reinterpret_cast<uint8_t*>(buf.data());
      const int got = swr_convert(swr_, &outPtr, maxOut, nullptr, 0);
      if (got > 0) pushSamples(buf.data(), got);
      if (graph_ && av_buffersrc_add_frame(src_, nullptr) >= 0) {
        AVFrame* out = av_frame_alloc();
        while (av_buffersink_get_frame(sink_, out) >= 0) {
          const float* p = reinterpret_cast<const float*>(out->data[0]);
          fifo_.insert(fifo_.end(), p, p + size_t(out->nb_samples) * 2);
          av_frame_unref(out);
        }
        av_frame_free(&out);
      }
      eof_ = true;
      return false;
    }
    const int rr = av_read_frame(fmt_, pkt);
    if (rr < 0) {
      avcodec_send_packet(dec_, nullptr);
      continue;
    }
    if (pkt->stream_index == stream_) avcodec_send_packet(dec_, pkt);
    av_packet_unref(pkt);
  }
}

void AudioDecoder::read(float* out, int frames) {
  size_t need = size_t(frames) * 2;
  while (fifo_.size() - fifoPos_ < need && fill()) {
  }
  const size_t avail = std::min(need, fifo_.size() - fifoPos_);
  if (avail > 0) std::memcpy(out, fifo_.data() + fifoPos_, avail * sizeof(float));
  if (avail < need) std::memset(out + avail, 0, (need - avail) * sizeof(float));
  fifoPos_ += avail;
  if (fifoPos_ > 1 << 16 && fifoPos_ * 2 > fifo_.size()) {
    fifo_.erase(fifo_.begin(), fifo_.begin() + std::ptrdiff_t(fifoPos_));
    fifoPos_ = 0;
  }
}

}  // namespace kv
