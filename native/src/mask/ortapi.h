#pragma once
// ONNX Runtime（オブジェクトマスクの AI を動かす部品）。onnxruntime.dll は、使うときに初めて読み込む
// （無い・読めない PC でも、アプリのほかの機能はそのまま使える）。C の API だけを使う（MinGW でもそのまま呼べる）。

#include <QString>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <onnxruntime_c_api.h>

namespace kv::mask {

/** ONNX Runtime の失敗（メッセージは ONNX Runtime の英語のまま） */
class OrtFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/** ONNX Runtime を読み込む（2 回目からは前の結果）。読めなければ false と、利用者に見せる理由 */
bool loadOrt(QString* error = nullptr);
/** 読み込まずに、ONNX Runtime があるかだけを確かめる（一度読み込みを試したあとは、その結果） */
bool ortInstalled(QString* error = nullptr);

/** テンソル（ONNX Runtime の値）。動かすと元は空になる */
class OrtTensor {
 public:
  OrtTensor() = default;
  explicit OrtTensor(OrtValue* v) : v_(v) {}
  ~OrtTensor();
  OrtTensor(OrtTensor&& o) noexcept : v_(o.v_) { o.v_ = nullptr; }
  OrtTensor& operator=(OrtTensor&& o) noexcept;
  OrtTensor(const OrtTensor&) = delete;
  OrtTensor& operator=(const OrtTensor&) = delete;

  OrtValue* get() const { return v_; }
  const float* floats() const;
  std::vector<int64_t> shape() const;
  size_t count() const;

 private:
  OrtValue* v_ = nullptr;
};

/** 呼ぶ側のメモリをそのまま使う入力（data は run が終わるまで残しておく） */
OrtTensor tensorFloat(const float* data, const std::vector<int64_t>& shape);
OrtTensor tensorInt32(const int32_t* data, const std::vector<int64_t>& shape);
OrtTensor tensorBool(const bool* data, const std::vector<int64_t>& shape);

/** 1 つのモデル（.onnx）。run は別のスレッドから同時に呼んでもよい（スレッドはアプリで 1 つの組を分け合う） */
class OrtModel {
 public:
  /** 読み込めなければ OrtFailure */
  explicit OrtModel(const QString& path);
  ~OrtModel();
  OrtModel(const OrtModel&) = delete;
  OrtModel& operator=(const OrtModel&) = delete;

  std::vector<OrtTensor> run(const std::vector<const char*>& inputNames, const std::vector<const OrtTensor*>& inputs,
                             const std::vector<const char*>& outputNames) const;

 private:
  OrtSession* session_ = nullptr;
};

}  // namespace kv::mask
