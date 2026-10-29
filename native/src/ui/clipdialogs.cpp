#include "ui/clipdialogs.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "core/editor.h"
#include "core/subtitles.h"
#include "core/timeline.h"
#include "mask/edgetam.h"
#include "mask/maskcontroller.h"
#include "mask/masktrack.h"
#include "media/ffutil.h"
#include "media/mediamanager.h"
#include "media/reverser.h"
#include "render/maskframes.h"
#include "render/stillframe.h"
#include "ui/widgets.h"

namespace kv {

namespace {

// AI の設定・キーの割り当てと同じ settings.ini（属性のペーストで最後に選んだものなど）
QString dialogSettingsPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("settings.ini"));
}

/** 速度の範囲（%）。音は atempo を重ねて、音程を変えずに速さを合わせる */
constexpr double kMinSpeedPct = 10;
constexpr double kMaxSpeedPct = 1000;
/** 写真の長さの上限（タイムラインの端のドラッグと同じ） */
constexpr double kMaxImageDuration = 600;

/** ファイル名に使えない文字を外した、再生位置の時刻（0-12.3 など） */
QString fileTime(double t) {
  QString s = formatTime(t, true);
  s.replace(QLatin1Char(':'), QLatin1Char('-'));
  return s;
}

/**
 * フレームの保持の写真に付けるオブジェクトマスク: 止めたフレーム（素材の時刻 pts）のマスク 1 枚を、新しいファイルにする
 * （新しい目印。元の動画をトラックし直しても、写真のマスクは変えない）。マスクのデータが無ければ nullopt
 */
std::optional<ObjectMask> frameHoldMask(const ObjectMask& m, const Asset* asset, double pts, QSize size) {
  const std::shared_ptr<const mask::MaskTrack> t = maskTrackFor(m, asset);
  if (!t || size.isEmpty()) return std::nullopt;
  const int i = t->nearest(pts);
  const std::shared_ptr<const std::vector<int8_t>> logits = i >= 0 ? t->logits(i) : nullptr;
  if (!logits || logits->size() != size_t(mask::kMaskSize) * mask::kMaskSize) return std::nullopt;
  auto one = std::make_shared<mask::MaskTrack>();
  one->width = size.width();
  one->height = size.height();
  one->frames.push_back({0.0, t->frames[size_t(i)].score, t->frames[size_t(i)].packed});
  const QString file = mask::newMaskFileName();
  if (!one->save(QDir(mask::maskDir()).filePath(file))) return std::nullopt;
  mask::MaskStore::instance().put(file, one);
  ObjectMask out = m;
  out.id = newId();
  out.file = file;
  // トラックし直すとき（ファイルが無い PC など）にクリックしたことにする点: このフレームで、一番はっきり被写体の所
  const auto best = std::max_element(logits->begin(), logits->end());
  if (*best > 0) {
    const int k = int(best - logits->begin());
    const MaskPoint p{(k % mask::kMaskSize + 0.5) / mask::kMaskSize, (k / mask::kMaskSize + 0.5) / mask::kMaskSize, true};
    out.prompts = {MaskPrompt{0.0, {p}}};
  } else {
    out.prompts = {MaskPrompt{0.0, m.prompts.front().points}};
  }
  return out;
}

}  // namespace

