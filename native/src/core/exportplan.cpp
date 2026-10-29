#include "core/exportplan.h"

#include <QDir>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/effects.h"
#include "core/filters.h"
#include "core/timeline.h"
#include "core/transitions.h"

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

// ---- 書き出す形式 ---------------------------------------------------------------------------

const std::vector<ExportFormatInfo>& exportFormats() {
  using F = ExportFormat;
  static const std::vector<ExportFormatInfo> list = {
      {F::Mp4H264, QStringLiteral("mp4"), QStringLiteral("MP4（H.264）"),
       QStringLiteral("いちばん広く再生できる形式です（YouTube・SNS・スマートフォン・パソコン）"), QStringLiteral("mp4"),
       QStringLiteral("MP4 ビデオ (*.mp4)"), QStringLiteral("mp4"), true, true},
      {F::Mp4H265, QStringLiteral("mp4-h265"), QStringLiteral("MP4（H.265 / HEVC）"),
       QStringLiteral("同じ画質で、H.264 より小さいファイルになります（古い機器・ソフトでは再生できないことがあります）"), QStringLiteral("mp4"),
       QStringLiteral("MP4 ビデオ (*.mp4)"), QStringLiteral("mp4"), true, true},
      {F::WebmVp9, QStringLiteral("webm"), QStringLiteral("WebM（VP9）"), QStringLiteral("Web ページに載せるための形式です（書き出しに時間がかかります）"),
       QStringLiteral("webm"), QStringLiteral("WebM ビデオ (*.webm)"), QStringLiteral("webm"), true, true},
      {F::MovProRes, QStringLiteral("prores"), QStringLiteral("MOV（Apple ProRes）"),
       QStringLiteral("ほかの編集ソフトに渡すための、劣化の少ない形式です（ファイルはとても大きくなります）"), QStringLiteral("mov"),
       QStringLiteral("QuickTime ムービー (*.mov)"), QStringLiteral("mov"), true, true},
      {F::Gif, QStringLiteral("gif"), QStringLiteral("GIF アニメーション"),
       QStringLiteral("音のない、くり返し再生する動画です（数十秒までの短いものがおすすめ）"), QStringLiteral("gif"),
       QStringLiteral("GIF アニメーション (*.gif)"), QStringLiteral("gif"), true, false},
      {F::Mp3, QStringLiteral("mp3"), QStringLiteral("MP3（音声だけ）"), QStringLiteral("映像は書き出さず、音だけを書き出します"), QStringLiteral("mp3"),
       QStringLiteral("MP3 オーディオ (*.mp3)"), QStringLiteral("mp3"), false, true},
      {F::Wav, QStringLiteral("wav"), QStringLiteral("WAV（音声だけ・無圧縮）"), QStringLiteral("映像は書き出さず、音だけを劣化なしで書き出します"),
       QStringLiteral("wav"), QStringLiteral("WAV オーディオ (*.wav)"), QStringLiteral("wav"), false, true},
  };
  return list;
}

const ExportFormatInfo& exportFormatInfo(ExportFormat f) {
  for (const ExportFormatInfo& i : exportFormats()) {
    if (i.format == f) return i;
  }
  return exportFormats().front();
}

bool exportFormatFromString(const QString& s, ExportFormat* out) {
  for (const ExportFormatInfo& i : exportFormats()) {
    if (i.id == s) {
      *out = i.format;
      return true;
    }
  }
  return false;
}

int gifFps(ExportQuality quality, int projectFps) {
  const int want = quality == ExportQuality::Fast ? 10 : quality == ExportQuality::High ? 20 : 15;
  return std::max(1, std::min(want, projectFps));
}

static long long jsRound(double v) {
  return (long long)std::floor(v + 0.5);
}

static int evenSize(double v) {
  return std::max(2, int(jsRound(v / 2) * 2));
}

static const QString kAformat =
    QStringLiteral("aformat=sample_fmts=fltp:sample_rates=%1:channel_layouts=stereo").arg(kAudioRate);

/** パン（バランス: 反対側を弱める。プレビューの panGainLeft / panGainRight と同じ）。真ん中なら何もしない */
static QStringList panFilters(double pan) {
  if (std::abs(pan) < 1e-6) return {};
  return {QStringLiteral("pan=stereo|c0=%1*c0|c1=%2*c1").arg(num6(panGainLeft(pan)), num6(panGainRight(pan))), kAformat};
}

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

/** クロマキーの色かぶりを抑える色（0: 抑えない / 1: 緑 / 2: 青）。プレビュー（imageops の despillType）と同じ決め方 */
static int despillKind(const QColor& key) {
  const int r = key.red(), g = key.green(), b = key.blue();
  if (g > r && g >= b) return 1;
  if (b > r && b > g) return 2;
  return 0;
}

/**
 * クロマキーのフィルター。色の違いは BT.601 の色み（U・V。テレビの範囲）で測る（プレビューと同じ）。境目がきれいになるよう、
 * 色の解像度を落とさない形（yuva444p）で行う。色かぶりの抑えは despill の既定の値（mix 0.5・expand 0）
 * - 透明にする色は U・V にしてから渡す（yuv=1）。RGB のまま渡すと、ffmpeg はフル レンジの式で変換するので、
 *   テレビの範囲にした映像の色みとずれる
 * - chromakey は、周りの 3×3 のうち画面の外の分を、前の画素の値（行の始めは透明にする色）のままにするので、
 *   上の端の 1 行と左の端の所々が半透明になる。1 画素広げて端の画素を繰り返してから行い、広げた分を切り戻す
 *   （プレビューと同じ「端は端の画素を繰り返す」になる）
 */
static QStringList chromaFilters(const ChromaKey& k) {
  const double r = k.color.red(), g = k.color.green(), b = k.color.blue();
  const int u = std::clamp(int(std::lround(128 + (-0.168736 * r - 0.331264 * g + 0.5 * b) * 224.0 / 255.0)), 0, 255);
  const int v = std::clamp(int(std::lround(128 + (0.5 * r - 0.418688 * g - 0.081312 * b) * 224.0 / 255.0)), 0, 255);
  const QString key = QStringLiteral("0x80%1%2").arg(u, 2, 16, QLatin1Char('0')).arg(v, 2, 16, QLatin1Char('0'));
  QStringList f{QStringLiteral("scale=out_color_matrix=bt601:out_range=tv"), QStringLiteral("format=yuva444p"),
                QStringLiteral("pad=w=iw+2:h=ih+2:x=1:y=1"),
                QStringLiteral("fillborders=left=1:right=1:top=1:bottom=1:mode=smear"),
                QStringLiteral("chromakey=color=%1:yuv=1:similarity=%2:blend=%3").arg(key, num6(std::max(1e-5, k.similarity)), num6(std::max(0.0, k.blend))),
                QStringLiteral("crop=w=iw-2:h=ih-2:x=1:y=1")};
  const int sp = k.despill ? despillKind(k.color) : 0;
  if (sp == 1) f << QStringLiteral("format=rgba") << QStringLiteral("despill=type=green:mix=0.5:expand=0:red=0:green=-1:blue=0:brightness=0:alpha=0");
  if (sp == 2) f << QStringLiteral("format=rgba") << QStringLiteral("despill=type=blue:mix=0.5:expand=0:red=0:green=0:blue=-1:brightness=0:alpha=0");
  return f;
}

/** 透明な所を黒で埋めて不透明にする（黒の上に重ねたのと同じ。メインのトラックのクロマキーで使う） */
static QString flattenOnBlack() {
  return QStringLiteral("format=gbrap,premultiply=inplace=1,format=gbrp");
}

static bool clipHasCrop(const ExportClip& c) {
  return c.cropL > 1e-6 || c.cropT > 1e-6 || c.cropR > 1e-6 || c.cropB > 1e-6;
}

/**
 * オブジェクトマスクの入力（PNG）を加えて、[idmk]（素材の大きさの gray。[idsrc] のフレームと同じ時刻）を作る文を graph に足す。
 * マスクを使わない（エフェクトを中だけに掛けるのに、エフェクトが無いときも）なら何もせず空を返す。frames は [idsrc] のフレーム数
 */
