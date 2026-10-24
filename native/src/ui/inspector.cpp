#include "ui/inspector.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QStandardPaths>
#include <QStyle>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "core/editor.h"
#include "core/filters.h"
#include "core/fontlibrary.h"
#include "core/textstyles.h"
#include "core/texttemplates.h"
#include "core/timeline.h"
#include "render/textrender.h"
#include "ui/animpanel.h"
#include "ui/plaintextinput.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

static QString pct(double v) {
  return formatPercent(v);
}

static QString signedNum(double v) {
  const long r = std::lround(v);
  if (r == 0) return QStringLiteral("0");
  return r > 0 ? QStringLiteral("+%1").arg(r) : QString::number(r);
}

/** 見出し（アイコン・名前・補足） */
class PanelHeader : public QWidget {
 public:
  explicit PanelHeader(QWidget* parent = nullptr) : QWidget(parent) {
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 12);
    h->setSpacing(8);
    icon_ = new QLabel(this);
    icon_->setFixedSize(16, 16);
    h->addWidget(icon_, 0, Qt::AlignTop);
    auto* v = new QVBoxLayout();
    v->setSpacing(1);
    title_ = makeLabel(QString(), "title", this);
    title_->setWordWrap(true);
    sub_ = makeLabel(QString(), "muted", this);
    v->addWidget(title_);
    v->addWidget(sub_);
    h->addLayout(v, 1);
  }
  void set(const QString& icon, const QString& title, const QString& sub) {
    icon_->setPixmap(theme::iconPixmap(icon, 16, theme::muted, devicePixelRatioF()));
    title_->setText(title);
    sub_->setText(sub);
    sub_->setVisible(!sub.isEmpty());
  }

 private:
  QLabel* icon_;
  QLabel* title_;
  QLabel* sub_;
};

/** フェードの長さのスライダー（0〜kMaxFade 秒を 0.1 秒刻みで。Shift で 1 秒、Alt で 0.01 秒）。0 は「なし」 */
static SliderRow* makeFadeSlider(const QString& label, QWidget* parent) {
  auto* s = new SliderRow(label, 0, kMaxFade, 0.1, [](double x) {
    return x < 0.005 ? QStringLiteral("なし") : QStringLiteral("%1秒").arg(x, 0, 'f', x < 1 && std::abs(x * 10 - std::round(x * 10)) > 1e-6 ? 2 : 1);
  }, parent);
  s->setUnit(0.1, 0.01);
  s->setValueBubble(true);
  return s;
}


static QVBoxLayout* pageLayout(QWidget* page) {
  auto* v = new QVBoxLayout(page);
  v->setContentsMargins(12, 12, 12, 12);
  v->setSpacing(0);
  return v;
}

static void addSection(QVBoxLayout* page, Section* s, bool separator = true) {
  if (separator) page->addWidget(makeSeparator());
  page->addWidget(s);
}

// ---- プロジェクト -----------------------------------------------------------

class ProjectPanel : public QWidget {
 public:
  explicit ProjectPanel(QWidget* parent = nullptr) : QWidget(parent) {
    auto* v = pageLayout(this);
    auto* header = new PanelHeader(this);
    header->set(QStringLiteral("sliders"), QStringLiteral("プロジェクト"), QString());
    v->addWidget(header);
    auto* fpsSec = new Section(QStringLiteral("フレームレート"), this);
    fps_ = new Segmented({{24, QStringLiteral("24fps")}, {30, QStringLiteral("30fps")}, {60, QStringLiteral("60fps")}}, 0, fpsSec);
    fpsSec->body()->addWidget(fps_);
    addSection(v, fpsSec);
    // 動画全体の再生速度。映像・文字・音をまとめて速く・遅くする（タイムラインの時間はそのまま。プレビューと書き出しに効く）
    auto* speedSec = new Section(QStringLiteral("再生速度（動画全体）"), this);
    std::vector<Segmented::Option> speeds;
    for (double s : {0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0}) speeds.push_back({s, QStringLiteral("%1x").arg(s)});
    speed_ = new Segmented(speeds, 4, speedSec);
    speedSec->body()->addWidget(speed_);
    speedNote_ = makeLabel(QString(), "muted", speedSec);
    speedNote_->setWordWrap(true);
    speedSec->body()->addWidget(speedNote_);
    addSection(v, speedSec);
    auto* info = new Section(QStringLiteral("情報"), this);
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(4);
    const QStringList names{QStringLiteral("長さ"), QStringLiteral("比率"), QStringLiteral("解像度"), QStringLiteral("クリップ数")};
    for (int i = 0; i < names.size(); ++i) {
      grid->addWidget(makeLabel(names[i], "muted", info), i, 0);
      values_[i] = new QLabel(info);
      values_[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
      grid->addWidget(values_[i], i, 1);
    }
    info->body()->addLayout(grid);
    addSection(v, info);
    v->addStretch(1);
    connect(fps_, &Segmented::changed, this, [](const QVariant& x) { editor().setFps(x.toInt()); });
    connect(speed_, &Segmented::changed, this, [](const QVariant& x) { editor().setProjectSpeed(x.toDouble()); });
  }
  void refresh() {
    const Editor& ed = editor();
    const auto& s = ed.project().settings;
    fps_->setValue(s.fps);
    speed_->setValue(s.speed);
    // 書き出す動画の長さ（速さを変えたときだけ出す）
    const bool sped = std::abs(s.speed - 1) > 1e-9;
    speedNote_->setVisible(sped);
    if (sped) {
      speedNote_->setText(QStringLiteral("書き出す長さ %1（元の長さ %2）。プレビューもこの速さで再生します")
                              .arg(formatTime(ed.layout().total / s.speed), formatTime(ed.layout().total)));
    }
    const QSize size = exportSize(s.aspect, 1080);
    values_[0]->setText(formatTime(ed.layout().total));
    values_[1]->setText(aspectToString(s.aspect));
    values_[2]->setText(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()));
    values_[3]->setText(QString::number(ed.project().clips.size()));
  }

 private:
  Segmented* fps_;
  Segmented* speed_;
  QLabel* speedNote_;
  QLabel* values_[4];
};

// ---- ビデオ・画像 -----------------------------------------------------------

