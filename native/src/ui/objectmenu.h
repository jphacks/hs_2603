#pragma once

#include <QPoint>
#include <functional>

#include "core/editor.h"

class QWidget;

namespace kv {

/** 右クリックメニューを出した場所（場所ごとに、出す項目が少し違う） */
enum class ObjectMenuPlace { Timeline, Preview };

/**
 * クリップ・テキスト・オーディオの右クリックメニュー（分割・複製・削除・段の移動・フェード・音・アニメーション）。
 * 出す前に、そのオブジェクトを選ぶ。editText は、プレビューで「文字を編集」を選んだときに呼ぶ（プレビューのテキストだけ）
 */
void execObjectMenu(QWidget* parent, const Selection& sel, const QPoint& globalPos, ObjectMenuPlace place,
                    const std::function<void()>& editText = {});

}  // namespace kv
