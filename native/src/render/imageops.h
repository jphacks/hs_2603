#pragma once

#include <QImage>
#include <QPainter>
#include <QPolygonF>
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

/**
 * 1 枚のフレームを画面サイズに合わせて配置する（全体表示 / 拡大 / ぼかし背景）。
 * keep は残す部分（素材に対する割合。切り抜き）で、残した部分は切り抜く前と同じ場所・大きさに描く（ほかは黒）
 */
QImage composeClipLayer(const QImage& frame, FitMode fit, QSize canvas, const QRectF& keep = QRectF(0, 0, 1, 1));

/**
 * 重ねるトラックの映像・画像を、painter に直接描く（全体表示の大きさに収めて、keep の部分だけを、s の位置・拡大・回転・伸縮で）。
 * 不透明度は painter 側で掛ける。frame は透明を含んでよい（クロマキーのあと）
 */
void drawClipMedia(QPainter& p, const QImage& frame, const QRectF& keep, const LayerState& s, QSize canvas);
/** drawClipMedia で描く範囲の四角（基準座標などの canvas の座標。左上・右上・右下・左下の順） */
QPolygonF clipMediaQuad(QSizeF frame, const QRectF& keep, const LayerState& s, QSizeF canvas);

/** クロマキーの色かぶりを抑える色の種類（0: 抑えない / 1: 緑 / 2: 青）。キーの色の、いちばん強い色で決める */
int despillType(const QColor& key);
/** クロマキーを掛けた画像（ARGB32。キーの色に近い部分ほど透明。色かぶりの抑えも掛ける） */
QImage chromaKeyImage(const QImage& src, const ChromaKey& k);

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
