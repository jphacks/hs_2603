#include "export/exporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

#include "core/editor.h"
#include "core/effects.h"
#include "core/filters.h"
#include "core/timeline.h"
#include "mask/masktrack.h"
#include "media/ffutil.h"
#include "render/compositor.h"
#include "render/maskframes.h"
#include "render/textrender.h"

#include <atomic>
#include <thread>

namespace kv {

ExportCheck checkExportable() {
  const Editor& ed = editor();
  const ProjectData& p = ed.project();
  if (p.clips.empty()) return {false, QStringLiteral("クリップがありません")};
  std::vector<QString> used;
  for (const auto& c : p.clips) used.push_back(c.assetId);
  for (const auto& o : p.overlays) {
    if (!o.isEffectLayer()) used.push_back(o.assetId);  // エフェクトレイヤーは素材を使わない
  }
  for (const auto& a : p.audios) used.push_back(a.assetId);
  for (const auto& id : used) {
    const Asset* a = ed.asset(id);
    if (!a) return {false, QStringLiteral("見つからないファイルがあります")};
    if (a->status == AssetStatus::Missing) return {false, QStringLiteral("ファイルが見つかりません: %1").arg(a->name)};
    if (a->status == AssetStatus::Error) return {false, QStringLiteral("読み込めないファイルがあります: %1").arg(a->name)};
    if (a->kind == AssetKind::Image && a->imagePath.isEmpty()) return {false, QStringLiteral("画像を準備しています")};
  }
  return {};
}

QString ffmpegPath() {
  const QString local = QCoreApplication::applicationDirPath() + QStringLiteral("/ffmpeg.exe");
  if (QFileInfo::exists(local)) return local;
  const QString env = qEnvironmentVariable("E_EDITOR_FFMPEG");
  if (!env.isEmpty() && QFileInfo::exists(env)) return env;
  const QString found = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  return found;
}

static int avutilMajor(const QString& ff) {
  static QString cachedFor;
  static int cached = 0;
  if (cachedFor == ff) return cached;
  QProcess p;
  p.start(ff, {QStringLiteral("-hide_banner"), QStringLiteral("-version")});
  p.waitForFinished(15000);
  const QString out = QString::fromUtf8(p.readAllStandardOutput());
  const auto m = QRegularExpression(QStringLiteral("libavutil\\s+(\\d+)\\.")).match(out);
  cachedFor = ff;
  cached = m.hasMatch() ? m.captured(1).toInt() : 58;
  return cached;
}

static QString summarizeError(const QString& stderrText) {
  QStringList lines;
  for (const QString& l : stderrText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) lines << l.trimmed();
  QStringList important;
  static const QRegularExpression re(QStringLiteral("error|invalid|failed|no such|cannot|unable|not found"), QRegularExpression::CaseInsensitiveOption);
  for (const QString& l : lines) {
    if (re.match(l).hasMatch()) important << l;
  }
  const QStringList& pick = important.isEmpty() ? lines : important;
  return pick.mid(std::max<qsizetype>(0, pick.size() - 4)).join(QLatin1Char('\n'));
}

/**
 * 字間が時間で変わる文字の、1 フレームごとの画像（連番）を書く。pattern は text-N-%05d.png の形で、0 番から並べる。
 * 各フレームの字間は、プレビューと同じ layerStateAt() で求め、同じ描き方（textrender）で描く。
 * 字間が前のフレームと同じなら、描き直さずに前の画像を写す。書けたら true
 */
static bool writeSpacingSequence(const ExportRequest& req, const TextItem& t, TextItem centered, QSize design, QSize out, const QString& pattern) {
  const double duration = std::min(t.duration, req.duration - t.start);
  const int frames = exportTextFrames(req, t.start, duration);
  if (frames <= 0) return false;
  const int fps = exportFps(req);
  QString prevFile;
  long long prevKey = 0;
  QElapsedTimer pump;
  pump.start();
  for (int f = 0; f < frames; ++f) {
    const double spacing = layerStateAt(t, double(f) / fps).letterSpacing;
    const long long key = std::llround(spacing * 1000);
    QString file = pattern;
    file.replace(QStringLiteral("%05d"), QStringLiteral("%1").arg(f, 5, 10, QLatin1Char('0')));
    if (f > 0 && key == prevKey) {
      if (!QFile::copy(prevFile, file)) return false;
    } else {
      centered.letterSpacing = spacing;
      if (!renderTextFrame(centered, design, out).save(file, "PNG", 90)) return false;
    }
    prevFile = file;
    prevKey = key;
    // 枚数が多いと時間がかかるので、ときどき画面を描き直して、固まっているように見せない（操作は受け付けない）
    if (pump.elapsed() > 100) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      pump.restart();
    }
  }
  return true;
}