class ClipPanel : public QWidget {
 public:
  explicit ClipPanel(QWidget* parent = nullptr) : QWidget(parent) {
    auto* v = pageLayout(this);
    header_ = new PanelHeader(this);
    v->addWidget(header_);

    // 重ねるトラックだけ: 開始の時間（タイムラインで左右にドラッグしても変えられる）
    timeSec_ = new Section(QStringLiteral("タイミング"), this);
    start_ = new SecondsField(QStringLiteral("開始"), 0, 36000, timeSec_);
    auto* fromHere = new QPushButton(QStringLiteral("現在位置から開始"), timeSec_);
    timeSec_->body()->addWidget(start_);
    timeSec_->body()->addWidget(fromHere);
    addSection(v, timeSec_, false);

    durationSec_ = new Section(QStringLiteral("表示時間"), this);
    duration_ = new SliderRow(QStringLiteral("表示時間"), 0.5, 20, 0.1, [](double x) { return QStringLiteral("%1秒").arg(x, 0, 'f', 1); }, durationSec_);
    duration_->setLabelVisible(false);
    durationSec_->body()->addWidget(duration_);
    addSection(v, durationSec_);

    speedSec_ = new Section(QStringLiteral("速度"), this);
    std::vector<Segmented::Option> speeds;
    for (double s : {0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0}) speeds.push_back({s, QStringLiteral("%1x").arg(s)});
    speed_ = new Segmented(speeds, 4, speedSec_);
    speedSec_->body()->addWidget(speed_);
    addSection(v, speedSec_);

    audioSec_ = new Section(QStringLiteral("音声"), this);
    volume_ = new SliderRow(QStringLiteral("音量"), 0, kMaxVolume, 0.01, pct, audioSec_);
    volume_->setValueBubble(true);  // ドラッグしている間（とホイール・矢印キーで変えている間）、つまみの近くに今の音量を出す
    volume_->setDefault(kMaxVolume);
    volume_->setUnit(0.01, 0.001);  // 1%（Alt で 0.1%）
    mute_ = new QCheckBox(QStringLiteral("ミュート"), audioSec_);
    // 時間ごとに音量を変えるときは、◆ で再生位置に音量のキーフレームを置く（オーディオと同じ仕組み）
    volKeys_ = new VolumeKeyControls(audioSec_);
    auto* volRow = new QHBoxLayout();
    volRow->setSpacing(4);
    volRow->addWidget(volume_, 1);
    volRow->addWidget(volKeys_, 0, Qt::AlignBottom);
    volNote_ = makeLabel(QString(), "note", audioSec_);
    volNote_->setWordWrap(true);
    volClear_ = new QPushButton(QStringLiteral("音量のキーフレームをすべて削除"), audioSec_);
    audioSec_->body()->addLayout(volRow);
    audioSec_->body()->addWidget(volNote_);
    audioSec_->body()->addWidget(volClear_);
    audioSec_->body()->addWidget(mute_);
    addSection(v, audioSec_);

    fitSec_ = new Section(QStringLiteral("表示方法"), this);
    fit_ = new Segmented({{int(FitMode::Contain), QStringLiteral("全体表示")},
                          {int(FitMode::Cover), QStringLiteral("拡大")},
                          {int(FitMode::Blur), QStringLiteral("ぼかし背景")}},
                         0, fitSec_);
    fitSec_->body()->addWidget(fit_);
    addSection(v, fitSec_);

    // 位置・拡大率・回転・不透明度（キーフレームでアニメーションできる）
    v->addWidget(makeSeparator());
    anim_ = new AnimPanel(this);
    v->addWidget(anim_);

    // サイズ（幅・高さだけの伸縮）と切り抜き。プレビューの枠の辺のつまみでも、幅・高さを変えられる
    sizeSec_ = new Section(QStringLiteral("サイズ・切り抜き"), this);
    sizeReset_ = new QPushButton(QStringLiteral("元に戻す"), sizeSec_);
    sizeReset_->setProperty("flat", true);
    sizeReset_->setCursor(Qt::PointingHandCursor);
    sizeSec_->setHeaderWidget(sizeReset_);
    stretchX_ = new SliderRow(QStringLiteral("幅"), kMinStretch, kMaxStretch, 0.01, pct, sizeSec_);
    stretchY_ = new SliderRow(QStringLiteral("高さ"), kMinStretch, kMaxStretch, 0.01, pct, sizeSec_);
    for (auto* s : {stretchX_, stretchY_}) {
      s->setDefault(1);
      s->setUnit(0.01, 0.001);
      s->setValueBubble(true);
    }
    auto* sizeNote = makeLabel(QStringLiteral("全体の大きさは「拡大率」で。幅・高さは、プレビューの枠の辺のつまみをドラッグしても変えられます"), "note", sizeSec_);
    sizeNote->setWordWrap(true);
    sizeSec_->body()->addWidget(stretchX_);
    sizeSec_->body()->addWidget(stretchY_);
    sizeSec_->body()->addWidget(sizeNote);
    sizeSec_->body()->addWidget(makeLabel(QStringLiteral("切り抜き（端から切り落とす）"), "muted", sizeSec_));
    const QString cropNames[4] = {QStringLiteral("上"), QStringLiteral("下"), QStringLiteral("左"), QStringLiteral("右")};
    for (int i = 0; i < 4; ++i) {
      crop_[i] = new SliderRow(cropNames[i], 0, kMaxCrop, 0.005, pct, sizeSec_);
      crop_[i]->setDefault(0);
      crop_[i]->setUnit(0.01, 0.001);
      crop_[i]->setValueBubble(true);
      sizeSec_->body()->addWidget(crop_[i]);
    }
    addSection(v, sizeSec_);

    // クロマキー（グリーンバック・ブルーバックの背景を透明にする）
    chromaSec_ = new Section(QStringLiteral("クロマキー"), this);
    chromaOn_ = new QCheckBox(QStringLiteral("背景の色を透明にする"), chromaSec_);
    auto* keyRow = new QHBoxLayout();
    keyRow->setSpacing(6);
    keyChip_ = new QLabel(chromaSec_);
    keyChip_->setFixedSize(22, 22);
    keyChip_->setAccessibleName(QStringLiteral("透明にする色"));
    auto* green = new QPushButton(QStringLiteral("緑"), chromaSec_);
    auto* blue = new QPushButton(QStringLiteral("青"), chromaSec_);
    auto* pick = new QPushButton(QStringLiteral("色を選ぶ…"), chromaSec_);
    // スポイト: 実際の映像の背景の色を、そのまま選べる（緑・青のボタンの色と少し違う背景のとき）
    auto* dropper = new QPushButton(QStringLiteral("スポイトでプレビューから選ぶ"), chromaSec_);
    green->setToolTip(QStringLiteral("グリーンバック（緑の背景）を透明にする"));
    blue->setToolTip(QStringLiteral("ブルーバック（青の背景）を透明にする"));
    dropper->setToolTip(QStringLiteral("プレビューの映像をクリックして、透明にする色（背景の色）を選ぶ。もう一度押すとやめる"));
    keyRow->addWidget(keyChip_);
    keyRow->addWidget(green);
    keyRow->addWidget(blue);
    keyRow->addWidget(pick, 1);
    similarity_ = new SliderRow(QStringLiteral("透明にする範囲"), 0.01, kMaxChromaSimilarity, 0.005,
                                [](double x) { return QString::number(std::lround(x * 100)); }, chromaSec_);
    similarity_->setDefault(ChromaKey{}.similarity);
    similarity_->setValueBubble(true);
    blend_ = new SliderRow(QStringLiteral("境目のなめらかさ"), 0, kMaxChromaBlend, 0.005,
                           [](double x) { return QString::number(std::lround(x * 100)); }, chromaSec_);
    blend_->setDefault(ChromaKey{}.blend);
    blend_->setValueBubble(true);
    // チェックボックスの文字は折り返さないので、短くして、くわしい説明はツールチップにする（設定欄の幅からはみ出さないように）
    despill_ = new QCheckBox(QStringLiteral("色かぶりを抑える"), chromaSec_);
    despill_->setToolTip(QStringLiteral("髪や輪郭に残る緑・青の色を目立たなくします"));
    chromaNote_ = makeLabel(QStringLiteral("メインのトラックでは、透明にした所は黒になります。後ろに映像を見せるには、重ねるトラックに置いてください"), "note",
                            chromaSec_);
    chromaNote_->setWordWrap(true);
    chromaSec_->body()->addWidget(chromaOn_);
    chromaSec_->body()->addLayout(keyRow);
    chromaSec_->body()->addWidget(dropper);
    chromaSec_->body()->addWidget(similarity_);
    chromaSec_->body()->addWidget(blend_);
    chromaSec_->body()->addWidget(despill_);
    chromaSec_->body()->addWidget(chromaNote_);
    addSection(v, chromaSec_);
    keyButtons_ = {green, blue, pick, dropper};

    auto* filterSec = new Section(QStringLiteral("フィルター"), this);
    auto* grid = new QGridLayout();
    grid->setSpacing(6);
    int i = 0;
    for (const auto& p : filterPresets()) {
      auto* b = new QToolButton(filterSec);
      b->setText(p.label);
      b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
      b->setIconSize(QSize(80, 50));
      b->setCheckable(true);
      b->setFocusPolicy(Qt::TabFocus);
      b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      b->setStyleSheet(QStringLiteral("QToolButton{color:#9b9b9b;border:1px solid transparent;border-radius:4px;padding:3px;font-size:8pt}"
                                      "QToolButton:hover{color:#e6e6e6}QToolButton:checked{border-color:#2680eb;color:#e6e6e6;background:transparent}"));
      const FilterPreset id = p.id;
      connect(b, &QToolButton::clicked, this, [this, id] {
        editor().updateClip(clipId_, [&](VideoClip& c) { c.color = colorFromPreset(id); });
      });
      grid->addWidget(b, i / 3, i % 3);
      filterButtons_.push_back(b);
      ++i;
    }
    filterSec->body()->addLayout(grid);
    addSection(v, filterSec);

    adjustSec_ = new Section(QStringLiteral("調整"), this);
    reset_ = new QPushButton(QStringLiteral("リセット"), adjustSec_);
    reset_->setProperty("flat", true);
    reset_->setCursor(Qt::PointingHandCursor);
    adjustSec_->setHeaderWidget(reset_);
    brightness_ = new SliderRow(QStringLiteral("明るさ"), colorLimits::brightness.min, colorLimits::brightness.max, 0.01,
                                [](double x) { return signedNum((x - 1) * 200); }, adjustSec_);
    contrast_ = new SliderRow(QStringLiteral("コントラスト"), colorLimits::contrast.min, colorLimits::contrast.max, 0.01,
                              [](double x) { return signedNum((x - 1) * 200); }, adjustSec_);
    saturation_ = new SliderRow(QStringLiteral("彩度"), colorLimits::saturation.min, colorLimits::saturation.max, 0.01,
                                [](double x) { return signedNum((x - 1) * 100); }, adjustSec_);
    temperature_ = new SliderRow(QStringLiteral("色温度"), colorLimits::temperature.min, colorLimits::temperature.max, 0.01,
                                 [](double x) { return signedNum(x * 100); }, adjustSec_);
    // 不透明度は「位置・拡大率・回転・不透明度」の欄と同じ値（キーフレームがあれば、再生位置の値）。どちらで変えても同じ
    opacity_ = new SliderRow(QStringLiteral("不透明度"), 0, 1, 0.01, pct, adjustSec_);
    opacity_->setDefault(1);
    opacity_->setUnit(0.01, 0.001);  // 1%（Alt で 0.1%）
    opacity_->setValueBubble(true);
    for (auto* s : {brightness_, contrast_, saturation_, temperature_, opacity_}) adjustSec_->body()->addWidget(s);
    addSection(v, adjustSec_);

    auto* fadeSec = new Section(QStringLiteral("フェード"), this);
    fadeIn_ = makeFadeSlider(QStringLiteral("イン"), fadeSec);
    fadeOut_ = makeFadeSlider(QStringLiteral("アウト"), fadeSec);
    fadeSec->body()->addWidget(fadeIn_);
    fadeSec->body()->addWidget(fadeOut_);
    addSection(v, fadeSec);
    v->addStretch(1);

    auto upd = [this](const std::function<void(VideoClip&)>& fn, const QString& merge = QString()) {
      editor().updateClip(clipId_, fn, merge.isEmpty() ? QString() : merge + clipId_);
    };
    connect(duration_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.out = c.in + x; }, QStringLiteral("img-dur-")); });
    connect(speed_, &Segmented::changed, this, [upd](const QVariant& x) {
      upd([&](VideoClip& c) {
        // 速度を変えると長さが変わる。キーフレームは、同じ映像の位置に合わせて時間を伸ばし縮みさせる
        const double ns = x.toDouble();
        const double f = c.speed / ns;
        c.anim = scaleAnimationTime(c.anim, f);
        for (auto& p : c.volumePoints) p.t *= f;
        c.speed = ns;
      });
    });
    connect(volClear_, &QPushButton::clicked, this, [this] { editor().clearVolumePoints(clipId_); });
    connect(volume_, &SliderRow::changed, this, [this, upd](double x) {
      const VideoClip* cur = editor().clip(clipId_);
      if (cur && !cur->volumePoints.empty()) {
        // 音量のキーフレームがあるとき: 再生位置のキーフレームの値を変える（無ければ、自動記録がオンのときだけ追加）
        if (!editor().setVolumeValue(clipId_, x, QStringLiteral("vol-") + clipId_)) refresh();
        return;
      }
      upd([&](VideoClip& c) {
        c.volume = x;
        c.muted = false;
      }, QStringLiteral("vol-"));
    });
    connect(&editor(), &Editor::timeChanged, this, [this] {
      if (!isVisible()) return;
      showVolume();
      showOpacity();
    });
    connect(mute_, &QCheckBox::toggled, this, [upd](bool on) { upd([&](VideoClip& c) { c.muted = on; }); });
    connect(fit_, &Segmented::changed, this, [upd](const QVariant& x) { upd([&](VideoClip& c) { c.fit = FitMode(x.toInt()); }); });
    connect(reset_, &QPushButton::clicked, this, [upd] { upd([](VideoClip& c) { c.color = colorFromPreset(c.color.preset); }); });
    connect(brightness_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.color.brightness = x; }, QStringLiteral("br-")); });
    connect(contrast_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.color.contrast = x; }, QStringLiteral("co-")); });
    connect(saturation_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.color.saturation = x; }, QStringLiteral("sa-")); });
    connect(temperature_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.color.temperature = x; }, QStringLiteral("te-")); });
    for (auto* s : {duration_, volume_, brightness_, contrast_, saturation_, temperature_, opacity_, fadeIn_, fadeOut_}) {
      connect(s, &SliderRow::released, this, [] { editor().endMerge(); });
    }
    connect(opacity_, &SliderRow::changed, this, [this](double x) {
      // 受け付けなかったとき（キーフレームがあって、自動記録がオフ）は、表示を元の値に戻す
      const QString merge = QStringLiteral("anim-%1-%2").arg(int(AnimProp::Opacity)).arg(clipId_);
      if (!editor().setPropValue(AnimProp::Opacity, x, merge)) showOpacity();
    });
    // フェードは自由な長さ（0.1 秒刻み）。クリップの長さに収め、イン・アウトは重ならないようにする
    // （収めた値は、変えたあとの refresh() でスライダーにも出る）
    connect(fadeIn_, &SliderRow::changed, this, [upd](double x) {
      upd([&](VideoClip& c) { c.fadeIn = fitFade(x, clipDuration(c, editor().project().settings.fps), c.fadeOut); }, QStringLiteral("fadein-"));
    });
    connect(fadeOut_, &SliderRow::changed, this, [upd](double x) {
      upd([&](VideoClip& c) { c.fadeOut = fitFade(x, clipDuration(c, editor().project().settings.fps), c.fadeIn); }, QStringLiteral("fadeout-"));
    });
    // 重ねるトラックの開始
    connect(start_, &SecondsField::changed, this, [this](double x) {
      editor().updateOverlay(clipId_, [&](OverlayItem& o) { o.start = x; }, QStringLiteral("ovstart-") + clipId_);
    });
    connect(start_, &SecondsField::released, this, [] { editor().endMerge(); });
    connect(fromHere, &QPushButton::clicked, this, [this] {
      editor().updateOverlay(clipId_, [](OverlayItem& o) { o.start = frameRound(editor().currentTime(), editor().project().settings.fps); });
    });
    // サイズ・切り抜き
    connect(stretchX_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.stretchX = x; }, QStringLiteral("sx-")); });
    connect(stretchY_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.stretchY = x; }, QStringLiteral("sy-")); });
    for (int i = 0; i < 4; ++i) {
      connect(crop_[i], &SliderRow::changed, this, [upd, i](double x) {
        upd([&](VideoClip& c) {
          double* sides[4] = {&c.cropT, &c.cropB, &c.cropL, &c.cropR};
          double* other[4] = {&c.cropB, &c.cropT, &c.cropR, &c.cropL};
          // 向かいの辺と合わせて kMaxCrop まで（残る部分が無くならないように）
          *sides[i] = std::clamp(x, 0.0, std::max(0.0, kMaxCrop - *other[i]));
        }, QStringLiteral("crop%1-").arg(i));
      });
    }
    for (auto* s : {stretchX_, stretchY_, crop_[0], crop_[1], crop_[2], crop_[3]}) {
      connect(s, &SliderRow::released, this, [] { editor().endMerge(); });
    }
    connect(sizeReset_, &QPushButton::clicked, this, [upd] {
      upd([](VideoClip& c) {
        c.stretchX = c.stretchY = 1;
        c.cropL = c.cropT = c.cropR = c.cropB = 0;
      });
    });
    // クロマキー
    connect(chromaOn_, &QCheckBox::toggled, this, [upd](bool on) { upd([&](VideoClip& c) { c.chroma.enabled = on; }); });
    connect(despill_, &QCheckBox::toggled, this, [upd](bool on) { upd([&](VideoClip& c) { c.chroma.despill = on; }); });
    connect(similarity_, &SliderRow::changed, this, [upd](double x) {
      upd([&](VideoClip& c) {
        c.chroma.similarity = x;
        c.chroma.enabled = true;
      }, QStringLiteral("cksim-"));
    });
    connect(blend_, &SliderRow::changed, this, [upd](double x) {
      upd([&](VideoClip& c) {
        c.chroma.blend = x;
        c.chroma.enabled = true;
      }, QStringLiteral("ckblend-"));
    });
    for (auto* s : {similarity_, blend_}) connect(s, &SliderRow::released, this, [] { editor().endMerge(); });
    auto setKey = [upd](const QColor& col) {
      upd([&](VideoClip& c) {
        c.chroma.color = col;
        c.chroma.enabled = true;
      });
    };
    connect(keyButtons_[0], &QPushButton::clicked, this, [setKey] { setKey(QColor(0, 255, 0)); });
    connect(keyButtons_[1], &QPushButton::clicked, this, [setKey] { setKey(QColor(0, 0, 255)); });
    connect(keyButtons_[2], &QPushButton::clicked, this, [this, setKey] {
      const VideoClip* c = editor().clip(clipId_);
      const QColor col = QColorDialog::getColor(c ? c->chroma.color : QColor(0, 255, 0), this, QStringLiteral("透明にする色"));
      if (col.isValid()) setKey(col);
    });
    // スポイト: プレビューの映像をクリックして色を選ぶ（もう一度押すと、やめる）
    connect(keyButtons_[3], &QPushButton::clicked, this, [this] {
      Editor& ed = editor();
      if (ed.chromaPick() == clipId_) {
        ed.setChromaPick({});
        return;
      }
      // 今の再生位置にその映像が出ていなければ、出ている所（始まり）へ動かす
      double start = 0, dur = 0;
      if (ed.clipSpan(clipId_, &start, &dur)) {
        const double t = ed.currentTime();
        if (t < start - 1e-9 || t >= start + dur - 1e-9) ed.seek(start);
      }
      ed.setChromaPick(clipId_);
    });
    connect(&editor(), &Editor::chromaPickChanged, this, [this] { showPicking(); });
    connect(&editor(), &Editor::thumbsChanged, this, [this](const QString& id) {
      if (id == thumbAsset_) thumbAsset_.clear();
      if (isVisible()) refresh();
    });
    // タブを切り替えたら、フィルターの見本は選び直したクリップの素材から作り直す
    connect(&editor(), &Editor::workspaceChanged, this, [this] { thumbAsset_.clear(); });
  }

  void setClip(const QString& id) {
    clipId_ = id;
    volKeys_->setTarget(id);
  }

  /** 音量のスライダーに、再生位置の音量（キーフレームが無ければクリップ全体の音量）を出す */
  void showVolume() {
    const Editor& ed = editor();
    const VideoClip* c = ed.clip(clipId_);
    if (!c) return;
    double start = 0, dur = 0;
    ed.clipSpan(c->id, &start, &dur);
    volume_->setValue(clipGainAt(*c, std::clamp(ed.currentTime() - start, 0.0, dur)));
  }

  /** 調整の不透明度に、再生位置の不透明度（キーフレームが無ければクリップの不透明度）を出す */
  void showOpacity() {
    const Editor& ed = editor();
    const Selection& s = ed.selection();
    if ((s.type != Selection::Clip && s.type != Selection::Overlay) || s.id != clipId_) return;
    opacity_->setValue(ed.currentLayerState().opacity);
  }

  /** スポイトでこの映像の色を選んでいる間は、スポイトのボタンを目立たせる */
  void showPicking() {
    QPushButton* b = keyButtons_[3];
    const bool on = !clipId_.isEmpty() && editor().chromaPick() == clipId_;
    if (b->property("primary").toBool() == on) return;
    b->setProperty("primary", on);
    b->style()->unpolish(b);
    b->style()->polish(b);
  }

  void refresh() {
    const Editor& ed = editor();
    const VideoClip* c = ed.clip(clipId_);
    if (!c) return;
    const OverlayItem* ov = ed.overlay(clipId_);
    anim_->refresh();
    volKeys_->refresh();
    // 音量のキーフレームがあるときは、スライダーに「再生位置の音量」を出し、動かすと再生位置のキーフレームを更新する
    const bool hasKeys = !c->volumePoints.empty();
    volClear_->setVisible(hasKeys);
    volNote_->setVisible(hasKeys);
    volNote_->setText(QStringLiteral("音量のキーフレーム %1 個。スライダーは再生位置の音量です（◆で追加・削除）").arg(c->volumePoints.size()));
    const Asset* a = ed.asset(c->assetId);
    double start = 0, dur = 0;
    ed.clipSpan(c->id, &start, &dur);
    const QString name = a ? a->name : QStringLiteral("ファイルが見つかりません");
    header_->set(c->isImage ? QStringLiteral("image") : QStringLiteral("film"), ov ? QStringLiteral("%1（重ねる）").arg(name) : name,
                 ov ? QStringLiteral("%1〜%2").arg(formatTime(start), formatTime(start + dur)) : formatDuration(dur));
    timeSec_->setVisible(ov != nullptr);
    if (ov) start_->setValue(ov->start);
    // 重ねるトラックは、いつも全体表示（余白は透明）なので、表示方法は出さない
    fitSec_->setVisible(!ov);
    chromaNote_->setVisible(!ov && c->chroma.enabled);
    durationSec_->setVisible(c->isImage);
    speedSec_->setVisible(!c->isImage);
    audioSec_->setVisible(!c->isImage && a && a->hasAudio);
    duration_->setValue(c->out - c->in);
    speed_->setValue(c->speed);
    showVolume();
    {
      const QSignalBlocker b(mute_);
      mute_->setChecked(c->muted);
    }
    fit_->setValue(int(c->fit));
    // サイズ・切り抜き
    stretchX_->setValue(c->stretchX);
    stretchY_->setValue(c->stretchY);
    crop_[0]->setValue(c->cropT);
    crop_[1]->setValue(c->cropB);
    crop_[2]->setValue(c->cropL);
    crop_[3]->setValue(c->cropR);
    sizeReset_->setVisible(hasStretch(*c) || hasCrop(*c));
    // クロマキー
    {
      const QSignalBlocker b1(chromaOn_);
      const QSignalBlocker b2(despill_);
      chromaOn_->setChecked(c->chroma.enabled);
      despill_->setChecked(c->chroma.despill);
    }
    keyChip_->setStyleSheet(QStringLiteral("background:%1;border:1px solid #5a5a5a;border-radius:4px").arg(c->chroma.color.name()));
    keyChip_->setToolTip(QStringLiteral("透明にする色: %1").arg(c->chroma.color.name().toUpper()));
    similarity_->setValue(c->chroma.similarity);
    blend_->setValue(c->chroma.blend);
    despill_->setEnabled(c->chroma.enabled);
    showPicking();
    // フィルターの見本（素材のサムネイルに色をつけたもの）
    if (thumbAsset_ != c->assetId) {
      thumbAsset_ = c->assetId;
      const auto* thumbs = ed.thumbs(c->assetId);
      QImage base = thumbs && !thumbs->empty() ? thumbs->front() : QImage();
      const qreal dpr = devicePixelRatioF();
      const QSize sz(int(80 * dpr), int(50 * dpr));
      if (base.isNull()) {
        base = QImage(sz, QImage::Format_RGB32);
        QPainter p(&base);
        QLinearGradient g(0, 0, sz.width(), 0);
        g.setColorAt(0, QColor(0xc9, 0x4b, 0x4b));
        g.setColorAt(0.33, QColor(0xc9, 0xa9, 0x4b));
        g.setColorAt(0.66, QColor(0x4b, 0xc9, 0x6b));
        g.setColorAt(1, QColor(0x4b, 0x8b, 0xc9));
        p.fillRect(base.rect(), g);
      }
      base = base.scaled(sz, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
      base = base.copy((base.width() - sz.width()) / 2, (base.height() - sz.height()) / 2, sz.width(), sz.height());
      const auto& presets = filterPresets();
      for (size_t i = 0; i < presets.size(); ++i) {
        QImage img = base;
        applyColor(img, colorFromPreset(presets[i].id));
        QPixmap pm = QPixmap::fromImage(img);
        pm.setDevicePixelRatio(dpr);
        filterButtons_[i]->setIcon(QIcon(pm));
      }
    }
    const auto& presets = filterPresets();
    for (size_t i = 0; i < presets.size(); ++i) filterButtons_[i]->setChecked(presets[i].id == c->color.preset);
    brightness_->setValue(c->color.brightness);
    contrast_->setValue(c->color.contrast);
    saturation_->setValue(c->color.saturation);
    temperature_->setValue(c->color.temperature);
    const ColorAdjust base = colorFromPreset(c->color.preset);
    brightness_->setDefault(base.brightness);
    contrast_->setDefault(base.contrast);
    saturation_->setDefault(base.saturation);
    temperature_->setDefault(base.temperature);
    const bool changed = std::abs(c->color.brightness - base.brightness) > 1e-4 || std::abs(c->color.contrast - base.contrast) > 1e-4 ||
                         std::abs(c->color.saturation - base.saturation) > 1e-4 || std::abs(c->color.temperature - base.temperature) > 1e-4;
    reset_->setVisible(changed);
    showOpacity();
    fadeIn_->setValue(c->fadeIn);
    fadeOut_->setValue(c->fadeOut);
  }

 private:
  QString clipId_;
  QString thumbAsset_;
  PanelHeader* header_;
  Section* timeSec_;
  SecondsField* start_;
  Section* fitSec_;
  Section* sizeSec_;
  QPushButton* sizeReset_;
  SliderRow* stretchX_;
  SliderRow* stretchY_;
  SliderRow* crop_[4];
  Section* chromaSec_;
  QCheckBox* chromaOn_;
  QLabel* keyChip_;
  std::vector<QPushButton*> keyButtons_;
  SliderRow* similarity_;
  SliderRow* blend_;
  QCheckBox* despill_;
  QLabel* chromaNote_;
  Section* durationSec_;
  Section* speedSec_;
  Section* audioSec_;
  Section* adjustSec_;
  SliderRow* duration_;
  Segmented* speed_;
  SliderRow* volume_;
  VolumeKeyControls* volKeys_;
  QLabel* volNote_;
  QPushButton* volClear_;
  AnimPanel* anim_;
  QCheckBox* mute_;
  Segmented* fit_;
  std::vector<QToolButton*> filterButtons_;
  QPushButton* reset_;
  SliderRow* brightness_;
  SliderRow* contrast_;
  SliderRow* saturation_;
  SliderRow* temperature_;
  SliderRow* opacity_;
  SliderRow* fadeIn_;
  SliderRow* fadeOut_;
};

