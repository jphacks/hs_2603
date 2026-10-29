#pragma once
// 編集状態。編集データ(project)は「元に戻す / やり直し」に対応する。

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "core/model.h"
#include "core/subtitles.h"
#include "core/timeline.h"

namespace kv {

/** ループ再生でくり返す範囲の、一番短い長さ（秒）。これより短い範囲はくり返さない（音が細切れになるので） */
constexpr double kMinLoopLength = 0.2;

struct Selection {
  /**
   * Clip はメインのビデオトラックのクリップ、Overlay は重ねるトラックの映像・画像。
   * Transition はメインのビデオの切れ目のトランジション（id は、切れ目の後ろのクリップの ID）
   */
  enum Type { None, Clip, Text, Audio, Overlay, Transition };
  Type type = None;
  QString id;
  bool operator==(const Selection&) const = default;
  explicit operator bool() const { return type != None; }
};

enum class ToastKind { Info, Success, Error };

ProjectData emptyProject();

class Editor : public QObject {
  Q_OBJECT
 public:
  static Editor& instance();

  // ---- 編集場所（タブ） ------------------------------------------------------
  // 編集場所はいくつでも持てる。それぞれが編集データ・元に戻す履歴・メディア一覧・選択・再生位置・
  // タイムラインの拡大率・保存先と「保存していない変更」の有無を別々に持つ。
  // 下の編集データや操作はすべて「今の編集場所」に対して働く。
  int workspaceCount() const { return int(ws_.size()); }
  int workspaceIndex() const { return active_; }
  /** タブに出す名前（利用者が付けた名前、なければ「編集N」） */
  QString workspaceName(int index) const;
  /** 空にすると「編集N」に戻る */
  void renameWorkspace(int index, const QString& name);
  bool workspaceDirty(int index) const;
  bool workspaceHasContent(int index) const;
  QString workspacePath(int index) const;
  /** 編集場所を閉じても変わらない番号。読み込み中の処理が、閉じられた編集場所を見分けるのに使う */
  int workspaceUid(int index) const;
  int indexOfWorkspaceUid(int uid) const;
  /** 保存先がそのファイルの編集場所の番号（なければ -1） */
  int workspaceWithPath(const QString& path) const;
  /** 空の編集場所を追加して切り替える。追加した番号を返す */
  int addWorkspace();
  /**
   * 読み込んだ内容を新しい編集場所として開き、切り替える。何も入っていない最初のままの編集場所があれば、
   * 新しく増やさずそこへ入れる。同じファイルを何度開いても ID が重ならないよう、ID は付け直す
   * （data の中の ID も付け直した値になる）。開いた番号を返す
   */
  int openWorkspace(WorkspaceData& data);
  /** 閉じる。確認は呼ぶ側で行う。最後の 1 つを閉じたときは、空の編集場所に置き換える */
  void closeWorkspace(int index);
  /** 編集場所を切り替える。再生は止める。同じ場所なら何もしない */
  void switchWorkspace(int index);
  /** どれかの編集場所に、タイムラインに置いたものがあるか */
  bool hasAnyContent() const;
  /** 全編集場所の内容（自動保存用。メディア一覧は使用中のものを含む） */
  ProjectFile snapshot() const;
  /** 1 つの編集場所の内容（保存用） */
  WorkspaceData snapshotOf(int index) const;
  /** タイムラインのスクロール位置は画面側のものだが、編集場所ごとに覚えておく */
  QPoint viewScroll() const { return {ws().scrollX, ws().scrollY}; }
  void setViewScroll(int x, int y) {
    ws().scrollX = x;
    ws().scrollY = y;
  }

  // ---- 編集データ ----------------------------------------------------------
  const ProjectData& project() const { return ws().project; }
  const TimelineLayout& layout() const;
  double contentEnd() const;
  /** 再生を止める時刻（ビデオがあればビデオの終わり） */
  double playEnd() const;
  int revision() const { return ws().revision; }
  bool canUndo() const { return !ws().past.empty(); }
  bool canRedo() const { return !ws().future.empty(); }
  bool dirty() const { return ws().dirty; }
  const QString& projectPath() const { return ws().path; }

  /** fn が true を返したときだけ変更として記録する。merge が前回と同じなら 1 回の操作にまとめる */
  void commit(const std::function<bool(ProjectData&)>& fn, const QString& merge = QString());
  // ---- 取り消せる作業のまとまり（プレビュー上の文字の直接編集、タイムラインのドラッグなど） ----------------
  // 編集データに反映しながら続ける作業を、元に戻す履歴の 1 つの編集操作にまとめる（merge が同じ間は 1 回）。
  // 始めるときの状態を覚えておき、キャンセルすると、履歴に残さずその状態に戻す。
  /** 始める。これまでの変更とは別の操作として記録し、キャンセルのために今の状態を覚える */
  void beginEditSession();
  /** 始めたときの状態に戻す（履歴には残さない）。始めてから何も変えていなければ何もしない */
  void cancelEditSession();
  /** 確定する。変更は、始めてからの 1 つの編集操作として履歴に残っている */
  void endEditSession();
  bool editSessionActive() const { return editSession_.active; }

