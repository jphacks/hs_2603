#include "ui/objectmenu.h"

#include <QAction>
#include <QActionGroup>
#include <QMenu>
#include <cmath>

#include "core/animtemplates.h"
#include "core/timeline.h"
#include "ui/keymap.h"
#include "ui/widgets.h"

namespace kv {

namespace {

/** メニューの項目名に、その操作のキー（キーボードショートカットの設定どおり）を添える */
QString withKeys(const QString& label, const QString& commandId) {
  const QString k = keymap::text(keymap::keys(commandId));
  return k.isEmpty() ? label : label + QLatin1Char('\t') + k;
}

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
  menu.addAction(withKeys(QStringLiteral("削除") + suffix, QStringLiteral("edit.delete")), parent, [] { editor().deleteSelection(); });

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
