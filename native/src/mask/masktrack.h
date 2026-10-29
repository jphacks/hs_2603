#pragma once
// オブジェクトマスクのデータ（素材のフレームごとの、被写体のマスク）。
// AI が作ったマスクのロジット（256×256。素材のフレーム全体に対応）を、素材の時刻（VideoDecoder::framePts）ごとに持つ。
// ファイル（generated フォルダーの objmask-*.kvmask）は一度書いたら変えない（作り直すたびに新しいファイル。分割・複製した映像や
// 元に戻す履歴は、同じファイルをそのまま使う）。描くときに必要な大きさへ広げ、境目・広げる・ぼかし・反転を掛ける（renderMask）。
// プレビュー・静止画・書き出しで、同じ renderMask を使う。

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace kv::mask {

/** 1 フレーム分のマスク */
struct MaskFrame {
  /** 素材の時刻（秒。VideoDecoder::framePts と同じ数え方。写真は 0） */
  double pts = 0;
  /** 被写体が写っているか（0 より大きければ写っている） */
  float score = 0;
  /** 256×256 のロジットを 1/8 刻みの int8 にして zlib で縮めたもの */
  QByteArray packed;
};

class MaskTrack {
 public:
  /** 時刻の順 */
  std::vector<MaskFrame> frames;
  /** 作った素材の大きさ（向きを直したあと）。違う大きさの素材には使わない */
  int width = 0;
  int height = 0;
  double frameDuration = 1.0 / 30;

  bool empty() const { return frames.empty(); }
  /** 時刻 pts に一番近いフレーム（無ければ -1）。トラックした範囲の外は、端のフレームのまま */
  int nearest(double pts) const;
  /** フレーム i のロジット（256×256 の int8。値 / 8 がロジット）。読めなければ nullptr */
  std::shared_ptr<const std::vector<int8_t>> logits(int index) const;

  /** 中身の写し（読み込んだロジットの覚えは写さない） */
  std::shared_ptr<MaskTrack> clone() const;
  /** マスクのロジット（256×256 の float）を、保存する形にする */
  static QByteArray pack(const std::vector<float>& lowRes);
  bool save(const QString& path, QString* error = nullptr) const;
  static std::shared_ptr<MaskTrack> load(const QString& path, QString* error = nullptr);

 private:
  mutable std::mutex cacheMutex_;
  mutable std::vector<std::pair<int, std::shared_ptr<const std::vector<int8_t>>>> cache_;
};

/** マスクの描き方（大きさは、描く画像のピクセル） */
struct MaskLook {
  /** 広げる（プラス）・狭める（マイナス）幅 */
  double expand = 0;
  /** 境目のぼかし */
  double feather = 0;
  /** 被写体の外側をマスクにする */
  bool invert = false;
  /** 映像の反転（マスクも同じ向きにする） */
  bool flipX = false;
  bool flipY = false;
};

/** マスクの画像（Grayscale8、size の大きさ。255 が被写体）。ロジットは素材のフレーム全体に対応する */
QImage renderMask(const std::vector<int8_t>& logits, QSize size, const MaskLook& look);

/** マスクのファイルの置き場所（%LOCALAPPDATA%\JPHACKS\e-editor\generated） */
QString maskDir();
/** 新しいマスクのファイル名（フォルダーは付けない） */
QString newMaskFileName();

/**
 * 読み込んだマスクの置き場（どのスレッドから使ってもよい）。ファイル名で読み、少しのあいだ覚えておく。
 * トラックしている途中の結果（live）は、マスクの ID ごとに、できた所まで出す
 */
class MaskStore {
 public:
  static MaskStore& instance();
  /** ファイル名（maskDir の中）のマスク。読めなければ nullptr */
  std::shared_ptr<const MaskTrack> get(const QString& fileName);
  /** 書いたばかりのマスクを、読み直さずに使えるようにする */
  void put(const QString& fileName, std::shared_ptr<const MaskTrack> track);
  void setLive(const QString& maskId, std::shared_ptr<const MaskTrack> track);
  std::shared_ptr<const MaskTrack> live(const QString& maskId) const;
  void clearLive(const QString& maskId);

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<QString, std::shared_ptr<const MaskTrack>>> files_;
  std::vector<std::pair<QString, std::shared_ptr<const MaskTrack>>> live_;
  std::vector<QString> missing_;
};

}  // namespace kv::mask
