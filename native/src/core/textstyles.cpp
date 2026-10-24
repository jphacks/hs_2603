#include "core/textstyles.h"

#include <algorithm>

#include "core/timeline.h"

namespace kv {

const std::vector<FontOption>& fontOptions() {
  // Windows に標準で入っているフォントを優先し、なければ近いものを使う
  static const std::vector<FontOption> fonts = {
      {FontId::Gothic, QStringLiteral("ゴシック"), {QStringLiteral("Yu Gothic UI"), QStringLiteral("Yu Gothic"), QStringLiteral("Meiryo")}},
      {FontId::Meiryo, QStringLiteral("メイリオ"), {QStringLiteral("Meiryo"), QStringLiteral("Yu Gothic UI")}},
      {FontId::Maru, QStringLiteral("丸ゴシック"), {QStringLiteral("HG丸ｺﾞｼｯｸM-PRO"), QStringLiteral("HGMaruGothicMPRO"), QStringLiteral("BIZ UDPGothic"), QStringLiteral("Meiryo")}},
      {FontId::Ud, QStringLiteral("UDゴシック"), {QStringLiteral("BIZ UDPGothic"), QStringLiteral("Yu Gothic UI"), QStringLiteral("Meiryo")}},
      {FontId::Mincho, QStringLiteral("明朝"), {QStringLiteral("Yu Mincho"), QStringLiteral("BIZ UDPMincho"), QStringLiteral("MS PMincho")}},
      {FontId::Kyokasho, QStringLiteral("教科書体"), {QStringLiteral("UD Digi Kyokasho NK-R"), QStringLiteral("UD デジタル 教科書体 NK-R"), QStringLiteral("UD Digi Kyokasho N-R"), QStringLiteral("Yu Gothic UI")}},
      {FontId::Impact, QStringLiteral("Impact"), {QStringLiteral("Impact"), QStringLiteral("Arial Black"), QStringLiteral("Yu Gothic UI")}},
  };
  return fonts;
}

QStringList fontFamilies(FontId id) {
  for (const auto& f : fontOptions()) {
    if (f.id == id) return f.families;
  }
  return fontOptions().front().families;
}

QString defaultText() {
  return QStringLiteral("テキスト");
}

QString presetSampleText() {
  return QStringLiteral("サンプル");
}

static TextItem base() {
  TextItem t;
  t.x = 0.5;
  t.y = 0.82;
  t.font = FontId::Gothic;
  t.size = 0.075;
  t.bold = true;
  t.color = QColor(0xff, 0xff, 0xff);
  t.strokeColor = QColor(0, 0, 0);
  t.strokeWidth = 0.12;
  t.boxColor = QColor(0, 0, 0);
  t.boxOpacity = 0;
  t.shadow = true;
  t.align = TextAlign::Center;
  t.animation = TextAnimation::Fade;
  return t;
}

const std::vector<TextPreset>& textPresets() {
  static const std::vector<TextPreset> presets = [] {
    std::vector<TextPreset> v;
    v.push_back({QStringLiteral("basic"), QStringLiteral("シンプル"), base()});
    {
      TextItem t = base();
      t.size = 0.085;
      t.strokeColor = QColor(0x11, 0x11, 0x11);
      t.strokeWidth = 0.22;
      t.shadow = false;
      v.push_back({QStringLiteral("bold"), QStringLiteral("太フチ"), t});
    }
    {
      TextItem t = base();
      t.size = 0.095;
      t.color = QColor(0xff, 0xe1, 0x4d);
      t.strokeColor = QColor(0xc2, 0x18, 0x5b);
      t.strokeWidth = 0.2;
      t.y = 0.78;
      t.animation = TextAnimation::SlideUp;
      v.push_back({QStringLiteral("variety"), QStringLiteral("バラエティ"), t});
    }
    {
      TextItem t = base();
      t.size = 0.058;
      t.bold = false;
      t.strokeWidth = 0;
      t.boxColor = QColor(0, 0, 0);
      t.boxOpacity = 0.62;
      t.shadow = false;
      t.y = 0.88;
      v.push_back({QStringLiteral("subtitle"), QStringLiteral("字幕"), t});
    }
    {
      TextItem t = base();
      t.font = FontId::Mincho;
      t.size = 0.13;
      t.strokeWidth = 0;
      t.shadow = true;
      t.y = 0.5;
      v.push_back({QStringLiteral("title"), QStringLiteral("タイトル"), t});
    }
    {
      TextItem t = base();
      t.font = FontId::Maru;
      t.size = 0.1;
      t.color = QColor(0xff, 0x5f, 0xa2);
      t.strokeColor = QColor(0xff, 0xff, 0xff);
      t.strokeWidth = 0.22;
      t.y = 0.5;
      t.animation = TextAnimation::SlideUp;
      v.push_back({QStringLiteral("pop"), QStringLiteral("ポップ"), t});
    }
    {
      TextItem t = base();
      t.font = FontId::Kyokasho;
      t.size = 0.064;
      t.bold = false;
      t.color = QColor(0x2b, 0x2b, 0x2b);
      t.strokeWidth = 0;
      t.boxColor = QColor(0xff, 0xf6, 0xd5);
      t.boxOpacity = 0.95;
      t.shadow = true;
      t.y = 0.16;
      v.push_back({QStringLiteral("note"), QStringLiteral("メモ"), t});
    }
    {
      TextItem t = base();
      t.font = FontId::Impact;
      t.size = 0.14;
      t.bold = false;
      t.color = QColor(0xff, 0xff, 0xff);
      t.strokeColor = QColor(0xff, 0x3b, 0x30);
      t.strokeWidth = 0.1;
      t.y = 0.5;
      v.push_back({QStringLiteral("impact"), QStringLiteral("インパクト"), t});
    }
    return v;
  }();
  return presets;
}

const TextPreset& textPreset(const QString& id) {
  for (const auto& p : textPresets()) {
    if (p.id == id) return p;
  }
  return textPresets().front();
}

TextItem makeText(const QString& presetId, double start, const QString& text) {
  TextItem t = textPreset(presetId).style;
  t.id = newId();
  t.text = text.isNull() ? defaultText() : text;
  t.start = std::max(0.0, start);
  t.duration = kDefaultTextDuration;
  return t;
}

TextItem applyTextPreset(const TextItem& item, const QString& presetId) {
  TextItem t = textPreset(presetId).style;
  t.id = item.id;
  t.text = item.text;
  t.start = item.start;
  t.duration = item.duration;
  t.track = item.track;
  t.x = item.x;
  t.y = item.y;
  return t;
}

const std::vector<QColor>& colorSwatches() {
  static const std::vector<QColor> swatches = {
      QColor(0xff, 0xff, 0xff), QColor(0x00, 0x00, 0x00), QColor(0xff, 0xe1, 0x4d), QColor(0xff, 0x5f, 0xa2),
      QColor(0xff, 0x3b, 0x30), QColor(0xff, 0x95, 0x00), QColor(0x34, 0xc7, 0x59), QColor(0x30, 0xb0, 0xff),
      QColor(0x58, 0x56, 0xd6), QColor(0x8e, 0x8e, 0x93)};
  return swatches;
}

}  // namespace kv
