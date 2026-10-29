#pragma once
// エフェクト（モザイク・ぼかし・シャープ・周辺減光・色の反転・ポスタリゼーション・ノイズ・色ずれ・塗りつぶし）。
// プレビュー（CPU で計算）と書き出し（ffmpeg のフィルター）で、同じ計算になるようにする。
// - 大きさの値は、掛ける画像の高さに対する割合。プレビュー（縮めたフレーム）と書き出し（元の大きさ）で同じ見た目になる
// - 書き出しは RGB（gbrp）で計算する（プレビューと同じ色の空間）。シャープとノイズは、明るさ（BT.601 フルレンジの Y）だけに掛ける
// - モザイクは「高さ方向のブロックの数」、ぼかしは「縮めて計算する大きさ」を割合から決めるので、解像度によらず同じ並び・同じぼけ方になる

#include <QImage>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

#include "core/model.h"

namespace kv {

struct EffectParam {
  /** 画面に出す名前（空なら、この値は使わない） */
  QString label;
  double min = 0;
  double max = 1;
  double def = 0;
  /** スライダーの刻み */
  double step = 0.01;
  /** 表示の形（Percent: 1.0 = 100%、Integer: 整数） */
  enum class Unit { Percent, Integer } unit = Unit::Percent;
};

struct EffectInfo {
  EffectType type;
  /** 保存に使う名前 */
  QString id;
  /** 画面に出す名前 */
  QString label;
  QString hint;
  /** クリップに掛けられる（設定欄・右クリックの「エフェクトを追加」に出す） */
  bool clip = true;
  /** エフェクトレイヤーで使える */
  bool layer = true;
  EffectParam amount;
  EffectParam size;
  EffectParam softness;
  bool usesColor = false;
};

const std::vector<EffectInfo>& effectInfos();
const EffectInfo& effectInfo(EffectType t);
QString effectTypeToString(EffectType t);
/** 知らない名前なら false */
bool effectTypeFromString(const QString& s, EffectType* out);
/** その種類の、はじめの値のエフェクト（新しい ID つき） */
Effect makeEffect(EffectType t);
/** 値を範囲に収める（保存データを読むとき・変えたとき） */
void clampEffect(Effect& e);
/** 掛けると見た目が変わるか（オンで、強さが 0 でない） */
bool effectActive(const Effect& e);
bool anyEffectActive(const std::vector<Effect>& effects);
/** 加工したフレームを使い回すときの目印（オンのエフェクトと、その値） */
QString effectsSignature(const std::vector<Effect>& effects);
/** ノイズのように、フレームごとに変わるエフェクトがあるか */
bool effectsVaryPerFrame(const std::vector<Effect>& effects);

/**
 * 画像に、オンのエフェクトを順に掛ける（ARGB32 にする。透明度はそのまま。色だけを変える）。
 * frame はノイズの種（フレームごとに模様が変わる）
 */
void applyEffects(QImage& img, const std::vector<Effect>& effects, long long frame);

/** 周辺減光の明るさの倍率の画像（Grayscale8、0〜255。255 がそのまま）。プレビューと書き出しで同じ式 */
QImage vignetteMask(int w, int h, const Effect& e);

// ---- エフェクトレイヤー（重ねるトラックの段に置き、下にあるものの範囲の中だけに効果を掛ける） ----

/** エフェクトレイヤーの見本（追加するときの、はじめの範囲と効果） */
struct EffectLayerPreset {
  QString id;
  QString label;
  QString hint;
};
const std::vector<EffectLayerPreset>& effectLayerPresets();
/** 見本 presetId のエフェクトレイヤー（ID・開始・段は呼ぶ側で決める）。知らない見本ならモザイク */
OverlayItem makeEffectLayer(const QString& presetId, Aspect aspect);
/** タイムラインなどに出す名前（掛けているエフェクトの名前。無ければ「調整レイヤー」など） */
QString effectLayerLabel(const OverlayItem& o);

/** 書き出しのフィルターグラフを組み立てるときに、別の入力（画像）が要るエフェクトのための道具 */
struct EffectGraphContext {
  /** 画像を一時フォルダーに書いて入力に加え、その入力の番号を返す（できなければ -1） */
  std::function<int(const QImage& image, const QString& name)> addImage;
};

/**
 * 書き出しのフィルターグラフの文（[input] → [output]）。入力は w×h の映像（どの形式でもよい）、出力は w×h の gbrp。
 * id はラベルの頭（ほかと重ならないもの）。オンのエフェクトが無ければ空を返す（呼ぶ側は input をそのまま使う）
 */
QStringList ffmpegEffectStatements(const std::vector<Effect>& effects, int w, int h, const QString& input, const QString& output, const QString& id,
                                   EffectGraphContext& ctx);

}  // namespace kv
