#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace kv::theme {

inline const QColor bg{0x11, 0x11, 0x11};
inline const QColor panel{0x1b, 0x1b, 0x1b};
inline const QColor panel2{0x22, 0x22, 0x22};
inline const QColor panel3{0x2c, 0x2c, 0x2c};
inline const QColor hover{0x36, 0x36, 0x36};
inline const QColor border{0x2a, 0x2a, 0x2a};
inline const QColor border2{0x3a, 0x3a, 0x3a};
inline const QColor text{0xe6, 0xe6, 0xe6};
inline const QColor muted{0x9b, 0x9b, 0x9b};
inline const QColor dim{0x6b, 0x6b, 0x6b};
inline const QColor accent{0x26, 0x80, 0xeb};
inline const QColor danger{0xe5, 0x48, 0x4d};
inline const QColor ok{0x3f, 0xb6, 0x7b};
inline const QColor warn{0xe9, 0xa2, 0x3b};
inline const QColor playhead{0xff, 0x40, 0x57};

void apply(QApplication& app);
/** アイコン（lucide の線画を色付けしたもの） */
QIcon icon(const QString& name, const QColor& color = text);
QPixmap iconPixmap(const QString& name, int size, const QColor& color, qreal dpr = 1.0);

}  // namespace kv::theme
