#pragma once

#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QSize>
#include <QTransform>

#include "core/model.h"
#include "core/timeline.h"

namespace kv {

/** RGB32 の画像に箱型ぼかしを passes 回かける（ffmpeg boxblur と同じ考え方） */
void boxBlurRgb(QImage& img, int radius, int passes);
/** Alpha8 の画像にガウスぼかし（箱型 3 回で近似） */
void gaussianBlurAlpha(QImage& img, double sigma);

QRectF containRect(QSizeF src, QSizeF dst);
QRectF coverRect(QSizeF src, QSizeF dst);

/** 1 枚のフレームを画面サイズに合わせて配置する（全体表示 / 拡大 / ぼかし背景） */
QImage composeClipLayer(const QImage& frame, FitMode fit, QSize canvas);

/**
 * 画面いっぱいのレイヤー（映像・文字の画像）を動かす変換: レイヤーの中心を (x*W, y*H) に置き、
 * そこを中心に回転（度。時計回りが正）・拡大する。extra は画面上のずらし（ピクセル）
 */
QTransform layerMatrix(const LayerState& s, QSizeF canvas, QPointF extra = QPointF());
/** 映像のレイヤーを動かして、黒い背景の上に描く（不透明度が低いほど黒へ近づく）。レイヤーは画面と同じ大きさ */
QImage transformLayer(const QImage& layer, const LayerState& s);
/** 文字などの画像（画面上の左上が pos）を、上の変換で painter に描く。不透明度は painter 側で掛ける */
void drawLayer(QPainter& p, const QImage& image, QPointF pos, const LayerState& s, QPointF extra, QSize canvas);

}  // namespace kv
