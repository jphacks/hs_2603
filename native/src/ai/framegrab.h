#pragma once
// AI に見せる画面の画像。プレビューと同じ見た目（表示方法・色・フェード・位置・テキスト）を、小さめの JPEG にする。

#include <QByteArray>
#include <QObject>
#include <QSize>
#include <functional>
#include <vector>

#include "core/model.h"

namespace kv::ai {

struct FrameShot {
  /** タイムラインの時間（秒） */
  double t = 0;
  QByteArray jpeg;
  QSize size;
};

/**
 * タイムラインの時間 times の画面を作る。今の編集場所の状態を写し取り、重い処理（動画のデコード）は別のスレッドで行う。
 * 終わったら done を context のスレッド（UI）で呼ぶ。context が先に消えたら呼ばない
 */
void grabFrames(const std::vector<double>& times, int longSide, QObject* context, std::function<void(std::vector<FrameShot>)> done);
/** 編集データ project（AI の案を当てはめた写しなど）の画面を作る。素材は今の編集場所のものを使う */
void grabFrames(const ProjectData& project, const std::vector<double>& times, int longSide, QObject* context,
                std::function<void(std::vector<FrameShot>)> done);

}  // namespace kv::ai
