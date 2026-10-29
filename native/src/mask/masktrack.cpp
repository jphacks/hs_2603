#include "mask/masktrack.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace kv::mask {

namespace {

constexpr int kSize = 256;  // ロジットの縦横
constexpr int kCount = kSize * kSize;
constexpr float kScale = 8.0f;  // 保存する int8 = ロジット × 8
const char kMagic[8] = {'K', 'V', 'M', 'A', 'S', 'K', '\x01', '\0'};
constexpr qint32 kVersion = 1;

/** 1 列の 2 乗距離（Felzenszwalb・Huttenlocher の方法）。f は 0（基準の画素）か大きい値 */
void distance1d(const float* f, int n, float* d, int* v, float* z) {
  constexpr float inf = std::numeric_limits<float>::infinity();
  int k = 0;
  v[0] = 0;
  z[0] = -inf;
  z[1] = inf;
  for (int q = 1; q < n; ++q) {
    float s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.0f * q - 2.0f * v[k]);
    while (k > 0 && s <= z[k]) {
      --k;
      s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.0f * q - 2.0f * v[k]);
    }
    ++k;
    v[k] = q;
    z[k] = s;
    z[k + 1] = inf;
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < q) ++k;
    const float dq = float(q - v[k]);
    d[q] = dq * dq + f[v[k]];
  }
}

/** 各画素から、seed（true の画素）の一番近い画素の中心までの距離（画素） */
std::vector<float> distanceTo(const std::vector<uint8_t>& seed, int w, int h) {
  constexpr float big = 1e20f;
  std::vector<float> grid(size_t(w) * h);
  for (size_t i = 0; i < grid.size(); ++i) grid[i] = seed[i] ? 0.0f : big;
  const int n = std::max(w, h);
  std::vector<float> f(n), d(n), z(n + 1);
  std::vector<int> v(n);
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) f[y] = grid[size_t(y) * w + x];
    distance1d(f.data(), h, d.data(), v.data(), z.data());
    for (int y = 0; y < h; ++y) grid[size_t(y) * w + x] = d[y];
  }
  for (int y = 0; y < h; ++y) {
    float* row = grid.data() + size_t(y) * w;
    std::copy(row, row + w, f.begin());
    distance1d(f.data(), w, d.data(), v.data(), z.data());
    for (int x = 0; x < w; ++x) row[x] = std::sqrt(std::min(d[x], big));
  }
  return grid;
}

/** 箱型ぼかし（1 行・1 列。端は端の値を伸ばす） */
void boxLine(const uint8_t* src, uint8_t* dst, int n, int stride, int r) {
  if (n <= 0) return;
  const int win = 2 * r + 1;
  int sum = 0;
  for (int i = -r; i <= r; ++i) sum += src[size_t(std::clamp(i, 0, n - 1)) * stride];
  for (int i = 0; i < n; ++i) {
    dst[size_t(i) * stride] = uint8_t((sum + win / 2) / win);
    const int out = std::clamp(i - r, 0, n - 1);
    const int in = std::clamp(i + r + 1, 0, n - 1);
    sum += int(src[size_t(in) * stride]) - int(src[size_t(out) * stride]);
  }
}

/** ガウスぼかし（箱型を 3 回） */
void blurGray(QImage& img, double sigma) {
  if (sigma < 0.3) return;
  const int w = img.width(), h = img.height();
  const int r = std::max(1, int(std::lround((std::sqrt(4.0 * sigma * sigma + 1) - 1) / 2)));
  std::vector<uint8_t> a(size_t(w) * h), b(size_t(w) * h);
  for (int y = 0; y < h; ++y) std::copy(img.constScanLine(y), img.constScanLine(y) + w, a.begin() + size_t(y) * w);
  for (int pass = 0; pass < 3; ++pass) {
    for (int y = 0; y < h; ++y) boxLine(a.data() + size_t(y) * w, b.data() + size_t(y) * w, w, 1, r);
    for (int x = 0; x < w; ++x) boxLine(b.data() + x, a.data() + x, h, w, r);
  }
  for (int y = 0; y < h; ++y) std::copy(a.begin() + size_t(y) * w, a.begin() + size_t(y + 1) * w, img.scanLine(y));
}

}  // namespace

int MaskTrack::nearest(double pts) const {
  if (frames.empty()) return -1;
  const auto it = std::lower_bound(frames.begin(), frames.end(), pts, [](const MaskFrame& f, double t) { return f.pts < t; });
  if (it == frames.end()) return int(frames.size()) - 1;
  if (it == frames.begin()) return 0;
  const auto prev = it - 1;
  return int((pts - prev->pts <= it->pts - pts ? prev : it) - frames.begin());
}

