#include "core/model.h"

#include <QUuid>

namespace kv {

QString newId() {
  return QUuid::createUuid().toString(QUuid::Id128).left(12);
}

QString assetKindToString(AssetKind k) {
  switch (k) {
    case AssetKind::Audio: return QStringLiteral("audio");
    case AssetKind::Image: return QStringLiteral("image");
    default: return QStringLiteral("video");
  }
}

AssetKind assetKindFromString(const QString& s) {
  if (s == u"audio") return AssetKind::Audio;
  if (s == u"image") return AssetKind::Image;
  return AssetKind::Video;
}

QString fitToString(FitMode f) {
  switch (f) {
    case FitMode::Cover: return QStringLiteral("cover");
    case FitMode::Blur: return QStringLiteral("blur");
    default: return QStringLiteral("contain");
  }
}

FitMode fitFromString(const QString& s) {
  if (s == u"cover") return FitMode::Cover;
  if (s == u"blur") return FitMode::Blur;
  return FitMode::Contain;
}

static const char* const kPresetIds[] = {"none", "vivid", "warm", "cool", "soft", "drama", "retro", "sepia", "mono"};

QString presetToString(FilterPreset p) {
  return QString::fromLatin1(kPresetIds[static_cast<int>(p)]);
}

FilterPreset presetFromString(const QString& s) {
  for (int i = 0; i < 9; ++i) {
    if (s == QLatin1String(kPresetIds[i])) return static_cast<FilterPreset>(i);
  }
  return FilterPreset::None;
}

QString animationToString(TextAnimation a) {
  switch (a) {
    case TextAnimation::None: return QStringLiteral("none");
    case TextAnimation::SlideUp: return QStringLiteral("slideUp");
    default: return QStringLiteral("fade");
  }
}

TextAnimation animationFromString(const QString& s) {
  if (s == u"none") return TextAnimation::None;
  if (s == u"slideUp") return TextAnimation::SlideUp;
  return TextAnimation::Fade;
}

static const char* const kFontIds[] = {"gothic", "maru", "mincho", "ud", "kyokasho", "impact", "meiryo"};

QString fontToString(FontId f) {
  return QString::fromLatin1(kFontIds[static_cast<int>(f)]);
}

FontId fontFromString(const QString& s) {
  for (int i = 0; i < 7; ++i) {
    if (s == QLatin1String(kFontIds[i])) return static_cast<FontId>(i);
  }
  return FontId::Gothic;
}

QString alignToString(TextAlign a) {
  switch (a) {
    case TextAlign::Left: return QStringLiteral("left");
    case TextAlign::Right: return QStringLiteral("right");
    default: return QStringLiteral("center");
  }
}

TextAlign alignFromString(const QString& s) {
  if (s == u"left") return TextAlign::Left;
  if (s == u"right") return TextAlign::Right;
  return TextAlign::Center;
}

QString aspectToString(Aspect a) {
  switch (a) {
    case Aspect::R9_16: return QStringLiteral("9:16");
    case Aspect::R1_1: return QStringLiteral("1:1");
    case Aspect::R4_5: return QStringLiteral("4:5");
    default: return QStringLiteral("16:9");
  }
}

Aspect aspectFromString(const QString& s) {
  if (s == u"9:16") return Aspect::R9_16;
  if (s == u"1:1") return Aspect::R1_1;
  if (s == u"4:5") return Aspect::R4_5;
  return Aspect::R16_9;
}

QString easingToString(Easing e) {
  switch (e) {
    case Easing::EaseIn: return QStringLiteral("easeIn");
    case Easing::EaseOut: return QStringLiteral("easeOut");
    case Easing::EaseInOut: return QStringLiteral("easeInOut");
    case Easing::Hold: return QStringLiteral("hold");
    default: return QStringLiteral("linear");
  }
}

Easing easingFromString(const QString& s) {
  if (s == u"easeIn") return Easing::EaseIn;
  if (s == u"easeOut") return Easing::EaseOut;
  if (s == u"easeInOut") return Easing::EaseInOut;
  if (s == u"hold") return Easing::Hold;
  return Easing::Linear;
}

static const char* const kPropIds[kAnimPropCount] = {"x", "y", "scale", "rotation", "opacity"};

QString propToString(AnimProp p) {
  return QString::fromLatin1(kPropIds[int(p)]);
}

bool propFromString(const QString& s, AnimProp* out) {
  for (int i = 0; i < kAnimPropCount; ++i) {
    if (s == QLatin1String(kPropIds[i])) {
      *out = static_cast<AnimProp>(i);
      return true;
    }
  }
  return false;
}

}  // namespace kv
