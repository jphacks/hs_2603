#pragma once
// 静止画（フレームの書き出し・フレームの保持）に使う 1 枚の画面。
// 合成はプレビューと同じ（render/compositor）。素材は縮めずに読む（写真は読み込み用に整えた画像、動画は元の大きさのフレーム）。

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <functional>

namespace kv {

/**
 * 今の編集場所の、タイムラインの時刻 t の画面を size の大きさで作る（動画のデコードと合成は別のスレッド）。
 * 終わったら done を UI のスレッドで呼ぶ（作れなければ空の画像）。context が先に消えたら呼ばない
 */
void renderStillFrame(double t, QSize size, QObject* context, std::function<void(QImage)> done);

/**
 * 動画 path の、素材の時刻 srcT のフレームを、元の大きさ（回転・画素の縦横比を直したもの）で読む（別のスレッド）。
 * 終わったら done を UI のスレッドで呼ぶ（読めなければ空の画像）
 */
void decodeSourceFrame(const QString& path, double srcT, QObject* context, std::function<void(QImage)> done);
/** decodeSourceFrame と同じ。読んだフレームの素材の時刻も渡す（オブジェクトマスクを、同じフレームのものにするため） */
void decodeSourceFrameAt(const QString& path, double srcT, QObject* context, std::function<void(QImage, double)> done);

}  // namespace kv
