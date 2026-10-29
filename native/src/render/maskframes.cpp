#include "render/maskframes.h"

#include <algorithm>
#include <cmath>

#include "mask/masktrack.h"

namespace kv {

std::shared_ptr<const mask::MaskTrack> maskTrackFor(const ObjectMask& m, const Asset* asset) {
  if (!m.has() || !asset) return nullptr;
  // トラックしている途中なら、できた所までの結果を使う（終わるとファイルになる）
  std::shared_ptr<const mask::MaskTrack> t = mask::MaskStore::instance().live(m.id);
  if (!t && !m.file.isEmpty()) t = mask::MaskStore::instance().get(m.file);
  if (!t || t->empty()) return nullptr;
  // 別の大きさの素材（リンクし直した別のファイルなど）には使わない
  if (asset->width > 0 && asset->height > 0 && t->width > 0 && t->height > 0 && (asset->width != t->width || asset->height != t->height)) return nullptr;
  return t;
}

FrameMask frameMaskFor(const VideoClip& c, const Asset* asset, double pts) {
  FrameMask fm;
  fm.cutout = c.mask.mode == MaskMode::Cutout;
  fm.invert = c.mask.invert;
  fm.expand = c.mask.expand;
  fm.feather = c.mask.feather;
  fm.sourceWidth = asset ? asset->width : 0;
  const std::shared_ptr<const mask::MaskTrack> t = maskTrackFor(c.mask, asset);
  if (!t) return fm;
  const int i = c.isImage ? 0 : t->nearest(pts);
  fm.logits = t->logits(i);
  if (fm.logits) fm.key = QStringLiteral("%1|%2|%3").arg(c.mask.file, c.mask.id).arg(i) + QStringLiteral("|%1").arg(qintptr(t.get()));
  if (fm.sourceWidth <= 0) fm.sourceWidth = t->width;
  return fm;
}

std::map<QString, FrameMask> resolveFrameMasks(const ProjectData& p, const FramePlan& plan, const AssetLookup& asset,
                                               const std::map<QString, double>& pts, const QString& bypassClipId) {
  std::map<QString, FrameMask> out;
  for (const FrameNeed& n : plan.needs) {
    const VideoClip* c = nullptr;
    if (n.key.startsWith(QLatin1String("clip:"))) {
      const QString id = n.key.mid(5);
      for (const auto& x : p.clips) {
        if (x.id == id) c = &x;
      }
    } else if (n.key.startsWith(QLatin1String("overlay:"))) {
      const QString id = n.key.mid(8);
      for (const auto& x : p.overlays) {
        if (x.id == id) c = &x;
      }
    }
    if (!c || !c->mask.has() || !c->mask.enabled || c->id == bypassClipId) continue;
    const auto it = pts.find(n.key);
    const FrameMask fm = frameMaskFor(*c, asset(n.assetId), it != pts.end() ? it->second : n.srcT);
    // データがまだ無い（トラックしていない・ファイルが無い）マスクは掛けない（映像はそのまま出す）
    if (fm.logits) out[n.key] = fm;
  }
  return out;
}

}  // namespace kv