  /** スライダーを離したときなど、次の変更を別の操作として記録する */
  void endMerge() {
    ws().lastMerge.clear();
    lastRejectMerge_.clear();
  }
  /**
   * fn の中で行った変更を、今の編集場所には残さずに試し、変更後の編集データを返す（AI の案のプレビュー用）。
   * fn の間だけ今の編集場所を写しに差し替え、画面には何も知らせない（シグナルを止める）。
   * 終わると、編集データ・元に戻す履歴・選択・再生位置・保存状態は、試す前のままに戻る
   */
  ProjectData tryChanges(const std::function<void()>& fn);
  void undo();
  void redo();

  // ---- 選択・再生位置 --------------------------------------------------------
  /** 主に選んでいるもの（設定欄に出すもの） */
  const Selection& selection() const { return ws().selection; }
  /** 1 つだけ選ぶ（一緒に選んでいたものは外す） */
  void select(const Selection& sel);
  // ---- 複数の選択とグループ（テキスト・オーディオ・重ねるトラックだけ。メインのビデオクリップは 1 つずつ） ----
  // Ctrl+クリックで一緒に選んだものと、選んだものと同じグループのものは、まとめて動かす・削除・複製する。
  /** テキスト・オーディオ・重ねるトラックか（一緒に選べる・グループにできる種類か） */
  static bool groupable(Selection::Type t) { return t == Selection::Text || t == Selection::Audio || t == Selection::Overlay; }
  /** Ctrl+クリック: 一緒に選ぶものに加える / 外す。一緒に選べない種類なら、ふつうに 1 つだけ選ぶ */
  void toggleSelect(const Selection& sel);
  /** 主に選ぶものを sel にする（一緒に選んでいるものはそのまま。sel が一緒に選んでいるものの中にあるとき） */
  void focusInSelection(const Selection& sel);
  /** 一緒に選んでいるもの（主に選んでいるものは含まない） */
  const std::vector<Selection>& extraSelection() const { return ws().extra; }
  /** まとめて扱うもの: 主に選んでいるもの・一緒に選んでいるもの・それらと同じグループのもの（主に選んでいるものが先頭） */
  std::vector<Selection> selectionSet() const;
  bool inSelectionSet(const Selection& sel) const;
  /** そのもののグループ（無ければ空） */
  QString groupOf(const Selection& sel) const;
  /** まとめて扱うもの（2 つ以上）を 1 つのグループにする */
  bool groupSelection();
  /** まとめて扱うもののグループを解く */
  bool ungroupSelection();
  bool canGroup() const;
  bool canUngroup() const;
  double currentTime() const { return ws().currentTime; }
  int seekToken() const { return seekToken_; }
  void seek(double t);
  void setTimeFromEngine(double t);
  bool playing() const { return playing_; }
  void play();
  void pause();
  void togglePlay();
  double pxPerSec() const { return ws().pxPerSec; }
  void setPxPerSec(double v);
  void requestFit() { emit fitRequested(); }
  void toast(const QString& message, ToastKind kind = ToastKind::Info) { emit toastRequested(message, kind); }

  // ---- クロマキーの色をプレビューから選ぶ（スポイト） ----
  /**
   * 映像 clipId（メイン・重ねるトラック）の透明にする色を、プレビューのクリックで選ぶ状態にする。空ならやめる。
   * 選んでいる間、プレビューはその映像のクロマキーを外して元の色で出す。ほかのものを選ぶ・再生する・
   * 編集場所を切り替える・その映像が無くなると、やめる
   */
  void setChromaPick(const QString& clipId);
  const QString& chromaPick() const { return chromaPick_; }

  // ---- オブジェクトマスクの被写体をプレビューから選ぶ ----
  /**
   * 映像 clipId（メイン・重ねるトラック）の被写体を、プレビューでカーソルを合わせてクリックして選ぶ状態にする。空ならやめる。
   * 選んでいる間、プレビューはその映像のマスクを外して出す。スポイトと同じく、ほかのものを選ぶ・再生する・
   * 編集場所を切り替える・その映像が無くなると、やめる（スポイトとは同時に使わない）
   */
  void setMaskPick(const QString& clipId);
  const QString& maskPick() const { return maskPick_; }
  /**
   * 同じマスクの目印（ObjectMask::id）の映像すべてに fn を掛ける（分割した映像など。トラックの結果を入れるとき）。
   * 1 回で元に戻せる。変えた映像の数
   */
  int updateMasks(const QString& maskId, const std::function<void(ObjectMask&)>& fn);