void execSpeedDialog(QWidget* parent, const QString& clipId) {
  Editor& ed = editor();
  const VideoClip* found = ed.clip(clipId);
  if (!found) return;
  const VideoClip orig = *found;
  if (ed.overlay(clipId) && ed.overlay(clipId)->isEffectLayer()) {
    ed.toast(QStringLiteral("エフェクトレイヤーの長さは、タイムラインで端をドラッグして変えてください"));
    return;
  }
  ed.pause();
  const int fps = ed.project().settings.fps;
  const double span = orig.out - orig.in;  // 素材の長さ（動画は速度を掛ける前）
  const Asset* a = ed.asset(orig.assetId);

  QDialog dlg(parent ? parent->window() : nullptr);
  dlg.setWindowTitle(QStringLiteral("速度・デュレーション"));
  dlg.setMinimumWidth(380);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(8);
  v->addWidget(makeLabel(a ? a->name : QStringLiteral("映像"), "title", &dlg));

  // 速くしすぎて、クリップが一番短い長さより短くならないように
  const double maxSpeedPct = orig.isImage ? kMaxSpeedPct : std::clamp(span / kMinClipDuration * 100, kMinSpeedPct, kMaxSpeedPct);
  NumberSpin* speed = nullptr;
  if (!orig.isImage) {
    v->addWidget(makeLabel(QStringLiteral("速度"), "muted", &dlg));
    speed = new NumberSpin(&dlg);
    speed->setRange(kMinSpeedPct, maxSpeedPct);
    speed->setDecimals(1);
    speed->setSuffix(QStringLiteral(" %"));
    speed->setUnit(1, 0.1);
    speed->setValue(orig.speed * 100);
    speed->setAccessibleName(QStringLiteral("速度（パーセント）"));
    speed->setObjectName(QStringLiteral("SpeedValue"));  // 操作の案内（ai/guidecatalog.cpp）で探す名前
    v->addWidget(speed);
  }
  v->addWidget(makeLabel(QStringLiteral("デュレーション（タイムラインの上の長さ）"), "muted", &dlg));
  auto* duration = new NumberSpin(&dlg);
  const double minDur = orig.isImage ? kMinClipDuration : std::max(kMinClipDuration, span / (maxSpeedPct / 100));
  const double maxDur = orig.isImage ? kMaxImageDuration : span / (kMinSpeedPct / 100);
  duration->setRange(minDur, std::max(minDur, maxDur));
  duration->setDecimals(2);
  duration->setSuffix(QStringLiteral(" 秒"));
  duration->setUnit(0.1, 0.01);
  duration->setValue(clipDuration(orig, fps));
  duration->setAccessibleName(QStringLiteral("デュレーション（秒）"));
  v->addWidget(duration);
  auto* frames = makeLabel(QString(), "dim", &dlg);
  v->addWidget(frames);
  const bool main = !ed.overlay(clipId);
  QStringList notes;
  if (!orig.isImage) notes << QStringLiteral("音の高さはそのままで、速さだけが変わります。キーフレームも、同じ映像の位置に合わせて伸び縮みします");
  if (main) notes << QStringLiteral("メインのビデオでは、後ろのクリップが詰まる・ずれます（動画の長さが変わります）");
  auto* note = makeLabel(notes.join(QLatin1Char('\n')), "note", &dlg);
  note->setWordWrap(true);
  v->addWidget(note);

  auto showFrames = [frames, duration, fps] {
    frames->setText(QStringLiteral("%1 フレーム（%2fps）").arg(std::lround(duration->value() * fps)).arg(fps));
  };
  showFrames();
  // 速度と長さは連動させる（片方を変えると、もう一方を合わせる）。最後に変えたほうの値を、そのまま使う
  // （長さを入れたときは、表示の桁で丸めた速度ではなく、その長さになる速度にする）
  auto durationEdited = std::make_shared<bool>(false);
  if (speed) {
    QObject::connect(speed, &QDoubleSpinBox::valueChanged, &dlg, [duration, span, showFrames, durationEdited](double pct) {
      *durationEdited = false;
      const QSignalBlocker b(duration);
      duration->setValue(span / std::max(0.01, pct / 100));
      showFrames();
    });
    QObject::connect(duration, &QDoubleSpinBox::valueChanged, &dlg, [speed, span, showFrames, durationEdited, maxSpeedPct](double d) {
      *durationEdited = true;
      const QSignalBlocker b(speed);
      speed->setValue(std::clamp(span / std::max(0.01, d) * 100, kMinSpeedPct, maxSpeedPct));
      showFrames();
    });
  } else {
    QObject::connect(duration, &QDoubleSpinBox::valueChanged, &dlg, [showFrames](double) { showFrames(); });
  }

  // 逆再生（動画だけ。逆向きの動画を作ってから入れ替える）
  QCheckBox* reverse = nullptr;
  if (!orig.isImage) {
    reverse = new QCheckBox(QStringLiteral("逆再生（音も逆になります）"), &dlg);
    reverse->setObjectName(QStringLiteral("SpeedReverse"));
    reverse->setChecked(!orig.reversedFrom.isEmpty());
    reverse->setToolTip(QStringLiteral("はじめて逆再生にするときは、逆向きの動画を作ります（少し時間がかかります）"));
    v->addWidget(reverse);
  }

  auto* buttons = new QDialogButtonBox(&dlg);
  auto* ok = buttons->addButton(QStringLiteral("OK"), QDialogButtonBox::AcceptRole);
  ok->setProperty("primary", true);
  ok->setDefault(true);
  buttons->addButton(QStringLiteral("キャンセル"), QDialogButtonBox::RejectRole);
  v->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  (speed ? static_cast<QWidget*>(speed) : duration)->setFocus();
  if (dlg.exec() != QDialog::Accepted) return;

  if (!ed.clip(clipId)) return;  // 画面を出している間に消された
  // 速度を変えたあとで、逆再生を切り替える（どちらも変えたときは、元に戻すのは 2 回）
  const bool reverseChanged = reverse && reverse->isChecked() != !orig.reversedFrom.isEmpty();
  struct ReverseAfter {
    bool run;
    QWidget* parent;
    QString id;
    ~ReverseAfter() {
      if (run) toggleReverseClip(parent, id);
    }
  } after{reverseChanged, parent, clipId};
  if (orig.isImage) {
    // 写真の長さは、フレームにそろえる（タイムラインの端のドラッグと同じ）
    const double d = std::max(1.0 / fps, frameRound(std::clamp(duration->value(), kMinClipDuration, kMaxImageDuration), fps));
    if (std::abs(d - (orig.out - orig.in)) < 1e-6) return;
    ed.updateClip(clipId, [&](VideoClip& x) {
      // 長さを変えたら、フェードとキーフレームは長さに収める
      x.out = x.in + d;
      x.fadeIn = fitFade(x.fadeIn, d, 0);
      x.fadeOut = fitFade(x.fadeOut, d, x.fadeIn);
    });
    return;
  }
  double ns = speed->value() / 100;
  if (*durationEdited) ns = span / std::max(1.0 / fps, frameRound(duration->value(), fps));
  ns = std::clamp(ns, kMinSpeedPct / 100, maxSpeedPct / 100);
  if (std::abs(ns - orig.speed) < 1e-6) return;
  ed.updateClip(clipId, [&](VideoClip& x) {
    // 速度を変えると長さが変わる。キーフレームと音量のキーフレームは、同じ映像の位置に合わせて時間を伸び縮みさせる
    const double f = x.speed / ns;
    x.anim = scaleAnimationTime(x.anim, f);
    for (auto& p : x.volumePoints) p.t *= f;
    x.speed = ns;
    const double d = clipDuration(x, fps);
    x.fadeIn = fitFade(x.fadeIn, d, 0);
    x.fadeOut = fitFade(x.fadeOut, d, x.fadeIn);
  });
  ed.toast(QStringLiteral("速度を %1% にしました（長さ %2）").arg(ns * 100, 0, 'g', 4).arg(formatTime(span / ns)));
}

