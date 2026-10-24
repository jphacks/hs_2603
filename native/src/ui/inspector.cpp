#include "ui/inspector.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "core/editor.h"
#include "core/filters.h"
#include "core/textstyles.h"
#include "core/timeline.h"
#include "render/textrender.h"
#include "ui/animpanel.h"
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

static QWidget* fadeRow(const QString& label, Segmented* seg, QWidget* parent) {
  auto* w = new QWidget(parent);
  auto* h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(10);
  auto* l = makeLabel(label, "muted", w);
  l->setFixedWidth(40);
  h->addWidget(l);
  h->addWidget(seg, 1);
  return w;
}

static std::vector<Segmented::Option> fadeOptions(const std::vector<double>& values) {
  std::vector<Segmented::Option> out;
  for (double v : values) out.push_back({v, v == 0 ? QStringLiteral("なし") : QStringLiteral("%1秒").arg(v)});
  return out;
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
  }
  void refresh() {
    const Editor& ed = editor();
    const auto& s = ed.project().settings;
    fps_->setValue(s.fps);
    const QSize size = exportSize(s.aspect, 1080);
    values_[0]->setText(formatTime(ed.layout().total));
    values_[1]->setText(aspectToString(s.aspect));
    values_[2]->setText(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()));
    values_[3]->setText(QString::number(ed.project().clips.size()));
  }

 private:
  Segmented* fps_;
  QLabel* values_[4];
};

// ---- ビデオ・画像 -----------------------------------------------------------

class ClipPanel : public QWidget {
 public:
  explicit ClipPanel(QWidget* parent = nullptr) : QWidget(parent) {
    auto* v = pageLayout(this);
    header_ = new PanelHeader(this);
    v->addWidget(header_);

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

    auto* fitSec = new Section(QStringLiteral("表示方法"), this);
    fit_ = new Segmented({{int(FitMode::Contain), QStringLiteral("全体表示")},
                          {int(FitMode::Cover), QStringLiteral("拡大")},
                          {int(FitMode::Blur), QStringLiteral("ぼかし背景")}},
                         0, fitSec);
    fitSec->body()->addWidget(fit_);
    addSection(v, fitSec);

    // 位置・拡大率・回転・不透明度（キーフレームでアニメーションできる）
    v->addWidget(makeSeparator());
    anim_ = new AnimPanel(this);
    v->addWidget(anim_);

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
    for (auto* s : {brightness_, contrast_, saturation_, temperature_}) adjustSec_->body()->addWidget(s);
    addSection(v, adjustSec_);

    auto* fadeSec = new Section(QStringLiteral("フェード"), this);
    fadeIn_ = new Segmented(fadeOptions({0, 0.5, 1, 2}), 0, fadeSec);
    fadeOut_ = new Segmented(fadeOptions({0, 0.5, 1, 2}), 0, fadeSec);
    fadeSec->body()->addWidget(fadeRow(QStringLiteral("イン"), fadeIn_, fadeSec));
    fadeSec->body()->addWidget(fadeRow(QStringLiteral("アウト"), fadeOut_, fadeSec));
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
      if (isVisible()) showVolume();
    });
    connect(mute_, &QCheckBox::toggled, this, [upd](bool on) { upd([&](VideoClip& c) { c.muted = on; }); });
    connect(fit_, &Segmented::changed, this, [upd](const QVariant& x) { upd([&](VideoClip& c) { c.fit = FitMode(x.toInt()); }); });
    connect(reset_, &QPushButton::clicked, this, [upd] { upd([](VideoClip& c) { c.color = colorFromPreset(c.color.preset); }); });
    connect(brightness_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.color.brightness = x; }, QStringLiteral("br-")); });
    connect(contrast_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.color.contrast = x; }, QStringLiteral("co-")); });
    connect(saturation_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.color.saturation = x; }, QStringLiteral("sa-")); });
    connect(temperature_, &SliderRow::changed, this, [upd](double x) { upd([&](VideoClip& c) { c.color.temperature = x; }, QStringLiteral("te-")); });
    for (auto* s : {duration_, volume_, brightness_, contrast_, saturation_, temperature_}) {
      connect(s, &SliderRow::released, this, [] { editor().endMerge(); });
    }
    connect(fadeIn_, &Segmented::changed, this, [upd](const QVariant& x) { upd([&](VideoClip& c) { c.fadeIn = x.toDouble(); }); });
    connect(fadeOut_, &Segmented::changed, this, [upd](const QVariant& x) { upd([&](VideoClip& c) { c.fadeOut = x.toDouble(); }); });
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
    double local = 0;
    for (const auto& l : ed.layout().items) {
      if (l.clip.id == c->id) local = std::clamp(ed.currentTime() - l.start, 0.0, l.duration);
    }
    volume_->setValue(clipGainAt(*c, local));
  }

  void refresh() {
    const Editor& ed = editor();
    const VideoClip* c = ed.clip(clipId_);
    if (!c) return;
    anim_->refresh();
    volKeys_->refresh();
    // 音量のキーフレームがあるときは、スライダーに「再生位置の音量」を出し、動かすと再生位置のキーフレームを更新する
    const bool hasKeys = !c->volumePoints.empty();
    volClear_->setVisible(hasKeys);
    volNote_->setVisible(hasKeys);
    volNote_->setText(QStringLiteral("音量のキーフレーム %1 個。スライダーは再生位置の音量です（◆で追加・削除）").arg(c->volumePoints.size()));
    const Asset* a = ed.asset(c->assetId);
    double dur = 0;
    for (const auto& l : ed.layout().items) {
      if (l.clip.id == c->id) dur = l.duration;
    }
    header_->set(c->isImage ? QStringLiteral("image") : QStringLiteral("film"), a ? a->name : QStringLiteral("ファイルが見つかりません"),
                 formatDuration(dur));
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
    fadeIn_->setValue(c->fadeIn);
    fadeOut_->setValue(c->fadeOut);
  }

 private:
  QString clipId_;
  QString thumbAsset_;
  PanelHeader* header_;
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
  Segmented* fadeIn_;
  Segmented* fadeOut_;
};

