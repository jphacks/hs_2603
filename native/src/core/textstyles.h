#pragma once

#include <QColor>
#include <QFont>
#include <QString>
#include <QStringList>
#include <vector>

#include "core/model.h"

namespace kv {

struct FontOption {
  FontId id;
  QString label;
  QStringList families;
};

const std::vector<FontOption>& fontOptions();
QStringList fontFamilies(FontId id);

/** スタイル一覧のカードをタイムラインへドラッグするときの MIME 形式（中身はスタイルの ID） */
inline constexpr const char* kTextPresetMime = "application/x-kantan-text-preset";

/** 追加したときの文字 */
QString defaultText();

/** スタイル一覧のカードに表示する見本の文字（全スタイル共通） */
QString presetSampleText();

struct TextPreset {
  QString id;
  QString label;
  TextItem style;  // id / text / start / duration 以外を使う
};

const std::vector<TextPreset>& textPresets();
const TextPreset& textPreset(const QString& id);
TextItem makeText(const QString& presetId, double start, const QString& text = QString());
/** スタイルだけを当てはめる（文字・時間・位置はそのまま） */
TextItem applyTextPreset(const TextItem& item, const QString& presetId);

const std::vector<QColor>& colorSwatches();

}  // namespace kv