void execPasteAttributesDialog(QWidget* parent) {
  Editor& ed = editor();
  if (!ed.hasAttributes()) {
    ed.toast(QStringLiteral("先に、映像を選んで「属性をコピー」してください"));
    return;
  }
  int targets = 0;
  for (const Selection& s : ed.selectionSet()) {
    if (s.type == Selection::Clip || s.type == Selection::Overlay) ++targets;
  }
  if (targets == 0) {
    ed.toast(QStringLiteral("貼り付ける映像を、タイムラインで選んでください（Ctrl+クリックで、いくつか一緒に選べます）"));
    return;
  }
  ed.pause();
  struct Part {
    int bit;
    QString label;
  };
  const std::vector<Part> parts = {
      {Editor::AttrMotion, QStringLiteral("位置・拡大率・回転・不透明度（キーフレームも）")},
      {Editor::AttrCrop, QStringLiteral("サイズ（幅・高さ）・切り抜き")},
      {Editor::AttrColor, QStringLiteral("色の調整（フィルター）")},
      {Editor::AttrEffects, QStringLiteral("エフェクト・反転")},
      {Editor::AttrChroma, QStringLiteral("クロマキー（背景の透過）")},
      {Editor::AttrMask, QStringLiteral("オブジェクトマスク（同じ素材ならマスクごと、違う素材なら使い方だけ）")},
      {Editor::AttrFade, QStringLiteral("フェードイン・フェードアウト")},
      {Editor::AttrVolume, QStringLiteral("音量（キーフレーム・ミュートも）")},
      {Editor::AttrSpeed, QStringLiteral("速度（長さが変わります）")},
  };
  const int allButSpeed = Editor::AttrMotion | Editor::AttrCrop | Editor::AttrColor | Editor::AttrEffects | Editor::AttrChroma |
                          Editor::AttrMask | Editor::AttrFade | Editor::AttrVolume;
  QSettings settings(dialogSettingsPath(), QSettings::IniFormat);
  const int last = settings.value(QStringLiteral("attributes/parts"), allButSpeed).toInt();

  QDialog dlg(parent ? parent->window() : nullptr);
  dlg.setWindowTitle(QStringLiteral("属性をペースト"));
  dlg.setMinimumWidth(380);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(8);
  auto* intro = makeLabel(QStringLiteral("コピーした映像の設定のうち、貼り付けるものを選んでください（選んでいる %1 個の映像に貼り付けます）").arg(targets),
                          "muted", &dlg);
  intro->setWordWrap(true);
  v->addWidget(intro);
  std::vector<std::pair<int, QCheckBox*>> boxes;
  for (const Part& p : parts) {
    auto* b = new QCheckBox(p.label, &dlg);
    b->setChecked(last & p.bit);
    v->addWidget(b);
    boxes.push_back({p.bit, b});
  }
  auto* buttons = new QDialogButtonBox(&dlg);
  auto* ok = buttons->addButton(QStringLiteral("貼り付け"), QDialogButtonBox::AcceptRole);
  ok->setProperty("primary", true);
  ok->setDefault(true);
  buttons->addButton(QStringLiteral("キャンセル"), QDialogButtonBox::RejectRole);
  v->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  auto sync = [&boxes, ok] {
    bool any = false;
    for (const auto& [bit, b] : boxes) any = any || b->isChecked();
    ok->setEnabled(any);
  };
  for (const auto& [bit, b] : boxes) QObject::connect(b, &QCheckBox::toggled, &dlg, sync);
  sync();
  if (dlg.exec() != QDialog::Accepted) return;
  int chosen = 0;
  for (const auto& [bit, b] : boxes) {
    if (b->isChecked()) chosen |= bit;
  }
  settings.setValue(QStringLiteral("attributes/parts"), chosen);
  const int n = editor().pasteAttributes(chosen);
  editor().toast(n > 0 ? QStringLiteral("%1 個の映像に、属性を貼り付けました").arg(n) : QStringLiteral("貼り付けられる映像がありませんでした"),
                 n > 0 ? ToastKind::Success : ToastKind::Info);
}

