#pragma once
// タイムラインの計算。プレビュー・書き出しの両方で同じ関数を使う。

#include <QRectF>
#include <QSize>
#include <QString>
#include <vector>

#include "core/model.h"

namespace kv {

constexpr int kAudioRate = 48000;
constexpr double kMinClipDuration = 0.2;
constexpr double kMinTextDuration = 0.3;
constexpr double kDefaultImageDuration = 4;
constexpr double kDefaultTextDuration = 3;
/** スライドの移動量（画面の高さに対する割合） */
constexpr double kTextSlideDistance = 0.04;

/** 編集時の基準解像度。文字の配置はこの座標で計算する */
QSize designSize(Aspect a);
/** 書き出しサイズ（偶数に揃える） */
QSize exportSize(Aspect a, int shortSide);
double aspectValue(Aspect a);

double frameRound(double t, int fps);
double clipDuration(const VideoClip& c, int fps);
int clipFrames(const VideoClip& c, int fps);

struct ClipLayout {
  VideoClip clip;
  int index = 0;
  double start = 0;
  double end = 0;
  double duration = 0;
};

struct TimelineLayout {
  std::vector<ClipLayout> items;
  double total = 0;
};

TimelineLayout layoutClips(const std::vector<VideoClip>& clips, int fps);
/** 時刻 t に表示されるクリップ（末尾より後ろなら最後のクリップ） */
const ClipLayout* findClipAt(const TimelineLayout& layout, double t);

/** フェードの係数 (0〜1)。ffmpeg の fade / afade と同じ直線的な変化 */
double fadeFactor(double local, double duration, double fadeIn, double fadeOut);
/**
 * フェードの長さを 0〜kMaxFade 秒と、オブジェクトの長さに収める。イン・アウトは重ならないよう、
 * もう一方（other）の残りまでにする。設定欄のスライダー・タイムラインのつまみ・右クリックメニューで同じ決まりを使う
 */
double fitFade(double value, double duration, double other);
double audioDuration(const AudioItem& a);

// ---- 音量キーフレーム ---------------------------------------------------------
// プレビューの再生も書き出しも、この計算と同じ音量の変わり方にする。

/** キーフレーム同士の最小の間隔（秒）。これより近いと音量が急に変わってクリックノイズになるので、近づけない */
constexpr double kMinVolumePointGap = 0.05;

/**
 * クリップの先頭から local 秒の音量。キーフレームが無ければ fallback。あるときは、最初より前と最後より後は
 * 端のキーフレームの音量を保ち、間は直線でつなぐ
 */
double volumeAt(const std::vector<VolumePoint>& points, double fallback, double local);
double audioGainAt(const AudioItem& a, double local);
/** すべてのキーフレームの時間を delta 秒ずらす（クリップの頭を切ったとき、同じ音の位置に合わせ直す） */
std::vector<VolumePoint> shiftVolumePoints(const std::vector<VolumePoint>& points, double delta);
/** 並び・範囲・最小の間隔をそろえる（保存データを読むときに使う） */
void normalizeVolumePoints(std::vector<VolumePoint>& points);
/**
 * クリップを cut 秒の位置で分けるときの、前半・後半それぞれのキーフレーム（後半は後半の先頭からの時間）。
 * 分け目に同じ音量のキーフレームを置いて、音量が途切れないようにする。キーフレームが無ければ両方とも空
 */
void splitVolumePoints(const AudioItem& a, double cut, std::vector<VolumePoint>& first, std::vector<VolumePoint>& second);

/** 音量キーフレームの点列を cut 秒で分ける（AudioItem・VideoClip 共通。fallback は点が無いときの音量） */
void splitVolumePointList(const std::vector<VolumePoint>& points, double fallback, double cut, std::vector<VolumePoint>& first,
                          std::vector<VolumePoint>& second);
/** ビデオクリップの、先頭から local 秒（タイムライン上の秒数）の音量 */
double clipGainAt(const VideoClip& c, double local);

// ---- キーフレームアニメーション（位置・拡大率・回転・不透明度） -----------------------
// プレビューも書き出しも、この計算と同じ変わり方にする（書き出しは exportplan.cpp が同じ式を ffmpeg の式にする）。

/** 同じ項目のキーフレーム同士の最小の間隔（秒）。1 フレームより短い。同じ時間に重ねて置かせない */
constexpr double kMinKeyGap = 1.0 / 60;
constexpr double kDefaultAnimSeconds = 0.6;

struct PropLimits {
  double min;
  double max;
  double def;
};
PropLimits propLimits(AnimProp p);
double clampProp(AnimProp p, double v);
/** 画面に出す名前（「位置 X」など） */
QString propLabel(AnimProp p);

/** 0〜1 の進み具合 u を、変わり方 e に合わせた 0〜1 にする（Hold は常に 0 = 次のキーフレームまで前の値のまま） */
double easeCurve(Easing e, double u);
QString easingLabel(Easing e);

/**
 * オブジェクトの先頭から local 秒の値。キーフレームが無ければ base。あるときは、最初より前と最後より後は
 * 端のキーフレームの値を保ち、間は easing に合わせてつなぐ
 */
double animValueAt(const std::vector<Keyframe>& keys, double base, double local);

/** 映像・文字の、ある時間の位置・拡大率・回転・不透明度 */
struct LayerState {
  double x = 0.5;
  double y = 0.5;
  double scale = 1;
  double rotation = 0;
  double opacity = 1;
  /** 字間（文字だけ。基準解像度の px） */
  double letterSpacing = 0;
  /** 伸縮（映像だけ。幅・高さだけの倍率で、拡大率に掛ける。アニメーションはしない） */
  double stretchX = 1;
  double stretchY = 1;
};
LayerState layerStateAt(const VideoClip& c, double local);
LayerState layerStateAt(const TextItem& t, double local);
// ---- 切り抜き・伸縮 -------------------------------------------------------------------------

/** 切り抜いているか */
bool hasCrop(const VideoClip& c);
/** 伸縮しているか（幅・高さの倍率が 1 でない） */
bool hasStretch(const VideoClip& c);
/** 切り抜き・伸縮の値を範囲に収める（左右・上下を合わせても kMaxCrop まで） */
void clampCropStretch(VideoClip& c);
/** 素材（大きさ src）の中で、残す部分（切り抜いたあと）の四角 */
QRectF cropSourceRect(const VideoClip& c, QSizeF src);
/** 素材を fitted の四角に収めて描くときの、残す部分の四角（切り抜いても、その場所のまま） */
QRectF cropTargetRect(const VideoClip& c, const QRectF& fitted);

/** 静的な値（キーフレームが無いときの値） */
double baseProp(const VideoClip& c, AnimProp p);
double baseProp(const TextItem& t, AnimProp p);
void setBaseProp(VideoClip& c, AnimProp p, double v);
void setBaseProp(TextItem& t, AnimProp p, double v);
/**
 * 位置・拡大・回転・不透明度に手を入れていない（従来どおりの描き方でよい）か。
 * 文字の位置は従来から x, y で持っているので、キーフレームが無ければ「手を入れていない」に数えない
 */
bool layerIsPlain(const VideoClip& c);
bool layerIsPlain(const TextItem& t);

/** その時間（tol 秒以内）にあるキーフレームの番号。無ければ -1 */
int findKey(const std::vector<Keyframe>& keys, double t, double tol = kMinKeyGap * 0.99);
/** t の順に挿入する。近く(kMinKeyGap 未満)に既にあるときは増やさず、その番号を返す（重複を作らない） */
int insertKey(std::vector<Keyframe>& keys, const Keyframe& k);
/** 範囲・並び・最小の間隔をそろえる（保存データを読むときに使う） */
void normalizeKeys(AnimProp p, std::vector<Keyframe>& keys);
/** すべてのキーフレームの時間を delta 秒ずらす */
Animation shiftAnimation(const Animation& a, double delta);
/** すべてのキーフレームの時間を factor 倍にする（速度を変えたとき、同じ映像の位置に合わせ直す） */
Animation scaleAnimationTime(const Animation& a, double factor);
/**
 * オブジェクトを cut 秒で分けるときの、前半・後半（後半は後半の先頭からの時間）。分け目に、分ける前と
 * 同じ値のキーフレームを置いて、動きが途切れないようにする。キーフレームが無い項目は両方とも空
 */
void splitAnimation(const Animation& a, double cut, Animation& first, Animation& second);
/** 先頭を from 秒切り落とす（切り落とした位置の値を先頭のキーフレームに残す）。時間は from 引いて詰める */
Animation cropAnimationStart(const Animation& a, double from);
/**
 * 左端を shift 秒だけ右へ詰めた（正）/ 左へ広げた（負）ときの、キーフレーム。同じ映像・同じ文字の位置に合わせ直す
 * （切り落とした分は先頭の値として残す）
 */
Animation trimAnimationStart(const Animation& a, double shift);
std::vector<VolumePoint> trimVolumePointsStart(const std::vector<VolumePoint>& points, double fallback, double shift);

struct AnimTimes {
  double fadeIn = 0;
  double fadeOut = 0;
};
AnimTimes textAnimTimes(double duration, TextAnimation anim);

struct AnimState {
  double alpha = 1;
  double offsetY = 0;
};
AnimState textAnimState(const TextItem& item, double t);
bool isTextVisible(const TextItem& item, double t);

// ---- テキストの段（トラック） --------------------------------------------------
// 段は TextItem::track に持つ。ここは「追加するときにどの段へ入れるか」を決めるためのもので、
// すでにあるテキストの段を変えることはない。

/** その段の中で、この時間の範囲に重なるテキストがないか（excludeId のテキストは数えない） */
bool textTrackFree(const std::vector<TextItem>& texts, int track, double start, double duration, const QString& excludeId = QString());
/** 追加するテキストを入れる段: 重ならない一番上の段。どの段も重なるなら、一番下にひとつ増やした段 */
int freeTextTrack(const std::vector<TextItem>& texts, double start, double duration, const QString& excludeId = QString());
/** 段を持たない古いプロジェクトを読んだときだけ使う。開始時間の順に、重ならない一番上の段へ入れる（以前の自動配置と同じ） */
void autoAssignTextTracks(std::vector<TextItem>& texts);
/** 使っている段の数（空の段は数えないが、間が空いていれば間の段も含む）。1 以上 */
int textTrackCount(const std::vector<TextItem>& texts);

// ---- 重ねるトラック ------------------------------------------------------------------------
// 段は OverlayItem::track に持つ（0 がメインの映像のすぐ上。数字が大きいほど前）。テキストの段と同じく、
// 追加するときに一度だけ決め、段を移す操作と上下のドラッグのときだけ変える。

/** タイムライン上の長さ（ビデオクリップと同じ数え方。フレームにそろえる） */
double overlayDuration(const OverlayItem& o, int fps);
bool isOverlayVisible(const OverlayItem& o, double t, int fps);
bool overlayTrackFree(const std::vector<OverlayItem>& items, int track, double start, double duration, int fps,
                      const QString& excludeId = QString());
/** 追加するものを入れる段: 重ならない一番下（メインに近い）の段。どの段も重なるなら、一番上にひとつ増やした段 */
int freeOverlayTrack(const std::vector<OverlayItem>& items, double start, double duration, int fps, const QString& excludeId = QString());
/** 使っている段の数（1 以上。間が空いていれば間の段も含む） */
int overlayTrackCount(const std::vector<OverlayItem>& items);

/** atempo は 0.5〜2.0 の範囲で掛け合わせる */
std::vector<double> atempoFactors(double speed);

QString formatTime(double t, bool withFraction = true);
QString formatDuration(double t);

}  // namespace kv
