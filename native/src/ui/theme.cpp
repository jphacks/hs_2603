#include "ui/theme.h"

#include <QApplication>
#include <QFont>
#include <QPainter>
#include <QPalette>
#include <QStyleFactory>
#include <QSvgRenderer>
#include <map>

namespace kv::theme {

static const char* kStyleSheet = R"(
QWidget { color: #e6e6e6; }
QMainWindow, QDialog { background: #1b1b1b; }
QToolTip { background: #2a2a2a; color: #e6e6e6; border: 1px solid #3a3a3a; padding: 4px 6px; }

QMenuBar { background: #1b1b1b; border: none; padding: 2px 4px; }
QMenuBar::item { background: transparent; padding: 5px 10px; border-radius: 4px; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: #2c2c2c; }
QMenu { background: #222222; border: 1px solid #3a3a3a; padding: 4px; }
QMenu::item { padding: 5px 28px 5px 12px; border-radius: 3px; }
QMenu::item:selected { background: #2680eb; color: #ffffff; }
QMenu::item:disabled { color: #6b6b6b; }
QMenu::separator { height: 1px; background: #3a3a3a; margin: 4px 2px; }

QSplitter::handle { background: #2a2a2a; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

QStatusBar { background: #1b1b1b; color: #9b9b9b; border-top: 1px solid #2a2a2a; }
QStatusBar::item { border: none; }

QPushButton {
  background: #2c2c2c; border: 1px solid #3a3a3a; border-radius: 4px;
  padding: 5px 12px; min-height: 18px;
}
QPushButton:hover { background: #363636; }
QPushButton:pressed { background: #303030; }
QPushButton:disabled { color: #6b6b6b; background: #242424; border-color: #2e2e2e; }
QPushButton[primary="true"] { background: #2680eb; border-color: #2680eb; color: #ffffff; font-weight: 600; }
QPushButton[primary="true"]:hover { background: #3a8ff0; border-color: #3a8ff0; }
QPushButton[primary="true"]:disabled { background: #22446b; border-color: #22446b; color: #8ea6c4; }
QPushButton[flat="true"] { background: transparent; border-color: transparent; }
QPushButton[flat="true"]:hover { background: #2c2c2c; }

QToolButton {
  background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 4px 6px;
}
QToolButton:hover { background: #2c2c2c; }
QToolButton:pressed { background: #303030; }
QToolButton:checked { background: #363636; }
QToolButton:disabled { color: #6b6b6b; }
QToolButton#playButton { background: #e6e6e6; border-radius: 19px; padding: 0; }
QToolButton#playButton:hover { background: #ffffff; }
QToolButton#playButton:disabled { background: #555555; }

#Segmented { background: #222222; border: 1px solid #2a2a2a; border-radius: 4px; }
#Segmented QToolButton { color: #9b9b9b; border-radius: 3px; padding: 4px 8px; }
#Segmented QToolButton:hover { color: #e6e6e6; background: transparent; }
#Segmented QToolButton:checked { background: #363636; color: #e6e6e6; }

QLineEdit, QPlainTextEdit, QDoubleSpinBox, QSpinBox, QComboBox {
  background: #222222; border: 1px solid #3a3a3a; border-radius: 4px; padding: 4px 6px;
  selection-background-color: #2680eb; selection-color: #ffffff;
}
QPlainTextEdit { background: #141414; font-size: 10.5pt; }
QLineEdit:focus, QPlainTextEdit:focus, QDoubleSpinBox:focus, QComboBox:focus { border-color: #2680eb; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox QAbstractItemView { background: #222222; border: 1px solid #3a3a3a; selection-background-color: #2680eb; outline: none; }
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button { width: 0; border: none; }

QSlider::groove:horizontal { height: 3px; background: #3d3d3d; border-radius: 1px; }
QSlider::sub-page:horizontal { background: #2680eb; border-radius: 1px; }
QSlider::handle:horizontal { width: 13px; height: 13px; margin: -5px 0; border-radius: 6px; background: #f2f2f2; }
QSlider::handle:horizontal:hover { background: #ffffff; }

QCheckBox { spacing: 8px; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #555555; border-radius: 3px; background: #222222; }
QCheckBox::indicator:checked { background: #2680eb; border-color: #2680eb; image: url(:/icons/check-mark.svg); }

QTabWidget::pane { border: none; }
QTabBar { qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: #9b9b9b; padding: 9px 0 7px 0; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: #e6e6e6; border-bottom-color: #e6e6e6; }
QTabBar::tab:hover { color: #e6e6e6; }

QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle { background: #3a3a3a; border-radius: 4px; margin: 2px; }
QScrollBar::handle:hover { background: #4a4a4a; }
QScrollBar::handle:vertical { min-height: 24px; }
QScrollBar::handle:horizontal { min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QListWidget { background: transparent; border: none; outline: none; }
QListWidget::item { border-radius: 4px; color: #e6e6e6; }
QListWidget::item:selected { background: #2c2c2c; }
QListWidget::item:hover { background: #262626; }

QProgressBar { background: #2c2c2c; border: none; border-radius: 3px; max-height: 6px; text-align: center; }
QProgressBar::chunk { background: #2680eb; border-radius: 3px; }

QLabel[role="muted"] { color: #9b9b9b; }
QLabel[role="dim"] { color: #6b6b6b; }
QLabel[role="section"] { color: #9b9b9b; font-weight: 600; }
QLabel[role="title"] { font-weight: 600; }
QLabel[role="note"] { color: #e9a23b; }
QLabel[role="error"] { color: #ff9a9e; }
QFrame[role="separator"] { background: #2a2a2a; max-height: 1px; min-height: 1px; border: none; }
)";

void apply(QApplication& app) {
  app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  QPalette pal;
  pal.setColor(QPalette::Window, panel);
  pal.setColor(QPalette::WindowText, text);
  pal.setColor(QPalette::Base, panel2);
  pal.setColor(QPalette::AlternateBase, panel3);
  pal.setColor(QPalette::ToolTipBase, panel3);
  pal.setColor(QPalette::ToolTipText, text);
  pal.setColor(QPalette::Text, text);
  pal.setColor(QPalette::Button, panel3);
  pal.setColor(QPalette::ButtonText, text);
  pal.setColor(QPalette::BrightText, Qt::white);
  pal.setColor(QPalette::Highlight, accent);
  pal.setColor(QPalette::HighlightedText, Qt::white);
  pal.setColor(QPalette::PlaceholderText, dim);
  pal.setColor(QPalette::Link, accent);
  pal.setColor(QPalette::Disabled, QPalette::Text, dim);
  pal.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
  pal.setColor(QPalette::Disabled, QPalette::WindowText, dim);
  app.setPalette(pal);
  QFont font;
  font.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Yu Gothic UI"), QStringLiteral("Meiryo")});
  font.setPointSizeF(9.5);
  font.setHintingPreference(QFont::PreferNoHinting);
  app.setFont(font);
  app.setStyleSheet(QString::fromLatin1(kStyleSheet));
}

QPixmap iconPixmap(const QString& name, int size, const QColor& color, qreal dpr) {
  const int px = int(size * dpr);
  QPixmap pm(px, px);
  pm.fill(Qt::transparent);
  QSvgRenderer svg(QStringLiteral(":/icons/%1.svg").arg(name));
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  svg.render(&p, QRectF(0, 0, px, px));
  p.setCompositionMode(QPainter::CompositionMode_SourceIn);
  p.fillRect(pm.rect(), color);
  p.end();
  pm.setDevicePixelRatio(dpr);
  return pm;
}

QIcon icon(const QString& name, const QColor& color) {
  static std::map<QString, QIcon> cache;
  const QString key = name + color.name(QColor::HexArgb);
  auto it = cache.find(key);
  if (it != cache.end()) return it->second;
  QIcon ic;
  for (int s : {16, 20, 24, 32, 48}) {
    ic.addPixmap(iconPixmap(name, s, color), QIcon::Normal);
    ic.addPixmap(iconPixmap(name, s, dim), QIcon::Disabled);
  }
  cache[key] = ic;
  return ic;
}

}  // namespace kv::theme
