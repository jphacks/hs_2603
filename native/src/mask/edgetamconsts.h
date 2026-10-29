#pragma once
// EdgeTAM の追跡で使う定数のうち、ONNX のグラフに入っていないもの（edgetamconsts.cpp）

namespace kv::mask {

/** 記憶を使わないフレーム（クリックした最初のフレーム）の特徴に足す値 [256] */
extern const float kNoMemoryEmbedding[256];
/** 記憶の時間の位置（行 r-1 が r フレーム前の記憶、行 6 がクリックしたフレームの記憶） [7][64] */
extern const float kMemoryTemporalPositionalEncoding[7][64];

}  // namespace kv::mask
