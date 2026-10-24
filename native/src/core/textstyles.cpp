#include "core/textstyles.h"

#include <algorithm>

#include "core/texttemplates.h"
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

QStringList fontFamilies(FontId font, const QString& family) {
  QStringList out = fontFamilies(font);
  if (!family.isEmpty()) out.prepend(family);
  return out;
}

bool sameFont(FontId a, const QString& familyA, FontId b, const QString& familyB) {
  if (!familyA.isEmpty() || !familyB.isEmpty()) return familyA == familyB;
  return a == b;
}

// ---- 一部だけ別のフォントにする範囲 ----------------------------------------------------------------

namespace {
/** 1 文字ごとのフォント（set が false なら基本のフォント） */
struct CharFont {
  bool set = false;
  FontId font = FontId::Gothic;
  QString family;
};

std::vector<CharFont> expandSpans(const std::vector<FontSpan>& spans, int length) {
  std::vector<CharFont> out(size_t(std::max(0, length)));
  for (const auto& s : spans) {
    const int a = std::clamp(s.start, 0, length);
    const int b = std::clamp(s.start + s.length, a, length);
    for (int i = a; i < b; ++i) out[size_t(i)] = {true, s.font, s.family};
  }
  return out;
}

std::vector<FontSpan> compressChars(const std::vector<CharFont>& chars) {
  std::vector<FontSpan> out;
  for (int i = 0; i < int(chars.size()); ++i) {
    const CharFont& c = chars[size_t(i)];
    if (!c.set) continue;
    if (!out.empty() && out.back().start + out.back().length == i && out.back().font == c.font && out.back().family == c.family) {
      ++out.back().length;
    } else {
      out.push_back({i, 1, c.font, c.family});
    }
  }
  return out;
}
}  // namespace

void normalizeFontSpans(TextItem& t) {
  if (t.fontSpans.empty()) return;
  std::vector<FontSpan> sorted = t.fontSpans;
  // 重なっていたら後ろの範囲を優先する（並べ直す前の順のまま 1 文字ずつに広げる）
  std::vector<CharFont> chars = expandSpans(sorted, int(t.text.size()));
  for (auto& c : chars) {
    if (c.set && sameFont(c.font, c.family, t.font, t.fontFamily)) c.set = false;  // 基本のフォントと同じ
  }
  t.fontSpans = compressChars(chars);
}

void adjustFontSpans(const QString& oldText, const QString& newText, std::vector<FontSpan>& spans) {
  if (spans.empty() || oldText == newText) return;
  const int oldLen = int(oldText.size());
  const int newLen = int(newText.size());
  // 前と後ろの同じ部分を除いた所を、書き換えた所とみなす
  int p = 0;
  while (p < oldLen && p < newLen && oldText[p] == newText[p]) ++p;
  int s = 0;
  while (s < oldLen - p && s < newLen - p && oldText[oldLen - 1 - s] == newText[newLen - 1 - s]) ++s;
  const int oldEnd = oldLen - s;  // 置き換えられた所: old[p, oldEnd)
  const int inserted = newLen - s - p;
  const std::vector<CharFont> before = expandSpans(spans, oldLen);
  // 入れた文字のフォント: 置き換えた文字があればその最初の文字、なければ前の文字（先頭なら次の文字）
  CharFont carry;
  if (oldEnd > p) carry = before[size_t(p)];
  else if (p > 0) carry = before[size_t(p - 1)];
  else if (oldLen > 0) carry = before[0];
  std::vector<CharFont> after;
  after.reserve(size_t(newLen));
  for (int i = 0; i < p; ++i) after.push_back(before[size_t(i)]);
  for (int i = 0; i < inserted; ++i) after.push_back(carry);
  for (int i = oldEnd; i < oldLen; ++i) after.push_back(before[size_t(i)]);
  spans = compressChars(after);
}

void setFontRange(TextItem& t, int from, int to, FontId font, const QString& family) {
  const int len = int(t.text.size());
  from = std::clamp(from, 0, len);
  to = std::clamp(to, 0, len);
  if (from > to) std::swap(from, to);
  if (from == to) return;
  std::vector<CharFont> chars = expandSpans(t.fontSpans, len);
  const bool base = sameFont(font, family, t.font, t.fontFamily);
  for (int i = from; i < to; ++i) chars[size_t(i)] = base ? CharFont{} : CharFont{true, font, family};
  t.fontSpans = compressChars(chars);
}

FontSpan fontAt(const TextItem& t, int index, bool* spanned) {
  if (spanned) *spanned = false;
  for (const auto& s : t.fontSpans) {
    if (index >= s.start && index < s.start + s.length) {
      if (spanned) *spanned = true;
      return s;
    }
  }
  return {index, 1, t.font, t.fontFamily};
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

TextPreset textPreset(const QString& id) {
  if (const TextPreset* user = textTemplates().find(id)) return *user;
  for (const auto& p : textPresets()) {
    if (p.id == id) return p;
  }
  return textPresets().front();
}

double presetDuration(const QString& id) {
  if (const TextPreset* user = textTemplates().find(id)) return std::max(kMinTextDuration, user->style.duration);
  return kDefaultTextDuration;
}

TextItem makeText(const QString& presetId, double start, const QString& text) {
  TextItem t = textPreset(presetId).style;
  t.id = newId();
  t.text = text.isNull() ? defaultText() : text;
  t.start = std::max(0.0, start);
  t.duration = presetDuration(presetId);
  t.fontSpans.clear();
  t.group.clear();
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
  t.group = item.group;
  t.fontSpans.clear();
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