// ---- テキスト ---------------------------------------------------------------

class TextPanel : public QWidget {
 public:
  explicit TextPanel(QWidget* parent = nullptr) : QWidget(parent) {
    auto* v = pageLayout(this);
    header_ = new PanelHeader(this);
    v->addWidget(header_);
    edit_ = new QPlainTextEdit(this);
    edit_->setPlaceholderText(QStringLiteral("テキストを入力"));
    edit_->setFixedHeight(76);
    edit_->setAccessibleName(QStringLiteral("テキスト"));
    edit_->setTabChangesFocus(true);
    v->addWidget(edit_);
    v->addSpacing(12);

    auto* styleSec = new Section(QStringLiteral("スタイル"), this);
    auto* grid = new QGridLayout();
    grid->setSpacing(6);
    int i = 0;
    for (const auto& preset : textPresets()) {
      auto* b = makeTextPresetButton(preset, QSize(124, 46), styleSec);
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
      grid->addWidget(b, i / 2, i % 2);
      ++i;
    }
    styleSec->body()->addLayout(grid);
    addSection(v, styleSec);

    auto* fontSec = new Section(QStringLiteral("フォント"), this);
    font_ = new QComboBox(fontSec);
    font_->setAccessibleName(QStringLiteral("フォント"));
    for (const auto& f : fontOptions()) {
      font_->addItem(f.label, int(f.id));
      QFont qf;
      qf.setFamilies(f.families);
      font_->setItemData(font_->count() - 1, qf, Qt::FontRole);
    }
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
    fontSec->body()->addWidget(font_);
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
    connect(font_, &QComboBox::activated, this, [this, upd](int idx) {
      const FontId f = FontId(font_->itemData(idx).toInt());
      upd([&](TextItem& t) { t.font = f; });
    });
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
    {
      const QSignalBlocker b(font_);
      font_->setCurrentIndex(font_->findData(int(t->font)));
    }
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
  QComboBox* font_;
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
    fadeIn_ = new Segmented(fadeOptions({0, 1, 2, 3}), 0, fadeSec);
    fadeOut_ = new Segmented(fadeOptions({0, 1, 2, 3}), 0, fadeSec);
    fadeSec->body()->addWidget(fadeRow(QStringLiteral("イン"), fadeIn_, fadeSec));
    fadeSec->body()->addWidget(fadeRow(QStringLiteral("アウト"), fadeOut_, fadeSec));
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
    connect(fadeIn_, &Segmented::changed, this, [upd](const QVariant& x) { upd([&](AudioItem& a) { a.fadeIn = x.toDouble(); }); });
    connect(fadeOut_, &Segmented::changed, this, [upd](const QVariant& x) { upd([&](AudioItem& a) { a.fadeOut = x.toDouble(); }); });
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
  Segmented* fadeIn_;
  Segmented* fadeOut_;
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

void Inspector::onSelection() {
  const Selection& s = editor().selection();
  project_->setVisible(s.type == Selection::None);
  clip_->setVisible(s.type == Selection::Clip);
  text_->setVisible(s.type == Selection::Text);
  audio_->setVisible(s.type == Selection::Audio);
  if (s.type == Selection::Clip) {
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
  if (s.type == Selection::Clip) clip_->refresh();
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