void execColorMatteDialog(QWidget* parent) {
  Editor& ed = editor();
  ed.pause();
  const QColor col = QColorDialog::getColor(QColor(0x1f, 0x3a, 0x5f), parent ? parent->window() : nullptr, QStringLiteral("カラーマットの色"));
  if (!col.isValid()) return;
  const QSize d = designSize(ed.project().settings.aspect);
  QImage img(d, QImage::Format_RGB32);
  img.fill(col.rgb());
  const QString id = media().addGeneratedImage(img, QStringLiteral("カラーマット %1").arg(col.name().toUpper()));
  if (id.isEmpty()) {
    ed.toast(QStringLiteral("カラーマットを作れませんでした"), ToastKind::Error);
    return;
  }
  // メインのビデオの、再生位置に一番近い切れ目（クリップの境目・最後）に入れる
  const TimelineLayout& layout = ed.layout();
  const double t = ed.currentTime();
  int index = int(layout.items.size());
  double best = std::numeric_limits<double>::infinity();
  for (size_t i = 0; i <= layout.items.size(); ++i) {
    const double b = i < layout.items.size() ? layout.items[i].start : layout.total;
    if (std::abs(b - t) < best) {
      best = std::abs(b - t);
      index = int(i);
    }
  }
  ed.addToTimeline({id}, index);
  ed.toast(QStringLiteral("カラーマット（%1）を入れました。長さはタイムラインで端をドラッグして変えられます").arg(col.name().toUpper()), ToastKind::Success);
}