  // ---- ループ再生（終わりまで行ったら、始めに戻って続ける。書き出す範囲があれば、その範囲をくり返す。アプリの設定に覚える） ----
  bool loopPlayback() const { return loop_; }
  void setLoopPlayback(bool on);

  // ---- プレビューのガイド（セーフマージン・三分割のグリッド。見るためだけで、書き出す動画には出ない。アプリの設定に覚える） ----
  bool safeMarginsVisible() const { return safeMargins_; }
  bool gridVisible() const { return grid_; }
  void setSafeMarginsVisible(bool on);
  void setGridVisible(bool on);

  // ---- 書き出す範囲（タイムラインの一部だけを書き出すとき。編集場所ごとに持ち、プロジェクトには保存しない） ----
  struct TimeRange {
    double start = 0;
    double end = 0;
  };
  /** 決めてある範囲（今の内容の長さに収めたもの。1 フレームより短くなったら空）。決めていなければ空 */
  std::optional<TimeRange> exportRange() const;
  /** 範囲の開始を t に（フレームにそろえる）。終了がまだ無い・開始より前なら、終了は内容の終わりにする */
  void setRangeStart(double t);
  /** 範囲の終了を t に（フレームにそろえる）。開始がまだ無い・終了より後ろなら、開始は 0 にする */
  void setRangeEnd(double t);
  /** a〜b を範囲にする（順番はどちらでもよい） */
  void setRange(double a, double b);
  void clearRange();
  /** ループ再生でくり返す範囲（書き出す範囲があればその範囲、無ければ 0〜再生の終わり） */
  TimeRange loopRange() const;

  // ---- メディア ------------------------------------------------------------
  const std::map<QString, Asset>& assets() const { return ws().assets; }
  const Asset* asset(const QString& id) const;
  /** どれかのタブにそのメディアがあるか */
  bool hasAsset(const QString& id) const;
  const std::vector<QString>& assetOrder() const { return ws().assetOrder; }
  /**
   * workspaceUid が負のときは今の編集場所。読み込みが終わるまでに切り替えても、取り込んだ編集場所に入る。
   * その編集場所がもう閉じられていたら何もしない
   */
  void addAsset(const Asset& a, int workspaceUid = -1);
  /** メディアの ID は全体で重複しないので、どの編集場所にあっても見つけて更新する */
  void updateAsset(const QString& id, const std::function<void(Asset&)>& fn);
  /**
   * メディアを別のファイルにつなぎ直す（リンクし直す）。fn でファイルの場所・長さなどを書き換える。
   * プロジェクトに保存する内容が変わるので、保存していない変更として扱い、画面も出し直す（元に戻す履歴には入れない）
   */
  void relinkAsset(const QString& id, const std::function<void(Asset&)>& fn);
  bool removeAsset(const QString& id);
  const std::vector<QImage>* thumbs(const QString& assetId) const;
  void setThumbs(const QString& assetId, std::vector<QImage> images);
  const Waveform* waveform(const QString& assetId) const;
  void setWaveform(const QString& assetId, Waveform w);