static QString clipMaskStatement(const ExportClip& clip, const QString& id, int frames, int FPS, const std::function<int(const QStringList&)>& addInput,
                                 QStringList& graph) {
  if (clip.maskPath.isEmpty() || clip.srcW < 2 || clip.srcH < 2 || frames <= 0) return {};
  if (!clip.maskCutout && !anyEffectActive(clip.effects)) return {};
  const QString label = id + QStringLiteral("mk");
  if (clip.maskSequence && !clip.isImage) {
    const int k = addInput({QStringLiteral("-framerate"), QString::number(FPS), QStringLiteral("-start_number"), QStringLiteral("0"), QStringLiteral("-i"),
                            clip.maskPath});
    // 素材の [idsrc] と同じく、時間の単位を 1/FPS、時刻をフレーム番号にする（同じ番号のフレームどうしが組になる）
    graph << QStringLiteral("[%1:v]format=gray,settb=1/%2,setpts=N,trim=end_frame=%3[%4]").arg(k).arg(FPS).arg(frames).arg(label);
  } else {
    const int k = addInput({QStringLiteral("-i"), clip.maskPath});
    graph << QStringLiteral("[%1:v]format=gray[%2]").arg(k).arg(label);
  }
  return label;
}

/**
 * 反転とエフェクト（素材そのものに、クロマキー・切り抜き・表示方法より前に掛ける。プレビューの prepareSourceFrame と同じ順）。
 * エフェクトの大きさは素材の大きさで決めるので、はじめに素材の大きさ（srcW×srcH）にそろえる。何も掛けないなら空
 */
static QStringList clipPreStatements(const ExportClip& clip, const QString& input, const QString& output, const QString& id, EffectGraphContext& ctx,
                                     const QString& mask = QString()) {
  const bool sized = clip.srcW >= 2 && clip.srcH >= 2;
  const bool fx = anyEffectActive(clip.effects) && sized;
  // オブジェクトマスク（[mask] は素材の大きさの gray。反転したあとの向き）: 背景を消す / エフェクトをマスクの中だけに掛ける
  const bool cut = !mask.isEmpty() && clip.maskCutout && sized;
  const bool fxMask = !mask.isEmpty() && !clip.maskCutout && fx;
  if (!clip.flipX && !clip.flipY && !fx && !cut) return {};
  QStringList head{QStringLiteral("scale='trunc(iw*sar/2)*2':'trunc(ih/2)*2',setsar=1")};
  if (fx || cut) head << QStringLiteral("scale=%1:%2").arg(clip.srcW).arg(clip.srcH) << QStringLiteral("setsar=1");
  if (clip.flipX) head << QStringLiteral("hflip");
  if (clip.flipY) head << QStringLiteral("vflip");
  QStringList out;
  if (fxMask) {
    // エフェクトをマスクの中だけに: エフェクトを掛けた映像の透明度をマスクにして、元の映像に重ねる（プレビューと同じ混ぜ方）
    const QString fl = id + QStringLiteral("fl"), org = id + QStringLiteral("org"), fxd = id + QStringLiteral("fx");
    out << QStringLiteral("[%1]%2,format=gbrp,split=2[%3][%4]").arg(input, head.join(u','), fl, org);
    out << ffmpegEffectStatements(clip.effects, clip.srcW, clip.srcH, fl, fxd, id, ctx);
    out << QStringLiteral("[%1][%2]alphamerge[%1a]").arg(fxd, mask);
    out << QStringLiteral("[%1][%2a]overlay=x=0:y=0:format=gbrp[%3]").arg(org, fxd, output);
    return out;
  }
  // 背景を消すときは、マスクを掛ける前の映像を一度別の名前にする
  const QString last = cut ? id + QStringLiteral("pm") : output;
  if (!fx) {
    out << QStringLiteral("[%1]%2[%3]").arg(input, head.join(u','), last);
  } else {
    const QString mid = id + QStringLiteral("fl");
    out << QStringLiteral("[%1]%2[%3]").arg(input, head.join(u','), mid);
    out << ffmpegEffectStatements(clip.effects, clip.srcW, clip.srcH, mid, last, id, ctx);
  }
  if (cut) {
    if (clip.chroma) {
      // クロマキーもあるとき: キーはここで（素材の大きさで）掛け、キーで透明にした所はそのまま、残った所にマスクを掛ける
      // （プレビューと同じく、クロマキーの透明度 × マスク。chromakey は透明度を書き換えるので、あとから掛け合わせる）
      out << QStringLiteral("[%1]%2,split=2[%3k1][%3k2]").arg(last, chromaFilters(*clip.chroma).join(u','), id);
      out << QStringLiteral("[%1k2]alphaextract[%1ka]").arg(id);
      out << QStringLiteral("[%1ka][%2]blend=all_mode=multiply[%1km]").arg(id, mask);
      out << QStringLiteral("[%1k1][%1km]alphamerge[%2]").arg(id, output);
    } else {
      out << QStringLiteral("[%1][%2]alphamerge[%3]").arg(last, mask, output);
    }
  }
  return out;
}

/**
 * 切り抜き・クロマキーのある素材と、重ねるトラックの素材を、画面の大きさ W×H の 1 枚にする文（input → output）。
 * 素材は、切り抜く前の全体を fit（全体表示 / 拡大）で収めたときの大きさ・場所のまま、残す部分だけを置く（プレビューと同じ）。
 * 背景は、メインのトラックなら黒（ぼかし背景のときは、切り抜く前の全体をぼかしたもの）、重ねるトラック（transparent）なら透明。
 * 重ねる前に BT.709 にそろえ、背景にも BT.709 の印を付けるので、このあとで色の規格がずれない
 */