void exportFrameAtPlayhead(QWidget* parent) {
  Editor& ed = editor();
  ed.pause();
  const ProjectData& p = ed.project();
  if (p.clips.empty() && p.overlays.empty() && p.texts.empty()) {
    ed.toast(QStringLiteral("書き出す画面がありません"));
    return;
  }
  const int fps = p.settings.fps;
  // 終わりの位置では、最後のフレームを書き出す
  const double total = ed.layout().total;
  double t = ed.currentTime();
  if (total > 0 && t > total - 0.5 / fps) t = std::max(0.0, total - 1.0 / fps);
  static QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  QString name = ed.projectPath().isEmpty() ? ed.workspaceName(ed.workspaceIndex()) : QFileInfo(ed.projectPath()).completeBaseName();
  if (name.isEmpty()) name = QStringLiteral("無題");
  QString filter;
  QString path = QFileDialog::getSaveFileName(parent ? parent->window() : nullptr, QStringLiteral("フレームを書き出し"),
                                              dir + QLatin1Char('/') + name + QLatin1Char('_') + fileTime(t) + QStringLiteral(".png"),
                                              QStringLiteral("PNG 画像 (*.png);;JPEG 画像 (*.jpg *.jpeg)"), &filter);
  if (path.isEmpty()) return;
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix != u"png" && suffix != u"jpg" && suffix != u"jpeg") path += filter.startsWith(QStringLiteral("JPEG")) ? QStringLiteral(".jpg") : QStringLiteral(".png");
  dir = QFileInfo(path).absolutePath();
  const bool jpg = QFileInfo(path).suffix().toLower() != u"png";
  const QSize size = exportSize(p.settings.aspect, 1080);
  ed.toast(QStringLiteral("フレームを書き出しています…"));
  renderStillFrame(t, size, parent, [path, jpg, size](QImage img) {
    if (img.isNull() || !img.save(path, jpg ? "JPG" : "PNG", jpg ? 95 : -1)) {
      editor().toast(QStringLiteral("フレームを書き出せませんでした"), ToastKind::Error);
      return;
    }
    editor().toast(QStringLiteral("フレームを書き出しました（%1 × %2）: %3").arg(size.width()).arg(size.height()).arg(QDir::toNativeSeparators(path)),
                   ToastKind::Success);
  });
}