  // ---- 編集操作 ------------------------------------------------------------
  void addToTimeline(const std::vector<QString>& assetIds, std::optional<int> index = std::nullopt,
                     std::optional<double> at = std::nullopt);
  void updateClip(const QString& id, const std::function<void(VideoClip&)>& fn, const QString& merge = QString());
  void moveClip(const QString& id, int toIndex);
  /** テキストを追加して選ぶ。位置を省略すると再生位置。動画の終わりぎわなら少し手前にずらす */
  QString addText(const QString& presetId, std::optional<double> at = std::nullopt);
  /** 指定した開始時間（フレームにそろえる）にそのまま追加して選ぶ。ドラッグ＆ドロップでの追加に使う */
  QString addTextAt(const QString& presetId, double start);
  /** その開始時間に追加するテキストの表示時間（wanted の長さ。動画の終わりをまたぐときは終わりまで） */
  double newTextDuration(double start, double wanted = kDefaultTextDuration) const;
  /** いま追加すると、そのテキストが入る段（追加前の表示に使う。何も変えない） */
  int textTrackFor(double start, double duration) const;
  /**
   * テキストを上（負）・下（正）の段へ移す。横に動かす・長さを変えるだけでは段は変わらず、
   * 段を変えるのはこの操作と、タイムラインで上下にドラッグしたときだけ。移った先の段のその時間に
   * ほかのテキストがあるときは、重ならないよう、そのテキストのすぐ上（上へ移すとき）・すぐ下（下へ移すとき）に新しい段を作って入れる
   */
  void moveTextTrack(const QString& id, int delta);
  /** テキストの段を target へ移す（段の境目に新しい段を差し込むこともできる。moveTextToTrack を参照）。変わったら true */
  bool placeTextTrack(const QString& id, const TextTrackTarget& target, const QString& merge = QString());
  /**
   * テキスト（kind = Text）かオーディオ（kind = Audio）の段 a と b を入れ替える（タイムラインの見出しの列のドラッグ）。
   * 段の中のものはまとめて動き、時間・長さは変わらない。元に戻す 1 回分。入れ替えたら true
   */
  bool swapTracks(Selection::Type kind, int a, int b);
  void updateText(const QString& id, const std::function<void(TextItem&)>& fn, const QString& merge = QString());
  void updateAudio(const QString& id, const std::function<void(AudioItem&)>& fn, const QString& merge = QString());
  // ---- 重ねるトラック ----
  /**
   * 動画・写真を重ねるトラックに追加して選ぶ。at から順に並べる（省略すると再生位置）。track を省略すると、重ならない段へ入れる。
   * 追加したもののうち最初の ID を返す
   */
  QString addOverlays(const std::vector<QString>& assetIds, std::optional<double> at = std::nullopt, std::optional<int> track = std::nullopt);
  void updateOverlay(const QString& id, const std::function<void(OverlayItem&)>& fn, const QString& merge = QString());
  /**
   * エフェクトレイヤー（モザイク・ぼかしなど。core/effects の effectLayerPresets の見本）を、重ねるトラックに追加して選ぶ。
   * at を省略すると再生位置。track を省略すると、その時間に重ねている映像より上の、空いている段へ入れる（下の映像にも効果が掛かるように）。
   * 追加したものの ID を返す
   */
  QString addEffectLayer(const QString& presetId, std::optional<double> at = std::nullopt, std::optional<int> track = std::nullopt);
  /** 重ねるトラックの段を、上（正 = 前）・下（負 = 後ろ）へ移す。一番上の段のさらに上へ 1 段だけ増やせる */
  void moveOverlayTrack(const QString& id, int delta);
  // ---- メインのビデオと重ねるトラックの間の移動（タイムラインで上下にドラッグしたとき） ----
  // ID と映像の設定（切り出し・速度・音量・フェード・色・切り抜き・クロマキー・アニメーションなど）は、そのまま持っていく。
  /**
   * メインのビデオのクリップ id を、重ねるトラックの段 track の start 秒へ移して選ぶ。後ろのクリップは前へ詰まる。
   * track は 0 がメインのすぐ上で、一番上の段のさらに上へ 1 段だけ増やせる。表示方法は重ねるトラックのいつもの「全体表示」。
   * 位置・大きさを変えていないクリップは、重ねるトラックに追加したときと同じ大きさ（画面の半分）にする（重なっているのが分かるように）。
   * 元に戻す 1 回分。移したら true
   */
  bool moveClipToOverlay(const QString& id, double start, int track);
  /**
   * 重ねるトラックの映像 id を、メインのビデオの index 番目へ移して選ぶ。重ねるトラックに追加したときの大きさ（画面の半分・中央）のままなら、
   * 画面いっぱいに戻す。表示方法は、メインのビデオに追加したときと同じにする（画面と縦横比が違う素材は「ぼかし背景」）。
   * merge が前の変更と同じなら、1 回の操作にまとめる（ドラッグの途中の動きと一緒に、元に戻す 1 回で戻る）。移したら true
   */
  bool moveOverlayToMain(const QString& id, int index, const QString& merge = QString());
  // ---- オーディオの音量キーフレーム（時間は、オーディオの先頭からの秒数） ----
  /** その時間にキーフレームを追加する（今の音量で作るので、音は変わらない）。近くに既にあるときは増やさない。その番号を返す */
  int addVolumePoint(const QString& audioId, double local);
  /**
   * 音量を再生位置の値として変える（映像・オーディオ共通）。キーフレームが無ければ全体の音量。ある場合は、再生位置に
   * キーフレームがあればその音量を更新し、無ければ自動記録がオンのときだけ追加する（オフなら何も変えず false）
   */
  bool setVolumeValue(const QString& id, double value, const QString& merge = QString());
  /** キーフレームの時間と音量（0〜100%）を変える。merge が同じ間は 1 回の操作にまとめる */
  void moveVolumePoint(const QString& audioId, int index, double local, double volume, const QString& merge = QString());
  void removeVolumePoint(const QString& audioId, int index);
  void clearVolumePoints(const QString& audioId);
  // ---- キーフレームアニメーション（選んでいる映像・文字が対象。時間は、そのオブジェクトの先頭からの秒数） ----
  struct KeyRef {
    AnimProp prop = AnimProp::X;
    int index = -1;
    bool operator==(const KeyRef&) const = default;
  };
  struct KeyMove {
    KeyRef key;
    double t = 0;  // 動かした先の時間
  };
  struct AnimPreset {
    QString id;
    QString label;
    QString hint;
  };
  /** 映像・文字を選んでいるか（アニメーションできる対象か） */
  bool animatable() const;
  /** 選んでいる映像・文字のアニメーション（対象でなければ nullptr） */
  const Animation* animation() const;
  /** 選んでいるオブジェクトのタイムライン上の開始と長さ */
  double objectStart() const;
  double objectDuration() const;
  /** 再生位置の、オブジェクトの先頭からの秒数（外のときは範囲内に収めない） */
  double objectLocalTime() const { return currentTime() - objectStart(); }
  /** 選んでいるオブジェクトの、今の再生位置での位置・拡大率・回転・不透明度 */
  LayerState currentLayerState() const;
  /** 自動キーフレーム記録: ON のとき、キーフレームのある項目を再生位置で変えると、その場にキーフレームを記録する */
  bool autoKey() const { return autoKey_; }
  void setAutoKey(bool on);
  /**
   * 選んでいるオブジェクトの項目 p の値を、今の再生位置の値として変える。
   * キーフレームが無い項目は静的な値を変える。ある項目は、再生位置にキーフレームがあればその値を更新し、無ければ
   * 自動記録が ON のときだけ記録する（OFF のときは何も変えない）。変更を受け付けたら true
   */
  bool setPropValue(AnimProp p, double value, const QString& merge = QString());
  /** 再生位置にキーフレームを追加する。既にあるときは取り除く（◆ボタン）。追加したら true */
  bool toggleKey(AnimProp p);
  /** 前後のキーフレームの時間へ再生位置を動かす。動かせたら true */
  bool seekToKey(AnimProp p, bool forward);
  void removeKeys(const std::vector<KeyRef>& keys);
  /** キーフレームの値だけを変える（時間・補間方式は変えない）。merge が同じ間は 1 回の操作にまとめる */
  bool setKeyValue(AnimProp p, int index, double value, const QString& merge = QString());
  /** キーフレームの時間を変える（動かす先がまだ並び・最小間隔・範囲に収まるときだけ）。merge が同じ間は 1 回の操作にまとめる */
  bool moveKeys(const std::vector<KeyMove>& moves, const QString& merge = QString());
  void setKeyEasing(const std::vector<KeyRef>& keys, Easing e);
  // コピー・貼り付け: 値と補間方式を保つ。貼り付けは、選んでいるオブジェクトの再生位置から
  bool copyKeys(const std::vector<KeyRef>& keys);
  bool hasKeyClipboard() const { return !keyClip_.empty(); }
  int pasteKeys(double local);
  // プリセット: 必要なキーフレームを作る。作ったあとも、キーフレームは自由に編集できる
  static const std::vector<AnimPreset>& animPresets();
  bool applyAnimPreset(const QString& presetId);
  /**
   * マイテンプレート（保存したテキストのアニメーション）を、選んでいるテキストに当てはめる。キーフレームと登場のアニメーションを
   * 置き換える（文字・見た目は変えない。当てはめたあとも、キーフレームは自由に編集できる）。元に戻す 1 回分。当てはめたら true
   */
  bool applyAnimTemplate(const QString& templateId);
  // 音量キーフレームは、オーディオにも映像(音声つき)にも使える。ID でどちらかを探す
  void deleteSelection();
  void duplicateSelection();
  // ---- コピー・貼り付け（タブをまたいで使える。編集2 で写して、編集1 に貼り付けるなど） ----
  /** 選んでいるもの（一緒に選んだもの・同じグループのものも）を、アプリの中のクリップボードに写す。写したものの数 */
  int copySelection();
  bool hasItemClipboard() const { return !itemClip_.empty(); }
  /**
   * 写したものを、今の編集場所の再生位置（at を渡せばその時間）に貼り付ける（並びと間隔はそのまま。1 回の「元に戻す」で消える）。
   * ビデオのクリップは clipIndex の位置（渡さなければ、その時間にあるクリップのうしろ。クリップの頭なら前）に入れる。
   * 使っているメディアがこの編集場所の一覧に無ければ、写しを一覧に足す（同じファイルがあれば、それを使う）。
   * 貼り付けた数を返し、added に足したメディアを入れる（呼ぶ側で、サムネイルなどを作る）
   */
  int pasteClipboard(std::vector<Asset>* added = nullptr, std::optional<double> at = std::nullopt, std::optional<int> clipIndex = std::nullopt);
  /** 写したものにビデオのクリップが入っているか（ドラッグで貼り付けるとき、差し込む位置を見せる） */
  bool clipboardHasClips() const { return !itemClip_.clips.empty(); }
  void splitAtPlayhead();
  // ---- Premiere と同じ編集の近道 ----
  /**
   * 再生位置まで詰める（Q: 前を詰める / W: 後ろを詰める）。選んでいるテキスト・オーディオ・重ねるものに再生位置がかかっていれば、その端を
   * 再生位置まで詰める（ほかは動かさない）。そうでなければ、再生位置のメインのビデオのクリップを詰め、後ろのクリップ・テキスト・オーディオ・
   * 重ねるものも、詰めた分だけ前へ詰める（リップル）。詰めたら true
   */
  bool rippleTrimToPlayhead(bool start);
  /** 再生位置にかかっているもの（メインのビデオ・重ねるトラック・テキスト・オーディオ）を、すべて再生位置で分ける。分けた数 */
  int splitAllAtPlayhead();
  /**
   * 選んでいるものを削除して、空いた所を詰める（リップル削除）。メインのビデオのクリップなら、その後ろにあるテキスト・オーディオ・重ねるものも
   * 一緒に前へ詰める。テキスト・オーディオ・重ねるものなら、同じ段の後ろのものを詰める。削除したら true
   */
  bool rippleDeleteSelection();
  /** その種類（Text / Audio / Overlay）の段 track の、時刻 t の空いた所（隙間）を詰める。詰めたら true */
  bool closeGap(Selection::Type kind, int track, double t);
  /** 隙間 [start, end) を返す（t が何かの上なら false）。closeGap と同じ決め方 */
  bool gapAt(Selection::Type kind, int track, double t, double* start, double* end) const;
  /**
   * 字幕（SRT などから読んだもの）を、スタイル presetId のテキストとして並べる（元に戻す 1 回分）。
   * その時間に何も無い段にまとめて置く（無ければ一番下に新しい段）。字幕どうしが重なるときは、前の字幕を次の字幕の始まりで切る。並べた数
   */
  int importSubtitles(const std::vector<SubtitleCue>& cues, const QString& presetId);