// ---- テキスト ---------------------------------------------------------------

class TextPanel : public QWidget {
 public:
  explicit TextPanel(QWidget* parent = nullptr) : QWidget(parent) {
    auto* v = pageLayout(this);
    header_ = new PanelHeader(this);
    v->addWidget(header_);
    // 日本語を変換している途中も、案内の文字（プレースホルダー）をすぐに消す入力欄
    edit_ = new PlainTextInput(this);
    edit_->setPlaceholderText(QStringLiteral("テキストを入力"));
    edit_->setFixedHeight(76);
    edit_->setAccessibleName(QStringLiteral("テキスト"));
    edit_->setTabChangesFocus(true);
    v->addWidget(edit_);
    v->addSpacing(12);

    auto* styleSec = new Section(QStringLiteral("スタイル"), this);
    // 組み込みのスタイルと、自分で保存したテンプレート（保存・名前の変更・削除で並べ直す）
    styleBox_ = new QWidget(styleSec);
    styleGrid_ = new QGridLayout(styleBox_);
    styleGrid_->setContentsMargins(0, 0, 0, 0);
    styleGrid_->setSpacing(6);
    styleSec->body()->addWidget(styleBox_);
    auto* saveTemplate = new QPushButton(theme::icon(QStringLiteral("plus")), QStringLiteral("今の見た目をテンプレートに保存…"), styleSec);
    saveTemplate->setToolTip(QStringLiteral("フォント・色・縁取り・背景・影・登場のアニメーション・キーフレームを保存します（文字は保存しません）"));
    styleSec->body()->addWidget(saveTemplate);
    addSection(v, styleSec);
    rebuildStyles();
    connect(&textTemplates(), &TextTemplates::changed, this, [this] { rebuildStyles(); });
    // テンプレートのカードは、読み込んだフォントで描くことがあるので、フォントが変わったら描き直す
    connect(&fontLibrary(), &FontLibrary::changed, this, [this] { rebuildStyles(); });
    connect(saveTemplate, &QPushButton::clicked, this, [this] { saveAsTemplate(); });

    auto* fontSec = new Section(QStringLiteral("フォント"), this);
    font_ = new QComboBox(fontSec);
    font_->setAccessibleName(QStringLiteral("フォント"));
    font_->setMaxVisibleItems(18);
    fontMore_ = makeToolButton(QStringLiteral("sliders"), QStringLiteral("フォントの読み込み・管理"), fontSec);
    fontMore_->setPopupMode(QToolButton::InstantPopup);
    auto* fontMenu = new QMenu(fontMore_);
    fontMore_->setMenu(fontMenu);
    connect(fontMenu, &QMenu::aboutToShow, this, [this, fontMenu] { fillFontMenu(fontMenu); });
    auto* fontRow = new QHBoxLayout();
    fontRow->setSpacing(4);
    fontRow->addWidget(font_, 1);
    fontRow->addWidget(fontMore_);
    fontNote_ = makeLabel(QString(), "note", fontSec);
    fontNote_->setWordWrap(true);
    fillFonts();
    connect(&fontLibrary(), &FontLibrary::changed, this, [this] {
      fillFonts();
      syncFont();
    });
    size_ = new SliderRow(QStringLiteral("サイズ"), 0.03, 0.22, 0.002, [](double x) { return QString::number(std::lround(x * 1000)); }, fontSec);
    // フォントの選択は、ホイールでは変えない（ホイールはパネルのスクロールに使う）。サイズは数値なので、ホイール・矢印キーで変えられる
    disableWheel(font_);
    bold_ = new QCheckBox(QStringLiteral("太字"), fontSec);
    shadow_ = new QCheckBox(QStringLiteral("影"), fontSec);
    auto* checks = new QHBoxLayout();
    checks->setSpacing(18);
    checks->addWidget(bold_);
    checks->addWidget(shadow_);
    checks->addStretch(1);
    // 字間: 文字の大きさの近くに置く（スライダー・数値入力欄・キーフレームの ◆）
    spacing_ = new LetterSpacingRow(fontSec);
    fontSec->body()->addLayout(fontRow);
    fontSec->body()->addWidget(fontNote_);
    fontSec->body()->addWidget(size_);
    fontSec->body()->addWidget(spacing_);
    fontSec->body()->addLayout(checks);
    addSection(v, fontSec);

    auto* colorSec = new Section(QStringLiteral("色"), this);
    color_ = new ColorSwatches(QStringLiteral("文字色"), colorSec);
    stroke_ = new ColorSwatches(QStringLiteral("縁取りの色"), colorSec);
    strokeWidth_ = new SliderRow(QStringLiteral("縁取りの太さ"), 0, 0.25, 0.005,
                                 [](double x) { return x == 0 ? QStringLiteral("なし") : QString::number(std::lround(x * 100)); }, colorSec);
    box_ = new QCheckBox(QStringLiteral("背景"), colorSec);
    boxColor_ = new ColorSwatches(QStringLiteral("背景色"), colorSec);
    boxOpacity_ = new SliderRow(QStringLiteral("背景の不透明度"), 0.1, 1, 0.01, pct, colorSec);
    boxOpacity_->setUnit(0.01, 0.001);  // 1%（Alt で 0.1%）
    colorSec->body()->addWidget(makeLabel(QStringLiteral("文字"), "muted", colorSec));
    colorSec->body()->addWidget(color_);
    colorSec->body()->addWidget(makeLabel(QStringLiteral("縁取り"), "muted", colorSec));
    colorSec->body()->addWidget(stroke_);
    colorSec->body()->addWidget(strokeWidth_);
    colorSec->body()->addWidget(box_);
    colorSec->body()->addWidget(boxColor_);
    colorSec->body()->addWidget(boxOpacity_);
    addSection(v, colorSec);

    auto* posSec = new Section(QStringLiteral("位置"), this);
    // (位置のボタンは、キーフレームのある項目にも使えるよう setPropValue で動かす)
    auto* posRow = new QHBoxLayout();
    posRow->setSpacing(6);
    struct PosButton {
      QString icon;
      QString label;
      double y;
    };
    for (const PosButton& pb : {PosButton{QStringLiteral("arrow-up-to-line"), QStringLiteral("上部"), 0.14},
                                PosButton{QStringLiteral("move-vertical"), QStringLiteral("中央"), 0.5},
                                PosButton{QStringLiteral("arrow-down-to-line"), QStringLiteral("下部"), 0.84}}) {
      auto* b = new QPushButton(theme::icon(pb.icon), pb.label, posSec);
      const double y = pb.y;
      connect(b, &QPushButton::clicked, this, [y] {
        // 位置の X・Y は、キーフレームがあれば再生位置のキーフレームとして記録される（無ければ静的な値を変える）
        editor().setPropValue(AnimProp::X, 0.5);
        editor().setPropValue(AnimProp::Y, y);
      });
      posRow->addWidget(b);
    }
    align_ = new Segmented({{int(TextAlign::Left), QStringLiteral("左"), QStringLiteral("align-left")},
                            {int(TextAlign::Center), QStringLiteral("中央"), QStringLiteral("align-center")},
                            {int(TextAlign::Right), QStringLiteral("右"), QStringLiteral("align-right")}},
                           0, posSec);
    posSec->body()->addLayout(posRow);
    posSec->body()->addWidget(align_);
    addSection(v, posSec);

    // 位置・拡大率・回転・不透明度（キーフレームでアニメーションできる）
    v->addWidget(makeSeparator());
    animPanel_ = new AnimPanel(this);
    v->addWidget(animPanel_);
    // 字間の行を操作したら、アニメーションの欄の入力欄（時間・値・補間方式）も字間のキーフレームを編集する
    connect(spacing_, &LetterSpacingRow::touched, animPanel_, [this] { animPanel_->setActiveProp(AnimProp::LetterSpacing); });

    auto* timeSec = new Section(QStringLiteral("タイミング"), this);
    auto* tg = new QHBoxLayout();
    tg->setSpacing(8);
    start_ = new SecondsField(QStringLiteral("開始"), 0, 36000, timeSec);
    length_ = new SecondsField(QStringLiteral("長さ"), 0.3, 600, timeSec);
    tg->addWidget(start_);
    tg->addWidget(length_);
    auto* fromHere = new QPushButton(QStringLiteral("現在位置から開始"), timeSec);
    timeSec->body()->addLayout(tg);
    timeSec->body()->addWidget(fromHere);
    addSection(v, timeSec);

    auto* animSec = new Section(QStringLiteral("登場のアニメーション"), this);
    anim_ = new Segmented({{int(TextAnimation::None), QStringLiteral("なし")},
                           {int(TextAnimation::Fade), QStringLiteral("フェード")},
                           {int(TextAnimation::SlideUp), QStringLiteral("スライド")}},
                          0, animSec);
    animSec->body()->addWidget(anim_);
    addSection(v, animSec);
    v->addStretch(1);

    auto upd = [this](const std::function<void(TextItem&)>& fn, const QString& merge = QString()) {
      editor().updateText(textId_, fn, merge.isEmpty() ? QString() : merge + textId_);
    };
    connect(edit_, &QPlainTextEdit::textChanged, this, [this, upd] {
      const QString s = edit_->toPlainText();
      const TextItem* t = editor().text(textId_);
      if (!t || t->text == s) return;
      upd([&](TextItem& x) { x.text = s; }, QStringLiteral("text-"));
    });
    connect(font_, &QComboBox::activated, this, [this](int idx) { chooseFont(font_->itemData(idx).toString()); });
    // 入力欄で文字を選ぶと、フォントの欄はその文字のフォントを出し、選んだフォントはその文字だけに使う
    connect(edit_, &QPlainTextEdit::cursorPositionChanged, this, [this] { syncFont(); });
    connect(edit_, &QPlainTextEdit::selectionChanged, this, [this] { syncFont(); });
    connect(size_, &SliderRow::changed, this, [upd](double x) { upd([&](TextItem& t) { t.size = x; }, QStringLiteral("size-")); });
    connect(bold_, &QCheckBox::toggled, this, [upd](bool on) { upd([&](TextItem& t) { t.bold = on; }); });
    connect(shadow_, &QCheckBox::toggled, this, [upd](bool on) { upd([&](TextItem& t) { t.shadow = on; }); });
    connect(color_, &ColorSwatches::changed, this, [upd](const QColor& c) { upd([&](TextItem& t) { t.color = c; }); });
    connect(stroke_, &ColorSwatches::changed, this, [upd](const QColor& c) {
      upd([&](TextItem& t) {
        t.strokeColor = c;
        if (t.strokeWidth <= 0) t.strokeWidth = 0.12;
      });
    });
    connect(strokeWidth_, &SliderRow::changed, this, [upd](double x) { upd([&](TextItem& t) { t.strokeWidth = x; }, QStringLiteral("sw-")); });
    connect(box_, &QCheckBox::toggled, this, [upd](bool on) { upd([&](TextItem& t) { t.boxOpacity = on ? 0.6 : 0; }); });
    connect(boxColor_, &ColorSwatches::changed, this, [upd](const QColor& c) { upd([&](TextItem& t) { t.boxColor = c; }); });
    connect(boxOpacity_, &SliderRow::changed, this, [upd](double x) { upd([&](TextItem& t) { t.boxOpacity = x; }, QStringLiteral("bo-")); });
    for (auto* s : {size_, strokeWidth_, boxOpacity_}) connect(s, &SliderRow::released, this, [] { editor().endMerge(); });
    connect(align_, &Segmented::changed, this, [upd](const QVariant& x) { upd([&](TextItem& t) { t.align = TextAlign(x.toInt()); }); });
    // ホイール・矢印キーで続けて変えたぶんは、元に戻す履歴の 1 回にまとめる
    connect(start_, &SecondsField::changed, this, [upd](double x) { upd([&](TextItem& t) { t.start = x; }, QStringLiteral("tstart-")); });
    connect(length_, &SecondsField::changed, this, [upd](double x) { upd([&](TextItem& t) { t.duration = x; }, QStringLiteral("tlen-")); });
    for (auto* f : {start_, length_}) connect(f, &SecondsField::released, this, [] { editor().endMerge(); });
    connect(fromHere, &QPushButton::clicked, this, [upd] { upd([](TextItem& t) { t.start = editor().currentTime(); }); });
    connect(anim_, &Segmented::changed, this, [upd](const QVariant& x) { upd([&](TextItem& t) { t.animation = TextAnimation(x.toInt()); }); });
  }