/**
 * オブジェクトマスクの PNG（Grayscale8、素材の大きさ。反転したあとの向き）を、[cNsrc]（素材を readStart から読み、速さで割って lead 秒ずらし、
 * fps で frames 枚にしたもの）のフレームと 1 対 1 で書く。pattern は objmask-…-%05d.png の形で、0 番から並べる（写真は pattern の 1 枚）。
 * n 番のフレームは、ffmpeg の fps と同じく「(素材の時刻 - readStart) / speed + lead を FPS で四捨五入した番号が n 以下のうち、最後のフレーム」
 * （無ければ最初のフレーム）。トラックした範囲の外は、端のマスクのまま（プレビューと同じ）。描くのは同じマスクの 1 回だけで、ほかは写す。書けたら true
 */
static bool writeObjectMaskFrames(const VideoClip& c, const Asset& a, const mask::MaskTrack& track, double readStart, double lead, int frames, int fps,
                                  QSize size, const QString& pattern) {
  if (frames <= 0 || track.empty()) return false;
  auto fileOf = [&](int n) {
    QString f = pattern;
    f.replace(QStringLiteral("%05d"), QStringLiteral("%1").arg(n, 5, 10, QLatin1Char('0')));
    return f;
  };
  // 書き出しの各フレームに使う、素材のフレーム（マスクの番号）
  const int count = int(track.frames.size());
  std::vector<int> pick(size_t(c.isImage ? 1 : frames), 0);
  if (!c.isImage) {
    const double speed = std::max(1e-6, c.speed);
    // -ss で読み始めの位置より前のフレームは捨てられる（残る最初のフレームが、始まりの隙間も埋める）
    int i = 0;
    while (i + 1 < count && track.frames[size_t(i)].pts < readStart - 1e-6) ++i;
    for (int n = 0; n < frames; ++n) {
      // 四捨五入（ちょうど半分は切り上げ）した番号が n 以下 ⇔ 時刻が (n + 0.5) / fps より前
      const double limit = readStart + ((n + 0.5) / fps - lead) * speed - 1e-7;
      while (i + 1 < count && track.frames[size_t(i + 1)].pts < limit) ++i;
      pick[size_t(n)] = i;
    }
  }
  // 同じマスクは 1 回だけ描く（素材のフレームレートが書き出しより低いと、同じフレームが続く）
  std::vector<int> unique;
  std::vector<size_t> slot(pick.size());
  for (size_t n = 0; n < pick.size(); ++n) {
    if (unique.empty() || unique.back() != pick[n]) unique.push_back(pick[n]);
    slot[n] = unique.size() - 1;
  }
  std::vector<QString> drawn(unique.size());
  std::atomic<size_t> next{0};
  std::atomic<int> running{0};
  std::atomic<bool> ok{true};
  auto worker = [&] {
    for (size_t u = next++; u < unique.size() && ok; u = next++) {
      const int index = unique[u];
      FrameMask fm = frameMaskFor(c, &a, c.isImage ? 0.0 : track.frames[size_t(index)].pts);
      if (!fm.logits) fm.logits = track.logits(index);
      const QImage img = frameMaskImage(fm, size, c);
      const QString file = c.isImage ? pattern : pattern + QStringLiteral(".u%1.png").arg(u);
      if (!img.save(file, "PNG", 90)) ok = false;
      drawn[u] = file;
    }
    --running;
  };
  {
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    running = threads;
    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) pool.emplace_back(worker);
    // 描いている間も、ときどき画面を描き直す（固まっているように見せない。操作は受け付けない）
    while (running.load() > 0) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 30);
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    for (auto& th : pool) th.join();
  }
  if (!ok) return false;
  if (c.isImage) return true;
  QElapsedTimer pump;
  pump.start();
  for (int n = 0; n < frames; ++n) {
    if (!QFile::copy(drawn[slot[size_t(n)]], fileOf(n))) return false;
    if (pump.elapsed() > 100) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
      pump.restart();
    }
  }
  for (const QString& f : drawn) QFile::remove(f);
  return true;
}

