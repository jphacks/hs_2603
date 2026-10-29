#pragma once
// 書き出し用の ffmpeg コマンド（入力・フィルターグラフ・出力オプション）を組み立てる。

#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

#include "core/model.h"

namespace kv {

enum class ExportQuality { Fast, Standard, High };

/** 書き出す形式（並びは一覧に出す順。保存には id を使う） */
enum class ExportFormat { Mp4H264, Mp4H265, WebmVp9, MovProRes, Gif, Mp3, Wav };

struct ExportFormatInfo {
  ExportFormat format;
  /** 設定に覚えるときの名前 */
  QString id;
  /** 画面に出す名前 */
  QString label;
  QString hint;
  /** 拡張子（"mp4" など。点は付けない） */
  QString extension;
  /** 保存先を選ぶ画面の、ファイルの種類 */
  QString fileFilter;
  /** ffmpeg の出力の形式（-f） */
  QString muxer;
  bool video = true;
  bool audio = true;
};
const std::vector<ExportFormatInfo>& exportFormats();
const ExportFormatInfo& exportFormatInfo(ExportFormat f);
/** 知らない名前なら false */
bool exportFormatFromString(const QString& s, ExportFormat* out);
/** GIF のフレームレート（品質ごと。プロジェクトのフレームレートより多くはしない） */
int gifFps(ExportQuality quality, int projectFps);

/**
 * 映像・文字の、位置・拡大率・回転・不透明度。transformed が false なら従来どおりの描き方
 * （位置は x, y。拡大・回転・不透明度は無し）で、ほかの値は使わない
 */
struct LayerSpec {
  bool transformed = false;
  double x = 0.5;
  double y = 0.5;
  double scale = 1;
  double rotation = 0;
  double opacity = 1;
  /** 伸縮（幅・高さだけの倍率。回転の前に掛ける。映像だけ） */
  double stretchX = 1;
  double stretchY = 1;
  Animation anim;
};

struct ExportClip {
  bool isImage = false;
  QString path;
  bool hasAudio = false;
  double in = 0;
  double duration = 0;
  double speed = 1;
  double volume = 1;
  /** 音量キーフレーム（クリップの先頭からのタイムライン上の秒数）。あるときは volume ではなくこちらで音量を変える */
  std::vector<VolumePoint> points;
  /** パン（-1 が左だけ、0 が真ん中、+1 が右だけ） */
  double pan = 0;
  /** 映像を出さない（トラックの非表示。その間は黒。音はそのまま） */
  bool hideVideo = false;
  LayerSpec layer;
  double fadeIn = 0;
  double fadeOut = 0;
  FitMode fit = FitMode::Contain;
  std::optional<ColorAdjust> color;
  /** 色の情報が付いていない動画に付ける setparams の値（例 "colorspace=bt709:range=tv"）。付いていれば空 */
  QString colorParams;
  /** 切り抜き（素材の左・上・右・下から切り落とす割合）。残した部分は、切り抜く前と同じ場所・大きさのまま */
  double cropL = 0;
  double cropT = 0;
  double cropR = 0;
  double cropB = 0;
  /** クロマキー（使うときだけ） */
  std::optional<ChromaKey> chroma;
  /** 反転とエフェクト（素材に、クロマキーの前に掛ける。プレビューと同じ順） */
  bool flipX = false;
  bool flipY = false;
  std::vector<Effect> effects;
  /** 素材の大きさ（正方ピクセルに直したあと。偶数）。エフェクトの大きさを決めるのに使う。0 なら分からない（エフェクトを掛けない） */
  int srcW = 0;
  int srcH = 0;
  /** 前のクリップからこのクリップへのトランジション（メインのトラックだけ。長さは transitionSpanFor でそろえて使う） */
  Transition transIn;
  /**
   * オブジェクトマスク（使うときだけ）。maskPath は Grayscale8 の PNG（素材の大きさ srcW×srcH、反転したあとの向き。255 が被写体）。
   * maskSequence なら連番のパターン（例 objmask-c2-%05d.png。0 番から、[cNsrc] のフレームと 1 対 1。写真は 1 枚）。
   * maskCutout なら背景を消す（透明度にする）、そうでなければエフェクトをマスクの中だけに掛ける
   */
  QString maskPath;
  bool maskSequence = false;
  bool maskCutout = true;
};

/** 重ねるトラックの映像・画像（表示方法はいつも全体表示で、余白は透明。位置・拡大などは clip.layer） */
struct ExportOverlay {
  ExportClip clip;
  /** タイムライン上の開始（秒） */
  double start = 0;
  /**
   * エフェクトレイヤー（素材なし。ここまでに重ねた画面の、範囲の中に clip.effects と clip.color を掛ける。長さは clip.duration）。
   * 範囲は maskPath の画像（Grayscale8 の PNG。255 が効果をそのまま掛ける所）。maskSequence なら連番のパターン
   * （例 mask-2-%05d.png。0 番から、重ねるフレーム数だけ並ぶ）、そうでなければ 1 枚を重ねる時間だけ伸ばす
   */
  bool effectLayer = false;
  QString maskPath;
  bool maskSequence = false;
};

struct PlannedText {
  /** 画像のファイル。sequence のときは、連番のパターン（例 text-3-%05d.png。0 番から、文字を重ねるフレーム数だけ並ぶ） */
  QString path;
  /**
   * 1 フレームごとに違う画像（字間が時間で変わる文字）。false なら、1 枚の画像を重ねる時間だけ伸ばす。
   * 連番の枚数は exportTextFrames() と同じにする
   */
  bool sequence = false;
  double start = 0;
  double duration = 0;
  TextAnimation animation = TextAnimation::None;
  /** transformed のときは、path は文字を画面の中央に置いた画像で、位置は layer.x, layer.y（とキーフレーム）で決める */
  LayerSpec layer;
};

struct ExportAudio {
  QString path;
  double start = 0;
  double in = 0;
  /** タイムライン上の長さ（秒） */
  double duration = 0;
  /** 読み出す速さ（重ねるトラックの動画の音。1 が元の速さ。音程はそのまま） */
  double speed = 1;
  double volume = 1;
  /** 音量キーフレーム（クリップの先頭からの秒数）。あるときは volume ではなくこちらで音量を変える */
  std::vector<VolumePoint> points;
  double fadeIn = 0;
  double fadeOut = 0;
  /** パン（-1 が左だけ、0 が真ん中、+1 が右だけ） */
  double pan = 0;
};

struct ExportRequest {
  QString outputPath;
  int width = 1920;
  int height = 1080;
  int fps = 30;
  ExportQuality quality = ExportQuality::Standard;
  ExportFormat format = ExportFormat::Mp4H264;
  double duration = 0;
  std::vector<ExportClip> clips;
  /** 重ねるトラック（この順に重ねる。後ろのものほど前。文字はその上） */
  std::vector<ExportOverlay> overlays;
  std::vector<ExportAudio> audios;
  /**
   * 書き出す範囲（タイムラインの秒数）。rangeEnd > rangeStart のときだけ、全体を組み立てたあとで、その範囲だけを切り出す
   * （フレームにそろえる）。そうでなければ全体
   */
  double rangeStart = 0;
  double rangeEnd = 0;
  /**
   * 動画全体の再生速度（1 が元の速さ）。全体を組み立てて範囲を切り出したあとで、映像はフレームの時間を 1/speed にして
   * フレームレートを合わせ直し、音は atempo（音程はそのまま）で、まとめて速さを変える
   */
  double speed = 1;
  /** エフェクトに使う画像（周辺減光の倍率など）を書く一時フォルダー。空なら、画像を使うエフェクトは掛けない */
  QString tempDir;
};

struct ExportPlan {
  QStringList inputArgs;
  QString filterGraph;
  /** 出力のオプション（-f の形式は muxer。ファイル名は呼ぶ側が最後に足す） */
  QStringList outputArgs;
  QString muxer = QStringLiteral("mp4");
  int totalFrames = 0;
  double duration = 0;
};

/** 指数表記を使わない数値の文字列化（JS の toFixed(6) と同じ） */
QString num6(double v);
/**
 * 音量キーフレームを ffmpeg の式（変数 t = クリップの先頭からの秒数）にする。
 * volumeAt() と同じ変わり方（端は保ち、間は直線）なので、プレビューと書き出しの音量が一致する
 */
QString volumeExpression(const std::vector<VolumePoint>& points, const QString& T = QStringLiteral("t"));
/**
 * 1 つの項目のキーフレームを ffmpeg の式にする。T は「オブジェクトの先頭からの秒数」を表す式（例 "t"、"(t-3.5)"）。
 * animValueAt() と同じ変わり方で、範囲は propLimits に収める
 */
QString keyframeExpression(const std::vector<Keyframe>& keys, double base, const QString& T, AnimProp prop);
/** 書き出す動画全体のフレーム数（buildExportPlan と同じ数え方） */
long long exportTotalFrames(const ExportRequest& req);
/** 書き出しのフレームレート（整数） */
int exportFps(const ExportRequest& req);
/**
 * 文字を重ねるフレーム数（buildExportPlan と同じ数え方）。動画の終わりまでに収め、開始が動画の外なら 0。
 * startFrame には、重ね始めるフレームを返す
 */
int exportTextFrames(const ExportRequest& req, double start, double duration, long long* startFrame = nullptr);

/**
 * メインのビデオのクリップを読む範囲（buildExportPlan と同じ数え方）。素材を readStart から読み、[cNsrc] は leadSec 秒ずらして
 * 書き出しのフレームレートで frames 枚（トランジションの前後に足す分も含む）にする。オブジェクトマスクの PNG を、同じフレームどうしで作るのに使う
 */
struct ClipReadWindow {
  double readStart = 0;
  double leadSec = 0;
  int frames = 0;
};
std::vector<ClipReadWindow> mainClipWindows(const ExportRequest& req);

ExportPlan buildExportPlan(const ExportRequest& req, const std::vector<PlannedText>& texts);

}  // namespace kv
