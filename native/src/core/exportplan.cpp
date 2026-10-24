#include "core/exportplan.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/filters.h"
#include "core/timeline.h"

namespace kv {

QString num6(double v) {
  if (!std::isfinite(v)) return QStringLiteral("0");
  QString s = QString::number(v, 'f', 6);
  if (s.contains(u'.')) {
    while (s.endsWith(u'0')) s.chop(1);
    if (s.endsWith(u'.')) s.chop(1);
  }
  if (s == u"-0" || s.isEmpty()) return QStringLiteral("0");
  return s;
}

static long long jsRound(double v) {
  return (long long)std::floor(v + 0.5);
}

static int evenSize(double v) {
  return std::max(2, int(jsRound(v / 2) * 2));
}

static const QString kAformat =
    QStringLiteral("aformat=sample_fmts=fltp:sample_rates=%1:channel_layouts=stereo").arg(kAudioRate);

// 素材ごとに色の規格（BT.601 / BT.709、フルレンジ / リミテッド）が違っても色がずれないよう、
// どの区間も BT.709・リミテッドに変換してから繋ぐ
static const QString kTo709 = QStringLiteral("scale=out_color_matrix=bt709:out_range=tv");
static const QString kTag709 = QStringLiteral("setparams=colorspace=bt709:range=tv:color_primaries=bt709:color_trc=bt709");

static QStringList fitStatements(FitMode fit, const QString& input, const QString& output, const QString& id, int W, int H) {
  // 非正方ピクセルの素材を正方ピクセルに直しておく
  const QString square = QStringLiteral("scale='trunc(iw*sar/2)*2':'trunc(ih/2)*2',setsar=1");
  const QString w = QString::number(W);
  const QString h = QString::number(H);
  if (fit == FitMode::Cover) {
    return {QStringLiteral("[%1]%2,scale=%3:%4:force_original_aspect_ratio=increase,crop=%3:%4,setsar=1[%5]").arg(input, square, w, h, output)};
  }
  if (fit == FitMode::Blur) {
    const QString bw = QString::number(evenSize(W / 8.0));
    const QString bh = QString::number(evenSize(H / 8.0));
    return {
        QStringLiteral("[%1]%2,split=2[%3f][%3b]").arg(input, square, id),
        QStringLiteral("[%1b]scale=%2:%3:force_original_aspect_ratio=increase,crop=%2:%3,scale=%4:%5,boxblur=8:2,scale=%2:%3,setsar=1[%1bb]")
            .arg(id, w, h, bw, bh),
        QStringLiteral("[%1f]scale=%2:%3:force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1[%1fs]").arg(id, w, h),
        QStringLiteral("[%1bb][%1fs]overlay=x=(W-w)/2:y=(H-h)/2[%2]").arg(id, output),
    };
  }
  return {QStringLiteral("[%1]%2,scale=%3:%4:force_original_aspect_ratio=decrease:force_divisible_by=2,pad=%3:%4:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1[%5]")
              .arg(input, square, w, h, output)};
}

QString volumeExpression(const std::vector<VolumePoint>& points) {
  if (points.empty()) return QStringLiteral("1");
  // 後ろのキーフレームから順に、入れ子の if(lt(t, 次の時間), 直線, それ以降) を作る
  QString expr = num6(points.back().volume);
  for (size_t i = points.size() - 1; i-- > 0;) {
    const VolumePoint& a = points[i];
    const VolumePoint& b = points[i + 1];
    const double span = b.t - a.t;
    QString seg;
    if (span <= 1e-9) {
      seg = num6(b.volume);
    } else {
      const double slope = (b.volume - a.volume) / span;
      seg = QStringLiteral("(%1+(%2)*(t-%3))").arg(num6(a.volume), num6(slope), num6(a.t));
    }
    expr = QStringLiteral("if(lt(t,%1),%2,%3)").arg(num6(b.t), seg, expr);
  }
  // 最初のキーフレームより前は、最初の音量のまま
  return QStringLiteral("if(lt(t,%1),%2,%3)").arg(num6(points.front().t), num6(points.front().volume), expr);
}

/** 進み具合 u（0〜1 の式）に変わり方を掛けた式。easeCurve() と同じ形 */
static QString easeExpr(Easing e, const QString& u) {
  switch (e) {
    case Easing::EaseIn: return QStringLiteral("(%1)*(%1)*(%1)").arg(u);
    case Easing::EaseOut: return QStringLiteral("(1-(1-(%1))*(1-(%1))*(1-(%1)))").arg(u);
    case Easing::EaseInOut: return QStringLiteral("(%1)*(%1)*(3-2*(%1))").arg(u);
    default: return QStringLiteral("(%1)").arg(u);
  }
}

QString keyframeExpression(const std::vector<Keyframe>& keys, double base, const QString& T, AnimProp prop) {
  const PropLimits lim = propLimits(prop);
  if (keys.empty()) return num6(clampProp(prop, base));
  // 後ろのキーフレームから順に、入れ子の if(lt(T, 次の時間), その区間の式, それ以降) を作る
  QString expr = num6(keys.back().value);
  for (size_t i = keys.size() - 1; i-- > 0;) {
    const Keyframe& a = keys[i];
    const Keyframe& b = keys[i + 1];
    const double span = b.t - a.t;
    QString seg;
    if (a.easing == Easing::Hold || span <= 1e-9) {
      seg = num6(a.easing == Easing::Hold ? a.value : b.value);
    } else {
      const QString u = QStringLiteral("(%1-%2)/%3").arg(T, num6(a.t), num6(span));
      seg = QStringLiteral("(%1+(%2)*%3)").arg(num6(a.value), num6(b.value - a.value), easeExpr(a.easing, u));
    }
    expr = QStringLiteral("if(lt(%1,%2),%3,%4)").arg(T, num6(b.t), seg, expr);
  }
  // 最初のキーフレームより前は、最初の値のまま
  expr = QStringLiteral("if(lt(%1,%2),%3,%4)").arg(T, num6(keys.front().t), num6(keys.front().value), expr);
  return QStringLiteral("clip(%1,%2,%3)").arg(expr, num6(lim.min), num6(lim.max));
}

/** 位置・拡大率・回転・不透明度を掛ける前の画像を、回転・拡大して [out] にするまでの文。重ねるのは呼び出し側 */
struct LayerChain {
  QStringList statements;
  QString xExpr;  // 重ねる x
  QString yExpr;  // 重ねる y（呼び出し側が、ほかのずらしを足してよい）
};

/**
 * in は yuva420p にできる画像。W×H の全画面レイヤーを、中心 (x*W, y*H) を基準に回転・拡大・不透明度を掛けて out にする。
 * T は重ねるときの「オブジェクトの先頭からの秒数」の式（重ねる側の t から作る）。レイヤー自身の時間（t）は先頭が 0
 */
static LayerChain buildLayerChain(const LayerSpec& L, const QString& in, const QString& out, int W, int H, const QString& overlayT) {
  LayerChain ch;
  const auto& kx = L.anim.keys(AnimProp::X);
  const auto& ky = L.anim.keys(AnimProp::Y);
  const auto& ks = L.anim.keys(AnimProp::Scale);
  const auto& kr = L.anim.keys(AnimProp::Rotation);
  const auto& ko = L.anim.keys(AnimProp::Opacity);
  const bool needRotate = !kr.empty() || std::abs(L.rotation) > 1e-6;
  const bool needScale = !ks.empty() || std::abs(L.scale - 1) > 1e-6;
  const bool keyedOpacity = !ko.empty();
  const bool needOpacity = keyedOpacity || L.opacity < 0.9999;

  // 回転するときは、どの角度でも欠けないよう、対角線の長さの正方形にする
  const int big = evenSize(std::ceil(std::hypot(double(W), double(H))) + 1);
  const int lw = needRotate ? big : W;
  const int lh = needRotate ? big : H;

  QStringList chain{QStringLiteral("[%1]format=yuva420p").arg(in)};
  if (needOpacity) {
    if (keyedOpacity) {
      // 1 画素ごとの式で、不透明度を時間で変える（色は変えず、透明度だけ掛ける）
      const QString op = keyframeExpression(ko, L.opacity, QStringLiteral("T"), AnimProp::Opacity);
      chain << QStringLiteral("geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='alpha(X,Y)*(%1)'").arg(op);
    } else {
      chain << QStringLiteral("lutyuv=a='val*%1'").arg(num6(L.opacity));
    }
  }
  if (needRotate) {
    const QString rot = keyframeExpression(kr, L.rotation, QStringLiteral("t"), AnimProp::Rotation);
    chain << QStringLiteral("rotate=a='(%1)*PI/180':ow=%2:oh=%3:c=black@0").arg(rot).arg(lw).arg(lh);
  }
  if (needScale) {
    const QString s = keyframeExpression(ks, L.scale, QStringLiteral("t"), AnimProp::Scale);
    chain << QStringLiteral("scale=w='max(2,trunc(%1*(%2)/2)*2)':h='max(2,trunc(%3*(%2)/2)*2)':eval=frame:flags=bilinear").arg(lw).arg(s).arg(lh);
  }
  ch.statements << chain.join(u',') + QStringLiteral("[%1]").arg(out);

  // 重ねる位置: 中心を (x*W, y*H) に合わせる。拡大後の大きさは、上の scale と同じ式
  const QString sT = keyframeExpression(ks, L.scale, overlayT, AnimProp::Scale);
  const QString sw = needScale ? QStringLiteral("max(2,trunc(%1*(%2)/2)*2)").arg(lw).arg(sT) : QString::number(lw);
  const QString sh = needScale ? QStringLiteral("max(2,trunc(%1*(%2)/2)*2)").arg(lh).arg(sT) : QString::number(lh);
  const QString xn = keyframeExpression(kx, L.x, overlayT, AnimProp::X);
  const QString yn = keyframeExpression(ky, L.y, overlayT, AnimProp::Y);
  ch.xExpr = QStringLiteral("(%1)*%2-(%3)/2").arg(xn).arg(W).arg(sw);
  ch.yExpr = QStringLiteral("(%1)*%2-(%3)/2").arg(yn).arg(H).arg(sh);
  return ch;
}

static QStringList fades(const char* filter, double fadeIn, double fadeOut, double dur) {
  QStringList out;
  const double fi = std::min(std::max(0.0, fadeIn), dur);
  const double fo = std::min(std::max(0.0, fadeOut), dur);
  const QString f = QString::fromLatin1(filter);
  if (fi > 0.001) out << QStringLiteral("%1=t=in:st=0:d=%2").arg(f, num6(fi));
  if (fo > 0.001) out << QStringLiteral("%1=t=out:st=%2:d=%3").arg(f, num6(dur - fo), num6(fo));
  return out;
}

ExportPlan buildExportPlan(const ExportRequest& req, const std::vector<PlannedText>& texts) {
  const int W = evenSize(req.width);
  const int H = evenSize(req.height);
  const int FPS = std::max(1, int(jsRound(req.fps)));
  if (req.clips.empty()) throw std::runtime_error("クリップがありません");

  ExportPlan plan;
  QStringList graph;
  int nextInput = 0;
  auto addInput = [&](const QStringList& args) {
    plan.inputArgs << args;
    return nextInput++;
  };

  // ---- ビデオトラック ------------------------------------------------------
  QString segments;
  long long totalFrames = 0;
  for (size_t i = 0; i < req.clips.size(); ++i) {
    const ExportClip& clip = req.clips[i];
    const int frames = std::max(1, int(jsRound(clip.duration * FPS)));
    totalFrames += frames;
    const double dur = double(frames) / FPS;
    const long long samples = jsRound(double(frames) * kAudioRate / FPS);
    const QString color = clip.color ? ffmpegColorFilter(*clip.color) : QString();
    const QString id = QStringLiteral("c%1").arg(i);

    int k;
    QString head;
    if (clip.isImage) {
      k = addInput({QStringLiteral("-i"), clip.path});
      head = QStringLiteral("[%1:v]null[%2src]").arg(k).arg(id);
    } else {
      QStringList args;
      if (clip.in > 0.0005) args << QStringLiteral("-ss") << num6(clip.in);
      args << QStringLiteral("-t") << num6(dur * clip.speed + 0.5) << QStringLiteral("-i") << clip.path;
      k = addInput(args);
      const QString speed = std::abs(clip.speed - 1) > 1e-6 ? QStringLiteral("/") + num6(clip.speed) : QString();
      const QString tag = clip.colorParams.isEmpty() ? QString() : QStringLiteral("setparams=%1,").arg(clip.colorParams);
      head = QStringLiteral("[%1:v]%2setpts=(PTS-STARTPTS)%3,fps=%4[%5src]").arg(k).arg(tag, speed).arg(FPS).arg(id);
    }
    graph << head;
    graph << fitStatements(clip.fit, id + QStringLiteral("src"), id + QStringLiteral("fit"), id, W, H);

    QStringList tail;
    if (!color.isEmpty()) tail << color;
    tail << kTo709 << QStringLiteral("format=yuv420p") << QStringLiteral("setsar=1");
    if (clip.isImage) {
      tail << QStringLiteral("settb=1/%1").arg(FPS) << QStringLiteral("loop=loop=%1:size=1:start=0").arg(frames - 1) << QStringLiteral("setpts=N");
    } else {
      tail << QStringLiteral("tpad=stop_mode=clone:stop_duration=%1").arg(num6(dur + 1)) << QStringLiteral("trim=end_frame=%1").arg(frames)
           << QStringLiteral("setpts=PTS-STARTPTS");
    }
    const QStringList fadeFilters = fades("fade", clip.fadeIn, clip.fadeOut, dur);
    if (clip.layer.transformed) {
      // 位置・拡大率・回転・不透明度を掛ける: 黒い背景の上に、動かしたレイヤーを重ねる（プレビューと同じ。フェードはその上から）
      graph << QStringLiteral("[%1fit]%2[%1t]").arg(id, tail.join(u','));
      const LayerChain ch = buildLayerChain(clip.layer, id + QStringLiteral("t"), id + QStringLiteral("l"), W, H, QStringLiteral("t"));
      graph << ch.statements;
      graph << QStringLiteral("color=c=black:s=%1x%2:r=%3,format=yuv420p[%4bg]").arg(W).arg(H).arg(FPS).arg(id);
      QStringList post{QStringLiteral("[%1bg][%1l]overlay=x='%2':y='%3':eval=frame:shortest=1:format=yuv420").arg(id, ch.xExpr, ch.yExpr),
                       QStringLiteral("setsar=1")};
      post << fadeFilters;
      graph << post.join(u',') + QStringLiteral("[v%1]").arg(i);
    } else {
      tail << fadeFilters;
      graph << QStringLiteral("[%1fit]%2[v%3]").arg(id, tail.join(u','), QString::number(i));
    }

    // 音声（ミュートのときは volume が 0 で渡される。キーフレームがあるときは、それで音量を変える）
    if (!clip.isImage && clip.hasAudio && (clip.volume > 0.0001 || !clip.points.empty())) {
      QStringList a{QStringLiteral("[%1:a]asetpts=PTS-STARTPTS").arg(k), QStringLiteral("aresample=%1").arg(kAudioRate), kAformat};
      for (double f : atempoFactors(clip.speed)) a << QStringLiteral("atempo=") + num6(f);
      if (!clip.points.empty()) {
        // 音量が時間で変わる: 1 サンプルごとに式で音量を掛ける（プレビューと同じ変わり方で、段差やクリックが出ない）
        const QString g = volumeExpression(clip.points);
        a << QStringLiteral("aeval=exprs='val(0)*(%1)|val(1)*(%1)':channel_layout=stereo").arg(g) << kAformat;
      } else if (std::abs(clip.volume - 1) > 1e-4) {
        a << QStringLiteral("volume=") + num6(clip.volume);
      }
      a << QStringLiteral("apad") << QStringLiteral("atrim=end_sample=%1").arg(samples) << QStringLiteral("asetpts=PTS-STARTPTS");
      a << fades("afade", clip.fadeIn, clip.fadeOut, dur);
      graph << QStringLiteral("%1[a%2]").arg(a.join(u',')).arg(i);
    } else {
      graph << QStringLiteral("anullsrc=channel_layout=stereo:sample_rate=%1,atrim=end_sample=%2,%3[a%4]").arg(kAudioRate).arg(samples).arg(kAformat).arg(i);
    }
    segments += QStringLiteral("[v%1][a%1]").arg(i);
  }

  graph << QStringLiteral("%1concat=n=%2:v=1:a=1[vcat][acat]").arg(segments).arg(req.clips.size());

  const double totalDuration = double(totalFrames) / FPS;
  const long long totalSamples = jsRound(double(totalFrames) * kAudioRate / FPS);

  // ---- テキスト -----------------------------------------------------------
  QString vLabel = QStringLiteral("vcat");
  for (size_t j = 0; j < texts.size(); ++j) {
    const PlannedText& t = texts[j];
    const long long startFrame = jsRound(t.start * FPS);
    if (startFrame >= totalFrames) continue;
    const int frames = int(std::max<long long>(1, std::min<long long>(jsRound(t.duration * FPS), totalFrames - startFrame)));
    const double dur = double(frames) / FPS;
    const AnimTimes times = textAnimTimes(dur, t.animation);
    const int k = addInput({QStringLiteral("-i"), t.path});
    QStringList parts{QStringLiteral("[%1:v]format=rgba").arg(k), QStringLiteral("settb=1/%1").arg(FPS),
                      QStringLiteral("loop=loop=%1:size=1:start=0").arg(frames - 1), QStringLiteral("setpts=N")};
    if (times.fadeIn > 0) parts << QStringLiteral("fade=t=in:st=0:d=%1:alpha=1").arg(num6(times.fadeIn));
    if (times.fadeOut > 0) parts << QStringLiteral("fade=t=out:st=%1:d=%2:alpha=1").arg(num6(dur - times.fadeOut), num6(times.fadeOut));
    const double start = double(startFrame) / FPS;
    QString slide;  // スライドの移動量（ピクセル）の式。重ねる側の t で決める
    if (t.animation == TextAnimation::SlideUp && times.fadeIn > 0) {
      slide = QStringLiteral("%1*pow(1-clip((t-%2)/%3,0,1),3)").arg(num6(kTextSlideDistance * H), num6(start), num6(times.fadeIn));
    }
    if (t.layer.transformed) {
      // 位置・拡大率・回転・不透明度を掛ける。回転・拡大・不透明度は文字だけの時間（先頭が 0）で行い、そのあとで開始位置へずらして重ねる
      parts << kTo709 << QStringLiteral("format=yuva420p");
      graph << QStringLiteral("%1[t%2s]").arg(parts.join(u','), QString::number(j));
      const QString overlayT = QStringLiteral("(t-%1)").arg(num6(start));
      const LayerChain ch = buildLayerChain(t.layer, QStringLiteral("t%1s").arg(j), QStringLiteral("t%1l").arg(j), W, H, overlayT);
      graph << ch.statements;
      graph << QStringLiteral("[t%1l]setpts=PTS+%2[t%1]").arg(j).arg(startFrame);
      const QString y = slide.isEmpty() ? ch.yExpr : QStringLiteral("%1+%2").arg(ch.yExpr, slide);
      graph << QStringLiteral("[%1][t%2]overlay=x='%3':y='%4':eval=frame:eof_action=pass[ov%2]").arg(vLabel).arg(j).arg(ch.xExpr, y);
    } else {
      if (startFrame > 0) parts << QStringLiteral("setpts=PTS+%1").arg(startFrame);
      parts << kTo709 << QStringLiteral("format=yuva420p");
      graph << QStringLiteral("%1[t%2]").arg(parts.join(u',')).arg(j);
      const QString y = slide.isEmpty() ? QStringLiteral("0") : QStringLiteral("'%1'").arg(slide);
      graph << QStringLiteral("[%1][t%2]overlay=x=0:y=%3:eof_action=pass[ov%2]").arg(vLabel).arg(j).arg(y);
    }
    vLabel = QStringLiteral("ov%1").arg(j);
  }
  graph << QStringLiteral("[%1]trim=end_frame=%2,format=yuv420p,%3[vout]").arg(vLabel).arg(totalFrames).arg(kTag709);

  // ---- オーディオ ---------------------------------------------------------
  QStringList mix{QStringLiteral("[acat]")};
  for (size_t j = 0; j < req.audios.size(); ++j) {
    const ExportAudio& a = req.audios[j];
    const long long startSample = jsRound(std::max(0.0, a.start) * kAudioRate);
    if (startSample >= totalSamples) continue;
    const double maxDur = double(totalSamples - startSample) / kAudioRate;
    const double dur = std::min(a.duration, maxDur);
    if (dur <= 0.01) continue;
    if (a.points.empty() && a.volume <= 0.0001) continue;
    QStringList args;
    if (a.in > 0.0005) args << QStringLiteral("-ss") << num6(a.in);
    args << QStringLiteral("-t") << num6(dur + 0.25) << QStringLiteral("-i") << a.path;
    const int k = addInput(args);
    QStringList parts{QStringLiteral("[%1:a]asetpts=PTS-STARTPTS").arg(k), QStringLiteral("aresample=%1").arg(kAudioRate), kAformat,
                      QStringLiteral("atrim=end_sample=%1").arg(jsRound(dur * kAudioRate))};
    if (!a.points.empty()) {
      // 音量が時間で変わる: 1 サンプルごとに式で音量を掛ける（プレビューと同じ変わり方で、段差やクリックが出ない）
      const QString g = volumeExpression(a.points);
      parts << QStringLiteral("aeval=exprs='val(0)*(%1)|val(1)*(%1)':channel_layout=stereo").arg(g) << kAformat;
    } else if (std::abs(a.volume - 1) > 1e-4) {
      parts << QStringLiteral("volume=") + num6(a.volume);
    }
    parts << fades("afade", a.fadeIn, a.fadeOut, dur);
    if (startSample > 0) parts << QStringLiteral("adelay=delays=%1S:all=1").arg(startSample);
    graph << QStringLiteral("%1[b%2]").arg(parts.join(u',')).arg(j);
    mix << QStringLiteral("[b%1]").arg(j);
  }
  QString aLabel = QStringLiteral("acat");
  if (mix.size() > 1) {
    graph << QStringLiteral("%1amix=inputs=%2:duration=first:dropout_transition=0:normalize=0,%3[amix]").arg(mix.join(QString())).arg(mix.size()).arg(kAformat);
    aLabel = QStringLiteral("amix");
  }
  // 音量を上げたときや BGM を重ねたときに音が割れないようにリミッターをかける（自動音量調整はしない）
  graph << QStringLiteral("[%1]alimiter=limit=0.97:level=0:latency=1,apad,atrim=end_sample=%2[aout]").arg(aLabel).arg(totalSamples);

  QString preset = QStringLiteral("faster");
  QString crf = QStringLiteral("20");
  if (req.quality == ExportQuality::Fast) {
    preset = QStringLiteral("veryfast");
    crf = QStringLiteral("23");
  } else if (req.quality == ExportQuality::High) {
    preset = QStringLiteral("medium");
    crf = QStringLiteral("18");
  }
  plan.outputArgs = {QStringLiteral("-map"), QStringLiteral("[vout]"), QStringLiteral("-map"), QStringLiteral("[aout]"),
                     QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-preset"), preset,
                     QStringLiteral("-crf"), crf, QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                     QStringLiteral("-color_range"), QStringLiteral("tv"), QStringLiteral("-colorspace"), QStringLiteral("bt709"),
                     QStringLiteral("-color_primaries"), QStringLiteral("bt709"), QStringLiteral("-color_trc"), QStringLiteral("bt709"),
                     QStringLiteral("-r"), QString::number(FPS), QStringLiteral("-c:a"), QStringLiteral("aac"),
                     QStringLiteral("-b:a"), QStringLiteral("192k"), QStringLiteral("-ar"), QString::number(kAudioRate),
                     QStringLiteral("-ac"), QStringLiteral("2"), QStringLiteral("-movflags"), QStringLiteral("+faststart"),
                     QStringLiteral("-max_muxing_queue_size"), QStringLiteral("4096"), QStringLiteral("-t"), num6(totalDuration)};
  plan.filterGraph = graph.join(QStringLiteral(";\n"));
  plan.totalFrames = int(totalFrames);
  plan.duration = totalDuration;
  return plan;
}

}  // namespace kv
