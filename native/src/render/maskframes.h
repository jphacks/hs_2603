#pragma once
// オブジェクトマスク: 合成に使う素材のフレーム（FrameNeed）ごとに、そのフレームのマスクを選ぶ。
// プレビュー・静止画・AI に見せる画面は、実際に使ったフレームの時刻（pts）で選ぶ（素材のフレームとマスクが、いつも同じフレームどうしになる）。

#include <QString>
#include <map>
#include <memory>

#include "core/model.h"
#include "render/compositor.h"

namespace kv {

namespace mask {
class MaskTrack;
}

/** マスクのデータ（トラックの途中ならその結果、無ければファイル）。使えない（無い・素材と大きさが違う）なら nullptr */
std::shared_ptr<const mask::MaskTrack> maskTrackFor(const ObjectMask& m, const Asset* asset);

/** 映像 c の、素材の時刻 pts のフレームのマスク（使わない・データが無ければ空の logits） */
FrameMask frameMaskFor(const VideoClip& c, const Asset* asset, double pts);

/**
 * plan の映像のうち、オブジェクトマスクを使うもののマスク（FrameNeed::key → マスク）。pts は key → 使ったフレームの時刻
 * （無ければ FrameNeed::srcT）。bypassClipId の映像は入れない（被写体を選んでいる間は、マスクを外して出す）
 */
std::map<QString, FrameMask> resolveFrameMasks(const ProjectData& p, const FramePlan& plan, const AssetLookup& asset,
                                               const std::map<QString, double>& pts, const QString& bypassClipId = QString());

}  // namespace kv