std::shared_ptr<const std::vector<int8_t>> MaskTrack::logits(int index) const {
  if (index < 0 || index >= int(frames.size())) return nullptr;
  {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (it->first != index) continue;
      auto hit = it->second;
      cache_.erase(it);
      cache_.insert(cache_.begin(), {index, hit});
      return hit;
    }
  }
  const QByteArray raw = qUncompress(frames[size_t(index)].packed);
  if (raw.size() != kCount) return nullptr;
  auto data = std::make_shared<std::vector<int8_t>>(kCount);
  std::memcpy(data->data(), raw.constData(), kCount);
  std::lock_guard<std::mutex> lock(cacheMutex_);
  cache_.insert(cache_.begin(), {index, data});
  if (cache_.size() > 8) cache_.pop_back();
  return data;
}

std::shared_ptr<MaskTrack> MaskTrack::clone() const {
  auto t = std::make_shared<MaskTrack>();
  t->frames = frames;
  t->width = width;
  t->height = height;
  t->frameDuration = frameDuration;
  return t;
}

QByteArray MaskTrack::pack(const std::vector<float>& lowRes) {
  QByteArray raw(kCount, '\0');
  for (int i = 0; i < kCount; ++i) {
    const float v = i < int(lowRes.size()) ? lowRes[size_t(i)] : -1024.0f;
    raw[i] = char(int8_t(std::clamp(int(std::lround(v * kScale)), -127, 127)));
  }
  return qCompress(raw, 6);
}

bool MaskTrack::save(const QString& path, QString* error) const {
  QDir().mkpath(QFileInfo(path).absolutePath());
  const QString part = path + QStringLiteral(".part");
  QFile f(part);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (error) *error = QStringLiteral("マスクのファイルを書けません: %1").arg(f.errorString());
    return false;
  }
  f.write(kMagic, sizeof(kMagic));
  QDataStream s(&f);
  s.setVersion(QDataStream::Qt_6_5);
  s.setByteOrder(QDataStream::LittleEndian);
  s << kVersion << qint32(width) << qint32(height) << frameDuration << quint32(frames.size());
  for (const MaskFrame& fr : frames) s << fr.pts << fr.score << fr.packed;
  const bool ok = s.status() == QDataStream::Ok && f.flush();
  f.close();
  if (!ok) {
    QFile::remove(part);
    if (error) *error = QStringLiteral("マスクのファイルを書けません");
    return false;
  }
  QFile::remove(path);
  if (!QFile::rename(part, path)) {
    QFile::remove(part);
    if (error) *error = QStringLiteral("マスクのファイルを置けません");
    return false;
  }
  return true;
}

std::shared_ptr<MaskTrack> MaskTrack::load(const QString& path, QString* error) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (error) *error = QStringLiteral("マスクのファイルが見つかりません");
    return nullptr;
  }
  char magic[sizeof(kMagic)] = {};
  if (f.read(magic, sizeof(magic)) != qint64(sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
    if (error) *error = QStringLiteral("マスクのファイルではありません");
    return nullptr;
  }
  QDataStream s(&f);
  s.setVersion(QDataStream::Qt_6_5);
  s.setByteOrder(QDataStream::LittleEndian);
  qint32 version = 0, w = 0, h = 0;
  double fd = 0;
  quint32 n = 0;
  s >> version >> w >> h >> fd >> n;
  if (s.status() != QDataStream::Ok || version != kVersion || n > 2'000'000) {
    if (error) *error = QStringLiteral("マスクのファイルを読めません（新しい版で作られたものかもしれません）");
    return nullptr;
  }
  auto t = std::make_shared<MaskTrack>();
  t->width = w;
  t->height = h;
  t->frameDuration = fd > 0 ? fd : 1.0 / 30;
  t->frames.reserve(n);
  for (quint32 i = 0; i < n && s.status() == QDataStream::Ok; ++i) {
    MaskFrame fr;
    s >> fr.pts >> fr.score >> fr.packed;
    t->frames.push_back(std::move(fr));
  }
  if (s.status() != QDataStream::Ok) {
    if (error) *error = QStringLiteral("マスクのファイルが壊れています");
    return nullptr;
  }
  std::stable_sort(t->frames.begin(), t->frames.end(), [](const MaskFrame& a, const MaskFrame& b) { return a.pts < b.pts; });
  return t;
}