static QStringList mediaFitStatements(const ExportClip& clip, FitMode fit, const QString& input, const QString& output, const QString& id,
                                      int W, int H, int FPS, bool transparent, const QString& to709, const QString& tag709, bool alphaIn = false) {
  const QString square = QStringLiteral("scale='trunc(iw*sar/2)*2':'trunc(ih/2)*2',setsar=1");
  const double l = clip.cropL, t = clip.cropT;
  const double cw = std::max(0.05, 1 - clip.cropL - clip.cropR);
  const double ch = std::max(0.05, 1 - clip.cropT - clip.cropB);
  QStringList out;
  QString media = id + QStringLiteral("fa");
  const QString bg = id + QStringLiteral("mbg");  // 位置・拡大などを掛けるときの黒い背景（[idbg]）とは別の名前にする
  if (fit == FitMode::Blur && !transparent) {
    const QString bw = QString::number(evenSize(W / 8.0));
    const QString bh = QString::number(evenSize(H / 8.0));
    // クロマキーがあるときは、ぼかす前に透明にした所を黒にする（メインのトラックでは透明にした所は黒。ぼかし背景も、
    // その映像をぼかしたものにする。プレビューと同じ）
    QString key;
    if (clip.chroma) key = QStringLiteral(",") + chromaFilters(*clip.chroma).join(u',') + QStringLiteral(",") + flattenOnBlack();
    // オブジェクトマスクで背景を消した映像（透明度つき）も、ぼかす前に透明な所を黒にする
    else if (alphaIn) key = QStringLiteral(",") + flattenOnBlack();
    out << QStringLiteral("[%1]%2,split=2[%3fa][%3fb]").arg(input, square, id);
    out << QStringLiteral("[%1fb]scale=%2:%3:force_original_aspect_ratio=increase,crop=%2:%3%9,scale=%4:%5,boxblur=8:2,scale=%2:%3,setsar=1,%6,format=yuv420p,%7[%8]")
               .arg(id).arg(W).arg(H).arg(bw, bh, to709, tag709, bg, key);
  } else {
    out << QStringLiteral("[%1]%2[%3]").arg(input, square, media);
    out << QStringLiteral("color=c=%1:s=%2x%3:r=%4,format=%5,%6[%7]")
               .arg(transparent ? QStringLiteral("black@0") : QStringLiteral("black"))
               .arg(W)
               .arg(H)
               .arg(FPS)
               .arg(transparent ? QStringLiteral("yuva420p") : QStringLiteral("yuv420p"), tag709, bg);
  }
  // 素材: 切り抜き → 切り抜く前の全体を収める倍率で縮める（全体表示は min、拡大は max）→ クロマキー → 色 → BT.709
  QStringList m;
  if (clipHasCrop(clip)) {
    m << QStringLiteral("crop=w='iw*%1':h='ih*%2':x='iw*%3':y='ih*%4'").arg(num6(cw), num6(ch), num6(l), num6(t));
  }
  const QString fn = fit == FitMode::Cover && !transparent ? QStringLiteral("max") : QStringLiteral("min");
  const QString s = QStringLiteral("%1(%2*%3/iw,%4*%5/ih)").arg(fn).arg(W).arg(num6(cw)).arg(H).arg(num6(ch));
  m << QStringLiteral("scale=w='max(2,trunc(iw*%1/2)*2)':h='max(2,trunc(ih*%1/2)*2)'").arg(s) << QStringLiteral("setsar=1");
  if (clip.chroma) {
    m << chromaFilters(*clip.chroma);
    // ぼかし背景の上では、透明にした所から後ろのぼかしが見えないよう黒で埋める（ほかの表示方法は、黒い背景の上に重ねる）
    if (fit == FitMode::Blur && !transparent) m << flattenOnBlack();
  } else if (alphaIn && fit == FitMode::Blur && !transparent) {
    m << flattenOnBlack();  // オブジェクトマスクで背景を消した映像も同じ
  }
  // 重ねるトラックは、素材だけに色を掛ける（メインのトラックは、これまでどおり背景と合わせたあとで掛ける）
  if (transparent && clip.color) {
    const QString color = ffmpegColorFilter(*clip.color);
    if (!color.isEmpty()) m << color;
  }
  m << to709 << QStringLiteral("format=yuva420p") << tag709;
  out << QStringLiteral("[%1]%2[%3m]").arg(media, m.join(u','), id);
  // 切り抜く前の全体（幅 w/cw・高さ h/ch）を画面の中央に置いたときの、残す部分の場所に重ねる
  out << QStringLiteral("[%1][%2m]overlay=x='(W-w/%3)/2+%4*w/%3':y='(H-h/%5)/2+%6*h/%5':shortest=1:format=yuv420[%7]")
             .arg(bg, id, num6(cw), num6(l), num6(ch), num6(t), output);
  return out;
}