  /** 前（forward = false）・次の編集点（クリップ・テキスト・オーディオの端）へ再生位置を動かす。動いたら true */
  bool seekToEdit(bool forward);
  /** 選んでいるテキスト・オーディオ・重ねるもの（一緒に選んだものも）を、frames フレームずらす（同じ段のほかのものに当たるなら動かさない） */
  bool nudgeSelection(int frames);
  /**
   * 映像（メイン・重ねるトラック）の音を、同じ時間のオーディオとして切り離す（映像の音は消す。音量・キーフレーム・フェードはそのまま）。
   * 速度を変えた映像は切り離せない。切り離したオーディオの ID（できなければ空。理由は知らせる）
   */
  QString detachAudio(const QString& clipId);
  /**
   * フレームの保持: 再生位置でメインのクリップを分け、写真の素材 imageAssetId（そのフレームを画像にしたもの）を seconds 秒、間に入れる。
   * 入れた写真のクリップは、元のクリップと同じ表示方法・色・エフェクト・位置などにする（キーフレームは、その時間の値にする）。入れたら true
   */
  bool insertFrameHold(const QString& clipId, double local, const QString& imageAssetId, double seconds,
                       const std::optional<ObjectMask>& holdMask = std::nullopt);
  /**
   * 逆再生にする / やめる。reversedAssetId は、クリップの素材を逆向きに作り直した動画（やめるときは空）。素材を入れ替えて、
   * 切り出す所を逆向きの動画の時刻に直す（同じ所を逆向きに映す。タイムラインの長さ・キーフレームはそのまま）。1 回で元に戻せる。変えたら true
   */
  bool setClipReversed(const QString& clipId, const QString& reversedAssetId);

