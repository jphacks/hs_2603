#include "render/textrender.h"

#include <QFontMetricsF>
#include <QPainterPath>
#include <QSet>
#include <QTextBoundaryFinder>
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
  // 字間: 文字と文字の間隔を px で足し引きする（0 のときは設定せず、フォント本来の間隔のまま）。
  // 幅の測定（QFontMetricsF）も、描画（QPainterPath::addText）も、この QFont の字間を使うので、配置・折り返し・範囲が同じになる
  if (std::abs(item.letterSpacing) > 1e-6) f.setLetterSpacing(QFont::AbsoluteSpacing, item.letterSpacing);
  return f;
}

/**
 * 1 行の見た目の幅。Qt は字間を各文字の後ろに足すので、行の最後の文字の後ろに付く 1 回分は引く
 * （引かないと、字間が広いほど右に余白が付いて、中央揃え・右揃えがずれる）
 */
static double lineAdvance(const QFontMetricsF& fm, const QString& line, double spacing) {
  if (line.isEmpty()) return 0;
  return std::max(0.0, fm.horizontalAdvance(line) - spacing);
}

// 行頭に来てはいけない文字（禁則処理の簡易版）
static bool noLineStart(QChar c) {
  static const QString chars = QStringLiteral("、。，．,.・：；:;？！?!ー〜」』）)】〕〉》’”ゃゅょっぁぃぅぇぉャュョッァィゥェォ");
  return chars.contains(c);
}

static bool isAlnum(QChar c) {
  return c.isLetterOrNumber() && c.unicode() < 0x80;
}

/** starts には、各行の先頭が text の何文字目（UTF-16）かを入れる（入力位置と行の対応づけに使う） */
static QStringList wrapLines(const QFontMetricsF& fm, const QString& text, double maxWidth, double spacing, QList<int>* starts) {
  QStringList out;
  int paraStart = 0;
  for (const QString& para : text.split(QLatin1Char('\n'))) {
    if (para.isEmpty()) {
      out << QString();
      if (starts) *starts << paraStart;
      paraStart += 1;  // 改行の分
      continue;
    }
    QString line;
    int lineStartIdx = paraStart;
    int pos = paraStart;  // いま見ている文字の位置
    const QList<uint> codepoints = para.toUcs4();
    for (uint cp : codepoints) {
      const QString ch = QString::fromUcs4(reinterpret_cast<const char32_t*>(&cp), 1);
      const QString test = line + ch;
      if (!line.isEmpty() && lineAdvance(fm, test, spacing) > maxWidth && !noLineStart(ch.front())) {
        const int space = int(line.lastIndexOf(QLatin1Char(' ')));
        if (space > 0 && isAlnum(ch.front()) && isAlnum(line.back())) {
          // 英単語は途中で切らない（行の最後の空白は、行に入れずに捨てる）
          out << line.left(space);
          if (starts) *starts << lineStartIdx;
          line = line.mid(space + 1) + ch;
          lineStartIdx += space + 1;
        } else {
          out << line;
          if (starts) *starts << lineStartIdx;
          line = ch;
          lineStartIdx = pos;
        }
      } else {
        line = test;
      }
      pos += int(ch.size());
    }
    out << line;
    if (starts) *starts << lineStartIdx;
    paraStart += int(para.size()) + 1;
  }
  return out;
}

static QPainterPath linePath(const TextItem& item, const TextLayout& L, int i);