  void setText(const QString& id) { textId_ = id; }
  void focusText() {
    edit_->setFocus(Qt::OtherFocusReason);
    edit_->selectAll();
  }

  /** スタイルの一覧（組み込み + 自分で保存したテンプレート）を並べ直す */
  void rebuildStyles() {
    while (QLayoutItem* it = styleGrid_->takeAt(0)) {
      if (QWidget* w = it->widget()) w->deleteLater();
      delete it;
    }
    std::vector<TextPreset> all = textPresets();
    for (const auto& p : textTemplates().list()) all.push_back(p);
    int i = 0;
    for (const auto& preset : all) {
      auto* b = makeTextPresetButton(preset, QSize(124, 46), styleBox_);
      const QString id = preset.id;
      connect(b, &QToolButton::clicked, this, [this, id] {
        editor().commit([&](ProjectData& p) {
          for (auto& t : p.texts) {
            if (t.id == textId_) {
              t = applyTextPreset(t, id);
              return true;
            }
          }
          return false;
        });
      });
      if (TextTemplates::isUserId(id)) {
        // 自分で保存したテンプレート: 右クリックで名前の変更・削除
        b->setToolTip(QStringLiteral("%1（マイテンプレート）\n右クリックで名前の変更・削除").arg(preset.label));
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QWidget::customContextMenuRequested, b, [b, id](const QPoint& pos) { execTemplateMenu(b, id, b->mapToGlobal(pos)); });
      }
      styleGrid_->addWidget(b, i / 2, i % 2);
      ++i;
    }
  }

  /** 選んでいるテキストの見た目を、名前を付けてテンプレートにする */
  void saveAsTemplate() { saveTextAsTemplate(this, textId_); }

  /** フォントの一覧（組み込みの種類 → 同梱のフォント → 読み込んだフォント → 読み込む） */
  void fillFonts() {
    const QSignalBlocker b(font_);
    font_->clear();
    auto add = [this](const QString& label, const QString& data, const QStringList& families) {
      font_->addItem(label, data);
      QFont qf;
      qf.setFamilies(families);
      font_->setItemData(font_->count() - 1, qf, Qt::FontRole);
    };
    for (const auto& f : fontOptions()) add(f.label, QStringLiteral("id:") + fontToString(f.id), f.families);
    const auto& bundled = fontLibrary().bundled();
    if (!bundled.empty()) {
      font_->insertSeparator(font_->count());
      for (const auto& e : bundled) add(e.label, QStringLiteral("family:") + e.family, {e.family});
    }
    const auto& imported = fontLibrary().imported();
    if (!imported.empty()) {
      font_->insertSeparator(font_->count());
      for (const auto& e : imported) add(e.label, QStringLiteral("family:") + e.family, {e.family});
    }
    font_->insertSeparator(font_->count());
    font_->addItem(theme::icon(QStringLiteral("folder-open")), QStringLiteral("フォントを読み込む…"), QStringLiteral("import"));
  }

  /** 「…」のメニュー: フォントの読み込みと、読み込んだフォントを外す */
  void fillFontMenu(QMenu* menu) {
    menu->clear();
    menu->addAction(QStringLiteral("フォントを読み込む… (.ttf / .otf / .ttc)"), this, [this] { importFont(); });
    QMenu* remove = menu->addMenu(QStringLiteral("読み込んだフォントを外す"));
    const auto& imported = fontLibrary().imported();
    remove->setEnabled(!imported.empty());
    for (const auto& e : imported) {
      const QString family = e.family;
      remove->addAction(e.label, this, [this, family] {
        const auto r = QMessageBox::question(this, QStringLiteral("フォントを外す"),
                                             QStringLiteral("「%1」を一覧から外しますか？\nこのフォントを使っている文字は、組み込みのフォントで表示されます。").arg(family));
        if (r != QMessageBox::Yes) return;
        QString err;
        if (!fontLibrary().removeImported(family, &err)) editor().toast(err, ToastKind::Error);
        else editor().toast(QStringLiteral("「%1」を外しました").arg(family));
      });
    }
    menu->addSeparator();
    QAction* hint = menu->addAction(QStringLiteral("文字の一部を選んでからフォントを選ぶと、その文字だけが変わります"));
    hint->setEnabled(false);
  }

  /** 入力欄で選んでいる範囲（選んでいなければ空）。文字の位置は TextItem::text と同じ */
  std::pair<int, int> editSelection() const {
    const QTextCursor c = edit_->textCursor();
    return {c.selectionStart(), c.selectionEnd()};
  }

  /** フォントの欄に、全体のフォント（入力欄で一部を選んでいるときは、選んだ範囲の先頭の文字のフォント）を出す */
  void syncFont() {
    const TextItem* t = editor().text(textId_);
    if (!t) return;
    const std::pair<int, int> sel = editSelection();
    const int a = sel.first;
    const int b = sel.second;
    const bool partial = b > a && !(a == 0 && b >= int(t->text.size()));
    FontId fid = t->font;
    QString family = t->fontFamily;
    if (partial) {
      const FontSpan f = fontAt(*t, a);
      fid = f.font;
      family = f.family;
    }
    const QString data = family.isEmpty() ? QStringLiteral("id:") + fontToString(fid) : QStringLiteral("family:") + family;
    const QSignalBlocker block(font_);
    int idx = font_->findData(data);
    if (idx < 0 && !family.isEmpty()) {
      // このパソコンに無いフォント（ほかのパソコンで読み込んだフォントなど）: 一覧に出して、代わりのフォントで描いていると知らせる
      font_->insertItem(0, QStringLiteral("%1（このパソコンにありません）").arg(family), data);
      idx = 0;
    }
    font_->setCurrentIndex(idx);
    if (partial) {
      fontNote_->setText(QStringLiteral("選んだ %1 文字だけに使います（全部を選ぶと、全体のフォントになります）").arg(b - a));
    } else if (!t->fontSpans.empty()) {
      fontNote_->setText(QStringLiteral("一部の文字は別のフォントです。入力欄で文字を選んでからフォントを選ぶと、その文字だけが変わります"));
    } else if (!family.isEmpty() && !FontLibrary::available(family)) {
      fontNote_->setText(QStringLiteral("このフォントが無いため、「%1」で表示しています").arg(fontOptions().front().label));
    } else {
      fontNote_->clear();
    }
    fontNote_->setVisible(!fontNote_->text().isEmpty());
  }

  /** フォントの欄で選んだ: 入力欄で一部を選んでいればその文字だけ、そうでなければ全体のフォントにする */
  void chooseFont(const QString& data) {
    if (data == u"import") {
      importFont();
      return;
    }
    FontId fid = FontId::Gothic;
    QString family;
    if (data.startsWith(QStringLiteral("id:"))) {
      fid = fontFromString(data.mid(3));
    } else if (data.startsWith(QStringLiteral("family:"))) {
      family = data.mid(7);
    } else {
      syncFont();
      return;
    }
    applyFont(fid, family);
  }

  void applyFont(FontId fid, const QString& family) {
    const TextItem* cur = editor().text(textId_);
    if (!cur) return;
    const std::pair<int, int> sel = editSelection();
    const int a = sel.first;
    const int b = sel.second;
    const bool whole = b <= a || (a == 0 && b >= int(cur->text.size()));
    const bool all = b > a && whole;
    editor().updateText(textId_, [&](TextItem& t) {
      if (!whole) {
        // 選んだ文字だけ（ファミリー名のフォントが無いパソコンでは、全体の組み込みの種類で描く）
        setFontRange(t, a, b, family.isEmpty() ? fid : t.font, family);
        return;
      }
      if (family.isEmpty()) t.font = fid;
      t.fontFamily = family;
      if (all) t.fontSpans.clear();  // 全部を選んで変えたときは、一部だけ変えていたフォントもそろえる
    });
    syncFont();
  }

  /** フォントのファイルを選んで読み込み、選んでいる文字（範囲）に使う */
  void importFont() {
    static QString lastDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("フォントを読み込む"), lastDir, FontLibrary::fileFilter());
    if (file.isEmpty()) {
      syncFont();
      return;
    }
    lastDir = QFileInfo(file).absolutePath();
    QString err;
    const QStringList families = fontLibrary().importFile(file, &err);
    if (families.isEmpty()) {
      editor().toast(err.isEmpty() ? QStringLiteral("フォントを読み込めませんでした") : err, ToastKind::Error);
      syncFont();
      return;
    }
    editor().toast(QStringLiteral("フォント「%1」を読み込みました。次からもフォントの一覧に出ます").arg(families.join(QStringLiteral("」「"))),
                   ToastKind::Success);
    if (editor().text(textId_)) applyFont(FontId::Gothic, families.front());
  }

  void refresh() {
    const TextItem* t = editor().text(textId_);
    if (!t) return;
    animPanel_->refresh();
    spacing_->refresh();
    header_->set(QStringLiteral("type"), QStringLiteral("テキスト"),
                 QStringLiteral("%1〜%2").arg(formatTime(t->start), formatTime(t->start + t->duration)));
    if (edit_->toPlainText() != t->text) {
      const QSignalBlocker b(edit_);
      edit_->setPlainText(t->text);
    }
    syncFont();
    size_->setValue(t->size);
    {
      const QSignalBlocker b1(bold_);
      const QSignalBlocker b2(shadow_);
      const QSignalBlocker b3(box_);
      bold_->setChecked(t->bold);
      shadow_->setChecked(t->shadow);
      box_->setChecked(t->boxOpacity > 0);
    }
    color_->setColor(t->color);
    stroke_->setColor(t->strokeColor);
    strokeWidth_->setValue(t->strokeWidth);
    boxColor_->setColor(t->boxColor);
    boxColor_->setVisible(t->boxOpacity > 0);
    boxOpacity_->setVisible(t->boxOpacity > 0);
    boxOpacity_->setValue(t->boxOpacity);
    align_->setValue(int(t->align));
    start_->setValue(t->start);
    length_->setValue(t->duration);
    anim_->setValue(int(t->animation));
  }

 private:
  QString textId_;
  PanelHeader* header_;
  AnimPanel* animPanel_;
  QPlainTextEdit* edit_;
  QWidget* styleBox_;
  QGridLayout* styleGrid_;
  QComboBox* font_;
  QToolButton* fontMore_;
  QLabel* fontNote_;
  SliderRow* size_;
  LetterSpacingRow* spacing_;
  QCheckBox* bold_;
  QCheckBox* shadow_;
  ColorSwatches* color_;
  ColorSwatches* stroke_;
  SliderRow* strokeWidth_;
  QCheckBox* box_;
  ColorSwatches* boxColor_;
  SliderRow* boxOpacity_;
  Segmented* align_;
  SecondsField* start_;
  SecondsField* length_;
  Segmented* anim_;
};

