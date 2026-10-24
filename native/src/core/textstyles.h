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
/**
 * フォントを探す順のファミリー名。family（読み込んだフォント・同梱のフォント）が空でなければそれを先に、
 * 無いパソコンでは font（組み込みの種類）のフォントで描く
 */
QStringList fontFamilies(FontId font, const QString& family);

// ---- 文字の一部だけ別のフォントにする範囲（TextItem::fontSpans） ---------------------------------
// 範囲は文字列の位置（UTF-16）で持つ。文字を書き換えたら adjustFontSpans で付け直す（Editor::commit がまとめて行う）。

/** 範囲を文字の長さに収め、並べ直し、隣り合う同じフォントはまとめる。基本のフォントと同じ範囲は取り除く */
void normalizeFontSpans(TextItem& t);
/**
 * oldText を newText に書き換えたときの、範囲の付け直し。書き換えたのは 1 か所（前後の同じ部分を除いた所）とみなす。
 * 入れた文字は、置き換えた文字（なければ前の文字）のフォントを引き継ぐ
 */
void adjustFontSpans(const QString& oldText, const QString& newText, std::vector<FontSpan>& spans);
/** from〜to の文字のフォントを変える。基本のフォントと同じにした所は、範囲を取り除く（基本のフォントに従う） */
void setFontRange(TextItem& t, int from, int to, FontId font, const QString& family);
/** index の文字のフォント（範囲の外なら基本のフォント）。spanned には、範囲の中かを返す */
FontSpan fontAt(const TextItem& t, int index, bool* spanned = nullptr);
/** 2 つのフォントの指定が同じか（ファミリー名があればファミリー名で、無ければ組み込みの種類で比べる） */
bool sameFont(FontId a, const QString& familyA, FontId b, const QString& familyB);

/** スタイル一覧のカードをタイムラインへドラッグするときの MIME 形式（中身はスタイルの ID） */
inline constexpr const char* kTextPresetMime = "application/x-kantan-text-preset";

/** 追加したときの文字 */
QString defaultText();

/** スタイル一覧のカードに表示する見本の文字（全スタイル共通） */
QString presetSampleText();

struct TextPreset {
  QString id;
  QString label;
  /** id / text / start / track 以外を使う。duration は、自分で保存したテンプレートだけが使う（追加するときの長さ） */
  TextItem style;
};

/** 組み込みのスタイル */
const std::vector<TextPreset>& textPresets();
/** 組み込みのスタイルと、自分で保存したテンプレート（ID は "user:..."）。見つからなければ組み込みの最初のスタイル */
TextPreset textPreset(const QString& id);
/** そのスタイルで追加するときの長さ（テンプレートは保存したときの長さ、組み込みのスタイルは決まった長さ） */
double presetDuration(const QString& id);
TextItem makeText(const QString& presetId, double start, const QString& text = QString());
/** スタイルだけを当てはめる（文字・時間・位置・段・グループはそのまま。一部だけ変えたフォントは、スタイルのフォントにそろえる） */
TextItem applyTextPreset(const TextItem& item, const QString& presetId);

const std::vector<QColor>& colorSwatches();

}  // namespace kv