TextLayout layoutText(const TextItem& item, double W, double H) {
  TextLayout L;
  L.fontPx = std::max(4.0, std::round(item.size * H));
  L.font = textFont(item, int(L.fontPx));
  const QFontMetricsF fm(L.font);
  const bool hasBox = item.boxOpacity > 0.001;
  const double padX = hasBox ? L.fontPx * 0.45 : 0;
  const double padY = hasBox ? L.fontPx * 0.2 : 0;
  L.lines = wrapLines(fm, item.text.isEmpty() ? QStringLiteral(" ") : item.text, W * 0.92 - padX * 2, item.letterSpacing, &L.lineStart);
  L.lineHeight = L.fontPx * 1.3;
  L.ascent = fm.ascent();
  L.descent = fm.descent();
  L.blockW = L.fontPx * 0.5;
  for (const auto& l : L.lines) {
    const double w = lineAdvance(fm, l, item.letterSpacing);
    L.lineWidths << w;
    L.blockW = std::max(L.blockW, w);
  }
  L.blockH = L.lineHeight * L.lines.size();
  L.cx = item.x * W;
  L.cy = item.y * H;
  if (hasBox) L.box = QRectF(L.cx - L.blockW / 2 - padX, L.cy - L.blockH / 2 - padY, L.blockW + padX * 2, L.blockH + padY * 2);
  const double stroke = item.strokeWidth * L.fontPx;
  L.bounds = L.box ? *L.box : QRectF(L.cx - L.blockW / 2 - stroke, L.cy - L.blockH / 2 - stroke, L.blockW + stroke * 2, L.blockH + stroke * 2);
  if (item.letterSpacing < 0) {
    // 字間をマイナスにして、文字の進みが 0 以下になるほど詰めると、文字が行の幅の外へはみ出す。
    // 描く範囲が切れないよう、実際に描く形の範囲まで広げる（ふつうの詰め方では、行の幅の中に収まるので変わらない）
    QRectF ink;
    for (int i = 0; i < L.lines.size(); ++i) ink = ink.united(linePath(item, L, i).boundingRect());
    if (!ink.isNull()) L.bounds = L.bounds.united(ink.adjusted(-stroke, -stroke, stroke, stroke));
  }
  return L;
}

/** 行 i の左端の x（揃えに合わせる）。w はその行の幅 */
static double lineOriginX(const TextItem& item, const TextLayout& L, double w) {
  const double anchorX = item.align == TextAlign::Left ? L.cx - L.blockW / 2 : item.align == TextAlign::Right ? L.cx + L.blockW / 2 : L.cx;
  return item.align == TextAlign::Left ? anchorX : item.align == TextAlign::Right ? anchorX - w : anchorX - w / 2;
}

/** 行 i の文字の基準線 */
static double lineBaseline(const TextLayout& L, int i) {
  const double mid = L.cy - L.blockH / 2 + L.lineHeight * (i + 0.5);
  return mid + (L.ascent - L.descent) / 2;
}

static QPainterPath linePath(const TextItem& item, const TextLayout& L, int i) {
  QPainterPath path;
  path.addText(QPointF(lineOriginX(item, L, L.lineWidths.value(i)), lineBaseline(L, i)), L.font, L.lines[i]);
  return path;
}

// ---- 入力位置（文字の直接編集） -----------------------------------------------------------------

namespace {
struct LinePos {
  int line = 0;
  int offset = 0;  // 行の中の何文字目か
};
}  // namespace

/** 文字が空のときは、描画用の仮の空白ではなく、空の行として扱う */
static QString editLineText(const TextItem& item, const TextLayout& L, int i) {
  return item.text.isEmpty() ? QString() : L.lines.value(i);
}

static double editLineWidth(const TextItem& item, const TextLayout& L, int i) {
  return item.text.isEmpty() ? 0.0 : L.lineWidths.value(i);
}

static LinePos locateIndex(const TextItem& item, const TextLayout& L, int index, bool upstream) {
  const int n = int(L.lines.size());
  index = std::clamp(index, 0, int(item.text.size()));
  int li = 0;
  for (int i = n - 1; i >= 0; --i) {
    if (L.lineStart.value(i) <= index) {
      li = i;
      break;
    }
  }
  if (upstream && li > 0 && index == L.lineStart.value(li)) {
    // 折り返しの境目: 上の行の末尾に置く（捨てた空白や改行がある境目は、位置が違うので対象外）
    const int prevEnd = L.lineStart.value(li - 1) + int(editLineText(item, L, li - 1).size());
    if (prevEnd == index) --li;
  }
  const int len = int(editLineText(item, L, li).size());
  return {li, std::clamp(index - L.lineStart.value(li), 0, len)};
}

static double prefixAdvance(const QFontMetricsF& fm, const QString& line, int offset) {
  return offset <= 0 ? 0.0 : fm.horizontalAdvance(line.left(offset));
}

