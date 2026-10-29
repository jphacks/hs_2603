#pragma once
// EdgeTAM（動画の中の被写体を見つけて追いかける AI。Meta の SAM 2 を軽くしたもの。Apache License 2.0）を動かす。
// 4 つのグラフ（画像の特徴・マスク・記憶の照らし合わせ・記憶づくり）を、transformers の EdgeTamVideoModel と同じ順で呼ぶ
// （PyTorch の結果と、全フレームで同じマスクになることを確かめた。.agents/tasks/object-mask/plan.md）。

#include <QImage>
#include <QString>
#include <map>
#include <memory>
#include <vector>

namespace kv::mask {

class OrtModel;

/** 入力の画像の大きさ（縦横とも。素材のフレームを、縦横比を無視してこの大きさにする） */
constexpr int kModelSize = 1024;
/** マスク（ロジット）の大きさ（縦横とも。素材のフレーム全体に対応する） */
constexpr int kMaskSize = 256;

/** 1 フレームの画像の特徴（[32,256,256]・[64,128,128]・[256,64,64]） */
struct ImageFeatures {
  std::vector<float> f0, f1, f2;
  bool empty() const { return f2.empty(); }
};

/** クリックした点。x・y は素材のフレームに対する割合（0〜1。反転の前）。positive が false なら「ここは含めない」 */
struct PromptPoint {
  double x = 0.5;
  double y = 0.5;
  bool positive = true;
};

/** マスクを作った結果 */
struct Decoded {
  /** マスクのロジット [256,256]（0 より大きい所が被写体。写っていなければ全体が -1024） */
  std::vector<float> lowRes;
  /** [1024,1024] のロジット（記憶づくりに使う） */
  std::vector<float> highRes;
  /** 被写体を表す値 [256]（次のフレームで、同じものを探す手がかり） */
  std::vector<float> pointer;
  /** 被写体が写っているか（0 より大きければ写っている） */
  float score = 0;
};

/** 1 フレーム分の記憶（[512,64] のトークンと、その位置） */
struct Memory {
  std::vector<float> tokens;
  std::vector<float> pos;
};

class EdgeTam {
 public:
  /** モデル（アプリの models\edgetam）を読み込む（初めの 1 回だけ。数秒かかる）。読めなければ nullptr と理由 */
  static EdgeTam* shared(QString* error = nullptr);
  /**
   * モデルのファイルと AI の部品がそろっているか（モデルは読み込まない）。load なら ONNX Runtime を読み込んで確かめる
   * （false なら、ファイルがあるかだけ。画面の表示に使う）
   */
  static bool filesPresent(QString* error = nullptr, bool load = true);
  ~EdgeTam();

  /** 画像を 1024×1024 の入力にする（縦横比は無視。ImageNet の平均・標準偏差で正規化した CHW） */
  static std::vector<float> pixelsFrom(const QImage& image);
  ImageFeatures encode(const std::vector<float>& pixels) const;
  /** 記憶を使わないフレーム（最初にクリックしたフレーム）の、マスクに渡す特徴 */
  std::vector<float> withoutMemory(const ImageFeatures& f) const;
  /**
   * 記憶と照らし合わせた特徴（マスクに渡す [256,64,64]）。spatial は記憶と、その時間の位置の行（0〜6）。
   * pointers は被写体を表す値（[256] ずつ）
   */
  std::vector<float> attend(const ImageFeatures& f, const std::vector<std::pair<const Memory*, int>>& spatial,
                            const std::vector<const std::vector<float>*>& pointers) const;
  /** マスクを作る。points が空なら、記憶だけで追う（追いかけるフレーム） */
  Decoded decode(const ImageFeatures& f, const std::vector<float>& pix, const std::vector<PromptPoint>& points) const;
  /** このフレームの記憶を作る。fromPoints はクリックした点から作ったマスクか（そのときはマスクを 0/1 にしてから使う） */
  Memory remember(const ImageFeatures& f, const Decoded& d, bool fromPoints) const;

 private:
  EdgeTam() = default;
  std::unique_ptr<OrtModel> vision_, decoder_, attention_, memory_;
  /** 一番粗い特徴の位置 [4096,256]（どの画像でも同じなので、初めの 1 回で覚える） */
  mutable std::vector<float> pos2_;
};

/**
 * 1 つの被写体を追いかける記憶（SAM 2 の memory bank）。フレームは番号（クリックしたフレームが 0、後ろが +、前が -）で呼ぶ。
 * 同じ Tracker を、後ろへ追ったあとで前へ追うのに続けて使う（SAM 2 と同じく、前へ追うときの初めの数フレームは、後ろへ追った結果も記憶に使う）
 */
class Tracker {
 public:
  explicit Tracker(const EdgeTam& model) : m_(model) {}

  struct Result {
    std::vector<float> lowRes;
    float score = 0;
  };
  /** クリックしたフレーム。initial なら最初のクリック（記憶を使わない）、そうでなければ追いかけている途中の直し */
  Result prompt(int index, const ImageFeatures& f, const std::vector<PromptPoint>& points, bool initial, bool reverse);
  /** 記憶だけで追いかけるフレーム */
  Result track(int index, const ImageFeatures& f, bool reverse);
  /** 使わなくなった記憶を手放す（index の近くと、クリックしたフレームの近くだけ残す） */
  void forget(int index, bool reverse);
  bool hasPrompt() const { return !cond_.empty(); }

 private:
  struct Out {
    Memory mem;
    std::vector<float> pointer;
  };
  std::vector<float> conditioned(int index, const ImageFeatures& f, bool reverse) const;
  Result store(int index, const ImageFeatures& f, Decoded d, bool fromPoints, bool isCond);

  const EdgeTam& m_;
  std::map<int, Out> cond_;
  std::map<int, Out> noncond_;
};

}  // namespace kv::mask