QImage renderMask(const std::vector<int8_t>& logits, QSize size, const MaskLook& look) {
  const int w = std::max(1, size.width());
  const int h = std::max(1, size.height());
  QImage out(w, h, QImage::Format_Grayscale8);
  if (int(logits.size()) != kCount) {
    out.fill(look.invert ? 255 : 0);
    return out;
  }
  // 双線形で広げる（align_corners=False。transformers の後処理と同じ考え方）。ロジット 0 が境目で、±1 の幅でなめらかにする
  std::vector<int> x0(w), x1(w), y0(h), y1(h);
  std::vector<float> fx(w), fy(h);
  auto table = [](int n, std::vector<int>& a, std::vector<int>& b, std::vector<float>& f) {
    for (int i = 0; i < n; ++i) {
      const double s = std::clamp((i + 0.5) * kSize / n - 0.5, 0.0, double(kSize - 1));
      a[size_t(i)] = int(std::floor(s));
      b[size_t(i)] = std::min(a[size_t(i)] + 1, kSize - 1);
      f[size_t(i)] = float(s - a[size_t(i)]);
    }
  };
  table(w, x0, x1, fx);
  table(h, y0, y1, fy);
  std::vector<float> row(kSize);
  const float k = 0.5f / kScale;  // 値 / 8 がロジット。ロジット ±1 で 0〜1
  for (int y = 0; y < h; ++y) {
    const int8_t* r0 = logits.data() + size_t(y0[size_t(y)]) * kSize;
    const int8_t* r1 = logits.data() + size_t(y1[size_t(y)]) * kSize;
    const float t = fy[size_t(y)];
    for (int x = 0; x < kSize; ++x) row[size_t(x)] = r0[x] + (r1[x] - r0[x]) * t;
    uchar* dst = out.scanLine(y);
    for (int x = 0; x < w; ++x) {
      const float a = row[size_t(x0[size_t(x)])], b = row[size_t(x1[size_t(x)])];
      const float v = 0.5f + (a + (b - a) * fx[size_t(x)]) * k;
      dst[x] = uchar(std::clamp(int(v * 255.0f + 0.5f), 0, 255));
    }
  }
  // 広げる・狭める: 境目からの距離で決める（角は丸くなる）
  if (std::abs(look.expand) >= 0.25) {
    const bool grow = look.expand > 0;
    const float e = float(std::abs(look.expand));
    std::vector<uint8_t> seed(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
      const uchar* s = out.constScanLine(y);
      for (int x = 0; x < w; ++x) seed[size_t(y) * w + x] = grow ? s[x] >= 128 : s[x] < 128;
    }
    const std::vector<float> dist = distanceTo(seed, w, h);
    for (int y = 0; y < h; ++y) {
      uchar* s = out.scanLine(y);
      for (int x = 0; x < w; ++x) {
        const float d = dist[size_t(y) * w + x];
        const bool inside = s[x] >= 128;
        float v;
        if (grow) v = inside ? 1.0f : std::clamp(e - d + 1.0f, 0.0f, 1.0f);
        else v = inside ? std::clamp(d - e, 0.0f, 1.0f) : 0.0f;
        s[x] = uchar(std::lround(v * 255.0f));
      }
    }
  }
  if (look.feather > 0.01) blurGray(out, look.feather / 2.0);
  if (look.invert) {
    for (int y = 0; y < h; ++y) {
      uchar* s = out.scanLine(y);
      for (int x = 0; x < w; ++x) s[x] = uchar(255 - s[x]);
    }
  }
  if (look.flipX || look.flipY) out = out.mirrored(look.flipX, look.flipY);
  return out;
}

QString maskDir() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("generated"));
}

QString newMaskFileName() {
  return QStringLiteral("objmask-%1.kvmask").arg(QUuid::createUuid().toString(QUuid::Id128).left(20));
}

MaskStore& MaskStore::instance() {
  static MaskStore s;
  return s;
}

std::shared_ptr<const MaskTrack> MaskStore::get(const QString& fileName) {
  if (fileName.isEmpty()) return nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = files_.begin(); it != files_.end(); ++it) {
      if (it->first != fileName) continue;
      auto hit = it->second;
      files_.erase(it);
      files_.insert(files_.begin(), {fileName, hit});
      return hit;
    }
    if (std::find(missing_.begin(), missing_.end(), fileName) != missing_.end()) return nullptr;
  }
  // ファイル名だけを受け付ける（プロジェクトファイルに書いてある名前で、ほかのフォルダーのファイルを読まないように）
  const QString name = QFileInfo(fileName).fileName();
  std::shared_ptr<const MaskTrack> t = name == fileName ? MaskTrack::load(QDir(maskDir()).filePath(name)) : nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!t) {
    missing_.push_back(fileName);
    if (missing_.size() > 64) missing_.erase(missing_.begin());
    return nullptr;
  }
  files_.insert(files_.begin(), {fileName, t});
  if (files_.size() > 6) files_.pop_back();
  return t;
}

void MaskStore::put(const QString& fileName, std::shared_ptr<const MaskTrack> track) {
  std::lock_guard<std::mutex> lock(mutex_);
  missing_.erase(std::remove(missing_.begin(), missing_.end(), fileName), missing_.end());
  files_.erase(std::remove_if(files_.begin(), files_.end(), [&](const auto& p) { return p.first == fileName; }), files_.end());
  files_.insert(files_.begin(), {fileName, std::move(track)});
  if (files_.size() > 6) files_.pop_back();
}

void MaskStore::setLive(const QString& maskId, std::shared_ptr<const MaskTrack> track) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& p : live_) {
    if (p.first == maskId) {
      p.second = std::move(track);
      return;
    }
  }
  live_.push_back({maskId, std::move(track)});
}

std::shared_ptr<const MaskTrack> MaskStore::live(const QString& maskId) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& p : live_) {
    if (p.first == maskId) return p.second;
  }
  return nullptr;
}

void MaskStore::clearLive(const QString& maskId) {
  std::lock_guard<std::mutex> lock(mutex_);
  live_.erase(std::remove_if(live_.begin(), live_.end(), [&](const auto& p) { return p.first == maskId; }), live_.end());
}

}  // namespace kv::mask
