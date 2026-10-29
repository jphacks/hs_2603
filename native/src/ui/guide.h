#pragma once
// AI の「操作の案内」。AI が show_guide を使うと、画面の上に薄い矢印（ポインター）・押す所の枠・短い説明を出して、
// 押す場所を順に示す。示した所が押されたら、次の手順へ進む。案内は画面を指すだけで、編集はしない（元に戻す履歴も変えない）。
// 選ぶ・タブやメニューを開くといった準備が要るときは、その準備の場所を先に示す（ai/guidecatalog.h の need・view・via）

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QTimer>
#include <vector>

#include "ai/guidecatalog.h"
#include "ai/tools.h"

class QAction;
class QMenu;

namespace kv {

class MainWindow;
class GuideOverlay;

/** 案内の手順 1 つを、今の画面のどこで示すか */
struct GuideSpot {
  /** 示せる場所がある */
  bool ok = false;
  /** 今の画面には無い（ワークショップを出していない など。待っても出てこない） */
  bool unavailable = false;
  /** 押す所（画面の座標） */
  QRect rect;
  /** ドラッグの行き先（画面の座標。空なら押すだけ） */
  QRect drop;
  /** 吹き出しを重ねない所（開いているメニュー など。画面の座標） */
  QRect avoid;
  /** 手順の場所そのもの（false なら、先にすること＝準備の場所） */
  bool final = false;
  /** 先にすること（final でないとき）、押せない理由、示せない理由 */
  QString hint;
  /** 押されたら次へ進む操作（メニューの項目のとき） */
  QPointer<QAction> action;
  /** 中の項目が選ばれたら次へ進むメニュー（サブメニューそのものを示すとき） */
  QPointer<QMenu> menu;
};

class GuideController : public QObject {
  Q_OBJECT
 public:
  explicit GuideController(MainWindow* window);
  ~GuideController() override;

  /** 案内を始める（出している案内はやめて置き換える）。案内の番号を返す（カードが、自分の案内かを見分ける） */
  int start(const std::vector<ai::GuideStep>& steps);
  /** 案内をやめる */
  void stop();
  /** 今の手順を飛ばして、次へ */
  void next();
  bool active() const { return index_ >= 0; }
  int session() const { return session_; }
  /** 今の手順（0 から。案内していなければ -1） */
  int current() const { return index_; }
  int count() const { return int(steps_.size()); }

  /** 場所 id を、今の画面のどこで示すか（準備が要れば、その準備の場所。スクロールはしない） */
  GuideSpot resolve(const QString& id) const;
  /** 場所 id を今の画面では案内できない理由（できるなら空。AI への知らせに使う） */
  QString unavailable(const QString& id) const;
  /** 今の手順で示している場所 */
  const GuideSpot& spot() const { return spot_; }
  /** 今の手順を、今の画面で示せない理由（示せていれば空） */
  const QString& notice() const { return notice_; }
  /** 画面の上に出している案内の窓（テストで絵にする） */
  QWidget* overlay() const;

 signals:
  /** 手順が進んだ（current が count と同じなら、最後まで終わった） */
  void progressed(int session, int current, int count);
  /** 案内が終わった（finished: 最後まで進んだ。false は、やめた・置き換えた・しばらく使われなかった） */
  void ended(int session, bool finished);
  /** 今の手順を、今の画面では示せない（message はその理由。示せるようになったら空で知らせる） */
  void noticed(int session, const QString& message);

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override;

 private:
  GuideSpot resolveTarget(const ai::GuideTarget& t, int depth, bool reveal) const;
  /** 準備（need）が足りなければ、その準備の場所 */
  bool needSpot(ai::GuideNeed need, int depth, bool reveal, GuideSpot* out) const;
  /** タイムラインの中の場所 part */
  GuideSpot timelineSpot(const QString& part, bool reveal) const;
  /** メニューの項目（メニューを順に開く案内から） */
  GuideSpot actionSpot(QAction* action) const;
  /** 部品 w（見えていなければ、スクロールして見せる） */
  GuideSpot widgetSpot(QWidget* w, const QString& key, bool reveal) const;
  /** ビュー view を前に出す（閉じていれば開く）案内 */
  GuideSpot viewRoute(const QString& view) const;
  /** ドラッグの行き先（タイムラインの場所の名前。行き先が見えていなければ空） */
  QRect dropRect(const QString& part, bool reveal) const;
  /** 開いているダイアログの中の部品 name（無ければ nullptr） */
  QWidget* dialogWidget(const QString& name) const;
  /** 手順の場所が、もう済んでいる（タブを前に出した など）。最後の手順でなければ飛ばす */
  bool satisfied(const ai::GuideTarget& t) const;
  /** 部品を見せるためにスクロールしてよいか（同じものを何度も動かさない） */
  bool mayReveal(const QString& key) const;

  void tick();
  /** 案内 session の手順 index が済んだ（今もその手順なら、次へ）。押した直後に呼ばれるので、少し後で進める */
  void completeLater(int index);
  void complete(int session, int index);
  void goTo(int index);
  void finish(bool finished);
  /** 今の手順がメニューの操作なら、その操作が行われたら次へ進む（メニューから選んでも、ショートカットキーでも） */
  void watchStep();
  void unwatch();
  void showSpot();

  MainWindow* window_;
  GuideOverlay* overlay_;
  std::vector<ai::GuideStep> steps_;
  int index_ = -1;
  int session_ = 0;
  GuideSpot spot_;
  QTimer tick_;
  /** 最後に手順が進んでから（しばらく使われなければやめる） */
  QElapsedTimer idle_;
  /** 部品を見せるためにスクロールした時刻（手順ごと。同じものを何度も動かさない） */
  mutable QHash<QString, qint64> revealed_;
  QElapsedTimer clock_;
  // 押した所（示している場所を押したら、離したとき・落としたときに次へ）
  int pressIndex_ = -1;
  QRect pressDrop_;
  /** 同じ押す・離すが、親の部品にも届いたもの（同じ時刻）は、1 回と数える */
  quint64 pressStamp_ = 0;
  quint64 releaseStamp_ = 0;
  // 押されたら次へ進む操作・メニュー
  QPointer<QAction> watchedAction_;
  QPointer<QMenu> watchedMenu_;
  QMetaObject::Connection watchConn_;
  /** アプリを使っている（ほかのアプリに切り替えたら、案内の窓を隠す） */
  bool appActive_ = true;
  /** 最後に知らせた、示せない理由（変わったときだけ知らせる） */
  QString notice_;
};

}  // namespace kv