void frameHoldAtPlayhead(QWidget* parent) {
  Editor& ed = editor();
  ed.pause();
  if (ed.editSessionActive()) return;  // ドラッグ・設定の画面の途中では入れない
  if (ed.selection().type == Selection::Overlay) {
    ed.toast(QStringLiteral("フレームの保持は、メインのビデオのクリップで使えます（重ねるトラックの映像には使えません）"));
    return;
  }
  const TimelineLayout& layout = ed.layout();
  const double t = ed.currentTime();
  const ClipLayout* cur = t < layout.total - 1e-9 ? findClipAt(layout, t) : nullptr;
  if (!cur) {
    ed.toast(QStringLiteral("再生位置に、メインのビデオのクリップがありません"));
    return;
  }
  const VideoClip c = cur->clip;
  if (c.isImage) {
    ed.toast(QStringLiteral("写真のクリップは、もともと止まっています（長さは端をドラッグして変えられます）"));
    return;
  }
  const Asset* a = ed.asset(c.assetId);
  if (!a || a->status == AssetStatus::Missing || a->status == AssetStatus::Error) {
    ed.toast(QStringLiteral("素材のファイルが見つかりません"), ToastKind::Error);
    return;
  }
  const int fps = ed.project().settings.fps;
  const double local = frameRound(std::max(0.0, t - cur->start), fps);
  const double srcT = c.in + local * c.speed;
  const QString clipId = c.id;
  const QString name = QStringLiteral("フレーム保持（%1 %2）").arg(a->name, formatTime(t));
  const int uid = ed.workspaceUid(ed.workspaceIndex());
  ed.toast(QStringLiteral("フレームを取り出しています…"));
  const QString assetId = c.assetId;
  decodeSourceFrameAt(a->path, srcT, parent, [clipId, assetId, srcT, name, uid, fps](QImage img, double pts) {
    Editor& e = editor();
    if (img.isNull()) {
      e.toast(QStringLiteral("フレームを読み込めませんでした"), ToastKind::Error);
      return;
    }
    // 取り出している間に、タブを切り替えた・クリップを消した・素材を変えた・ドラッグの途中
    const VideoClip* now = e.clip(clipId);
    if (e.workspaceUid(e.workspaceIndex()) != uid || !now || e.overlay(clipId) || now->assetId != assetId || e.editSessionActive()) return;
    // 取り出している間にクリップの端・速度を変えていても、同じ素材のフレームの所に入れる（クリップの外になったら入れない）
    const double local = frameRound((srcT - now->in) / std::max(1e-6, now->speed), fps);
    if (local < -1e-6 || local > clipDuration(*now, fps) + 1e-6) return;
    const ObjectMask nowMask = now->mask;
    const QString id = media().addGeneratedImage(img, name);
    if (id.isEmpty()) {
      e.toast(QStringLiteral("静止画を保存できませんでした"), ToastKind::Error);
      return;
    }
    // オブジェクトマスク: 止めたフレームのマスクを付ける（データがまだ無ければ外す）
    const std::optional<ObjectMask> holdMask = nowMask.has() ? frameHoldMask(nowMask, e.asset(assetId), pts, img.size()) : std::nullopt;
    if (e.insertFrameHold(clipId, local, id, 2.0, holdMask)) {
      e.toast(QStringLiteral("再生位置のフレームで 2 秒止めました（静止画のクリップを入れました。長さは端をドラッグして変えられます）"),
              ToastKind::Success);
    }
  });
}

