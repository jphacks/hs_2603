#pragma once
// テキストの描画。プレビューと書き出し(PNG)で同じ関数を使う。
// 配置はいつも基準解像度 (designSize) の座標で計算し、描くときに拡大縮小する。

#include <QFont>
#include <QImage>
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
