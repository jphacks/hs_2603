#include "mask/edgetam.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "mask/edgetamconsts.h"
#include "mask/ortapi.h"

namespace kv::mask {

namespace {

constexpr int kFeat = 64;  // 一番粗い特徴の縦横（64×64 = 4096）
constexpr int kTokens = kFeat * kFeat;
constexpr int kHidden = 256;
constexpr int kMemDim = 64;
constexpr int kMemTokens = 512;
/** 被写体が写っていないフレームのマスクの値（transformers の NO_OBJ_SCORE） */
constexpr float kNoObject = -1024.0f;

QString modelDir() {
  return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models/edgetam"));
}

const char* const kFiles[] = {"vision_encoder.onnx", "mask_decoder.onnx", "memory_attention.onnx", "memory_encoder.onnx"};

std::vector<float> copyOut(const OrtTensor& t) {
  const float* p = t.floats();
  return std::vector<float>(p, p + t.count());
}

/** [rows, cols] → [cols, rows] */
std::vector<float> transpose(const float* src, int rows, int cols) {
  std::vector<float> out(size_t(rows) * cols);
  for (int r = 0; r < rows; ++r) {
    const float* s = src + size_t(r) * cols;
    for (int c = 0; c < cols; ++c) out[size_t(c) * rows + r] = s[c];
  }
  return out;
}

}  // namespace

bool EdgeTam::filesPresent(QString* error, bool load) {
  for (const char* f : kFiles) {
    if (!QFileInfo::exists(QDir(modelDir()).filePath(QString::fromLatin1(f)))) {
      if (error) *error = QStringLiteral("AI のモデル（models\\edgetam）が見つかりません。アプリを入れ直してください");
      return false;
    }
  }
  return load ? loadOrt(error) : ortInstalled(error);
}

EdgeTam* EdgeTam::shared(QString* error) {
  static std::mutex mutex;
  static std::unique_ptr<EdgeTam> instance;
  static QString failed;
  std::lock_guard<std::mutex> lock(mutex);
  if (instance) return instance.get();
  if (failed.isEmpty()) {
    QString why;
    if (!filesPresent(&why)) failed = why.isEmpty() ? QStringLiteral("AI を使えません") : why;
  }
  if (failed.isEmpty()) {
    try {
      std::unique_ptr<EdgeTam> m(new EdgeTam());
      const QDir dir(modelDir());
      m->vision_ = std::make_unique<OrtModel>(dir.filePath(QString::fromLatin1(kFiles[0])));
      m->decoder_ = std::make_unique<OrtModel>(dir.filePath(QString::fromLatin1(kFiles[1])));
      m->attention_ = std::make_unique<OrtModel>(dir.filePath(QString::fromLatin1(kFiles[2])));
      m->memory_ = std::make_unique<OrtModel>(dir.filePath(QString::fromLatin1(kFiles[3])));
      instance = std::move(m);
      return instance.get();
    } catch (const std::exception& e) {
      failed = QStringLiteral("AI のモデルを読み込めません: %1").arg(QString::fromUtf8(e.what()));
    }
  }
  if (error) *error = failed;
  return nullptr;
}

EdgeTam::~EdgeTam() = default;

std::vector<float> EdgeTam::pixelsFrom(const QImage& image) {
  QImage img = image.size() == QSize(kModelSize, kModelSize) ? image : image.scaled(kModelSize, kModelSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  if (img.format() != QImage::Format_RGB32 && img.format() != QImage::Format_ARGB32) img = img.convertToFormat(QImage::Format_RGB32);
  static const float mean[3] = {0.485f, 0.456f, 0.406f};
  static const float stdev[3] = {0.229f, 0.224f, 0.225f};
  const size_t plane = size_t(kModelSize) * kModelSize;
  std::vector<float> out(plane * 3);
  float* r = out.data();
  float* g = r + plane;
  float* b = g + plane;
  const float kr = 1.0f / (255.0f * stdev[0]), kg = 1.0f / (255.0f * stdev[1]), kb = 1.0f / (255.0f * stdev[2]);
  const float br = mean[0] / stdev[0], bg = mean[1] / stdev[1], bb = mean[2] / stdev[2];
  for (int y = 0; y < kModelSize; ++y) {
    const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
    const size_t o = size_t(y) * kModelSize;
    for (int x = 0; x < kModelSize; ++x) {
      const QRgb p = line[x];
      r[o + x] = qRed(p) * kr - br;
      g[o + x] = qGreen(p) * kg - bg;
      b[o + x] = qBlue(p) * kb - bb;
    }
  }
  return out;
}

ImageFeatures EdgeTam::encode(const std::vector<float>& pixels) const {
  const OrtTensor in = tensorFloat(pixels.data(), {1, 3, kModelSize, kModelSize});
  std::vector<OrtTensor> out = vision_->run({"pixel_values"}, {&in}, {"feats0", "feats1", "feats2", "pos2"});
  ImageFeatures f;
  f.f0 = copyOut(out[0]);
  f.f1 = copyOut(out[1]);
  f.f2 = copyOut(out[2]);
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  if (pos2_.empty()) pos2_ = transpose(out[3].floats(), kHidden, kTokens);  // [256,4096] → [4096,256]
  return f;
}

std::vector<float> EdgeTam::withoutMemory(const ImageFeatures& f) const {
  std::vector<float> pix = f.f2;
  for (int c = 0; c < kHidden; ++c) {
    float* p = pix.data() + size_t(c) * kTokens;
    const float add = kNoMemoryEmbedding[c];
    for (int i = 0; i < kTokens; ++i) p[i] += add;
  }
  return pix;
}

std::vector<float> EdgeTam::attend(const ImageFeatures& f, const std::vector<std::pair<const Memory*, int>>& spatial,
                                   const std::vector<const std::vector<float>*>& pointers) const {
  const size_t blockSize = size_t(kMemTokens) * kMemDim;
  std::vector<float> mem(spatial.size() * blockSize);
  std::vector<float> memPos(spatial.size() * blockSize);
  for (size_t b = 0; b < spatial.size(); ++b) {
    const Memory& m = *spatial[b].first;
    const float* tpos = kMemoryTemporalPositionalEncoding[std::clamp(spatial[b].second, 0, 6)];
    std::memcpy(mem.data() + b * blockSize, m.tokens.data(), blockSize * sizeof(float));
    float* dst = memPos.data() + b * blockSize;
    for (int t = 0; t < kMemTokens; ++t) {
      const float* src = m.pos.data() + size_t(t) * kMemDim;
      for (int c = 0; c < kMemDim; ++c) dst[size_t(t) * kMemDim + c] = src[c] + tpos[c];
    }
  }
  // 被写体を表す値（[256]）は、64 ずつの 4 つのトークンに分ける。位置は 0（EdgeTAM はポインターに時間の位置を使わない）
  std::vector<float> ptr(pointers.size() * kHidden);
  for (size_t i = 0; i < pointers.size(); ++i) std::memcpy(ptr.data() + i * kHidden, pointers[i]->data(), kHidden * sizeof(float));
  const std::vector<float> ptrPos(ptr.size(), 0.0f);
  const std::vector<float> cur = transpose(f.f2.data(), kHidden, kTokens);  // [256,4096] → [4096,256]
  std::vector<float> pos;
  {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    pos = pos2_;
  }
  const int64_t spatialLen = int64_t(spatial.size()) * kMemTokens;
  const int64_t pointerLen = int64_t(pointers.size()) * (kHidden / kMemDim);
  const OrtTensor tCur = tensorFloat(cur.data(), {kTokens, 1, kHidden});
  const OrtTensor tPos = tensorFloat(pos.data(), {kTokens, 1, kHidden});
  const OrtTensor tMem = tensorFloat(mem.data(), {spatialLen, 1, kMemDim});
  const OrtTensor tMemPos = tensorFloat(memPos.data(), {spatialLen, 1, kMemDim});
  const OrtTensor tPtr = tensorFloat(ptr.data(), {pointerLen, 1, kMemDim});
  const OrtTensor tPtrPos = tensorFloat(ptrPos.data(), {pointerLen, 1, kMemDim});
  std::vector<OrtTensor> out = attention_->run(
      {"current_vision_features", "current_vision_position_embeddings", "spatial_memory", "spatial_memory_position_embeddings", "pointer_memory",
       "pointer_memory_position_embeddings"},
      {&tCur, &tPos, &tMem, &tMemPos, &tPtr, &tPtrPos}, {"conditioned_features"});
  // [1,1,4096,256] → [256,4096]（マスクに渡す [1,256,64,64]）
  return transpose(out[0].floats(), kTokens, kHidden);
}

Decoded EdgeTam::decode(const ImageFeatures& f, const std::vector<float>& pix, const std::vector<PromptPoint>& points) const {
  std::vector<float> pts;
  std::vector<int32_t> labels;
  if (points.empty()) {
    // 点が無い（記憶だけで追う）: 何も指さない点を 1 つ（ラベル -1）
    pts = {0.0f, 0.0f};
    labels = {-1};
  } else {
    for (const PromptPoint& p : points) {
      pts.push_back(float(std::clamp(p.x, 0.0, 1.0) * kModelSize));
      pts.push_back(float(std::clamp(p.y, 0.0, 1.0) * kModelSize));
      labels.push_back(p.positive ? 1 : 0);
    }
  }
  const int64_t n = int64_t(labels.size());
  const OrtTensor t0 = tensorFloat(f.f0.data(), {1, 32, 256, 256});
  const OrtTensor t1 = tensorFloat(f.f1.data(), {1, 64, 128, 128});
  const OrtTensor t2 = tensorFloat(pix.data(), {1, kHidden, kFeat, kFeat});
  const OrtTensor tp = tensorFloat(pts.data(), {1, 1, n, 2});
  const OrtTensor tl = tensorInt32(labels.data(), {1, 1, n});
  std::vector<OrtTensor> out = decoder_->run({"feats0", "feats1", "feats2", "input_points", "input_labels"}, {&t0, &t1, &t2, &tp, &tl},
                                             {"pred_masks", "high_res_masks", "object_pointer", "object_score_logits"});
  Decoded d;
  d.lowRes = copyOut(out[0]);
  d.highRes = copyOut(out[1]);
  d.pointer = copyOut(out[2]);
  d.score = out[3].count() > 0 ? out[3].floats()[0] : 0.0f;
  d.lowRes.resize(size_t(kMaskSize) * kMaskSize, kNoObject);
  d.highRes.resize(size_t(kModelSize) * kModelSize, kNoObject);
  d.pointer.resize(kHidden, 0.0f);
  // 被写体が写っていないフレームは、マスクを空にする（transformers と同じ。記憶にも「写っていない」として入る）
  if (!(d.score > 0)) {
    std::fill(d.lowRes.begin(), d.lowRes.end(), kNoObject);
    std::fill(d.highRes.begin(), d.highRes.end(), kNoObject);
  }
  return d;
}

Memory EdgeTam::remember(const ImageFeatures& f, const Decoded& d, bool fromPoints) const {
  const bool flag[1] = {fromPoints};
  const OrtTensor tf = tensorFloat(f.f2.data(), {1, kHidden, kFeat, kFeat});
  const OrtTensor tm = tensorFloat(d.highRes.data(), {1, 1, kModelSize, kModelSize});
  const OrtTensor tb = tensorBool(flag, {1});
  std::vector<OrtTensor> out = memory_->run({"vision_features", "pred_masks_high_res", "is_mask_from_pts"}, {&tf, &tm, &tb},
                                            {"memory_tokens", "memory_pos_enc"});
  Memory m;
  m.tokens = copyOut(out[0]);
  m.pos = copyOut(out[1]);
  m.tokens.resize(size_t(kMemTokens) * kMemDim, 0.0f);
  m.pos.resize(size_t(kMemTokens) * kMemDim, 0.0f);
  return m;
}

// ---- Tracker（SAM 2 の memory bank。transformers の EdgeTamVideoModel と同じ選び方） ----

std::vector<float> Tracker::conditioned(int index, const ImageFeatures& f, bool reverse) const {
  // 記憶: クリックしたフレーム（時間の位置は行 6）と、直前の 6 フレーム（r フレーム前は行 r-1。逆向きに追うときは後ろのフレーム）
  std::vector<std::pair<const Memory*, int>> spatial;
  for (const auto& [i, o] : cond_) spatial.push_back({&o.mem, 6});
  for (int r = 6; r >= 1; --r) {
    const auto it = noncond_.find(reverse ? index + r : index - r);
    if (it != noncond_.end()) spatial.push_back({&it->second.mem, r - 1});
  }
  // 被写体を表す値: これまでに通ったクリックしたフレームと、直前の 15 フレーム
  std::vector<const std::vector<float>*> pointers;
  for (const auto& [i, o] : cond_) {
    if (reverse ? i >= index : i <= index) pointers.push_back(&o.pointer);
  }
  for (int d = 1; d < 16; ++d) {
    const auto it = noncond_.find(reverse ? index + d : index - d);
    if (it != noncond_.end()) pointers.push_back(&it->second.pointer);
  }
  return m_.attend(f, spatial, pointers);
}

Tracker::Result Tracker::store(int index, const ImageFeatures& f, Decoded d, bool fromPoints, bool isCond) {
  Out o;
  o.mem = m_.remember(f, d, fromPoints);
  o.pointer = std::move(d.pointer);
  if (isCond) {
    noncond_.erase(index);
    cond_[index] = std::move(o);
  } else {
    noncond_[index] = std::move(o);
  }
  Result r;
  r.lowRes = std::move(d.lowRes);
  r.score = d.score;
  return r;
}

Tracker::Result Tracker::prompt(int index, const ImageFeatures& f, const std::vector<PromptPoint>& points, bool initial, bool reverse) {
  // 最初のクリックは記憶を使わない。追いかけている途中で足した点は、それまでの記憶と照らし合わせた特徴に点を足す。
  // どちらも「クリックしたフレーム」として、このあとのフレームの記憶にずっと使う
  const std::vector<float> pix = initial || cond_.empty() ? m_.withoutMemory(f) : conditioned(index, f, reverse);
  return store(index, f, m_.decode(f, pix, points), true, true);
}

Tracker::Result Tracker::track(int index, const ImageFeatures& f, bool reverse) {
  return store(index, f, m_.decode(f, conditioned(index, f, reverse), {}), false, false);
}

void Tracker::forget(int index, bool reverse) {
  // これから使うのは、今のフレームの近く（後ろ・前の 16 フレーム）と、クリックしたフレームのすぐ隣（逆向きに追い始めるときに使う）
  for (auto it = noncond_.begin(); it != noncond_.end();) {
    const int i = it->first;
    const bool nearNow = reverse ? (i >= index && i <= index + 16) : (i <= index && i >= index - 16);
    const bool nearStart = std::abs(i) <= 16;
    if (nearNow || nearStart) ++it;
    else it = noncond_.erase(it);
  }
}

}  // namespace kv::mask
