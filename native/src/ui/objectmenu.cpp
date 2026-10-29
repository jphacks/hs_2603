#include "ui/objectmenu.h"

#include <QAction>
#include <QActionGroup>
#include <QColorDialog>
#include <QMenu>
#include <algorithm>
#include <cmath>

#include "core/animtemplates.h"
#include "core/effects.h"
#include "core/filters.h"
#include "core/textstyles.h"
#include "core/texttemplates.h"
#include "core/timeline.h"
#include "core/transitions.h"
#include "mask/maskcontroller.h"
#include "media/mediamanager.h"
#include "ui/clipdialogs.h"
#include "ui/effectlist.h"
#include "ui/keymap.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

void copyItems() {
  Editor& ed = editor();
  const int n = ed.copySelection();
  if (n <= 0) {
    ed.toast(QStringLiteral("コピーするものを、タイムラインで選んでください"));
    return;
  }
  ed.toast(ed.workspaceCount() > 1 ? QStringLiteral("%1 個コピーしました（ほかのタブにも貼り付けられます）").arg(n)
                                   : QStringLiteral("%1 個コピーしました").arg(n));
}

/** 貼り付けて、足したメディアの準備と知らせをする（at を渡せばその時間、無ければ再生位置） */
static void pasteWith(std::optional<double> at, std::optional<int> clipIndex) {
  Editor& ed = editor();
  if (!ed.hasItemClipboard()) {
    ed.toast(QStringLiteral("貼り付けるものがありません。タイムラインで選んで「コピー」してください"));
    return;
  }
  ed.pause();
  std::vector<Asset> added;
  const int n = ed.pasteClipboard(&added, at, clipIndex);
  // 一覧に足したメディアは、サムネイル・波形・写真の表示用の画像を作る（タブを開いたときと同じ）
  for (const Asset& a : added) {
    if (a.status != AssetStatus::Missing) media().prepare(a);
  }
  if (n <= 0) {
    ed.toast(QStringLiteral("貼り付けられませんでした"), ToastKind::Error);
    return;
  }
  ed.toast(added.empty() ? QStringLiteral("%1 個貼り付けました").arg(n)
                         : QStringLiteral("%1 個貼り付けました（使っているメディア %2 個を、このタブの一覧に入れました）").arg(n).arg(added.size()));
}

void pasteItems() {
  pasteWith(std::nullopt, std::nullopt);
}

void pasteItemsAt(double t, std::optional<int> clipIndex) {
  pasteWith(std::max(0.0, t), clipIndex);
  // 落とした所を見せる
  editor().seek(std::max(0.0, t));
}

void copyItemsToTab(int index) {
  Editor& ed = editor();
  if (index < 0 || index >= ed.workspaceCount() || index == ed.workspaceIndex()) return;
  if (ed.copySelection() <= 0) {
    ed.toast(QStringLiteral("コピーするものを、タイムラインで選んでください"));
    return;
  }
  ed.switchWorkspace(index);
  pasteItems();
}

QString withKeys(const QString& label, const QString& commandId) {
  const QString k = keymap::text(keymap::keys(commandId));
  return k.isEmpty() ? label : label + QLatin1Char('\t') + k;
}

