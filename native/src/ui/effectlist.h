#pragma once
// 設定欄の「エフェクト」の一覧（モザイク・ぼかしなど）。選んでいる映像（メイン・重ねるトラック・エフェクトレイヤー）の
// effects を、追加・削除・並べ替え・オン/オフ・値の変更で編集する。並べた順に上から掛ける。

#include <QString>
#include <QWidget>
#include <vector>

#include "core/model.h"

class QMenu;
class QPushButton;
class QVBoxLayout;
class QLabel;

namespace kv {

class EffectRow;

/** エフェクトの種類のアイコン名（設定欄・左の「エフェクト」タブ・右クリックで同じものを使う） */
QString effectIconName(EffectType t);
/**
 * 左の「エフェクト」タブのカードをドラッグするときの形式。中身は "transition:<名前>"（ビデオの切れ目へ）、
 * "effect:<名前>"（映像へ）、"layer:<見本の名前>"（重ねるトラックへ。エフェクトレイヤー）
 */
inline constexpr const char* kEffectMime = "application/x-e-editor-effect";

/** 「エフェクトを追加」のメニューを作る（layer: エフェクトレイヤー用の種類も出す）。選んだら clipId に足して、その ID を返す */
void fillAddEffectMenu(QMenu* menu, const QString& clipId, bool layer);
/** 映像 clipId にエフェクトを足す（並びの最後。元に戻す 1 回分）。足したエフェクトの ID（足せなければ空） */
QString addEffectToClip(const QString& clipId, EffectType type);
/** 選んでいる映像（一緒に選んだものも）にエフェクトを足す（元に戻す 1 回分）。足した数。選んでいなければ知らせて 0 */
int addEffectToSelection(EffectType type);
/**
 * トランジションを付ける: 選んでいるトランジションがあれば、その種類を変える。無ければ、再生位置に一番近い切れ目に付ける。
 * 付けたら（変えたら）true。切れ目が無いときは知らせて false
 */
bool applyTransitionNearPlayhead(TransitionType type);
/** エフェクトレイヤー（見本 presetId）を、再生位置の空いている段に置いて選ぶ。置いたものの ID */
QString addEffectLayerAtPlayhead(const QString& presetId);

class EffectList : public QWidget {
 public:
  explicit EffectList(QWidget* parent = nullptr);
  /** 編集する映像（メイン・重ねるトラック）。layer はエフェクトレイヤーか（塗りつぶしも選べる） */
  void setTarget(const QString& clipId, bool layer);
  /** 編集データに合わせて出し直す（並びが変わったときだけ作り直す。ドラッグ中のスライダーはそのまま） */
  void refresh();

 private:
  void rebuild(const std::vector<Effect>& effects);
  QString clipId_;
  bool layer_ = false;
  QString structure_;
  QVBoxLayout* rows_;
  QLabel* empty_;
  QPushButton* add_;
  QMenu* addMenu_;
  std::vector<EffectRow*> items_;
};

}  // namespace kv
