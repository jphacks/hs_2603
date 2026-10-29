#pragma once

#include <QPoint>
#include <functional>
#include <optional>

#include "core/editor.h"

class QWidget;

namespace kv {

/** メニューの項目名に、その操作のキー（キーボードショートカットの設定どおり）を添える */
QString withKeys(const QString& label, const QString& commandId);

/** 右クリックメニューを出した場所（場所ごとに、出す項目が少し違う） */
enum class ObjectMenuPlace { Timeline, Preview };

/**
 * クリップ・テキスト・オーディオの右クリックメニュー（分割・複製・削除・段の移動・フェード・音・アニメーション）。
 * 出す前に、そのオブジェクトを選ぶ。editText は、プレビューで「文字を編集」を選んだときに呼ぶ（プレビューのテキストだけ）
 */
void execObjectMenu(QWidget* parent, const Selection& sel, const QPoint& globalPos, ObjectMenuPlace place,
                    const std::function<void()>& editText = {});

/** 選んでいるものをコピーする（ほかのタブにも貼り付けられる）。結果は画面の下に知らせる */
void copyItems();
/** コピーしたものを、今のタブの再生位置に貼り付ける（使っているメディアも、このタブの一覧に入れる） */
void pasteItems();
/** 選んでいるものを、ほかのタブ（index）にコピーする: そのタブに切り替えて、そのタブの再生位置に貼り付ける */
void copyItemsToTab(int index);
/** コピーしたものを、今のタブの時間 t に貼り付ける（ドラッグで落としたとき。ビデオのクリップは clipIndex の位置） */
void pasteItemsAt(double t, std::optional<int> clipIndex);

/** タイムラインで選んだものを、ほかのタブへドラッグで持っていくときの MIME 形式（中身は写し元のタブの ID。物はアプリのクリップボード） */
inline constexpr const char* kTimelineItemsMime = "application/x-e-editor-timeline-items";

}  // namespace kv