  /** 選んでいる映像の、今の設定（色・エフェクト・位置など）を写しておく（属性のペースト用） */
  bool copyAttributes();
  bool hasAttributes() const { return attrClip_.has_value(); }
  /** 写しておいた設定のうち、parts に入っているものを、選んでいる映像（一緒に選んだものも）に貼り付ける。貼り付けた数 */
  enum AttrPart {
    AttrColor = 1,
    AttrEffects = 2,
    AttrMotion = 4,
    AttrCrop = 8,
    AttrChroma = 16,
    AttrFade = 32,
    AttrVolume = 64,
    AttrSpeed = 128,
    AttrMask = 256,  // オブジェクトマスク（同じ素材ならマスクごと、違う素材なら使い方の設定だけ）
  };
  int pasteAttributes(int parts);

  // ---- トランジション（メインのビデオの切れ目。後ろのクリップの transIn に持つ） ----
  /**
   * 後ろのクリップ clipId に入るトランジションを付ける・種類を変える（長さは、省略すると今の長さか、はじめの長さ）。
   * 先頭のクリップには付けられない。付けたら選んで true
   */
  bool setTransition(const QString& clipId, TransitionType type, std::optional<double> duration = std::nullopt, const QString& merge = QString());
  /** トランジションの長さを変える（変えたら true）。次に付けるトランジションの長さは、rememberTransitionDefault で覚える */
  bool setTransitionDuration(const QString& clipId, double seconds, const QString& merge = QString());
  /** トランジションの間、音もクロスフェードするか（変えたら true） */
  bool setTransitionAudioCrossfade(const QString& clipId, bool on);
  /** そのクリップのトランジションの種類・長さを、次に付けるトランジションの既定として覚える（ドラッグ・スライダーを離したとき） */
  void rememberTransitionDefault(const QString& clipId);
  bool removeTransition(const QString& clipId);
  /** すべての切れ目に、同じトランジションを付ける（付けた数） */
  int applyTransitionToAll(TransitionType type, double duration, bool audioCrossfade = true);
  /** すべての切れ目のトランジションを外す（外した数） */
  int removeAllTransitions();
  /** 時刻 t に一番近い切れ目の、後ろのクリップの ID（maxDistance 秒より遠ければ空） */
  QString nearestCut(double t, double maxDistance) const;
  /** トランジションを付けたときの、はじめの種類・長さ（最後に選んだもの。アプリの設定に覚える） */
  static TransitionType defaultTransitionType();
  static double defaultTransitionDuration();
  static void setDefaultTransition(TransitionType type, double duration);