// ---- オーディオ -------------------------------------------------------------

class AudioPanel : public QWidget {
 public:
  explicit AudioPanel(QWidget* parent = nullptr) : QWidget(parent) {
    auto* v = pageLayout(this);
    header_ = new PanelHeader(this);
    v->addWidget(header_);
    auto* volSec = new Section(QStringLiteral("音量"), this);
    volume_ = new SliderRow(QStringLiteral("音量"), 0, kMaxVolume, 0.01, pct, volSec);
    volume_->setLabelVisible(false);
    volume_->setValueBubble(true);
    volume_->setDefault(kMaxVolume);
    volume_->setUnit(0.01, 0.001);  // 1%（Alt で 0.1%）
    volKeys_ = new VolumeKeyControls(volSec);
    auto* volRow = new QHBoxLayout();
    volRow->setSpacing(4);
    volRow->addWidget(volume_, 1);
    volRow->addWidget(volKeys_, 0, Qt::AlignBottom);
    volSec->body()->addLayout(volRow);
    // 時間ごとに音量を変えるには、◆ か、タイムラインの音量ラインにキーフレームを置く
    keyNote_ = makeLabel(QString(), "note", volSec);
    keyNote_->setWordWrap(true);
    keyClear_ = new QPushButton(QStringLiteral("キーフレームをすべて削除"), volSec);
    volSec->body()->addWidget(keyNote_);
    volSec->body()->addWidget(keyClear_);
    addSection(v, volSec);
    auto* fadeSec = new Section(QStringLiteral("フェード"), this);
    fadeIn_ = makeFadeSlider(QStringLiteral("イン"), fadeSec);
    fadeOut_ = makeFadeSlider(QStringLiteral("アウト"), fadeSec);
    fadeSec->body()->addWidget(fadeIn_);
    fadeSec->body()->addWidget(fadeOut_);
    addSection(v, fadeSec);
    auto* timeSec = new Section(QStringLiteral("タイミング"), this);
    start_ = new SecondsField(QStringLiteral("開始"), 0, 36000, timeSec);
    note_ = makeLabel(QStringLiteral("動画より長い部分は書き出されません"), "note", timeSec);
    note_->setWordWrap(true);
    fitEnd_ = new QPushButton(QStringLiteral("動画の長さに合わせる"), timeSec);
    timeSec->body()->addWidget(start_);
    timeSec->body()->addWidget(note_);
    timeSec->body()->addWidget(fitEnd_);
    addSection(v, timeSec);
    v->addStretch(1);

    auto upd = [this](const std::function<void(AudioItem&)>& fn, const QString& merge = QString()) {
      editor().updateAudio(audioId_, fn, merge.isEmpty() ? QString() : merge + audioId_);
    };
    connect(volume_, &SliderRow::changed, this, [this, upd](double x) {
      const AudioItem* cur = editor().audio(audioId_);
      if (cur && !cur->volumePoints.empty()) {
        // 音量のキーフレームがあるとき: 再生位置のキーフレームの値を変える（無ければ、自動記録がオンのときだけ追加）
        if (!editor().setVolumeValue(audioId_, x, QStringLiteral("avol-") + audioId_)) refresh();
        return;
      }
      upd([&](AudioItem& a) { a.volume = x; }, QStringLiteral("avol-"));
    });
    connect(&editor(), &Editor::timeChanged, this, [this] {
      if (isVisible()) showVolume();
    });
    connect(volume_, &SliderRow::released, this, [] { editor().endMerge(); });
    connect(keyClear_, &QPushButton::clicked, this, [this] { editor().clearVolumePoints(audioId_); });
    // フェードは自由な長さ（0.1 秒刻み）。オーディオの長さに収め、イン・アウトは重ならないようにする
    connect(fadeIn_, &SliderRow::changed, this, [upd](double x) {
      upd([&](AudioItem& a) { a.fadeIn = fitFade(x, audioDuration(a), a.fadeOut); }, QStringLiteral("afadein-"));
    });
    connect(fadeOut_, &SliderRow::changed, this, [upd](double x) {
      upd([&](AudioItem& a) { a.fadeOut = fitFade(x, audioDuration(a), a.fadeIn); }, QStringLiteral("afadeout-"));
    });
    for (auto* s : {fadeIn_, fadeOut_}) connect(s, &SliderRow::released, this, [] { editor().endMerge(); });
    connect(start_, &SecondsField::changed, this, [upd](double x) { upd([&](AudioItem& a) { a.start = x; }, QStringLiteral("astart-")); });
    connect(start_, &SecondsField::released, this, [] { editor().endMerge(); });
    connect(fitEnd_, &QPushButton::clicked, this, [this, upd] {
      const double total = editor().layout().total;
      const AudioItem* item = editor().audio(audioId_);
      if (!item) return;
      const Asset* asset = editor().asset(item->assetId);
      const double assetDur = asset ? asset->duration : item->out;
      upd([&](AudioItem& a) {
        a.out = std::min(assetDur, a.in + (total - a.start));
        if (a.fadeOut <= 0) a.fadeOut = 2;
      });
    });
  }