QLineF textCaretLine(const TextItem& item, const TextLayout& L, int index, bool upstream) {
  const LinePos lp = locateIndex(item, L, index, upstream);
  const QFontMetricsF fm(L.font);
  const QString line = editLineText(item, L, lp.line);
  const double x = lineOriginX(item, L, editLineWidth(item, L, lp.line)) + prefixAdvance(fm, line, lp.offset);
  const double base = lineBaseline(L, lp.line);
  return QLineF(x, base - L.ascent, x, base + L.descent);
}

int textIndexAt(const TextItem& item, const TextLayout& L, QPointF p, bool* upstream) {
  if (upstream) *upstream = false;
  const int n = int(L.lines.size());
  const int li = std::clamp(int(std::floor((p.y() - (L.cy - L.blockH / 2)) / L.lineHeight)), 0, std::max(0, n - 1));
  const QString line = editLineText(item, L, li);
  const QFontMetricsF fm(L.font);
  const double rel = p.x() - lineOriginX(item, L, editLineWidth(item, L, li));
  // 文字の境目ごとの x を調べて、いちばん近い境目を選ぶ（合字や結合文字の途中には置かない）
  int best = 0;
  double bestD = std::abs(rel);
  QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, line);
  for (int pos = finder.toNextBoundary(); pos != -1; pos = finder.toNextBoundary()) {
    const double d = std::abs(rel - fm.horizontalAdvance(line.left(pos)));
    if (d < bestD) {
      bestD = d;
      best = pos;
    }
  }
  const int index = L.lineStart.value(li) + best;
  if (upstream && best == line.size() && li + 1 < n && L.lineStart.value(li + 1) == index) *upstream = true;
  return index;
}

QList<QRectF> textRangeRects(const TextItem& item, const TextLayout& L, int from, int to) {
  QList<QRectF> out;
  if (from > to) std::swap(from, to);
  const QFontMetricsF fm(L.font);
  const int n = int(L.lines.size());
  for (int i = 0; i < n; ++i) {
    const QString line = editLineText(item, L, i);
    const int ls = L.lineStart.value(i);
    const int le = ls + int(line.size());
    const int s = std::max(from, ls);
    const int e = std::min(to, le);
    // 行の終わりの改行も選んでいるときは、右に少し広げて、改行が選ばれているのが見えるようにする
    const bool newline = i + 1 < n && to > le && from <= le && L.lineStart.value(i + 1) > le && item.text.mid(le, 1) == QStringLiteral("\n");
    if (s >= e && !newline) continue;
    const double ox = lineOriginX(item, L, editLineWidth(item, L, i));
    const double x0 = ox + prefixAdvance(fm, line, std::max(s, ls) - ls);
    double x1 = ox + prefixAdvance(fm, line, std::max(e, s) - ls);
    if (newline) x1 += L.fontPx * 0.3;
    const double base = lineBaseline(L, i);
    out << QRectF(x0, base - L.ascent, std::max(1.0, x1 - x0), L.ascent + L.descent);
  }
  return out;
}

int textMoveVertical(const TextItem& item, const TextLayout& L, int index, bool upstream, int dir, double x, bool* upstreamOut) {
  if (upstreamOut) *upstreamOut = false;
  const LinePos lp = locateIndex(item, L, index, upstream);
  const int target = lp.line + (dir < 0 ? -1 : 1);
  if (target < 0) return 0;
  if (target >= int(L.lines.size())) return int(item.text.size());
  const double y = L.cy - L.blockH / 2 + L.lineHeight * (target + 0.5);
  return textIndexAt(item, L, QPointF(x, y), upstreamOut);
}

int textLineEdge(const TextItem& item, const TextLayout& L, int index, bool upstream, bool end, bool* upstreamOut) {
  if (upstreamOut) *upstreamOut = false;
  const LinePos lp = locateIndex(item, L, index, upstream);
  const int ls = L.lineStart.value(lp.line);
  if (!end) return ls;
  const int e = ls + int(editLineText(item, L, lp.line).size());
  if (upstreamOut) *upstreamOut = lp.line + 1 < int(L.lines.size()) && L.lineStart.value(lp.line + 1) == e;
  return e;
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
  // 字間も絵柄を決める値（変えたときに、古い画像が残らないようキーに入れる）
  return QStringLiteral("%1|").arg(QString::number(item.letterSpacing, 'f', 3)) +
         QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
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
