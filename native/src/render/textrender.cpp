#include "render/textrender.h"

#include <QFontMetricsF>
#include <QPainterPath>
#include <QSet>
#include <algorithm>
#include <cmath>

#include "core/textstyles.h"
#include "render/imageops.h"

namespace kv {

QFont textFont(const TextItem& item, int pixelSize) {
  QFont f;
  f.setFamilies(fontFamilies(item.font));
  f.setPixelSize(std::max(4, pixelSize));
  f.setWeight(item.bold ? QFont::Bold : QFont::Normal);
  f.setHintingPreference(QFont::PreferNoHinting);
  f.setStyleStrategy(QFont::PreferAntialias);
  return f;
}

// 行頭に来てはいけない文字（禁則処理の簡易版）
static bool noLineStart(QChar c) {
  static const QString chars = QStringLiteral("、。，．,.・：；:;？！?!ー〜」』）)】〕〉》’”ゃゅょっぁぃぅぇぉャュョッァィゥェォ");
  return chars.contains(c);
}

static bool isAlnum(QChar c) {
  return c.isLetterOrNumber() && c.unicode() < 0x80;
}

static QStringList wrapLines(const QFontMetricsF& fm, const QString& text, double maxWidth) {
  QStringList out;
  for (const QString& para : text.split(QLatin1Char('\n'))) {
    if (para.isEmpty()) {
      out << QString();
      continue;
    }
    QString line;
    const QList<uint> codepoints = para.toUcs4();
    for (uint cp : codepoints) {
      const QString ch = QString::fromUcs4(reinterpret_cast<const char32_t*>(&cp), 1);
      const QString test = line + ch;
      if (!line.isEmpty() && fm.horizontalAdvance(test) > maxWidth && !noLineStart(ch.front())) {
        const int space = int(line.lastIndexOf(QLatin1Char(' ')));
        if (space > 0 && isAlnum(ch.front()) && isAlnum(line.back())) {
          // 英単語は途中で切らない
          out << line.left(space);
          line = line.mid(space + 1) + ch;
        } else {
          out << line;
          line = ch;
        }
      } else {
        line = test;
      }
    }
    out << line;
  }
  return out;
}

TextLayout layoutText(const TextItem& item, double W, double H) {
  TextLayout L;
  L.fontPx = std::max(4.0, std::round(item.size * H));
  L.font = textFont(item, int(L.fontPx));
  const QFontMetricsF fm(L.font);
  const bool hasBox = item.boxOpacity > 0.001;
  const double padX = hasBox ? L.fontPx * 0.45 : 0;
  const double padY = hasBox ? L.fontPx * 0.2 : 0;
  L.lines = wrapLines(fm, item.text.isEmpty() ? QStringLiteral(" ") : item.text, W * 0.92 - padX * 2);
  L.lineHeight = L.fontPx * 1.3;
  L.ascent = fm.ascent();
  L.descent = fm.descent();
  L.blockW = L.fontPx * 0.5;
  for (const auto& l : L.lines) {
    const double w = fm.horizontalAdvance(l);
    L.lineWidths << w;
    L.blockW = std::max(L.blockW, w);
  }
  L.blockH = L.lineHeight * L.lines.size();
  L.cx = item.x * W;
  L.cy = item.y * H;
  if (hasBox) L.box = QRectF(L.cx - L.blockW / 2 - padX, L.cy - L.blockH / 2 - padY, L.blockW + padX * 2, L.blockH + padY * 2);
  const double stroke = item.strokeWidth * L.fontPx;
  L.bounds = L.box ? *L.box : QRectF(L.cx - L.blockW / 2 - stroke, L.cy - L.blockH / 2 - stroke, L.blockW + stroke * 2, L.blockH + stroke * 2);
  return L;
}

static QPainterPath linePath(const TextItem& item, const TextLayout& L, int i) {
  const double anchorX = item.align == TextAlign::Left ? L.cx - L.blockW / 2 : item.align == TextAlign::Right ? L.cx + L.blockW / 2 : L.cx;
  const double w = L.lineWidths.value(i);
  const double x = item.align == TextAlign::Left ? anchorX : item.align == TextAlign::Right ? anchorX - w : anchorX - w / 2;
  const double mid = L.cy - L.blockH / 2 + L.lineHeight * (i + 0.5);
  const double baseline = mid + (L.ascent - L.descent) / 2;
  QPainterPath path;
  path.addText(QPointF(x, baseline), L.font, L.lines[i]);
  return path;
}

static void drawShapes(QPainter& p, const TextItem& item, const TextLayout& L, bool forShadow) {
  if (L.box) {
    const double r = std::min(L.fontPx * 0.22, L.box->height() / 2);
    QColor c = forShadow ? QColor(0, 0, 0) : item.boxColor;
    if (!forShadow) c.setAlphaF(float(std::clamp(item.boxOpacity, 0.0, 1.0)));
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(*L.box, r, r);
    if (forShadow) return;
  }
  const double stroke = item.strokeWidth * L.fontPx;
  for (int i = 0; i < L.lines.size(); ++i) {
    const QPainterPath path = linePath(item, L, i);
    if (stroke > 0.01) {
      QPen pen(forShadow ? QColor(0, 0, 0) : item.strokeColor, stroke * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      p.strokePath(path, pen);
    }
    p.fillPath(path, forShadow ? QColor(0, 0, 0) : item.color);
  }
}

void drawTextLayout(QPainter& p, const TextItem& item, const TextLayout& L, double pixelScale) {
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::TextAntialiasing);
  if (item.shadow) {
    // 影: 形を黒で描いてぼかし、少し下にずらして重ねる
    const QTransform tr = p.worldTransform();
    const QRectF devBounds = tr.mapRect(L.bounds.adjusted(-L.fontPx, -L.fontPx, L.fontPx, L.fontPx));
    const QRect area = devBounds.toAlignedRect();
    if (area.width() > 0 && area.height() > 0 && area.width() < 16384 && area.height() < 16384) {
      QImage mask(area.size(), QImage::Format_Alpha8);
      mask.fill(0);
      {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing);
        mp.translate(-area.topLeft());
        mp.setWorldTransform(tr, true);
        drawShapes(mp, item, L, true);
      }
      gaussianBlurAlpha(mask, L.fontPx * 0.12 * pixelScale / 2);
      p.save();
      p.resetTransform();
      p.setOpacity(0.55 * p.opacity());
      p.drawImage(QPointF(area.left(), area.top() + L.fontPx * 0.05 * pixelScale), mask);
      p.restore();
    }
  }
  drawShapes(p, item, L, false);
  p.restore();
}