void toggleReverseClip(QWidget* parent, const QString& clipId) {
  Editor& ed = editor();
  ed.pause();
  if (ed.editSessionActive()) return;  // ドラッグ・設定の画面の途中では変えない
  const VideoClip* c = ed.clip(clipId);
  if (!c) return;
  const OverlayItem* ov = ed.overlay(clipId);
  if (c->isImage || (ov && ov->isEffectLayer())) {
    ed.toast(QStringLiteral("逆再生は、動画のクリップで使えます（写真・エフェクトレイヤーには使えません）"));
    return;
  }
  // 逆再生をやめる: 元の動画に戻す（作った動画はメディアの一覧に残すので、また逆再生にするときはすぐ）
  if (!c->reversedFrom.isEmpty()) {
    if (ed.setClipReversed(clipId, QString())) ed.toast(QStringLiteral("逆再生をやめました"));
    else ed.toast(QStringLiteral("元の動画が見つからないので、逆再生をやめられません"), ToastKind::Error);
    return;
  }
  const Asset* a = ed.asset(c->assetId);
  if (!a || a->status == AssetStatus::Missing || a->status == AssetStatus::Error || a->kind != AssetKind::Video) {
    ed.toast(QStringLiteral("素材のファイルが見つかりません"), ToastKind::Error);
    return;
  }
  ProbeInfo info;
  QString probeError;
  probeMedia(a->path, info, probeError);  // フレームレート（分からなければ 30 として作る）
  const QString assetId = a->id;
  const QString name = QStringLiteral("%1（逆再生）").arg(QFileInfo(a->name).completeBaseName());
  const int uid = ed.workspaceUid(ed.workspaceIndex());
  auto* job = new ReverseJob(a->path, a->duration, info.fps, a->width, a->height, a->hasAudio, parent);
  QWidget* owner = parent ? parent->window() : nullptr;
  auto* dlg = new QProgressDialog(QStringLiteral("逆再生の動画を作っています…（%1）").arg(a->name), QStringLiteral("キャンセル"), 0, 1000, owner);
  dlg->setWindowTitle(QStringLiteral("逆再生"));
  dlg->setWindowModality(Qt::WindowModal);
  dlg->setMinimumDuration(400);
  dlg->setAutoClose(false);
  dlg->setAutoReset(false);
  dlg->setValue(0);
  QObject::connect(job, &ReverseJob::progress, dlg, [dlg](double f) { dlg->setValue(int(std::lround(std::clamp(f, 0.0, 1.0) * 1000))); });
  QObject::connect(dlg, &QProgressDialog::canceled, job, [job, dlg] {
    job->cancel();
    job->deleteLater();
    dlg->deleteLater();
    editor().toast(QStringLiteral("逆再生の動画を作るのをやめました"));
  });
  QObject::connect(job, &ReverseJob::finished, job, [job, dlg, clipId, assetId, name, uid](bool ok, const QString& error) {
    QObject::disconnect(dlg, &QProgressDialog::canceled, job, nullptr);
    dlg->close();
    dlg->deleteLater();
    job->deleteLater();
    Editor& e = editor();
    if (!ok) {
      e.toast(QStringLiteral("逆再生の動画を作れませんでした: %1").arg(error), ToastKind::Error);
      return;
    }
    // 作っている間に、タブを切り替えた・クリップを消した・素材を変えた
    const VideoClip* now = e.clip(clipId);
    if (e.workspaceUid(e.workspaceIndex()) != uid || !now || now->assetId != assetId || !now->reversedFrom.isEmpty()) return;
    const QString revId = media().addGeneratedVideo(job->output(), name);
    if (revId.isEmpty()) {
      e.toast(QStringLiteral("逆再生の動画を読み込めませんでした"), ToastKind::Error);
      return;
    }
    if (e.setClipReversed(clipId, revId)) {
      e.toast(QStringLiteral("逆再生にしました（音も逆になります。もう一度選ぶと元に戻します）"), ToastKind::Success);
    }
  });
  job->start();
}

void toggleObjectMaskPick(const QString& clipId) {
  Editor& ed = editor();
  // 選んでいる途中ならやめる（別の映像を決めて呼んだときは、その映像で選び直す）
  if (!ed.maskPick().isEmpty() && (clipId.isEmpty() || clipId == ed.maskPick())) {
    ed.setMaskPick({});
    return;
  }
  if (ed.editSessionActive()) return;  // ドラッグ・設定の画面の途中では始めない
  ed.pause();
  QString why;
  if (!mask::maskController().available(&why)) {
    ed.toast(why, ToastKind::Error);
    return;
  }
  // どの映像の被写体を選ぶか: 決めてあればそれ、無ければ選んでいる映像、それも無ければ再生位置のメインの映像
  QString id = clipId;
  if (id.isEmpty()) {
    const Selection s = ed.selection();
    if (s.type == Selection::Clip || s.type == Selection::Overlay) id = s.id;
  }
  if (id.isEmpty()) {
    if (const ClipLayout* cur = findClipAt(ed.layout(), ed.currentTime())) id = cur->clip.id;
  }
  const VideoClip* c = id.isEmpty() ? nullptr : ed.clip(id);
  const OverlayItem* ov = c ? ed.overlay(id) : nullptr;
  if (!c || (ov && ov->isEffectLayer())) {
    ed.toast(QStringLiteral("マスクする映像（動画・写真）を、タイムラインで選んでください"));
    return;
  }
  const Asset* a = ed.asset(c->assetId);
  if (!a || a->status == AssetStatus::Missing || a->status == AssetStatus::Error) {
    ed.toast(QStringLiteral("素材のファイルが見つかりません"), ToastKind::Error);
    return;
  }
  // 今の再生位置にその映像が出ていなければ、出ている所（始まり）へ動かす
  double start = 0, dur = 0;
  if (ed.clipSpan(id, &start, &dur)) {
    const double t = ed.currentTime();
    if (t < start - 1e-9 || t >= start + dur - 1e-9) ed.seek(start);
  }
  if (ed.selection().id != id) ed.select({ov ? Selection::Overlay : Selection::Clip, id});
  ed.setMaskPick(id);
}