namespace {

/** オブジェクトのタイムライン上の開始と長さ。見つからなければ false */
bool timeSpan(const Selection& sel, double* start, double* duration) {
  const Editor& ed = editor();
  if (sel.type == Selection::Clip || sel.type == Selection::Overlay) {
    return ed.clipSpan(sel.id, start, duration);
  } else if (sel.type == Selection::Text) {
    if (const TextItem* t = ed.text(sel.id)) {
      *start = t->start;
      *duration = t->duration;
      return true;
    }
  } else if (sel.type == Selection::Audio) {
    if (const AudioItem* a = ed.audio(sel.id)) {
      *start = a->start;
      *duration = audioDuration(*a);
      return true;
    }
  }
  return false;
}

/** フェードの長さを選ぶ下のメニュー（今の長さに印を付ける。ほかの長さは設定欄やタイムラインのつまみで自由に変えられる） */
void addFadeMenu(QMenu* menu, const QString& title, double current, const std::function<void(double)>& apply) {
  QMenu* sub = menu->addMenu(title);
  auto* group = new QActionGroup(sub);
  bool matched = false;
  for (double v : {0.0, 0.5, 1.0, 2.0, 3.0}) {
    QAction* a = sub->addAction(v == 0 ? QStringLiteral("なし") : QStringLiteral("%1秒").arg(v));
    a->setCheckable(true);
    const bool on = std::abs(current - v) < 1e-3;
    matched |= on;
    a->setChecked(on);
    group->addAction(a);
    QObject::connect(a, &QAction::triggered, sub, [apply, v] { apply(v); });
  }
  // 決まった長さ以外のときは、今の長さを見せる（選べない）
  if (!matched) {
    sub->addSeparator();
    QAction* now = sub->addAction(QStringLiteral("今: %1秒").arg(current, 0, 'f', 2));
    now->setEnabled(false);
  }
}

/** 印を付けて選べる項目（同じまとまりの中で 1 つだけ） */
QAction* addChoice(QMenu* menu, QActionGroup* group, const QString& label, bool checked, QObject* context, const std::function<void()>& apply) {
  QAction* a = menu->addAction(label);
  a->setCheckable(true);
  a->setChecked(checked);
  if (group) group->addAction(a);
  QObject::connect(a, &QAction::triggered, context, [apply] { apply(); });
  return a;
}

/**
 * 映像の操作: エフェクトを追加・反転・速度・音声の切り離し・フレームの保持（メインのビデオ）・トランジション（メインのビデオの切れ目）・
 * 属性のコピー / ペースト。start / duration は、その映像のタイムライン上の範囲
 */
void addClipActions(QMenu* menu, QWidget* parent, const VideoClip& c, bool mainTrack, bool timeline, double start, double duration) {
  Editor& ed = editor();
  const QString id = c.id;
  const OverlayItem* ov = ed.overlay(id);
  const bool layer = ov && ov->isEffectLayer();
  menu->addSeparator();
  QMenu* fx = menu->addMenu(theme::icon(QStringLiteral("sparkles")), QStringLiteral("エフェクトを追加"));
  fillAddEffectMenu(fx, id, layer);
  if (!layer) {
    QAction* fh = menu->addAction(QStringLiteral("左右反転"));
    fh->setCheckable(true);
    fh->setChecked(c.flipX);
    QObject::connect(fh, &QAction::toggled, parent, [id](bool on) { editor().updateClip(id, [&](VideoClip& x) { x.flipX = on; }); });
    QAction* fv = menu->addAction(QStringLiteral("上下反転"));
    fv->setCheckable(true);
    fv->setChecked(c.flipY);
    QObject::connect(fv, &QAction::toggled, parent, [id](bool on) { editor().updateClip(id, [&](VideoClip& x) { x.flipY = on; }); });
    menu->addAction(withKeys(QStringLiteral("速度・デュレーション..."), QStringLiteral("edit.speed")), parent, [parent, id] { execSpeedDialog(parent, id); });
    const Asset* a = ed.asset(c.assetId);
    if (!c.isImage && a && a->hasAudio) {
      menu->addAction(withKeys(QStringLiteral("音声を切り離す"), QStringLiteral("edit.detachAudio")), parent, [id] { editor().detachAudio(id); });
    }
    if (!c.isImage) {
      // 逆再生（逆向きの動画を作って入れ替える。もう一度選ぶと元に戻す）
      QAction* rev = menu->addAction(withKeys(QStringLiteral("逆再生"), QStringLiteral("edit.reverse")));
      rev->setCheckable(true);
      rev->setChecked(!c.reversedFrom.isEmpty());
      QObject::connect(rev, &QAction::triggered, parent, [parent, id] { toggleReverseClip(parent, id); });
    }
  }
  if (mainTrack && timeline && !c.isImage) {
    // 再生位置がそのクリップの中にあるときだけ（その時間のフレームで止める）
    const double t = ed.currentTime();
    QAction* hold = menu->addAction(withKeys(QStringLiteral("フレームを保持（再生位置で 2 秒止める）"), QStringLiteral("edit.frameHold")), parent,
                                    [parent] { frameHoldAtPlayhead(parent); });
    hold->setEnabled(t >= start - 1e-6 && t < start + duration - 1e-6);
  }
  // メインのビデオの切れ目: 頭（前のクリップとの間）・お尻（次のクリップとの間）に、既定のトランジションを付ける・外す
  if (mainTrack) {
    const auto& items = ed.layout().items;
    int index = -1;
    for (const auto& l : items) {
      if (l.clip.id == id) index = l.index;
    }
    if (index >= 0 && items.size() > 1) {
      QMenu* tr = menu->addMenu(theme::icon(QStringLiteral("blend")), QStringLiteral("トランジション"));
      const TransitionType def = Editor::defaultTransitionType();
      const QString defLabel = transitionInfo(def).label;
      const QString nextId = index + 1 < int(items.size()) ? items[size_t(index) + 1].clip.id : QString();
      QAction* head = tr->addAction(QStringLiteral("頭に「%1」を付ける（前のクリップとの切れ目）").arg(defLabel), parent, [id, def] { editor().setTransition(id, def); });
      head->setEnabled(index > 0);
      QAction* tail = tr->addAction(QStringLiteral("お尻に「%1」を付ける（次のクリップとの切れ目）").arg(defLabel), parent,
                                    [nextId, def] { editor().setTransition(nextId, def); });
      tail->setEnabled(!nextId.isEmpty());
      const bool hasHead = index > 0 && c.transIn.type != TransitionType::None;
      const bool hasTail = !nextId.isEmpty() && items[size_t(index) + 1].clip.transIn.type != TransitionType::None;
      if (hasHead || hasTail) tr->addSeparator();
      if (hasHead) tr->addAction(QStringLiteral("頭のトランジションを外す"), parent, [id] { editor().removeTransition(id); });
      if (hasTail) tr->addAction(QStringLiteral("お尻のトランジションを外す"), parent, [nextId] { editor().removeTransition(nextId); });
      tr->addSeparator();
      tr->addAction(QStringLiteral("ほかの種類は、左の「エフェクト」タブから切れ目へドラッグします"))->setEnabled(false);
    }
  }
  menu->addAction(withKeys(QStringLiteral("属性をコピー"), QStringLiteral("edit.copyAttributes")), parent, [] { copySelectedAttributes(); });
  QAction* paste = menu->addAction(withKeys(QStringLiteral("属性をペースト..."), QStringLiteral("edit.pasteAttributes")), parent,
                                   [parent] { execPasteAttributesDialog(parent); });
  paste->setEnabled(ed.hasAttributes());
}

/** 映像・写真: 背景を透過（クロマキー）と、プリセット（フィルター・表示方法）。エフェクトレイヤーはフィルターだけ */
void addVisualMenus(QMenu* menu, QWidget* parent, const VideoClip& c, bool overlay) {
  const QString id = c.id;
  const OverlayItem* ov = editor().overlay(id);
  if (ov && ov->isEffectLayer()) {
    // エフェクトレイヤー: 下にある映像の色を、まとめて調整する（クロマキー・表示方法は使わない）
    menu->addSeparator();
    QMenu* filters = menu->addMenu(QStringLiteral("色の調整（フィルター）"));
    auto* fg = new QActionGroup(filters);
    for (const auto& p : filterPresets()) {
      const FilterPreset pid = p.id;
      addChoice(filters, fg, p.label, p.id == c.color.preset, parent,
                [id, pid] { editor().updateClip(id, [&](VideoClip& x) { x.color = colorFromPreset(pid); }); });
    }
    return;
  }
  menu->addSeparator();
  QMenu* bg = menu->addMenu(QStringLiteral("背景を透過（クロマキー）"));
  QAction* on = bg->addAction(QStringLiteral("背景の色を透明にする"));
  on->setCheckable(true);
  on->setChecked(c.chroma.enabled);
  QObject::connect(on, &QAction::toggled, parent, [id](bool v) { editor().updateClip(id, [&](VideoClip& x) { x.chroma.enabled = v; }); });
  bg->addSeparator();
  auto setKey = [id](const QColor& col) {
    editor().updateClip(id, [&](VideoClip& x) {
      x.chroma.color = col;
      x.chroma.enabled = true;
    });
  };
  bg->addAction(QStringLiteral("緑の背景（グリーンバック）を透明に"), parent, [setKey] { setKey(QColor(0, 255, 0)); });
  bg->addAction(QStringLiteral("青の背景（ブルーバック）を透明に"), parent, [setKey] { setKey(QColor(0, 0, 255)); });
  bg->addAction(QStringLiteral("色を選んで透明に…"), parent, [parent, id, setKey] {
    const VideoClip* cur = editor().clip(id);
    const QColor col = QColorDialog::getColor(cur ? cur->chroma.color : QColor(0, 255, 0), parent, QStringLiteral("透明にする色"));
    if (col.isValid()) setKey(col);
  });
  bg->addAction(QStringLiteral("スポイトでプレビューから選ぶ"), parent, [id] {
    // 右の設定欄の「スポイトでプレビューから選ぶ」と同じ（今の再生位置にその映像が出ていなければ、始まりへ動かす）
    Editor& e = editor();
    double start = 0, dur = 0;
    if (e.clipSpan(id, &start, &dur)) {
      const double t = e.currentTime();
      if (t < start - 1e-9 || t >= start + dur - 1e-9) e.seek(start);
    }
    e.setChromaPick(id);
    e.toast(QStringLiteral("プレビューの映像で、透明にしたい色の所をクリックしてください（Esc でやめる）"));
  });
  if (!overlay) {
    // メインのビデオでは、透明にした所は黒になる。後ろの映像を見せるには、重ねるトラックへ移す
    bg->addSeparator();
    bg->addAction(QStringLiteral("メインのビデオでは、透明にした所は黒になります"))->setEnabled(false);
    bg->addAction(QStringLiteral("重ねるトラックへ移す（後ろの映像が見えるように）"), parent, [id] {
      Editor& e = editor();
      double start = 0, dur = 0;
      if (!e.clipSpan(id, &start, &dur)) return;
      e.moveClipToOverlay(id, start, freeOverlayTrack(e.project().overlays, start, dur, e.project().settings.fps));
    });
  }

  // オブジェクトマスク（AI）: 被写体をクリックして選ぶと、マスクして動きを追いかける（右の設定欄と同じ操作）
  QMenu* om = menu->addMenu(theme::icon(QStringLiteral("scan-face")), QStringLiteral("オブジェクトマスク（AI）"));
  const ObjectMask& m = c.mask;
  const bool picking = editor().maskPick() == id;
  om->addAction(withKeys(picking ? QStringLiteral("被写体を選ぶのをやめる")
                                 : m.has() ? QStringLiteral("被写体を選び直す・直す…") : QStringLiteral("被写体をクリックして選ぶ…"),
                         QStringLiteral("edit.objectMask")),
                parent, [id] { toggleObjectMaskPick(id); });
  if (m.has()) {
    const int state = mask::maskController().state(m.id);
    if (state != 0) {
      om->addAction(QStringLiteral("トラックを止める"), parent, [mid = m.id] { mask::maskController().stop(mid); });
    } else if (c.isImage) {
      om->addAction(QStringLiteral("マスクを作り直す"), parent, [id] { mask::maskController().track(id, mask::TrackDirection::Both); });
    } else {
      om->addAction(QStringLiteral("前後をトラック"), parent, [id] { mask::maskController().track(id, mask::TrackDirection::Both); });
      om->addAction(QStringLiteral("前へトラック"), parent, [id] { mask::maskController().track(id, mask::TrackDirection::Backward); });
      om->addAction(QStringLiteral("後ろへトラック"), parent, [id] { mask::maskController().track(id, mask::TrackDirection::Forward); });
    }
    om->addSeparator();
    QAction* use = om->addAction(QStringLiteral("マスクを使う"));
    use->setCheckable(true);
    use->setChecked(m.enabled);
    QObject::connect(use, &QAction::toggled, parent, [id](bool v) { editor().updateClip(id, [&](VideoClip& x) { x.mask.enabled = v; }); });
    auto* mg = new QActionGroup(om);
    addChoice(om, mg, QStringLiteral("背景を消す（被写体だけを残す）"), m.mode == MaskMode::Cutout, parent,
              [id] { editor().updateClip(id, [](VideoClip& x) { x.mask.mode = MaskMode::Cutout; }); });
    addChoice(om, mg, QStringLiteral("エフェクトを被写体の中だけに"), m.mode == MaskMode::Effects, parent,
              [id] { editor().updateClip(id, [](VideoClip& x) { x.mask.mode = MaskMode::Effects; }); });
    QAction* inv = om->addAction(QStringLiteral("反転（被写体の外側にする）"));
    inv->setCheckable(true);
    inv->setChecked(m.invert);
    QObject::connect(inv, &QAction::toggled, parent, [id](bool v) { editor().updateClip(id, [&](VideoClip& x) { x.mask.invert = v; }); });
    om->addSeparator();
    om->addAction(theme::icon(QStringLiteral("trash")), QStringLiteral("マスクを削除"), parent, [id] {
      if (editor().maskPick() == id) editor().setMaskPick({});
      mask::maskController().remove(id);
    });
  }

  QMenu* pre = menu->addMenu(QStringLiteral("プリセット"));
  QMenu* filters = pre->addMenu(QStringLiteral("フィルター"));
  auto* fg = new QActionGroup(filters);
  for (const auto& p : filterPresets()) {
    const FilterPreset pid = p.id;
    addChoice(filters, fg, p.label, p.id == c.color.preset, parent,
              [id, pid] { editor().updateClip(id, [&](VideoClip& x) { x.color = colorFromPreset(pid); }); });
  }
  if (!overlay) {
    // 重ねるトラックは、いつも全体表示（余白は透明）
    QMenu* fit = pre->addMenu(QStringLiteral("表示方法"));
    auto* gg = new QActionGroup(fit);
    const std::pair<FitMode, QString> fits[] = {{FitMode::Contain, QStringLiteral("全体表示")},
                                                {FitMode::Cover, QStringLiteral("拡大（画面いっぱい）")},
                                                {FitMode::Blur, QStringLiteral("ぼかし背景")}};
    for (const auto& [mode, label] : fits) {
      const FitMode m = mode;
      addChoice(fit, gg, label, c.fit == m, parent, [id, m] { editor().updateClip(id, [&](VideoClip& x) { x.fit = m; }); });
    }
  }
}

/** 文字: 背景（帯）の有り無しと色、プリセット（スタイル・マイテンプレート） */
void addTextMenus(QMenu* menu, QWidget* parent, const TextItem& t) {
  const QString id = t.id;
  menu->addSeparator();
  QMenu* bg = menu->addMenu(QStringLiteral("背景"));
  auto* group = new QActionGroup(bg);
  const bool none = t.boxOpacity <= 0.001;
  addChoice(bg, group, QStringLiteral("背景なし（透明）"), none, parent, [id] { editor().updateText(id, [](TextItem& x) { x.boxOpacity = 0; }); });
  bg->addSeparator();
  struct Box {
    QString label;
    QColor color;
    double opacity;
  };
  const Box boxes[] = {{QStringLiteral("黒い帯（半透明）"), QColor(0, 0, 0), 0.62},
                       {QStringLiteral("白い帯"), QColor(0xff, 0xff, 0xff), 0.92},
                       {QStringLiteral("クリーム色の帯"), QColor(0xff, 0xf6, 0xd5), 0.95},
                       {QStringLiteral("黄色の帯"), QColor(0xff, 0xe1, 0x4d), 0.95}};
  for (const Box& b : boxes) {
    const bool on = !none && t.boxColor == b.color && std::abs(t.boxOpacity - b.opacity) < 0.01;
    const QColor col = b.color;
    const double op = b.opacity;
    addChoice(bg, group, b.label, on, parent, [id, col, op] {
      editor().updateText(id, [&](TextItem& x) {
        x.boxColor = col;
        x.boxOpacity = op;
      });
    });
  }
  bg->addAction(QStringLiteral("背景の色を選ぶ…"), parent, [parent, id] {
    const TextItem* cur = editor().text(id);
    const QColor col = QColorDialog::getColor(cur ? cur->boxColor : QColor(0, 0, 0), parent, QStringLiteral("背景の色"));
    if (!col.isValid()) return;
    editor().updateText(id, [&](TextItem& x) {
      x.boxColor = col;
      if (x.boxOpacity <= 0.001) x.boxOpacity = 0.6;
    });
  });

  // プリセット: 文字・時間・位置はそのままで、見た目だけを変える（右の設定欄の「スタイル」と同じ）
  QMenu* pre = menu->addMenu(QStringLiteral("プリセット（スタイル）"));
  auto apply = [id](const QString& presetId) { editor().updateText(id, [&](TextItem& x) { x = applyTextPreset(x, presetId); }); };
  for (const auto& p : textPresets()) {
    const QString pid = p.id;
    pre->addAction(p.label, parent, [apply, pid] { apply(pid); });
  }
  pre->addSeparator();
  QMenu* mine = pre->addMenu(QStringLiteral("マイテンプレート"));
  for (const auto& p : textTemplates().list()) {
    const QString pid = p.id;
    mine->addAction(p.label, parent, [apply, pid] { apply(pid); });
  }
  if (textTemplates().list().empty()) mine->addAction(QStringLiteral("（まだありません）"))->setEnabled(false);
}

}  // namespace