  void setAudio(const QString& id) {
    audioId_ = id;
    volKeys_->setTarget(id);
  }

  /** 音量のスライダーに、再生位置の音量（キーフレームが無ければ全体の音量）を出す */
  void showVolume() {
    const AudioItem* a = editor().audio(audioId_);
    if (!a) return;
    volume_->setValue(audioGainAt(*a, std::clamp(editor().currentTime() - a->start, 0.0, audioDuration(*a))));
  }

  void refresh() {
    const Editor& ed = editor();
    const AudioItem* a = ed.audio(audioId_);
    if (!a) return;
    const Asset* asset = ed.asset(a->assetId);
    const double dur = audioDuration(*a);
    header_->set(QStringLiteral("music"), asset ? asset->name : QStringLiteral("ファイルが見つかりません"),
                 QStringLiteral("%1〜%2").arg(formatTime(a->start), formatTime(a->start + dur)));
    // キーフレームがあるときは、スライダーに「再生位置の音量」を出し、動かすと再生位置のキーフレームを更新する
    const bool hasKeys = !a->volumePoints.empty();
    volKeys_->refresh();
    keyClear_->setVisible(hasKeys);
    keyNote_->setText(hasKeys ? QStringLiteral("音量のキーフレーム %1 個。スライダーは再生位置の音量です。◆で追加・削除、音量ラインの点のドラッグでも調整できます")
                                    .arg(a->volumePoints.size())
                              : QStringLiteral("◆ を押すと、再生位置に音量のキーフレームを置けます。音量ラインのダブルクリックでも追加できます"));
    showVolume();
    fadeIn_->setValue(a->fadeIn);
    fadeOut_->setValue(a->fadeOut);
    start_->setValue(a->start);
    const double total = ed.layout().total;
    note_->setVisible(total > 0 && a->start + dur > total + 0.05);
    fitEnd_->setEnabled(total > a->start);
  }