  // ---- マーカー（目盛りの上の印。名前・色・メモ） ----
  /** 時刻 t にマーカーを置く（1 フレーム以内にあれば増やさず、それを返す）。置いたマーカーの ID */
  // ---- トラックの非表示・ミュート（タイムラインの見出しの目・スピーカーの印。元に戻せる。保存もする） ----
  enum class TrackFlag {
    MainVideoHidden,  // メインのビデオの映像を出さない（lane は使わない）
    MainAudioMuted,   // メインのビデオの音を出さない（lane は使わない）
    TextHidden,       // テキストの段 lane（0 が一番上）を出さない
    OverlayHidden,    // 重ねるトラックの段 lane（0 がメインのすぐ上）の映像を出さない
    OverlayMuted,     // 重ねるトラックの段 lane の音を出さない
    AudioMuted,       // オーディオの段 lane（0 が一番上）の音を出さない
  };
  bool trackFlag(TrackFlag flag, int lane = 0) const;
  /** 変えたら true */
  bool setTrackFlag(TrackFlag flag, int lane, bool on);
  /** 非表示・ミュートにしている段があるか */
  bool anyTrackFlag() const;
  /** すべての段を表示して、ミュートも外す */
  void clearTrackFlags();

  QString addMarker(double t);
  const Marker* marker(const QString& id) const;
  void updateMarker(const QString& id, const std::function<void(Marker&)>& fn, const QString& merge = QString());
  void removeMarker(const QString& id);
  void clearMarkers();
  /** 再生位置より後ろ（forward）・前の、一番近いマーカーへ動く。動いたら true */
  bool seekToMarker(bool forward);

  void setAspect(Aspect a);
  void setFps(int fps);
  /** 動画全体の再生速度（kMinProjectSpeed〜kMaxProjectSpeed）。プレビューの再生と書き出しに効く */
  void setProjectSpeed(double speed);

  /** メインのビデオクリップと、重ねるトラックの映像・画像（どちらも映像の設定は同じ） */
  const VideoClip* clip(const QString& id) const;
  const OverlayItem* overlay(const QString& id) const;
  const TextItem* text(const QString& id) const;
  const AudioItem* audio(const QString& id) const;
  /** 映像（メイン・重ねるトラック）のタイムライン上の開始と長さ。見つからなければ false */
  bool clipSpan(const QString& id, double* start, double* duration) const;

  /** 全編集場所を入れ替える（前回の編集内容の復元） */
  void resetSession(const ProjectFile& file);
  /** 今の編集場所を、このファイルに保存したことにする */
  void markSaved(const QString& path);

 signals:
  void projectChanged();
  void selectionChanged();
  void timeChanged();
  void playingChanged();
  void assetsChanged();
  void thumbsChanged(const QString& assetId);
  void waveformChanged(const QString& assetId);
  void zoomChanged();
  void fitRequested();
  void dirtyChanged();
  void toastRequested(const QString& message, kv::ToastKind kind);
  void projectReset();
  /** 今の編集場所が変わる直前（画面側が、いまの表示位置などを覚えておくため） */
  void workspaceAboutToChange();
  void autoKeyChanged();
  /** 書き出す範囲が変わった */
  void rangeChanged();
  /** スポイト（クロマキーの色をプレビューから選ぶ）を始めた / やめた */
  void chromaPickChanged();
  /** オブジェクトマスクの被写体を選び始めた / やめた */
  void maskPickChanged();
  /** ループ再生のオン・オフが変わった */
  void loopChanged();
  /** プレビューのガイド（セーフマージン・グリッド）の表示が変わった */
  void guidesChanged();
  /** 属性のコピー（copyAttributes）で、貼り付けられる設定ができた */
  void attributesChanged();
  /** 今の編集場所が変わった。このあと projectChanged などで中身の更新も通知される */
  void workspaceChanged(int index);
  /** タブの一覧が変わった（追加・削除・名前・保存状態） */
  void workspacesChanged();
  /** 編集場所を閉じた。そのメディアのためにとっておいたものを手放してよい */
  void workspaceClosed(const QStringList& assetIds);