Exporter::Exporter(QObject* parent) : QObject(parent) {}

Exporter::~Exporter() {
  if (proc_) {
    canceled_ = true;
    proc_->kill();
    proc_->waitForFinished(3000);
  }
}

bool Exporter::start(const QString& outputPath, ExportQuality quality, int shortSide, QString* error, bool rangeOnly, ExportFormat format) {
  if (proc_) {
    if (error) *error = QStringLiteral("別の書き出しを実行中です");
    return false;
  }
  const QString ff = ffmpegPath();
  if (ff.isEmpty()) {
    if (error) *error = QStringLiteral("ffmpeg が見つかりません。アプリを再インストールしてください");
    return false;
  }
  const Editor& ed = editor();
  const ProjectData& p = ed.project();
  // 音だけの形式では、映像は組み立てても使わない（捨てる）ので、小さな大きさで組み立てて早く終える
  const QSize out = exportSize(p.settings.aspect, exportFormatInfo(format).video ? shortSide : 144);
  const QSize design = designSize(p.settings.aspect);
  const TimelineLayout& layout = ed.layout();

  ExportRequest req;
  req.outputPath = outputPath;
  req.width = out.width();
  req.height = out.height();
  req.fps = p.settings.fps;
  req.quality = quality;
  req.format = format;
  req.duration = layout.total;
  req.speed = p.settings.speed;  // 動画全体の再生速度
  if (rangeOnly) {
    const std::optional<Editor::TimeRange> range = ed.exportRange();
    if (!range) {
      if (error) *error = QStringLiteral("書き出す範囲が決まっていません");
      return false;
    }
    req.rangeStart = range->start;
    req.rangeEnd = range->end;
  }
  // 文字・エフェクトの範囲などの画像を書く一時フォルダー（書き出しが終わったら消す）
  work_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/e-editor-export-XXXXXX"));
  if (!work_->isValid()) {
    if (error) *error = QStringLiteral("一時フォルダを作れません");
    return false;
  }
  req.tempDir = work_->path();
  QHash<QString, QString> colorParams;  // 同じ素材を何度も開かない
  // ビデオクリップ 1 つ分（メインのトラックも重ねるトラックも、映像の設定は同じ）
  auto toExportClip = [&](const VideoClip& c, const Asset* a, double duration, bool forceLayer) {
    ExportClip ec;
    ec.isImage = c.isImage;
    ec.path = QDir::toNativeSeparators(c.isImage && !a->imagePath.isEmpty() ? a->imagePath : a->path);
    ec.hasAudio = a->hasAudio;
    ec.in = c.in;
    ec.duration = duration;
    ec.speed = c.isImage ? 1 : c.speed;
    // メインのビデオの音を消している（トラックのミュート）ときも、クリップのミュートと同じ
    const bool muted = c.muted || (!forceLayer && p.mainAudioMuted);
    ec.volume = muted ? 0 : c.volume;
    if (!muted) ec.points = c.volumePoints;
    ec.pan = c.pan;
    ec.hideVideo = !forceLayer && p.mainVideoHidden;
    if (forceLayer || !layerIsPlain(c)) {
      ec.layer.transformed = true;
      ec.layer.x = c.x;
      ec.layer.y = c.y;
      ec.layer.scale = c.scale;
      ec.layer.rotation = c.rotation;
      ec.layer.opacity = c.opacity;
      ec.layer.stretchX = c.stretchX;
      ec.layer.stretchY = c.stretchY;
      ec.layer.anim = c.anim;
    }
    ec.fadeIn = c.fadeIn;
    ec.fadeOut = c.fadeOut;
    ec.fit = c.fit;
    if (!isNeutralColor(c.color)) ec.color = c.color;
    if (!c.isImage) {
      auto it = colorParams.find(a->path);
      if (it == colorParams.end()) it = colorParams.insert(a->path, assumedColorParams(a->path));
      ec.colorParams = *it;
    }
    ec.cropL = c.cropL;
    ec.cropT = c.cropT;
    ec.cropR = c.cropR;
    ec.cropB = c.cropB;
    if (c.chroma.enabled) ec.chroma = c.chroma;
    // 反転とエフェクト。エフェクトの大きさは素材の大きさで決める（正方ピクセルに直したあとの、偶数の大きさ）
    ec.flipX = c.flipX;
    ec.flipY = c.flipY;
    for (const Effect& e : c.effects) {
      if (effectActive(e)) ec.effects.push_back(e);
    }
    ec.srcW = std::max(0, a->width / 2 * 2);
    ec.srcH = std::max(0, a->height / 2 * 2);
    // トランジション（メインのトラックだけ。重ねるトラックでは使わない）
    if (!forceLayer) ec.transIn = c.transIn;
    return ec;
  };
  // オブジェクトマスクを使う映像（フレームごとの PNG は、全部の映像をそろえて、読む範囲が決まってから作る）
  struct MaskItem {
    bool overlay = false;
    size_t index = 0;
    VideoClip clip;
    Asset asset;
    std::shared_ptr<const mask::MaskTrack> track;
  };
  std::vector<MaskItem> maskItems;
  auto noteMask = [&](const VideoClip& c, const Asset* a, bool overlay, size_t index) {
    if (!a || !c.mask.has() || !c.mask.enabled || c.isImage != (a->kind == AssetKind::Image)) return;
    if (c.mask.mode == MaskMode::Effects && !anyEffectActive(c.effects)) return;  // 掛けるエフェクトが無い
    std::shared_ptr<const mask::MaskTrack> t = maskTrackFor(c.mask, a);
    if (t) maskItems.push_back({overlay, index, c, *a, std::move(t)});
  };
  for (const auto& l : layout.items) {
    const VideoClip& c = l.clip;
    const Asset* a = ed.asset(c.assetId);
    if (!a) continue;
    req.clips.push_back(toExportClip(c, a, l.duration, false));
    noteMask(c, a, false, req.clips.size() - 1);
  }
  // 重ねるトラック: 段の順（下の段から）、同じ段は時間の順に重ねる。動画の音も、音量・フェード・キーフレームどおりに鳴らす
  std::vector<const OverlayItem*> overlays;
  for (const auto& o : p.overlays) overlays.push_back(&o);
  std::stable_sort(overlays.begin(), overlays.end(), [](const OverlayItem* a, const OverlayItem* b) {
    if (a->track != b->track) return a->track < b->track;
    return a->start < b->start;
  });
  for (const OverlayItem* o : overlays) {
    // 非表示の段の映像・レイヤーは重ねない（音は、段のミュートで別に決める）
    const bool hidden = isOverlayHidden(p, *o);
    if (o->isEffectLayer()) {
      // エフェクトレイヤー: 範囲の画像を、プレビューと同じ計算で描いておく（動かないなら 1 枚、動くならフレームごとの連番）
      if (o->start >= layout.total || hidden) continue;
      const double dur = overlayDuration(*o, p.settings.fps);
      ExportOverlay eo;
      eo.effectLayer = true;
      eo.start = o->start;
      eo.clip.duration = dur;
      for (const Effect& e : o->effects) {
        if (effectActive(e)) eo.clip.effects.push_back(e);
      }
      if (!isNeutralColor(o->color)) eo.clip.color = o->color;
      if (eo.clip.effects.empty() && !eo.clip.color) continue;  // 何も変わらない
      const int frames = exportTextFrames(req, o->start, dur);
      if (frames <= 0) continue;
      const int index = int(req.overlays.size());
      if (effectLayerMaskStatic(*o)) {
        const QString png = work_->filePath(QStringLiteral("mask-%1.png").arg(index));
        if (!effectLayerMask(*o, 0, dur, out).save(png, "PNG", 90)) continue;
        eo.maskPath = QDir::toNativeSeparators(png);
      } else {
        const QString pattern = work_->filePath(QStringLiteral("mask-%1-%2.png").arg(index).arg(QStringLiteral("%05d")));
        const int fps = exportFps(req);
        QElapsedTimer pump;
        pump.start();
        bool ok = true;
        for (int f = 0; f < frames && ok; ++f) {
          QString file = pattern;
          file.replace(QStringLiteral("%05d"), QStringLiteral("%1").arg(f, 5, 10, QLatin1Char('0')));
          ok = effectLayerMask(*o, double(f) / fps, dur, out).save(file, "PNG", 90);
          // 枚数が多いと時間がかかるので、ときどき画面を描き直して、固まっているように見せない（操作は受け付けない）
          if (pump.elapsed() > 100) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            pump.restart();
          }
        }
        if (!ok) continue;
        eo.maskSequence = true;
        eo.maskPath = QDir::toNativeSeparators(pattern);
      }
      req.overlays.push_back(eo);
      continue;
    }
    const Asset* a = ed.asset(o->assetId);
    if (!a || o->start >= layout.total) continue;
    const double dur = overlayDuration(*o, p.settings.fps);
    ExportOverlay eo;
    eo.clip = toExportClip(*o, a, dur, true);
    eo.clip.fit = FitMode::Contain;
    eo.start = o->start;
    if (!hidden) {
      req.overlays.push_back(eo);
      noteMask(*o, a, true, req.overlays.size() - 1);
    }
    if (!o->isImage && a->hasAudio && !o->muted && !isOverlayMuted(p, *o) && (o->volume > 0.0001 || !o->volumePoints.empty())) {
      ExportAudio ea;
      ea.path = QDir::toNativeSeparators(a->path);
      ea.start = o->start;
      ea.in = o->in;
      ea.duration = dur;
      ea.speed = o->speed;
      ea.volume = o->volume;
      ea.points = o->volumePoints;
      ea.fadeIn = o->fadeIn;
      ea.fadeOut = o->fadeOut;
      ea.pan = o->pan;
      req.audios.push_back(ea);
    }
  }
  for (const auto& au : p.audios) {
    const Asset* a = ed.asset(au.assetId);
    if (!a || au.start >= layout.total || isAudioMuted(p, au)) continue;  // ミュートした段の音は鳴らさない
    ExportAudio ea;
    ea.path = QDir::toNativeSeparators(a->path);
    ea.start = au.start;
    ea.in = au.in;
    ea.duration = audioDuration(au);
    ea.volume = au.volume;
    ea.points = au.volumePoints;
    ea.fadeIn = au.fadeIn;
    ea.fadeOut = au.fadeOut;
    ea.pan = au.pan;
    req.audios.push_back(ea);
  }

  std::vector<PlannedText> texts;
  int i = 0;
  // 下の段の文字から重ねる（タイムラインの上の段の文字ほど手前。プレビューと同じ順）
  for (const size_t index : textPaintOrder(p.texts)) {
    const TextItem& t = p.texts[index];
    if (t.start >= layout.total || t.text.trimmed().isEmpty() || isTextHidden(p, t)) continue;  // 非表示の段の文字は重ねない
    const QString png = work_->filePath(QStringLiteral("text-%1.png").arg(i++));
    PlannedText pt;
    const bool plain = layerIsPlain(t);
    if (plain) {
      // 動かさない文字: 位置まで描き込んだ画像をそのまま重ねる（従来どおり）
      if (!renderTextFrame(t, design, out).save(png, "PNG")) continue;
    } else {
      // 動かす文字: 画面の中央に置いた画像を、位置・拡大率・回転・不透明度（とキーフレーム）で動かす
      TextItem centered = t;
      centered.x = 0.5;
      centered.y = 0.5;
      if (!t.anim.keys(AnimProp::LetterSpacing).empty()) {
        // 字間が時間で変わる文字: 字間は文字の形そのものを変えるので、ffmpeg の式では表せない。
        // プレビューと同じ描き方（textrender）で、1 フレームごとの画像（連番）を作る
        const QString pattern = work_->filePath(QStringLiteral("text-%1-%2.png").arg(i - 1).arg(QStringLiteral("%05d")));
        if (!writeSpacingSequence(req, t, centered, design, out, pattern)) continue;
        pt.sequence = true;
        pt.path = QDir::toNativeSeparators(pattern);
      } else if (!renderTextFrame(centered, design, out).save(png, "PNG")) {
        continue;
      }
      pt.layer.transformed = true;
      pt.layer.x = t.x;
      pt.layer.y = t.y;
      pt.layer.scale = t.scale;
      pt.layer.rotation = t.rotation;
      pt.layer.opacity = t.opacity;
      pt.layer.anim = t.anim;
    }
    if (!pt.sequence) pt.path = QDir::toNativeSeparators(png);
    pt.start = t.start;
    pt.duration = std::min(t.duration, layout.total - t.start);
    pt.animation = t.animation;
    texts.push_back(pt);
  }

  // オブジェクトマスク: 書き出しのフレームごとに、そのフレームに使う素材のフレームのマスクを PNG にする（プレビューと同じ描き方。
  // 素材の大きさで、反転したあとの向き）。作れなかった映像は、マスクを掛けずに書き出す
  if (!maskItems.empty()) {
    const std::vector<ClipReadWindow> windows = mainClipWindows(req);
    const int fps = exportFps(req);
    int n = 0;
    for (const MaskItem& item : maskItems) {
      ExportClip& ec = item.overlay ? req.overlays[item.index].clip : req.clips[item.index];
      if (ec.srcW < 2 || ec.srcH < 2) continue;
      double readStart = 0, lead = 0;
      int frames = 0;
      if (item.overlay) {
        readStart = item.clip.in;
        frames = exportTextFrames(req, req.overlays[item.index].start, ec.duration);
      } else {
        readStart = windows[item.index].readStart;
        lead = windows[item.index].leadSec;
        frames = windows[item.index].frames;
      }
      const QString base = QStringLiteral("objmask-%1").arg(n++);
      const QString pattern = item.clip.isImage ? work_->filePath(base + QStringLiteral(".png")) : work_->filePath(base + QStringLiteral("-%05d.png"));
      if (!writeObjectMaskFrames(item.clip, item.asset, *item.track, readStart, lead, frames, fps, QSize(ec.srcW, ec.srcH), pattern)) continue;
      ec.maskPath = QDir::toNativeSeparators(pattern);
      ec.maskSequence = !item.clip.isImage;
      ec.maskCutout = item.clip.mask.mode == MaskMode::Cutout;
    }
  }

  ExportPlan plan;
  try {
    plan = buildExportPlan(req, texts);
  } catch (const std::exception& e) {
    if (error) *error = QString::fromUtf8(e.what());
    return false;
  }
  const QString graphFile = work_->filePath(QStringLiteral("graph.txt"));
  {
    QFile f(graphFile);
    if (!f.open(QIODevice::WriteOnly)) {
      if (error) *error = QStringLiteral("一時ファイルを書き込めません");
      return false;
    }
    f.write(plan.filterGraph.toUtf8());
  }
  // FFmpeg 7 以降は -/filter_complex、それより前は -filter_complex_script
  const QStringList graphArgs = avutilMajor(ff) >= 59 ? QStringList{QStringLiteral("-/filter_complex"), QDir::toNativeSeparators(graphFile)}
                                                      : QStringList{QStringLiteral("-filter_complex_script"), QDir::toNativeSeparators(graphFile)};
  outPath_ = QDir::toNativeSeparators(outputPath);
  tmpOut_ = outPath_ + QStringLiteral(".part");
  QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"), QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("warning")};
  args << plan.inputArgs << graphArgs << plan.outputArgs;
  args << QStringLiteral("-f") << plan.muxer << QStringLiteral("-progress") << QStringLiteral("pipe:1") << QStringLiteral("-nostats") << tmpOut_;
  if (qEnvironmentVariableIsSet("E_EDITOR_DEBUG_EXPORT")) {
    QFile dbg(QDir::tempPath() + QStringLiteral("/e-editor-last-export.txt"));
    if (dbg.open(QIODevice::WriteOnly)) dbg.write((args.join(QLatin1Char('\n')) + QStringLiteral("\n\n") + plan.filterGraph).toUtf8());
  }

  duration_ = plan.duration;
  canceled_ = false;
  stdoutBuf_.clear();
  stderrBuf_.clear();
  proc_ = new QProcess(this);
  proc_->setProgram(ff);
  proc_->setArguments(args);
  connect(proc_, &QProcess::readyReadStandardOutput, this, [this] {
    stdoutBuf_ += proc_->readAllStandardOutput();
    int nl;
    while ((nl = int(stdoutBuf_.indexOf('\n'))) >= 0) {
      const QByteArray line = stdoutBuf_.left(nl).trimmed();
      stdoutBuf_.remove(0, nl + 1);
      if (!line.startsWith("out_time_us=") && !line.startsWith("out_time_ms=")) continue;
      bool ok = false;
      const double done = line.mid(line.indexOf('=') + 1).toLongLong(&ok) / 1e6;
      if (!ok || duration_ <= 0) continue;
      const double ratio = std::clamp(done / duration_, 0.0, 0.999);
      const double elapsed = timer_.elapsed() / 1000.0;
      const double eta = ratio > 0.02 ? elapsed * (1 - ratio) / ratio : -1;
      emit progress(ratio, elapsed, eta);
    }
  });
  connect(proc_, &QProcess::readyReadStandardError, this, [this] {
    stderrBuf_ += proc_->readAllStandardError();
    if (stderrBuf_.size() > 64000) stderrBuf_ = stderrBuf_.right(32000);
  });
  connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { onFinished(code); });
  connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) onFinished(-1);
  });
  timer_.start();
  proc_->start();
  return true;
}

