#pragma once
// 字幕ファイル（SRT・WebVTT）の読み書き。読んだ字幕は、テキストとしてタイムラインに並べる（Editor::importSubtitles）

#include <QByteArray>
#include <QString>
#include <vector>

namespace kv {

struct SubtitleCue {
  /** タイムラインの秒数 */
  double start = 0;
  double end = 0;
  /** 字幕の文字（改行は \n。<i> などの書式の印は外したもの） */
  QString text;
};

/**
 * SRT / WebVTT の中身を読む（文字コードは UTF-8・UTF-16（BOM 付き）・Windows の文字コード（Shift_JIS など）を見分ける）。
 * 時刻の順に並べて返す。読めた字幕が無ければ空（error に理由）
 */
std::vector<SubtitleCue> parseSubtitles(const QByteArray& data, QString* error = nullptr);
/** 字幕を SRT（vtt なら WebVTT）の中身にする（UTF-8） */
QByteArray writeSubtitles(const std::vector<SubtitleCue>& cues, bool vtt = false);

}  // namespace kv