QString volumeExpression(const std::vector<VolumePoint>& points, const QString& T) {
  if (points.empty()) return QStringLiteral("1");
  // 後ろのキーフレームから順に、入れ子の if(lt(T, 次の時間), 直線, それ以降) を作る
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
      seg = QStringLiteral("(%1+(%2)*(%4-%3))").arg(num6(a.volume), num6(slope), num6(a.t), T);
    }
    expr = QStringLiteral("if(lt(%4,%1),%2,%3)").arg(num6(b.t), seg, expr, T);
  }
  // 最初のキーフレームより前は、最初の音量のまま
  return QStringLiteral("if(lt(%4,%1),%2,%3)").arg(num6(points.front().t), num6(points.front().volume), expr, T);
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
static LayerChain buildLayerChain(const LayerSpec& L, const QString& in, const QString& out, int W, int H, const QString& overlayT,
                                  double timeOffset = 0) {
  LayerChain ch;
  // レイヤー自身の時間（t・geq の T）。トランジションのために前に足したフレームがあるときは、その分を引いて、オブジェクトの先頭を 0 にする
  const QString lt = timeOffset > 1e-9 ? QStringLiteral("(t-%1)").arg(num6(timeOffset)) : QStringLiteral("t");
  const QString gt = timeOffset > 1e-9 ? QStringLiteral("(T-%1)").arg(num6(timeOffset)) : QStringLiteral("T");
  const auto& kx = L.anim.keys(AnimProp::X);
  const auto& ky = L.anim.keys(AnimProp::Y);
  const auto& ks = L.anim.keys(AnimProp::Scale);
  const auto& kr = L.anim.keys(AnimProp::Rotation);
  const auto& ko = L.anim.keys(AnimProp::Opacity);
  const bool needRotate = !kr.empty() || std::abs(L.rotation) > 1e-6;
  const bool needScale = !ks.empty() || std::abs(L.scale - 1) > 1e-6;
  const bool keyedOpacity = !ko.empty();
  const bool needOpacity = keyedOpacity || L.opacity < 0.9999;
  // 伸縮（幅・高さだけの倍率）は、回転の前に、素材の向きのまま掛ける（プレビューの layerMatrix と同じ順）
  const bool stretched = std::abs(L.stretchX - 1) > 1e-6 || std::abs(L.stretchY - 1) > 1e-6;
  const int bw = stretched ? evenSize(W * std::clamp(L.stretchX, kMinStretch, kMaxStretch)) : W;
  const int bh = stretched ? evenSize(H * std::clamp(L.stretchY, kMinStretch, kMaxStretch)) : H;

  // 回転するときは、どの角度でも欠けないよう、対角線の長さの正方形にする
  const int big = evenSize(std::ceil(std::hypot(double(bw), double(bh))) + 1);
  const int lw = needRotate ? big : bw;
  const int lh = needRotate ? big : bh;

  QStringList chain{QStringLiteral("[%1]format=yuva420p").arg(in)};
  if (stretched) chain << QStringLiteral("scale=%1:%2:flags=bicubic,setsar=1").arg(bw).arg(bh);
  if (needOpacity) {
    if (keyedOpacity) {
      // 1 画素ごとの式で、不透明度を時間で変える（色は変えず、透明度だけ掛ける）
      const QString op = keyframeExpression(ko, L.opacity, gt, AnimProp::Opacity);
      chain << QStringLiteral("geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='alpha(X,Y)*(%1)'").arg(op);
    } else {
      chain << QStringLiteral("lutyuv=a='val*%1'").arg(num6(L.opacity));
    }
  }
  if (needRotate) {
    const QString rot = keyframeExpression(kr, L.rotation, lt, AnimProp::Rotation);
    chain << QStringLiteral("rotate=a='(%1)*PI/180':ow=%2:oh=%3:c=black@0").arg(rot).arg(lw).arg(lh);
  }
  if (needScale) {
    const QString s = keyframeExpression(ks, L.scale, lt, AnimProp::Scale);
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

/** フェードイン・アウト（offset は、オブジェクトの先頭の時刻。トランジションのために前に足したフレームがあるときは、その分） */
static QStringList fades(const char* filter, double fadeIn, double fadeOut, double dur, double offset = 0) {
  QStringList out;
  const double fi = std::min(std::max(0.0, fadeIn), dur);
  const double fo = std::min(std::max(0.0, fadeOut), dur);
  const QString f = QString::fromLatin1(filter);
  if (fi > 0.001) out << QStringLiteral("%1=t=in:st=%2:d=%3").arg(f, num6(offset), num6(fi));
  if (fo > 0.001) out << QStringLiteral("%1=t=out:st=%2:d=%3").arg(f, num6(offset + dur - fo), num6(fo));
  return out;
}

int exportFps(const ExportRequest& req) {
  return std::max(1, int(jsRound(req.fps)));
}

long long exportTotalFrames(const ExportRequest& req) {
  const int FPS = exportFps(req);
  long long total = 0;
  for (const ExportClip& clip : req.clips) total += std::max(1, int(jsRound(clip.duration * FPS)));
  return total;
}

static int textFramesOf(long long totalFrames, int FPS, double start, double duration, long long* startFrame) {
  const long long s = jsRound(start * FPS);
  if (startFrame) *startFrame = s;
  if (s >= totalFrames) return 0;
  return int(std::max<long long>(1, std::min<long long>(jsRound(duration * FPS), totalFrames - s)));
}

int exportTextFrames(const ExportRequest& req, double start, double duration, long long* startFrame) {
  return textFramesOf(exportTotalFrames(req), exportFps(req), start, duration, startFrame);
}

std::vector<ClipReadWindow> mainClipWindows(const ExportRequest& req) {
  const int FPS = exportFps(req);
  const size_t n = req.clips.size();
  std::vector<int> counts(n);
  for (size_t i = 0; i < n; ++i) counts[i] = std::max(1, int(jsRound(req.clips[i].duration * FPS)));
  std::vector<TransitionSpan> spans(n);
  for (size_t i = 1; i < n; ++i) spans[i] = transitionSpanFor(req.clips[i].transIn, counts[i - 1], counts[i], FPS);
  std::vector<ClipReadWindow> out(n);
  for (size_t i = 0; i < n; ++i) {
    const ExportClip& clip = req.clips[i];
    // トランジションのために、前（このクリップに入るトランジションの切れ目より前の分）と後ろ（次のトランジションの分）に足すフレーム
    const int pre = spans[i].before;
    const int post = i + 1 < n ? spans[i + 1].after : 0;
    const double preSec = double(pre) / FPS;
    // 素材の中で、足したフレームの始まり（足りなければ 0 から読み、最初のフレームを止めて足す）
    const double wantStart = clip.in - preSec * clip.speed;
    ClipReadWindow& w = out[i];
    w.frames = pre + counts[i] + post;
    w.readStart = clip.isImage ? 0 : std::max(0.0, wantStart);
    // 素材の最初より前を使う分（タイムラインの秒。端数のまま。フレームに丸めると、クリップの中身が半フレームずれることがある）
    w.leadSec = !clip.isImage && pre > 0 ? std::max(0.0, (w.readStart - wantStart) / std::max(1e-6, clip.speed)) : 0;
  }
  return out;
}

ExportPlan buildExportPlan(const ExportRequest& req, const std::vector<PlannedText>& texts) {
  const int W = evenSize(req.width);
  const int H = evenSize(req.height);
  const int FPS = exportFps(req);
  if (req.clips.empty()) throw std::runtime_error("クリップがありません");

  ExportPlan plan;
  QStringList graph;
  int nextInput = 0;
  auto addInput = [&](const QStringList& args) {
    plan.inputArgs << args;
    return nextInput++;
  };
  // エフェクトに使う画像（周辺減光の倍率など）は、一時フォルダーに書いて入力に加える
  int effectImages = 0;
  EffectGraphContext fxctx;
  fxctx.addImage = [&](const QImage& img, const QString& name) -> int {
    if (req.tempDir.isEmpty()) return -1;
    const QString path = QDir(req.tempDir).filePath(QStringLiteral("%1-%2.png").arg(name).arg(effectImages++));
    if (!img.save(path, "PNG")) return -1;
    return addInput({QStringLiteral("-i"), QDir::toNativeSeparators(path)});
  };

  // ---- ビデオトラック ------------------------------------------------------
  // トランジション（切れ目を真ん中にして、前のクリップの続きと次のクリップの手前を xfade で混ぜる。動画の長さは変えない）。
  // 範囲はプレビューと同じ決め方（transitionSpanFor）。前のクリップは後ろに after、次のクリップは前に before のフレームを足して作る
  const size_t clipCount = req.clips.size();
  std::vector<int> clipFrameCounts(clipCount);
  for (size_t i = 0; i < clipCount; ++i) clipFrameCounts[i] = std::max(1, int(jsRound(req.clips[i].duration * FPS)));
  std::vector<TransitionSpan> spans(clipCount);
  bool anyTransition = false;
  for (size_t i = 1; i < clipCount; ++i) {
    spans[i] = transitionSpanFor(req.clips[i].transIn, clipFrameCounts[i - 1], clipFrameCounts[i], FPS);
    anyTransition = anyTransition || spans[i].active();
  }
  // 音もクロスフェードする切れ目（トランジションが入る所だけ）。前のクリップの音は後ろに after、次のクリップの音は前に before の分を足して、
  // 足した所どうしを acrossfade（qsin。プレビューの mixSourceGain と同じ）で重ねる。動画の長さ（音の長さ）は変わらない
  std::vector<bool> audioXf(clipCount, false);
  for (size_t i = 1; i < clipCount; ++i) audioXf[i] = spans[i].active() && req.clips[i].transIn.audioCrossfade;
  auto samplesOf = [&](int frames) { return jsRound(double(frames) * kAudioRate / FPS); };
  QString segments;
  std::vector<int> extFrames(clipCount);
  long long totalFrames = 0;
  // 素材を読む範囲（オブジェクトマスクの PNG を作るときと、同じ数え方）
  const std::vector<ClipReadWindow> windows = mainClipWindows(req);
  for (size_t i = 0; i < req.clips.size(); ++i) {
    const ExportClip& clip = req.clips[i];
    const int frames = clipFrameCounts[i];
    totalFrames += frames;
    const double dur = double(frames) / FPS;
    const long long samples = jsRound(double(frames) * kAudioRate / FPS);
    // 音の前後に足す分（サンプル）
    const long long aHead = audioXf[i] ? samplesOf(spans[i].before) : 0;
    const long long aTail = i + 1 < clipCount && audioXf[i + 1] ? samplesOf(spans[i + 1].after) : 0;
    const double aHeadSec = double(aHead) / kAudioRate;
    const QString color = clip.color ? ffmpegColorFilter(*clip.color) : QString();
    const QString id = QStringLiteral("c%1").arg(i);
    // トランジションのために、前（このクリップに入るトランジションの切れ目より前の分）と後ろ（次のトランジションの分）に足すフレーム
    const int pre = spans[i].before;
    const int ext = windows[i].frames;
    extFrames[i] = ext;
    const double preSec = double(pre) / FPS;
    const double readStart = windows[i].readStart;
    const double leadSec = windows[i].leadSec;

    int k = -1;
    QString head;
    if (clip.isImage && clip.hideVideo) {
      // メインのビデオを非表示: 写真は読まない（音も無い）
    } else if (clip.isImage) {
      k = addInput({QStringLiteral("-i"), clip.path});
      head = QStringLiteral("[%1:v]null[%2src]").arg(k).arg(id);
    } else {
      QStringList args;
      if (readStart > 0.0005) args << QStringLiteral("-ss") << num6(readStart);
      args << QStringLiteral("-t") << num6(double(ext) / FPS * clip.speed + 0.5) << QStringLiteral("-i") << clip.path;
      k = addInput(args);
      const QString speed = std::abs(clip.speed - 1) > 1e-6 ? QStringLiteral("/") + num6(clip.speed) : QString();
      const QString tag = clip.colorParams.isEmpty() ? QString() : QStringLiteral("setparams=%1,").arg(clip.colorParams);
      // 時刻は、読み始めの位置（ファイルの始まり）を 0 にしたまま使う（プレビューと同じ数え方。映像が音より少し遅れて始まる素材でも、
      // フレームと音がずれない）。素材の最初より前を使うトランジションは、その分だけ後ろへずらす。始まりの隙間は、
      // 最初のフレームを止めて埋める（fps の start_time=0）
      const QString shift = leadSec > 1e-6 ? QStringLiteral("+%1/TB").arg(num6(leadSec)) : QString();
      head = QStringLiteral("[%1:v]%2setpts=PTS%3%4,fps=%5:start_time=0[%6src]").arg(k).arg(tag, speed, shift).arg(FPS).arg(id);
    }
    if (clip.hideVideo) {
      // メインのビデオを非表示（トラックの目の印）: 映像の代わりに、同じ長さの黒（音はこのあと、いつもどおり作る）
      QStringList black{QStringLiteral("color=c=black:s=%1x%2:r=%3").arg(W).arg(H).arg(FPS), QStringLiteral("format=yuv420p"), kTag709,
                        QStringLiteral("setsar=1"), QStringLiteral("trim=end_frame=%1").arg(ext), QStringLiteral("setpts=PTS-STARTPTS")};
      if (anyTransition) black << QStringLiteral("fps=%1").arg(FPS);
      graph << black.join(u',') + QStringLiteral("[v%1]").arg(i);
    } else {
      graph << head;
      // オブジェクトマスク（素材のフレームと 1 対 1 の PNG。写真は 1 枚）
      const QString mask = clipMaskStatement(clip, id, ext, FPS, addInput, graph);
      const bool maskCut = !mask.isEmpty() && clip.maskCutout && clip.srcW >= 2 && clip.srcH >= 2;
      // 反転・エフェクト（とオブジェクトマスク）は、素材に先に掛ける
      QString src = id + QStringLiteral("src");
      if (const QStringList pre = clipPreStatements(clip, src, id + QStringLiteral("pre"), id, fxctx, mask); !pre.isEmpty()) {
        graph << pre;
        src = id + QStringLiteral("pre");
      }
      // 背景を消したときは、クロマキーはもう掛けてある（マスクと掛け合わせるため）
      ExportClip fitClip = clip;
      if (maskCut) fitClip.chroma.reset();
      // 切り抜き・クロマキー・背景を消したときは、残す部分（透明にしなかった部分）を黒い背景（ぼかし背景）の上に置く。無ければこれまでどおり
      if (clipHasCrop(clip) || clip.chroma || maskCut) {
        graph << mediaFitStatements(fitClip, clip.fit, src, id + QStringLiteral("fit"), id, W, H, FPS, false, kTo709, kTag709, maskCut);
      } else {
        graph << fitStatements(clip.fit, src, id + QStringLiteral("fit"), id, W, H);
      }

      QStringList tail;
      if (!color.isEmpty()) tail << color;
      tail << kTo709 << QStringLiteral("format=yuv420p") << QStringLiteral("setsar=1");
      if (clip.isImage) {
        tail << QStringLiteral("settb=1/%1").arg(FPS) << QStringLiteral("loop=loop=%1:size=1:start=0").arg(ext - 1) << QStringLiteral("setpts=N");
      } else {
        tail << QStringLiteral("tpad=stop_mode=clone:stop_duration=%1").arg(num6(double(ext) / FPS + 1)) << QStringLiteral("trim=end_frame=%1").arg(ext)
             << QStringLiteral("setpts=PTS-STARTPTS");
      }
      // フェード・アニメーションの時間は、クリップの先頭（足したフレームの後ろ）を 0 にする
      QStringList fadeFilters = fades("fade", clip.fadeIn, clip.fadeOut, dur, preSec);
      // トランジションでつなぐときは、どの区間も同じフレームレートの印にそろえる（xfade が確かめる）
      if (anyTransition) fadeFilters << QStringLiteral("fps=%1").arg(FPS);
      if (clip.layer.transformed) {
        // 位置・拡大率・回転・不透明度を掛ける: 黒い背景の上に、動かしたレイヤーを重ねる（プレビューと同じ。フェードはその上から）
        graph << QStringLiteral("[%1fit]%2[%1t]").arg(id, tail.join(u','));
        const QString localT = pre > 0 ? QStringLiteral("(t-%1)").arg(num6(preSec)) : QStringLiteral("t");
        const LayerChain ch = buildLayerChain(clip.layer, id + QStringLiteral("t"), id + QStringLiteral("l"), W, H, localT, preSec);
        graph << ch.statements;
        graph << QStringLiteral("color=c=black:s=%1x%2:r=%3,format=yuv420p,%4[%5bg]").arg(W).arg(H).arg(FPS).arg(kTag709, id);
        QStringList post{QStringLiteral("[%1bg][%1l]overlay=x='%2':y='%3':eval=frame:shortest=1:format=yuv420").arg(id, ch.xExpr, ch.yExpr),
                         QStringLiteral("setsar=1")};
        post << fadeFilters;
        graph << post.join(u',') + QStringLiteral("[v%1]").arg(i);
      } else {
        tail << fadeFilters;
        graph << QStringLiteral("[%1fit]%2[v%3]").arg(id, tail.join(u','), QString::number(i));
      }
    }

    // 音声（ミュートのときは volume が 0 で渡される。キーフレームがあるときは、それで音量を変える）。
    // 音は、トランジションで足したフレームを使わず、クリップの長さのまま（読み始めを前にずらしたときは、その分を飛ばす）。
    // 音もクロスフェードする切れ目では、前（aHead）・後ろ（aTail）にも素材の音を足す（足した所の音量・フェードは、クリップの端の値のまま）
    const long long aTotal = aHead + samples + aTail;
    if (!clip.isImage && k >= 0 && clip.hasAudio && (clip.volume > 0.0001 || !clip.points.empty())) {
      // 音の時刻も、映像と同じく読み始めの位置を 0 にしたまま使う（プレビューと同じ。素材の音が少し遅れて始まるときは、
      // 始まりを無音で埋める。AAC の頭の詰め物などで、映像と音の始まりがずれないように）
      QStringList a{QStringLiteral("[%1:a]aresample=%2:async=1:first_pts=0").arg(k).arg(kAudioRate)};
      // 音を読み始めたい素材の時刻（前に足すときは、その分だけ前）。素材の最初より前の分は、無音で埋める
      const double aStartSrc = clip.in - aHeadSec * clip.speed;
      const double skip = aStartSrc - readStart;
      if (skip > 1e-6) a << QStringLiteral("atrim=start=%1").arg(num6(skip)) << QStringLiteral("asetpts=PTS-STARTPTS");
      a << QStringLiteral("aresample=%1").arg(kAudioRate) << kAformat;
      for (double f : atempoFactors(clip.speed)) a << QStringLiteral("atempo=") + num6(f);
      if (skip < -1e-6) {
        const long long pad = jsRound(-skip / std::max(1e-6, clip.speed) * kAudioRate);
        if (pad > 0) a << QStringLiteral("adelay=delays=%1S:all=1").arg(pad);
      }
      // 音量の式・フェードの時間は、クリップの先頭（前に足した音の後ろ）を 0 にする
      const QString T = aHead > 0 ? QStringLiteral("(t-%1)").arg(num6(aHeadSec)) : QStringLiteral("t");
      if (!clip.points.empty()) {
        // 音量が時間で変わる: 1 サンプルごとに式で音量を掛ける（プレビューと同じ変わり方で、段差やクリックが出ない）
        const QString g = volumeExpression(clip.points, T);
        a << QStringLiteral("aeval=exprs='val(0)*(%1)|val(1)*(%1)':channel_layout=stereo").arg(g) << kAformat;
      } else if (std::abs(clip.volume - 1) > 1e-4) {
        a << QStringLiteral("volume=") + num6(clip.volume);
      }
      a << panFilters(clip.pan);
      a << QStringLiteral("apad") << QStringLiteral("atrim=end_sample=%1").arg(aTotal) << QStringLiteral("asetpts=PTS-STARTPTS");
      a << fades("afade", clip.fadeIn, clip.fadeOut, dur, aHeadSec);
      graph << QStringLiteral("%1[a%2]").arg(a.join(u',')).arg(i);
    } else {
      graph << QStringLiteral("anullsrc=channel_layout=stereo:sample_rate=%1,atrim=end_sample=%2,%3[a%4]").arg(kAudioRate).arg(aTotal).arg(kAformat).arg(i);
    }
    segments += QStringLiteral("[v%1][a%1]").arg(i);
  }

  if (!anyTransition) {
    graph << QStringLiteral("%1concat=n=%2:v=1:a=1[vcat][acat]").arg(segments).arg(req.clips.size());
  } else {
    // トランジションのある切れ目: 前のクリップの終わりの D フレームと、次のクリップの始まりの D フレームを xfade で混ぜた D フレームを、
    // それぞれの残り（真ん中）の間に入れて、順につなぐ（混ぜる所だけ RGB にする。ほかの所は色を変えない）
    QStringList pieces;
    for (size_t i = 0; i < clipCount; ++i) {
      const int dIn = spans[i].frames();
      const int dOut = i + 1 < clipCount ? spans[i + 1].frames() : 0;
      const int ext = extFrames[i];
      const int body = std::max(0, ext - dIn - dOut);
      const QString v = QStringLiteral("v%1").arg(i);
      QStringList outs;
      if (dIn > 0) outs << v + QStringLiteral("h");
      if (body > 0) outs << v + QStringLiteral("b");
      if (dOut > 0) outs << v + QStringLiteral("e");
      if (outs.size() > 1) {
        QString labels;
        for (const QString& o : outs) labels += QStringLiteral("[%1s]").arg(o);
        graph << QStringLiteral("[%1]split=%2%3").arg(v).arg(outs.size()).arg(labels);
      }
      auto source = [&](const QString& o) { return outs.size() > 1 ? o + QStringLiteral("s") : v; };
      if (dIn > 0) graph << QStringLiteral("[%1]trim=end_frame=%2,setpts=PTS-STARTPTS[%3]").arg(source(v + QStringLiteral("h"))).arg(dIn).arg(v + QStringLiteral("h"));
      if (body > 0) {
        graph << QStringLiteral("[%1]trim=start_frame=%2:end_frame=%3,setpts=PTS-STARTPTS[%4]")
                     .arg(source(v + QStringLiteral("b")))
                     .arg(dIn)
                     .arg(dIn + body)
                     .arg(v + QStringLiteral("b"));
      }
      if (dOut > 0) {
        graph << QStringLiteral("[%1]trim=start_frame=%2,setpts=PTS-STARTPTS[%3]").arg(source(v + QStringLiteral("e"))).arg(dIn + body).arg(v + QStringLiteral("e"));
      }
      if (dIn > 0) {
        const QString x = QStringLiteral("x%1").arg(i);
        const QString name = transitionInfo(req.clips[i].transIn.type).xfade;
        graph << QStringLiteral("[v%1e]format=gbrp[%2a]").arg(i - 1).arg(x);
        graph << QStringLiteral("[%1h]format=gbrp[%2b]").arg(v, x);
        graph << QStringLiteral("[%1a][%1b]xfade=transition=%2:duration=%3:offset=0,%4,format=yuv420p,%5[%1]")
                     .arg(x, name, num6(double(dIn) / FPS), kTo709, kTag709);
        pieces << x;
      }
      if (body > 0) pieces << v + QStringLiteral("b");
    }
    QString labels;
    for (const QString& p : pieces) labels += QStringLiteral("[%1]").arg(p);
    graph << QStringLiteral("%1concat=n=%2:v=1:a=0[vcat]").arg(labels).arg(pieces.size());
    // 音: クロスフェードしない切れ目はそのままつなぎ、クロスフェードする切れ目は、前後に足した音どうしを acrossfade で重ねる
    // （前のクリップの終わりの after 分と、次のクリップの始まりの before 分。qsin どうしなので、重ねている間も音の大きさは変わらない）
    QString acc;
    QString run;  // まだつないでいない、そのままつなぐクリップの音
    int runCount = 0;
    int joins = 0;
    auto flushRun = [&] {
      if (runCount == 0) return;
      QString label;
      if (runCount == 1 && acc.isEmpty()) {
        label = run.mid(1, run.size() - 2);
      } else {
        label = QStringLiteral("aj%1").arg(joins++);
        graph << QStringLiteral("%1%2concat=n=%3:v=0:a=1[%4]").arg(acc.isEmpty() ? QString() : QStringLiteral("[%1]").arg(acc), run).arg(runCount + (acc.isEmpty() ? 0 : 1)).arg(label);
      }
      acc = label;
      run.clear();
      runCount = 0;
    };
    for (size_t i = 0; i < clipCount; ++i) {
      if (i > 0 && audioXf[i]) {
        flushRun();
        const long long n = samplesOf(spans[i].before) + samplesOf(spans[i].after);
        const QString label = QStringLiteral("aj%1").arg(joins++);
        graph << QStringLiteral("[%1][a%2]acrossfade=ns=%3:c1=qsin:c2=qsin[%4]").arg(acc).arg(i).arg(n).arg(label);
        acc = label;
        continue;
      }
      run += QStringLiteral("[a%1]").arg(i);
      ++runCount;
    }
    flushRun();
    graph << QStringLiteral("[%1]anull[acat]").arg(acc);
  }

  const long long totalSamples = jsRound(double(totalFrames) * kAudioRate / FPS);

  // ---- 重ねるトラック ----------------------------------------------------------
  // メインの映像の上に、並べた順（後ろのものほど前）に重ねる。素材は全体表示の大きさで透明の余白を付けた 1 枚にし、
  // 位置・拡大率・回転・伸縮・不透明度（とキーフレーム）で動かして、開始の時間から重ねる（文字はこのあと、いちばん前に重ねる）
  QString vLabel = QStringLiteral("vcat");
  for (size_t j = 0; j < req.overlays.size(); ++j) {
    const ExportOverlay& ov = req.overlays[j];
    const ExportClip& clip = ov.clip;
    long long startFrame = 0;
    const int frames = textFramesOf(totalFrames, FPS, ov.start, clip.duration, &startFrame);
    if (frames <= 0) continue;
    const double dur = double(frames) / FPS;
    const QString id = QStringLiteral("o%1").arg(j);
    if (ov.effectLayer) {
      // エフェクトレイヤー: ここまでの画面を 2 つに分け、片方のその時間だけに効果を掛けて、範囲の画像を透明度にして重ねる。
      // 時間は、どちらも「フレーム番号 / FPS」にそろえる（効果を掛けた画面と範囲の画像が、同じフレームどうしで組になるように）
      const long long endFrame = startFrame + frames;
      graph << QStringLiteral("[%1]split=2[%2b][%2s]").arg(vLabel, id);
      graph << QStringLiteral("[%1s]trim=start_frame=%2:end_frame=%3,settb=1/%4,setpts=N+%2[%1t]").arg(id).arg(startFrame).arg(endFrame).arg(FPS);
      QString fxLabel = id + QStringLiteral("t");
      if (const QStringList fx = ffmpegEffectStatements(clip.effects, W, H, fxLabel, id + QStringLiteral("fx"), id, fxctx); !fx.isEmpty()) {
        graph << fx;
        fxLabel = id + QStringLiteral("fx");
      }
      QStringList tail{QStringLiteral("format=gbrp")};
      if (clip.color) {
        const QString color = ffmpegColorFilter(*clip.color);
        if (!color.isEmpty()) tail << color;
      }
      tail << kTo709 << QStringLiteral("format=yuv420p") << kTag709;
      graph << QStringLiteral("[%1]%2[%3c]").arg(fxLabel, tail.join(u','), id);
      // 範囲の画像（プレビューと同じ計算で描いたもの）
      QStringList m;
      if (ov.maskSequence) {
        const int k = addInput({QStringLiteral("-framerate"), QString::number(FPS), QStringLiteral("-start_number"), QStringLiteral("0"),
                                QStringLiteral("-i"), ov.maskPath});
        m << QStringLiteral("[%1:v]format=gray").arg(k) << QStringLiteral("settb=1/%1").arg(FPS) << QStringLiteral("setpts=N")
          << QStringLiteral("trim=end_frame=%1").arg(frames);
      } else {
        const int k = addInput({QStringLiteral("-i"), ov.maskPath});
        m << QStringLiteral("[%1:v]format=gray").arg(k) << QStringLiteral("settb=1/%1").arg(FPS)
          << QStringLiteral("loop=loop=%1:size=1:start=0").arg(frames - 1);
      }
      m << QStringLiteral("setpts=N+%1").arg(startFrame);
      graph << m.join(u',') + QStringLiteral("[%1m]").arg(id);
      graph << QStringLiteral("[%1c][%1m]alphamerge[%1a]").arg(id);
      graph << QStringLiteral("[%1b][%1a]overlay=eof_action=pass:format=yuv420[ov%1]").arg(id);
      vLabel = QStringLiteral("ov") + id;
      continue;
    }
    if (clip.isImage) {
      const int k = addInput({QStringLiteral("-i"), clip.path});
      graph << QStringLiteral("[%1:v]null[%2src]").arg(k).arg(id);
    } else {
      QStringList args;
      if (clip.in > 0.0005) args << QStringLiteral("-ss") << num6(clip.in);
      args << QStringLiteral("-t") << num6(dur * clip.speed + 0.5) << QStringLiteral("-i") << clip.path;
      const int k = addInput(args);
      const QString speed = std::abs(clip.speed - 1) > 1e-6 ? QStringLiteral("/") + num6(clip.speed) : QString();
      const QString tag = clip.colorParams.isEmpty() ? QString() : QStringLiteral("setparams=%1,").arg(clip.colorParams);
      // 時刻は、読み始めの位置を 0 にしたまま使う（メインのビデオと同じ。始まりの隙間は、最初のフレームを止めて埋める）
      graph << QStringLiteral("[%1:v]%2setpts=PTS%3,fps=%4:start_time=0[%5src]").arg(k).arg(tag, speed).arg(FPS).arg(id);
    }
    // オブジェクトマスク（素材のフレームと 1 対 1 の PNG。写真は 1 枚）
    const QString mask = clipMaskStatement(clip, id, frames, FPS, addInput, graph);
    const bool maskCut = !mask.isEmpty() && clip.maskCutout && clip.srcW >= 2 && clip.srcH >= 2;
    QString src = id + QStringLiteral("src");
    if (const QStringList pre = clipPreStatements(clip, src, id + QStringLiteral("pre"), id, fxctx, mask); !pre.isEmpty()) {
      graph << pre;
      src = id + QStringLiteral("pre");
    }
    ExportClip fitClip = clip;
    if (maskCut) fitClip.chroma.reset();  // クロマキーは、マスクと掛け合わせるために、もう掛けてある
    graph << mediaFitStatements(fitClip, FitMode::Contain, src, id + QStringLiteral("fit"), id, W, H, FPS, true, kTo709, kTag709, maskCut);
    QStringList tail{QStringLiteral("format=yuva420p"), QStringLiteral("setsar=1")};
    if (clip.isImage) {
      tail << QStringLiteral("settb=1/%1").arg(FPS) << QStringLiteral("loop=loop=%1:size=1:start=0").arg(frames - 1) << QStringLiteral("setpts=N");
    } else {
      tail << QStringLiteral("tpad=stop_mode=clone:stop_duration=%1").arg(num6(dur + 1)) << QStringLiteral("trim=end_frame=%1").arg(frames)
           << QStringLiteral("setpts=PTS-STARTPTS");
    }
    // フェードは透明度で（下の映像が見えてくる・見えなくなる）
    const double fi = std::min(std::max(0.0, clip.fadeIn), dur);
    const double fo = std::min(std::max(0.0, clip.fadeOut), dur);
    if (fi > 0.001) tail << QStringLiteral("fade=t=in:st=0:d=%1:alpha=1").arg(num6(fi));
    if (fo > 0.001) tail << QStringLiteral("fade=t=out:st=%1:d=%2:alpha=1").arg(num6(dur - fo), num6(fo));
    graph << QStringLiteral("[%1fit]%2[%1t]").arg(id, tail.join(u','));
    const double start = double(startFrame) / FPS;
    const QString overlayT = QStringLiteral("(t-%1)").arg(num6(start));
    const LayerChain ch = buildLayerChain(clip.layer, id + QStringLiteral("t"), id + QStringLiteral("l"), W, H, overlayT);
    graph << ch.statements;
    graph << QStringLiteral("[%1l]setpts=PTS+%2[%1]").arg(id).arg(startFrame);
    graph << QStringLiteral("[%1][%2]overlay=x='%3':y='%4':eval=frame:eof_action=pass:format=yuv420[ov%2]").arg(vLabel, id, ch.xExpr, ch.yExpr);
    vLabel = QStringLiteral("ov") + id;
  }

  // ---- テキスト -----------------------------------------------------------
  for (size_t j = 0; j < texts.size(); ++j) {
    const PlannedText& t = texts[j];
    long long startFrame = 0;
    const int frames = textFramesOf(totalFrames, FPS, t.start, t.duration, &startFrame);
    if (frames <= 0) continue;
    const double dur = double(frames) / FPS;
    const AnimTimes times = textAnimTimes(dur, t.animation);
    QStringList parts;
    if (t.sequence) {
      // 1 フレームごとの連番画像（字間が時間で変わる文字）。枚数が多くても少なくても、重ねるフレーム数でそろえる
      const int k = addInput({QStringLiteral("-framerate"), QString::number(FPS), QStringLiteral("-start_number"), QStringLiteral("0"), QStringLiteral("-i"), t.path});
      parts << QStringLiteral("[%1:v]format=rgba").arg(k) << QStringLiteral("settb=1/%1").arg(FPS) << QStringLiteral("setpts=N")
            << QStringLiteral("trim=end_frame=%1").arg(frames) << QStringLiteral("setpts=N");
    } else {
      const int k = addInput({QStringLiteral("-i"), t.path});
      parts << QStringLiteral("[%1:v]format=rgba").arg(k) << QStringLiteral("settb=1/%1").arg(FPS)
            << QStringLiteral("loop=loop=%1:size=1:start=0").arg(frames - 1) << QStringLiteral("setpts=N");
    }
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
  // 書き出す範囲（指定が無ければ全体）。範囲のときは、全体を組み立てたあとで切り出し、時間を 0 からに付け直す
  long long outStart = 0;
  long long outEnd = totalFrames;
  if (req.rangeEnd > req.rangeStart) {
    outStart = std::clamp<long long>(jsRound(req.rangeStart * FPS), 0, std::max<long long>(0, totalFrames - 1));
    outEnd = std::clamp<long long>(jsRound(req.rangeEnd * FPS), outStart + 1, totalFrames);
  }
  const bool ranged = outStart > 0 || outEnd < totalFrames;
  // 動画全体の再生速度。範囲を切り出したあとで、映像はフレームの時間を 1/S にしてフレームレートを合わせ直す
  const double S = std::clamp(req.speed, kMinProjectSpeed, kMaxProjectSpeed);
  const bool sped = std::abs(S - 1) > 1e-9;
  {
    QStringList post;
    if (ranged) {
      post << QStringLiteral("trim=start_frame=%1:end_frame=%2").arg(outStart).arg(outEnd) << QStringLiteral("setpts=PTS-STARTPTS");
    } else {
      post << QStringLiteral("trim=end_frame=%1").arg(totalFrames);
    }
    if (sped) post << QStringLiteral("setpts=(PTS-STARTPTS)/%1").arg(num6(S)) << QStringLiteral("fps=%1").arg(FPS);
    post << QStringLiteral("format=yuv420p") << kTag709;
    graph << QStringLiteral("[%1]%2[vout]").arg(vLabel, post.join(u','));
  }

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
    // 速さを変えた動画の音（重ねるトラック）は、読み出す長さを速さの分だけ増やし、atempo で音程を変えずに速さを合わせる
    const double sp = std::clamp(a.speed, 0.1, 16.0);
    QStringList args;
    if (a.in > 0.0005) args << QStringLiteral("-ss") << num6(a.in);
    args << QStringLiteral("-t") << num6(dur * sp + 0.25) << QStringLiteral("-i") << a.path;
    const int k = addInput(args);
    // 時刻は読み始めの位置を 0 にしたまま（メインのビデオの音と同じ。プレビューと同じ数え方）
    QStringList parts{QStringLiteral("[%1:a]aresample=%2:async=1:first_pts=0").arg(k).arg(kAudioRate), kAformat};
    for (double f : atempoFactors(sp)) parts << QStringLiteral("atempo=") + num6(f);
    parts << QStringLiteral("atrim=end_sample=%1").arg(jsRound(dur * kAudioRate));
    if (!a.points.empty()) {
      // 音量が時間で変わる: 1 サンプルごとに式で音量を掛ける（プレビューと同じ変わり方で、段差やクリックが出ない）
      const QString g = volumeExpression(a.points);
      parts << QStringLiteral("aeval=exprs='val(0)*(%1)|val(1)*(%1)':channel_layout=stereo").arg(g) << kAformat;
    } else if (std::abs(a.volume - 1) > 1e-4) {
      parts << QStringLiteral("volume=") + num6(a.volume);
    }
    parts << panFilters(a.pan);
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
  {
    QStringList post{QStringLiteral("alimiter=limit=0.97:level=0:latency=1"), QStringLiteral("apad")};
    if (ranged) {
      const long long s0 = jsRound(double(outStart) * kAudioRate / FPS);
      const long long s1 = jsRound(double(outEnd) * kAudioRate / FPS);
      post << QStringLiteral("atrim=start_sample=%1:end_sample=%2").arg(s0).arg(s1) << QStringLiteral("asetpts=PTS-STARTPTS");
    } else {
      post << QStringLiteral("atrim=end_sample=%1").arg(totalSamples);
    }
    // 動画全体の再生速度: 音程を変えずに速さだけ変える（atempo は 1 段で 0.5〜2 倍なので、掛け合わせる）
    if (sped) {
      if (!ranged) post << QStringLiteral("asetpts=PTS-STARTPTS");
      for (double f : atempoFactors(S)) post << QStringLiteral("atempo=") + num6(f);
    }
    graph << QStringLiteral("[%1]%2[aout]").arg(aLabel, post.join(u','));
  }
  // 書き出す動画の長さ（範囲の長さを、全体の再生速度で割ったもの）
  const double outDuration = double(outEnd - outStart) / FPS / S;

  // ---- 形式ごとの出力（映像・音のエンコーダーと入れ物）。映像はどれも BT.709・リミテッドの [vout]、音は 48kHz ステレオの [aout] ----
  const int q = req.quality == ExportQuality::Fast ? 0 : req.quality == ExportQuality::High ? 2 : 1;
  const QStringList colorTags{QStringLiteral("-color_range"), QStringLiteral("tv"), QStringLiteral("-colorspace"), QStringLiteral("bt709"),
                              QStringLiteral("-color_primaries"), QStringLiteral("bt709"), QStringLiteral("-color_trc"), QStringLiteral("bt709")};
  const QStringList stereo{QStringLiteral("-ar"), QString::number(kAudioRate), QStringLiteral("-ac"), QStringLiteral("2")};
  const QStringList mapBoth{QStringLiteral("-map"), QStringLiteral("[vout]"), QStringLiteral("-map"), QStringLiteral("[aout]")};
  QStringList out;
  switch (req.format) {
    case ExportFormat::Mp4H264: {
      static const char* presets[3] = {"veryfast", "faster", "medium"};
      static const char* crfs[3] = {"23", "20", "18"};
      out << mapBoth << QStringLiteral("-c:v") << QStringLiteral("libx264") << QStringLiteral("-preset") << QString::fromLatin1(presets[q])
          << QStringLiteral("-crf") << QString::fromLatin1(crfs[q]) << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << colorTags
          << QStringLiteral("-r") << QString::number(FPS) << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a")
          << QStringLiteral("192k") << stereo << QStringLiteral("-movflags") << QStringLiteral("+faststart");
      break;
    }
    case ExportFormat::Mp4H265: {
      // H.265 の crf は、H.264 より 4〜6 大きい値で同じくらいの画質。hvc1 の印で、Apple の機器でも再生できる
      static const char* presets[3] = {"veryfast", "faster", "medium"};
      static const char* crfs[3] = {"28", "24", "21"};
      out << mapBoth << QStringLiteral("-c:v") << QStringLiteral("libx265") << QStringLiteral("-preset") << QString::fromLatin1(presets[q])
          << QStringLiteral("-crf") << QString::fromLatin1(crfs[q]) << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
          << QStringLiteral("-tag:v") << QStringLiteral("hvc1") << QStringLiteral("-x265-params") << QStringLiteral("log-level=error") << colorTags
          << QStringLiteral("-r") << QString::number(FPS) << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a")
          << QStringLiteral("192k") << stereo << QStringLiteral("-movflags") << QStringLiteral("+faststart");
      break;
    }
    case ExportFormat::WebmVp9: {
      static const char* crfs[3] = {"36", "32", "28"};
      static const char* speeds[3] = {"5", "4", "2"};
      out << mapBoth << QStringLiteral("-c:v") << QStringLiteral("libvpx-vp9") << QStringLiteral("-crf") << QString::fromLatin1(crfs[q])
          << QStringLiteral("-b:v") << QStringLiteral("0") << QStringLiteral("-deadline") << QStringLiteral("good") << QStringLiteral("-cpu-used")
          << QString::fromLatin1(speeds[q]) << QStringLiteral("-row-mt") << QStringLiteral("1") << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
          << colorTags << QStringLiteral("-r") << QString::number(FPS) << QStringLiteral("-c:a") << QStringLiteral("libopus") << QStringLiteral("-b:a")
          << QStringLiteral("160k") << stereo;
      break;
    }
    case ExportFormat::MovProRes: {
      // 高速 = ProRes 422 LT、標準 = ProRes 422、高画質 = ProRes 422 HQ。音は無圧縮
      out << mapBoth << QStringLiteral("-c:v") << QStringLiteral("prores_ks") << QStringLiteral("-profile:v") << QString::number(q + 1)
          << QStringLiteral("-vendor") << QStringLiteral("apl0") << QStringLiteral("-pix_fmt") << QStringLiteral("yuv422p10le") << colorTags
          << QStringLiteral("-r") << QString::number(FPS) << QStringLiteral("-c:a") << QStringLiteral("pcm_s16le") << stereo;
      break;
    }
    case ExportFormat::Gif: {
      // フレームごとに一番合う 256 色を選ぶ（全体で 1 つの色の表を作る方法と違い、全部のフレームをためておかなくてよい）
      const int gfps = gifFps(req.quality, FPS);
      graph << QStringLiteral("[vout]fps=%1,scale=in_color_matrix=bt709:in_range=tv,format=bgra,split[gifa][gifb]").arg(gfps)
            << QStringLiteral("[gifa]palettegen=stats_mode=single:max_colors=256[gifp]")
            << QStringLiteral("[gifb][gifp]paletteuse=new=1:dither=sierra2_4a[gifout]")
            << QStringLiteral("[aout]anullsink");  // 音は使わない
      out << QStringLiteral("-map") << QStringLiteral("[gifout]") << QStringLiteral("-an") << QStringLiteral("-loop") << QStringLiteral("0");
      break;
    }
    case ExportFormat::Mp3: {
      static const char* rates[3] = {"160k", "192k", "320k"};
      graph << QStringLiteral("[vout]nullsink");  // 映像は使わない
      out << QStringLiteral("-map") << QStringLiteral("[aout]") << QStringLiteral("-c:a") << QStringLiteral("libmp3lame") << QStringLiteral("-b:a")
          << QString::fromLatin1(rates[q]) << stereo;
      break;
    }
    case ExportFormat::Wav: {
      graph << QStringLiteral("[vout]nullsink");  // 映像は使わない
      out << QStringLiteral("-map") << QStringLiteral("[aout]") << QStringLiteral("-c:a")
          << (q == 2 ? QStringLiteral("pcm_s24le") : QStringLiteral("pcm_s16le")) << stereo;
      break;
    }
  }
  out << QStringLiteral("-max_muxing_queue_size") << QStringLiteral("4096") << QStringLiteral("-t") << num6(outDuration);
  plan.outputArgs = out;
  plan.muxer = exportFormatInfo(req.format).muxer;
  plan.filterGraph = graph.join(QStringLiteral(";\n"));
  plan.totalFrames = int(std::max<long long>(1, jsRound(double(outEnd - outStart) / S)));
  plan.duration = outDuration;
  return plan;
}

}  // namespace kv