void detachSelectedAudio() {
  Editor& ed = editor();
  const Selection s = ed.selection();
  if (s.type != Selection::Clip && s.type != Selection::Overlay) {
    ed.toast(QStringLiteral("音声を切り離す映像を、タイムラインで選んでください"));
    return;
  }
  ed.detachAudio(s.id);  // できないときは、理由を知らせる
}

void copySelectedAttributes() {
  Editor& ed = editor();
  if (ed.copyAttributes()) {
    ed.toast(QStringLiteral("映像の設定（色・エフェクト・動きなど）をコピーしました。ほかの映像を選んで「属性をペースト」で写せます"));
  } else {
    ed.toast(QStringLiteral("属性をコピーする映像を、タイムラインで選んでください"));
  }
}

void importSubtitlesFile(QWidget* parent) {
  Editor& ed = editor();
  ed.pause();
  static QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString path = QFileDialog::getOpenFileName(parent ? parent->window() : nullptr, QStringLiteral("字幕を読み込む"), dir,
                                                    QStringLiteral("字幕ファイル (*.srt *.vtt);;すべてのファイル (*)"));
  if (path.isEmpty()) return;
  dir = QFileInfo(path).absolutePath();
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    ed.toast(QStringLiteral("ファイルを開けませんでした: %1").arg(f.errorString()), ToastKind::Error);
    return;
  }
  QString error;
  const std::vector<SubtitleCue> cues = parseSubtitles(f.readAll(), &error);
  if (cues.empty()) {
    ed.toast(error, ToastKind::Error);
    return;
  }
  const int n = ed.importSubtitles(cues, QStringLiteral("subtitle"));
  const double total = ed.layout().total;
  const bool beyond = total > 0 && cues.back().end > total + 0.05;
  ed.toast(QStringLiteral("字幕を %1 個並べました（「字幕」のスタイル。まとめて選んで、見た目を変えられます）%2")
               .arg(n)
               .arg(beyond ? QStringLiteral("。動画より後ろの字幕は書き出されません") : QString()),
           ToastKind::Success);
}

void exportSubtitlesFile(QWidget* parent) {
  Editor& ed = editor();
  ed.pause();
  std::vector<SubtitleCue> cues;
  for (const TextItem& t : ed.project().texts) {
    if (t.text.trimmed().isEmpty()) continue;
    cues.push_back({t.start, t.start + t.duration, t.text});
  }
  if (cues.empty()) {
    ed.toast(QStringLiteral("書き出すテキストがありません"));
    return;
  }
  std::stable_sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
  static QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  QString name = ed.projectPath().isEmpty() ? ed.workspaceName(ed.workspaceIndex()) : QFileInfo(ed.projectPath()).completeBaseName();
  if (name.isEmpty()) name = QStringLiteral("無題");
  QString filter;
  QString path = QFileDialog::getSaveFileName(parent ? parent->window() : nullptr, QStringLiteral("字幕を書き出す"), dir + QLatin1Char('/') + name + QStringLiteral(".srt"),
                                              QStringLiteral("SRT 字幕 (*.srt);;WebVTT 字幕 (*.vtt)"), &filter);
  if (path.isEmpty()) return;
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix != u"srt" && suffix != u"vtt") path += filter.startsWith(QStringLiteral("WebVTT")) ? QStringLiteral(".vtt") : QStringLiteral(".srt");
  dir = QFileInfo(path).absolutePath();
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(writeSubtitles(cues, QFileInfo(path).suffix().toLower() == u"vtt")) < 0 || !f.commit()) {
    ed.toast(QStringLiteral("書き出せませんでした: %1").arg(f.errorString()), ToastKind::Error);
    return;
  }
  ed.toast(QStringLiteral("テキスト %1 個を字幕ファイルに書き出しました: %2").arg(cues.size()).arg(QDir::toNativeSeparators(path)), ToastKind::Success);
}

}  // namespace kv