 private:
  Editor();

  /** 編集場所ごとの状態 */
  struct Workspace {
    int uid = 0;
    int number = 1;
    QString customName;
    QString path;
    bool dirty = false;
    ProjectData project = emptyProject();
    std::vector<ProjectData> past;
    std::vector<ProjectData> future;
    QString lastMerge;
    int revision = 0;
    mutable int layoutRevision = -1;
    mutable TimelineLayout layout;
    std::map<QString, Asset> assets;
    std::vector<QString> assetOrder;
    Selection selection;
    /** 一緒に選んでいるもの（Ctrl+クリック。テキスト・オーディオ・重ねるトラックだけ） */
    std::vector<Selection> extra;
    double currentTime = 0;
    double pxPerSec = 60;
    int scrollX = 0;
    int scrollY = 0;
    /** 書き出す範囲（決めていなければ空） */
    std::optional<TimeRange> range;
  };
  Workspace& ws() { return ws_[size_t(active_)]; }
  const Workspace& ws() const { return ws_[size_t(active_)]; }
  /** ID のメディアを持つ編集場所（なければ nullptr） */
  Workspace* workspaceOfAsset(const QString& id);
  /** 空の編集場所を作る。まだ使われていない「編集N」の番号を割り当てる */
  Workspace makeWorkspace();
  /** 今の編集場所が変わったことを、画面に知らせる */
  void announceActivated();
  /** 閉じる編集場所のために持っているサムネイル・波形を手放す */
  QStringList releaseAssetsOf(const Workspace& w);

  void afterProjectChange();
  Selection validSelection(const Selection& s) const;
  void setDirty(bool d);

  // 少なくとも 1 つはある。切り替えで動かすのは active_ だけで、中身のコピーや移動はしない
  std::vector<Workspace> ws_;
  int active_ = 0;
  int nextUid_ = 1;

  // サムネイルと波形はメディアの ID で引くので、編集場所をまたいで 1 つの置き場にまとめる
  std::map<QString, std::vector<QImage>> thumbs_;
  std::map<QString, Waveform> waveforms_;

  /** copySelection で写したもの（編集場所をまたいで使う） */
  struct ItemClipboard {
    std::vector<VideoClip> clips;
    std::vector<OverlayItem> overlays;
    std::vector<TextItem> texts;
    std::vector<AudioItem> audios;
    /** 写したものが使っているメディア（貼り付け先の一覧に無ければ足す） */
    std::vector<Asset> assets;
    /** テキスト・オーディオ・重ねるものの、いちばん早い開始（貼り付けると、ここが再生位置になる） */
    double origin = 0;
    bool empty() const { return clips.empty() && overlays.empty() && texts.empty() && audios.empty(); }
    int count() const { return int(clips.size() + overlays.size() + texts.size() + audios.size()); }
  };
  ItemClipboard itemClip_;

  int seekToken_ = 0;
  /** 最後に 1 フレームずつ動かしたときの seekToken_（間に再生位置を動かしたら、続けて押しても別の操作にする） */
  int nudgeSeekToken_ = -1;
  bool playing_ = false;
  /** スポイトで色を選んでいる映像（選んでいなければ空） */
  QString chromaPick_;
  /** オブジェクトマスクの被写体を選んでいる映像（選んでいなければ空） */
  QString maskPick_;
  bool loop_ = false;
  bool safeMargins_ = false;
  bool grid_ = false;
  /** copyAttributes で写した映像の設定 */
  std::optional<VideoClip> attrClip_;
  /** 写した設定が、エフェクトレイヤーのものか */
  bool attrFromLayer_ = false;

  /** 取り消せる作業を始めたときの状態（キャンセルで戻す） */
  struct EditSession {
    bool active = false;
    int uid = 0;
    int revision = 0;
    bool dirty = false;
    ProjectData project;
    size_t pastSize = 0;
    /** 履歴がいっぱいのとき、始めたあとの最初の変更で押し出される、いちばん古い履歴 */
    std::optional<ProjectData> droppedFront;
    std::vector<ProjectData> future;
  };
  EditSession editSession_;

  bool autoKey_ = true;
  struct ClipKey {
    AnimProp prop = AnimProp::X;
    double dt = 0;
    double value = 0;
    Easing easing = Easing::Linear;
  };
  std::vector<ClipKey> keyClip_;
  QString lastRejectMerge_;
};

inline Editor& editor() {
  return Editor::instance();
}

FitMode defaultFit(const Asset& a, Aspect aspect);

}  // namespace kv
