#pragma once
// トランジション（メインのビデオの切れ目で、前のクリップから次のクリップへ切り替える効果）。
// - 後ろのクリップの transIn に持つ（そのクリップに入るときの切り替え）。動画の長さは変えない
// - 切れ目を真ん中にして、前のクリップの終わりの後ろ（素材の続き）と、次のクリップの始まりの前（素材の手前）を使う。
//   素材が足りないときは、端のフレームを止めて使う（書き出しの tpad の clone と同じ）
// - 混ぜ方は、書き出しの ffmpeg の xfade と同じ式（RGB で計算する。進み具合は xfade と同じく 1 → 0）

#include <QImage>
#include <QString>
#include <vector>

#include "core/model.h"
#include "core/timeline.h"

namespace kv {

struct TransitionInfo {
  TransitionType type = TransitionType::None;
  /** 保存に使う名前 */
  QString id;
  /** 画面に出す名前 */
  QString label;
  QString hint;
  /** ffmpeg の xfade の transition の名前 */
  QString xfade;
  /** 一覧の分類（ディゾルブ・ワイプ・スライド・ズーム） */
  QString group;
};

/** トランジションの一覧（None は含まない。一覧に出す順） */
const std::vector<TransitionInfo>& transitionInfos();
const TransitionInfo& transitionInfo(TransitionType t);
QString transitionToString(TransitionType t);
/** 知らない名前なら false */
bool transitionFromString(const QString& s, TransitionType* out);
/** 一覧に出すときのアイコン名 */
QString transitionIconName(TransitionType t);

constexpr double kDefaultTransitionDuration = 1.0;
constexpr double kMinTransitionDuration = 0.1;
constexpr double kMaxTransitionDuration = 5.0;

/** 切れ目のトランジションの実際の範囲（フレーム数） */
struct TransitionSpan {
  /** 切れ目より前（前のクリップの終わりの分）と、後ろ（次のクリップの始まりの分） */
  int before = 0;
  int after = 0;
  int frames() const { return before + after; }
  bool active() const { return before + after >= 2; }
};

/**
 * 長さ wantFrames のトランジションを、前のクリップ（prevFrames）と次のクリップ（nextFrames）の間に置いたときの範囲。
 * 切れ目を真ん中にし、前のクリップの後ろ半分・次のクリップの前半分までに収める（隣のトランジションと重ならない）。2 フレームより短いなら無し
 */
TransitionSpan transitionSpanFrames(int wantFrames, int prevFrames, int nextFrames);
/** トランジション tr の範囲（長さを範囲に収めて、フレーム数にしたもの）。プレビューと書き出しで同じ決め方 */
TransitionSpan transitionSpanFor(const Transition& tr, int prevFrames, int nextFrames, int fps);
/** layout の index 番目のクリップに入るトランジションの範囲（先頭のクリップ・トランジションの無いクリップは空） */
TransitionSpan transitionSpanAt(const TimelineLayout& layout, int index, int fps);
/** トランジションの、実際の長さ（秒）。置けないときは 0 */
double transitionEffectiveDuration(const TimelineLayout& layout, int index, int fps);

/** 時刻 t のフレームが、トランジションの中か */
struct ActiveTransition {
  /** 後ろのクリップの番号（layout.items の添字） */
  int index = -1;
  TransitionSpan span;
  /** トランジションの最初のフレーム（タイムライン全体の番号） */
  long long startFrame = 0;
  /** トランジションの中の何フレーム目か（0 から） */
  int k = 0;
  /** xfade と同じ進み具合（1 = 前のクリップだけ → 0 に近づく） */
  float progress = 1;
};
bool transitionAt(const TimelineLayout& layout, double t, int fps, ActiveTransition* out);
/** 時刻 t のフレームの番号（プレビューと書き出しで同じ数え方） */
long long frameIndexAt(double t, int fps);

/**
 * 同じ大きさの 2 枚（a: 前のクリップ、b: 次のクリップ）を、xfade と同じ式で混ぜた画面（RGB32）。
 * 大きさが違うときは b に合わせる
 */
QImage blendTransition(const QImage& a, const QImage& b, TransitionType type, float progress);

}  // namespace kv