 private:
  QString audioId_;
  PanelHeader* header_;
  SliderRow* volume_;
  VolumeKeyControls* volKeys_;
  QLabel* keyNote_;
  QPushButton* keyClear_;
  SliderRow* fadeIn_;
  SliderRow* fadeOut_;
  SecondsField* start_;
  QLabel* note_;
  QPushButton* fitEnd_;
};

// ---- Inspector --------------------------------------------------------------

Inspector::Inspector(QWidget* parent) : QScrollArea(parent) {
  setWidgetResizable(true);
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* container = new QWidget(this);
  auto* v = new QVBoxLayout(container);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  // いくつかを一緒に選んでいるとき（Ctrl+クリック・グループ）の案内と、グループ化のボタン
  multi_ = new QFrame(container);
  multi_->setStyleSheet(QStringLiteral("QFrame{background:#1f2a3a;border-bottom:1px solid #2f4466}"));
  auto* mv = new QVBoxLayout(multi_);
  mv->setContentsMargins(12, 8, 12, 8);
  mv->setSpacing(6);
  multiText_ = new QLabel(multi_);
  multiText_->setWordWrap(true);
  multiText_->setStyleSheet(QStringLiteral("color:#cfe0ff;background:transparent;border:none"));
  auto* mb = new QHBoxLayout();
  mb->setSpacing(6);
  group_ = new QPushButton(QStringLiteral("グループ化 (Ctrl+G)"), multi_);
  ungroup_ = new QPushButton(QStringLiteral("グループ解除"), multi_);
  mb->addWidget(group_);
  mb->addWidget(ungroup_);
  mb->addStretch(1);
  mv->addWidget(multiText_);
  mv->addLayout(mb);
  v->addWidget(multi_);
  multi_->setVisible(false);
  connect(group_, &QPushButton::clicked, this, [] { editor().groupSelection(); });
  connect(ungroup_, &QPushButton::clicked, this, [] { editor().ungroupSelection(); });
  project_ = new ProjectPanel(container);
  clip_ = new ClipPanel(container);
  text_ = new TextPanel(container);
  audio_ = new AudioPanel(container);
  for (QWidget* w : std::initializer_list<QWidget*>{project_, clip_, text_, audio_}) v->addWidget(w);
  setWidget(container);
  Editor& ed = editor();
  connect(&ed, &Editor::selectionChanged, this, &Inspector::onSelection);
  connect(&ed, &Editor::projectChanged, this, &Inspector::refreshCurrent);
  connect(&ed, &Editor::assetsChanged, this, [this] {
    if (clip_->isVisible()) clip_->refresh();
    if (audio_->isVisible()) audio_->refresh();
  });
  onSelection();
}