QPixmap renderTextPresetCard(const TextPreset& preset, QSize cardSize, qreal dpr) {
  QPixmap pm(int(std::ceil(cardSize.width() * dpr)), int(std::ceil(cardSize.height() * dpr)));
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(0x3a, 0x3a, 0x3a), 1));
  p.setBrush(QColor(0x2a, 0x2a, 0x2a));
  p.drawRoundedRect(QRectF(0.5, 0.5, cardSize.width() - 1, cardSize.height() - 1), 4, 4);

  TextItem item = makeText(preset.id, 0, presetSampleText());
  item.x = 0.5;
  item.y = 0.5;
  item.size = 0.36;  // スタイル本来の大きさは使わず、全カードで同じ大きさから始める

  // 小さな見本では整数のピクセルサイズで粗くならないよう、4 倍の座標で描いて縮める
  const double k = 4;
  const double W = cardSize.width() * k;
  const double H = cardSize.height() * k;
  const double maxW = W * 0.84;
  const double maxH = H * 0.74;
  // 大きさの計測では折り返しが起きないよう、幅を十分に広くとる
  auto measure = [&] { return layoutText(item, W * 100, H).bounds; };
  for (int i = 0; i < 8; ++i) {
    const QRectF b = measure();
    const double over = std::max(b.width() / maxW, b.height() / maxH);
    if (over <= 1) break;
    item.size *= std::max(0.5, 0.98 / over);
  }

  p.scale(1 / k, 1 / k);
  drawTextLayout(p, item, layoutText(item, W, H), dpr / k);
  return pm;
}

AnimState previewTextState(const TextItem& item, double t, const QString& editingId) {
  if (!editingId.isEmpty() && editingId == item.id) return {1, 0};
  return textAnimState(item, t);
}

QString textSpriteKey(const TextItem& item, QSize canvas) {
  return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
      .arg(item.text, fontToString(item.font), QString::number(item.size), QString::number(item.bold), item.color.name(QColor::HexArgb),
           item.strokeColor.name(QColor::HexArgb), QString::number(item.strokeWidth), item.boxColor.name(QColor::HexArgb),
           QString::number(item.boxOpacity)) +
         QStringLiteral("|%1|%2|%3|%4|%5|%6")
             .arg(QString::number(item.shadow), alignToString(item.align), QString::number(item.x), QString::number(item.y))
             .arg(canvas.width())
             .arg(canvas.height());
}

TextSprite renderTextSprite(const TextItem& item, QSize design, QSize canvas) {
  TextSprite s;
  s.key = textSpriteKey(item, canvas);
  const double scale = double(canvas.width()) / design.width();
  const TextLayout L = layoutText(item, design.width(), design.height());
  const double margin = L.fontPx * 0.4;
  const double bx = L.bounds.x() - margin;
  const double by = L.bounds.y() - margin;
  const int w = std::max(1, int(std::ceil((L.bounds.width() + margin * 2) * scale)));
  const int h = std::max(1, int(std::ceil((L.bounds.height() + margin * 2) * scale)));
  if (w > 16384 || h > 16384) return s;
  s.image = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
  s.image.fill(Qt::transparent);
  {
    QPainter p(&s.image);
    p.scale(scale, scale);
    p.translate(-bx, -by);
    drawTextLayout(p, item, L, scale);
  }
  s.pos = QPointF(bx * scale, by * scale);
  return s;
}

QImage renderTextFrame(const TextItem& item, QSize design, QSize out) {
  QImage frame(out, QImage::Format_ARGB32_Premultiplied);
  frame.fill(Qt::transparent);
  const TextSprite s = renderTextSprite(item, design, out);
  if (!s.image.isNull()) {
    QPainter p(&frame);
    p.drawImage(s.pos, s.image);
  }
  return frame;
}

}  // namespace kv