void Exporter::cancel() {
  if (!proc_) return;
  canceled_ = true;
  proc_->kill();
}

void Exporter::onFinished(int code) {
  if (!proc_) return;
  QProcess* p = proc_;
  proc_ = nullptr;
  p->deleteLater();
  const double elapsed = timer_.elapsed() / 1000.0;
  if (canceled_) {
    QFile::remove(tmpOut_);
    work_.reset();
    emit finished(false, true, QString(), QString(), elapsed);
    return;
  }
  if (code != 0 || !QFileInfo::exists(tmpOut_)) {
    QFile::remove(tmpOut_);
    work_.reset();
    QString msg = summarizeError(QString::fromUtf8(stderrBuf_));
    if (msg.isEmpty()) msg = code == -1 ? QStringLiteral("ffmpeg を起動できませんでした") : QStringLiteral("書き出しに失敗しました");
    emit finished(false, false, QString(), msg, elapsed);
    return;
  }
  QFile::remove(outPath_);
  if (!QFile::rename(tmpOut_, outPath_)) {
    work_.reset();
    emit finished(false, false, QString(), QStringLiteral("保存先に書き込めませんでした"), elapsed);
    return;
  }
  work_.reset();
  emit progress(1, elapsed, 0);
  emit finished(true, false, outPath_, QString(), elapsed);
}

}  // namespace kv