void execObjectMenu(QWidget* parent, const Selection& sel, const QPoint& globalPos, ObjectMenuPlace place,
                    const std::function<void()>& editText) {
  Editor& ed = editor();
  double start = 0;
  double duration = 0;
  if (!sel || !timeSpan(sel, &start, &duration)) return;
  ed.pause();
  // 一緒に選んでいるもの・同じグループのものを右クリックしたときは、一緒に選んだままにする（まとめて複製・削除・グループ化できる）
  if (Editor::groupable(sel.type) && ed.inSelectionSet(sel)) ed.focusInSelection(sel);
  else ed.select(sel);
  const bool timeline = place == ObjectMenuPlace::Timeline;
  const size_t many = ed.selectionSet().size();

  QMenu menu(parent);
  // ---- 編集 ----
  if (timeline) {
    // 再生位置がそのオブジェクトの中にあるときだけ分けられる
    const double t = ed.currentTime();
    QAction* split = menu.addAction(withKeys(QStringLiteral("再生位置で分割"), QStringLiteral("edit.split")), parent,
                                    [] { editor().splitAtPlayhead(); });
    split->setEnabled(t > start + 1e-6 && t < start + duration - 1e-6);
  }
  if (sel.type == Selection::Text && !timeline && editText) menu.addAction(QStringLiteral("文字を編集"), parent, editText);
  // 見た目をマイテンプレートにする（右の設定欄の「今の見た目をテンプレートに保存」と同じ）
  if (sel.type == Selection::Text) {
    const QString textId = sel.id;
    menu.addAction(QStringLiteral("見た目をテンプレートに保存…"), parent, [parent, textId] { saveTextAsTemplate(parent, textId); });
  }
  const QString suffix = many > 1 ? QStringLiteral("（%1 個まとめて）").arg(many) : QString();
  menu.addAction(withKeys(QStringLiteral("複製") + suffix, QStringLiteral("edit.duplicate")), parent, [] { editor().duplicateSelection(); });
  menu.addAction(withKeys(QStringLiteral("コピー") + suffix, QStringLiteral("edit.copy")), parent, [] { copyItems(); });
  QAction* paste = menu.addAction(withKeys(QStringLiteral("貼り付け（再生位置に）"), QStringLiteral("edit.paste")), parent, [] { pasteItems(); });
  paste->setEnabled(ed.hasItemClipboard());
  // ほかのタブ（編集1 など）へ持っていく: そのタブに切り替えて、そのタブの再生位置に貼り付ける
  if (ed.workspaceCount() > 1) {
    QMenu* to = menu.addMenu(QStringLiteral("ほかのタブにコピー") + suffix);
    for (int i = 0; i < ed.workspaceCount(); ++i) {
      if (i == ed.workspaceIndex()) continue;
      to->addAction(ed.workspaceName(i), parent, [i] { copyItemsToTab(i); });
    }
  }
  menu.addAction(withKeys(QStringLiteral("削除") + suffix, QStringLiteral("edit.delete")), parent, [] { editor().deleteSelection(); });
  if (timeline) {
    // 消して、空いた所を詰める（メインのビデオなら、後ろのテキスト・オーディオ・重ねるものも一緒に詰める）
    menu.addAction(withKeys(QStringLiteral("リップル削除（削除して詰める）") + suffix, QStringLiteral("edit.rippleDelete")), parent,
                   [] { editor().rippleDeleteSelection(); });
  }

  // ---- グループ（テキスト・オーディオ・重ねるトラック。Ctrl+クリックで一緒に選んでから） ----
  if (Editor::groupable(sel.type)) {
    menu.addSeparator();
    QAction* group = menu.addAction(withKeys(QStringLiteral("グループ化"), QStringLiteral("edit.group")), parent, [] { editor().groupSelection(); });
    group->setEnabled(ed.canGroup());
    QAction* ungroup = menu.addAction(withKeys(QStringLiteral("グループ解除"), QStringLiteral("edit.ungroup")), parent, [] { editor().ungroupSelection(); });
    ungroup->setEnabled(ed.canUngroup());
    if (!ed.canGroup() && !ed.canUngroup()) {
      QAction* hint = menu.addAction(QStringLiteral("Ctrl を押しながらクリックすると、いくつか一緒に選べます"));
      hint->setEnabled(false);
    }
  }

  // ---- テキストの段（上下にドラッグしても変えられる） ----
  if (sel.type == Selection::Text && timeline) {
    const TextItem* t = ed.text(sel.id);
    menu.addSeparator();
    const QString id = sel.id;
    QAction* up = menu.addAction(withKeys(QStringLiteral("上の段へ"), QStringLiteral("edit.textUp")), parent,
                                 [id] { editor().moveTextTrack(id, -1); });
    up->setEnabled(t && t->track > 0);
    menu.addAction(withKeys(QStringLiteral("下の段へ"), QStringLiteral("edit.textDown")), parent, [id] { editor().moveTextTrack(id, 1); });
  }
  // ---- 重ねるトラックの段（前・後ろ。上下にドラッグしても変えられる） ----
  if (sel.type == Selection::Overlay) {
    const OverlayItem* o = ed.overlay(sel.id);
    menu.addSeparator();
    const QString id = sel.id;
    menu.addAction(withKeys(QStringLiteral("前へ（上の段へ）"), QStringLiteral("edit.textUp")), parent, [id] { editor().moveOverlayTrack(id, 1); });
    QAction* down = menu.addAction(withKeys(QStringLiteral("後ろへ（下の段へ）"), QStringLiteral("edit.textDown")), parent,
                                   [id] { editor().moveOverlayTrack(id, -1); });
    down->setEnabled(o && o->track > 0);
  }

  // ---- フェード・音（映像とオーディオ） ----
  const QString id = sel.id;
  if (sel.type == Selection::Clip || sel.type == Selection::Overlay) {
    const VideoClip* c = ed.clip(id);
    if (c) {
      menu.addSeparator();
      const int fps = ed.project().settings.fps;
      addFadeMenu(&menu, QStringLiteral("フェードイン"), c->fadeIn, [id, fps](double v) {
        editor().updateClip(id, [&](VideoClip& x) { x.fadeIn = fitFade(v, clipDuration(x, fps), x.fadeOut); });
      });
      addFadeMenu(&menu, QStringLiteral("フェードアウト"), c->fadeOut, [id, fps](double v) {
        editor().updateClip(id, [&](VideoClip& x) { x.fadeOut = fitFade(v, clipDuration(x, fps), x.fadeIn); });
      });
      const Asset* a = ed.asset(c->assetId);
      if (!c->isImage && a && a->hasAudio) {
        QAction* mute = menu.addAction(QStringLiteral("音を消す"));
        mute->setCheckable(true);
        mute->setChecked(c->muted);
        QObject::connect(mute, &QAction::toggled, parent, [id](bool on) { editor().updateClip(id, [&](VideoClip& x) { x.muted = on; }); });
      }
    }
  } else if (sel.type == Selection::Audio) {
    if (const AudioItem* au = ed.audio(id)) {
      menu.addSeparator();
      addFadeMenu(&menu, QStringLiteral("フェードイン"), au->fadeIn, [id](double v) {
        editor().updateAudio(id, [&](AudioItem& x) { x.fadeIn = fitFade(v, audioDuration(x), x.fadeOut); });
      });
      addFadeMenu(&menu, QStringLiteral("フェードアウト"), au->fadeOut, [id](double v) {
        editor().updateAudio(id, [&](AudioItem& x) { x.fadeOut = fitFade(v, audioDuration(x), x.fadeIn); });
      });
      if (!au->volumePoints.empty()) {
        menu.addAction(QStringLiteral("音量キーフレームをすべて削除"), parent, [id] { editor().clearVolumePoints(id); });
      }
    }
  }

  // ---- 映像の操作（エフェクト・反転・速度・音声の切り離し・フレームの保持・トランジション・属性） ----
  if (sel.type == Selection::Clip || sel.type == Selection::Overlay) {
    if (const VideoClip* c = ed.clip(id)) addClipActions(&menu, parent, *c, sel.type == Selection::Clip, timeline, start, duration);
  }

  // ---- 背景を透過・プリセット（映像・写真と文字。右の設定欄と同じ変更） ----
  if (sel.type == Selection::Clip || sel.type == Selection::Overlay) {
    if (const VideoClip* c = ed.clip(id)) addVisualMenus(&menu, parent, *c, ed.overlay(id) != nullptr);
  } else if (sel.type == Selection::Text) {
    if (const TextItem* t = ed.text(id)) addTextMenus(&menu, parent, *t);
  }

  // ---- アニメーション（映像と文字。作ったあとも、キーフレームは自由に編集できる） ----
  if (sel.type == Selection::Clip || sel.type == Selection::Text || sel.type == Selection::Overlay) {
    menu.addSeparator();
    QMenu* anim = menu.addMenu(QStringLiteral("アニメーションを付ける"));
    for (const Editor::AnimPreset& p : Editor::animPresets()) {
      QAction* a = anim->addAction(p.label, parent, [id = p.id] { editor().applyAnimPreset(id); });
      a->setToolTip(p.hint);
    }
    // 文字: 自分で保存したアニメーション（マイテンプレート）と、今の動きの保存
    if (sel.type == Selection::Text) {
      anim->addSeparator();
      QMenu* mine = anim->addMenu(QStringLiteral("マイテンプレート"));
      for (const AnimTemplate& t : animTemplates().list()) {
        mine->addAction(t.name, parent, [tid = t.id] { applyAnimTemplateWithToast(tid); });
      }
      if (animTemplates().list().empty()) mine->addAction(QStringLiteral("（まだありません）"))->setEnabled(false);
      const TextItem* t = ed.text(id);
      QAction* save = anim->addAction(QStringLiteral("今の動きをマイテンプレートに保存…"), parent, [parent, id] { saveAnimTemplate(parent, id); });
      save->setEnabled(t && AnimTemplates::hasAnimation(*t));
    }
    anim->setToolTipsVisible(true);
  }

  menu.exec(globalPos);
}

}  // namespace kv
