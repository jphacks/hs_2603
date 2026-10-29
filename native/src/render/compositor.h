#pragma once
// 1 枚の画面の合成。プレビュー（PreviewEngine）と AI に見せる画面（ai/framegrab）と静止画の書き出しで、同じ計算を使う。
//   1. planFrame(): その時刻の画面に要る素材のフレーム（動画の時刻・写真）の一覧を作る
//   2. 呼ぶ側が、一覧のフレームを用意する（プレビューは VideoSource から、AI と静止画はその場でデコードして）
//   3. composeFrame(): 用意したフレームから、メインの映像・重ねるトラック・文字を重ねた画面を作る
// composeFrame は編集データ（Editor）にもメディアの管理（MediaManager）にも触れないので、別のスレッドでも使える。

#include <QImage>
#include <QSize>
#include <QString>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "core/model.h"
#include "core/timeline.h"
#include "render/textrender.h"

namespace kv {

/** 合成に要る素材のフレーム 1 つ */
struct FrameNeed {
  /** "clip:<クリップの ID>"（メインのビデオ）/ "overlay:<ID>"（重ねるトラック） */
  QString key;
  QString assetId;
  /** 動画のファイル（写真のときは使わない。写真は assetId のプレビュー用の画像を使う） */
  QString path;
  bool isImage = false;
  /** 素材の中の時刻（秒） */
  double srcT = 0;
  /** クリップの速度（再生中に先読みする速さ。動画全体の再生速度は、呼ぶ側が掛ける） */
  double rate = 1;
};

struct FramePlan {
  /** この画面に使うフレーム */
  std::vector<FrameNeed> needs;
  /** もうすぐ使う動画の最初のフレーム（先読みしておくと、切り替わりで止まらない。合成には使わない） */
  std::vector<FrameNeed> preload;
};

using AssetLookup = std::function<const Asset*(const QString& id)>;

/** 時刻 t の画面に要るフレーム。素材が無いもの・見つからないものは入れない（その映像は描かない。メインなら黒） */
FramePlan planFrame(const ProjectData& p, const TimelineLayout& layout, const AssetLookup& asset, double t);

/**
 * ある映像の、このフレームのオブジェクトマスク（呼ぶ側が、使った素材のフレームの時刻で選んで渡す。render/maskframes）。
 * ロジットは素材のフレーム全体（反転の前）に対応する
 */
struct FrameMask {
  std::shared_ptr<const std::vector<int8_t>> logits;
  /** 同じマスクかを見分ける文字（ファイル・フレーム）。加工したフレームを使い回すのに使う */
  QString key;
  /** true: 被写体だけを残す（ほかは透明）/ false: エフェクトを被写体の中だけに掛ける */
  bool cutout = true;
  bool invert = false;
  /** 広げる・ぼかし（素材のピクセル）と、素材の幅（描く大きさに合わせて換算する） */
  double expand = 0;
  double feather = 0;
  int sourceWidth = 0;
};

struct ComposeOptions {
  /** 作る画面の大きさ（実ピクセル） */
  QSize canvas{960, 540};
  /** スポイトで色を選んでいる映像（その映像は、クロマキーを外して元の色で出す）。選んでいなければ空 */
  QString chromaPick;
  /** FrameNeed::key → その映像のオブジェクトマスク（マスクを使う映像だけ） */
  std::map<QString, FrameMask> masks;
  /** プレビュー上で直接編集している文字（確定前の内容 textOverride で描く）。編集していなければ空 */
  QString textOverrideId;
  QString textOverride;
  /** 一時停止中に選んでいる文字（登場のアニメーションの途中でも、はっきり出す）。無ければ空 */
  QString editingId;
};

/** 作り直さずに済むもの（加工したフレーム・文字の画像）。プレビューは持ち続けて使い回す。AI の画面は使わない（nullptr） */
struct ComposeCache {
  struct Processed {
    qint64 source = 0;
    QString params;
    QImage image;
  };
  std::map<QString, Processed> processed;
  /** 文字の画像（テキストの ID ごと） */
  std::map<QString, TextSprite> sprites;
  void clear() {
    processed.clear();
    sprites.clear();
  }
};

/** ノイズなど、フレームごとに変わるエフェクトの種（書き出しのフレーム番号と同じ数え方。写真のクリップはいつも 0） */
long long effectFrameIndex(const VideoClip& c, double t, int fps);

/**
 * 素材のフレームに、反転とエフェクトだけを掛けたもの（クロマキー・色の調整の前。スポイトで色を選ぶときもこれを使う）。
 * 反転もエフェクトも無ければ、そのまま返す。fxMask があれば、エフェクトはその中（255 の所）だけに掛ける（反転したあとの向きの、src と同じ大きさ）
 */
QImage prepareSourceFrame(const QImage& src, const VideoClip& c, long long frame, const QImage* fxMask = nullptr);

/**
 * オブジェクトマスクの画像（Grayscale8、size の大きさ。反転・広げる・ぼかしを掛けたもの）。プレビュー・静止画・書き出しで同じ計算。
 * mirror なら、映像の反転と同じ向きにする
 */
QImage frameMaskImage(const FrameMask& m, QSize size, const VideoClip& c, bool mirror = true);

/**
 * 素材のフレームに、その映像の加工（反転・エフェクト・クロマキー・オブジェクトマスクと、withColor なら色の調整）を掛けたもの。
 * 同じフレーム・同じ設定なら、cache の前の結果を使う。
 * onBlack なら、透明にした所を黒で埋める（メインのトラック）。chromaPick と同じ映像は、クロマキーを外す。mask はこのフレームのマスク
 */
QImage processSourceFrame(const QString& key, const QImage& src, const VideoClip& c, bool withColor, bool onBlack, const QString& chromaPick,
                          ComposeCache* cache, long long frame = 0, const FrameMask* mask = nullptr);

// ---- エフェクトレイヤー（重ねるトラックの段に置き、下にあるものの範囲の中だけに効果を掛ける） ----

/**
 * エフェクトレイヤー o の、先頭から local 秒の範囲の画像（Grayscale8、canvas の大きさ。255 が効果をそのまま掛ける所）。
 * 範囲の形・境目のぼかし・反転と、不透明度・フェード（効き目の強さ）も掛けたもの。プレビューと書き出しで同じ式
 */
QImage effectLayerMask(const OverlayItem& o, double local, double duration, QSize canvas);
/** 範囲の画像が時間で変わらないか（位置などのキーフレームもフェードも無い） */
bool effectLayerMaskStatic(const OverlayItem& o);
/** 画面 canvas（RGB32）に、エフェクトレイヤー o を掛ける（範囲の外はそのまま） */
void applyEffectLayer(QImage& canvas, const OverlayItem& o, double local, double duration, long long frame);

/**
 * frames（FrameNeed::key → 素材のフレーム）を使って、時刻 t の画面を作る（RGB32、opt.canvas の大きさ）。
 * frames に無い映像は描かない（メインの映像が無ければ黒）。layout は p.clips の並び（layoutClips）
 */
QImage composeFrame(const ProjectData& p, const TimelineLayout& layout, double t, const std::map<QString, QImage>& frames, const ComposeOptions& opt,
                    ComposeCache* cache);

}  // namespace kv
