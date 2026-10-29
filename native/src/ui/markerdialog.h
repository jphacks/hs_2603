#pragma once
// マーカー（タイムラインの目盛りの上の印）の名前・色・メモを編集する画面と、色の選択肢

#include <QColor>
#include <QString>
#include <vector>

class QMenu;
class QWidget;

namespace kv {

struct MarkerColor {
  QColor color;
  QString name;
};
/** マーカーの色の選択肢（Premiere と同じ 8 色。最初が既定の緑） */
const std::vector<MarkerColor>& markerColors();

/**
 * マーカー markerId の名前・色・メモを編集する画面を出す（OK で確定。元に戻す 1 回分）。
 * 「削除」を押したらマーカーを消して true（消さなければ false）
 */
bool execMarkerDialog(QWidget* parent, const QString& markerId);

/** 右クリックのメニューに、マーカーの色を選ぶ下のメニューを足す（今の色に印を付ける） */
void addMarkerColorMenu(QMenu* menu, const QString& markerId);

}  // namespace kv
