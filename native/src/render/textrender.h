#pragma once
// テキストの描画。プレビューと書き出し(PNG)で同じ関数を使う。
// 配置はいつも基準解像度 (designSize) の座標で計算し、描くときに拡大縮小する。

#include <QFont>
#include <QImage>
#include <QLineF>
#include <QList>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QStringList>
#include <optional>

#include "core/model.h"
#include "core/timeline.h"

namespace kv {

struct TextLayout {
  QFont font;
  double fontPx = 0;
  QStringList lines;
  /**
   * 各行の先頭が、元の文字列（item.text）の何文字目（UTF-16 の位置）か。
   * 行の文字は item.text.mid(lineStart[i], lines[i].size()) と同じ（折り返しで捨てた空白と改行は、行に入らない）
   */
  QList<int> lineStart;
  QList<double> lineWidths;
  double lineHeight = 0;
  double ascent = 0;
  double descent = 0;
  double blockW = 0;
  double blockH = 0;
  double cx = 0;
  double cy = 0;
  std::optional<QRectF> box;
  /** 縁取り・帯を含めた範囲（クリック判定用） */
  QRectF bounds;
};

QFont textFont(const TextItem& item, int pixelSize);
TextLayout layoutText(const TextItem& item, double W, double H);
/** painter は基準座標で描けるように変換しておく。pixelScale は基準座標 → 実ピクセルの倍率 */
void drawTextLayout(QPainter& p, const TextItem& item, const TextLayout& L, double pixelScale);

// ---- 入力位置（プレビュー上で文字を直接編集するときに使う） ----------------------------------------
// index は item.text の位置（UTF-16。0〜text.size()）。座標は layoutText と同じ基準解像度で、描画と同じ配置・字間・揃えを使う。
// upstream は、折り返しの境目（行末と次の行頭が同じ位置）で、入力位置を上の行の末尾に置くか（true）、次の行の先頭に置くか（false）。

/** 入力位置の縦線（上端から下端） */
QLineF textCaretLine(const TextItem& item, const TextLayout& L, int index, bool upstream = false);
/** 点 p にいちばん近い入力位置。折り返しの境目で行末に置くときは upstream に true を返す */
int textIndexAt(const TextItem& item, const TextLayout& L, QPointF p, bool* upstream = nullptr);
/** from〜to（選択範囲）を行ごとに囲む四角。改行を含む行は、右に少し広げる */
QList<QRectF> textRangeRects(const TextItem& item, const TextLayout& L, int from, int to);
/** 上（dir = -1）/ 下（+1）の行の、x にいちばん近い入力位置。先頭より上は 0、末尾より下は文字列の終わり */
int textMoveVertical(const TextItem& item, const TextLayout& L, int index, bool upstream, int dir, double x, bool* upstreamOut = nullptr);
/** 入力位置のある行の先頭（end = false）/ 末尾（true） */
int textLineEdge(const TextItem& item, const TextLayout& L, int index, bool upstream, bool end, bool* upstreamOut = nullptr);

/** 一時停止中に選んでいるテキストは、アニメーション中でもはっきり表示する */
AnimState previewTextState(const TextItem& item, double t, const QString& editingId);

struct TextPreset;

/**
 * スタイル一覧用のカード画像。全スタイルで同じ見本の文字を、同じ大きさのカードの中央に描く。
 * 文字の大きさは共通にし、はみ出すスタイルだけ枠に収まるまで縮める。
 */
QPixmap renderTextPresetCard(const TextPreset& preset, QSize cardSize, qreal dpr);

struct TextSprite {
  QString key;
  QImage image;
  QPointF pos;  // 画面上の左上（ピクセル）
};

QString textSpriteKey(const TextItem& item, QSize canvas);
TextSprite renderTextSprite(const TextItem& item, QSize design, QSize canvas);
/** 書き出し用: 出力解像度の透過画像 */
QImage renderTextFrame(const TextItem& item, QSize design, QSize out);

}  // namespace kv