void Inspector::refreshMulti() {
  const Editor& ed = editor();
  const std::vector<Selection> set = ed.selectionSet();
  const bool many = set.size() > 1;
  multi_->setVisible(many);
  if (!many) return;
  const bool grouped = !ed.groupOf(ed.selection()).isEmpty();
  multiText_->setText(grouped ? QStringLiteral("グループの %1 個を一緒に選んでいます。動かす・削除・複製は、まとめて行います（下は、クリックしたものの設定です）")
                                    .arg(set.size())
                              : QStringLiteral("%1 個を一緒に選んでいます。動かす・削除・複製は、まとめて行います（下は、最後に選んだものの設定です）")
                                    .arg(set.size()));
  group_->setVisible(ed.canGroup());
  ungroup_->setVisible(ed.canUngroup());
}

void Inspector::onSelection() {
  const Selection& s = editor().selection();
  refreshMulti();
  project_->setVisible(s.type == Selection::None);
  clip_->setVisible(s.type == Selection::Clip || s.type == Selection::Overlay);
  text_->setVisible(s.type == Selection::Text);
  audio_->setVisible(s.type == Selection::Audio);
  if (s.type == Selection::Clip || s.type == Selection::Overlay) {
    clip_->setClip(s.id);
    clip_->refresh();
  } else if (s.type == Selection::Text) {
    text_->setText(s.id);
    text_->refresh();
  } else if (s.type == Selection::Audio) {
    audio_->setAudio(s.id);
    audio_->refresh();
  } else {
    project_->refresh();
  }
  verticalScrollBar()->setValue(0);
}

void Inspector::refreshCurrent() {
  const Selection& s = editor().selection();
  refreshMulti();
  if (s.type == Selection::Clip || s.type == Selection::Overlay) clip_->refresh();
  else if (s.type == Selection::Text) text_->refresh();
  else if (s.type == Selection::Audio) audio_->refresh();
  else project_->refresh();
}

void Inspector::showEvent(QShowEvent* e) {
  QScrollArea::showEvent(e);
  // 隠れている間は、再生位置が変わっても出し直していない（isVisible() で省いている）ので、出たときに合わせる
  if (!e->spontaneous()) refreshCurrent();
}

void Inspector::focusText() {
  if (editor().selection().type != Selection::Text) return;
  QTimer::singleShot(0, this, [this] { text_->focusText(); });
}

}  // namespace kv
