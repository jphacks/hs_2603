#include "core/subtitles.h"

#include <QRegularExpression>
#include <QStringConverter>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace kv {

namespace {

/** 文字コードを見分けて、文字列にする（BOM → UTF-8 として正しいか → Windows の文字コード） */
QString decodeText(const QByteArray& data) {
  if (const auto bom = QStringConverter::encodingForData(data)) {
    QStringDecoder dec(*bom);
    return dec.decode(data);
  }
  QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
  const QString s = utf8.decode(data);
  if (!utf8.hasError()) return s;
  QStringDecoder sys(QStringConverter::System);
  return sys.decode(data);
}

/** "01:02:03,450" / "02:03.450" / "01:02:03" を秒にする。読めなければ負 */
double parseTime(const QString& s) {
  static const QRegularExpression re(QStringLiteral("^\\s*(?:(\\d+):)?(\\d{1,2}):(\\d{1,2})(?:[,.](\\d{1,3}))?\\s*$"));
  const auto m = re.match(s);
  if (!m.hasMatch()) return -1;
  const double h = m.captured(1).isEmpty() ? 0 : m.captured(1).toDouble();
  const double min = m.captured(2).toDouble();
  const double sec = m.captured(3).toDouble();
  QString frac = m.captured(4);
  double ms = 0;
  if (!frac.isEmpty()) {
    while (frac.size() < 3) frac += QLatin1Char('0');
    ms = frac.toDouble();
  }
  return h * 3600 + min * 60 + sec + ms / 1000;
}

QString formatTimestamp(double t, bool vtt) {
  const long long ms = std::llround(std::max(0.0, t) * 1000);
  const long long h = ms / 3600000;
  const long long m = (ms / 60000) % 60;
  const long long s = (ms / 1000) % 60;
  const long long f = ms % 1000;
  return QStringLiteral("%1:%2:%3%4%5")
      .arg(h, 2, 10, QLatin1Char('0'))
      .arg(m, 2, 10, QLatin1Char('0'))
      .arg(s, 2, 10, QLatin1Char('0'))
      .arg(vtt ? QLatin1Char('.') : QLatin1Char(','))
      .arg(f, 3, 10, QLatin1Char('0'));
}

/** <i> <b> <font ...> などの印と、{\an8} のような位置の印を外す */
QString stripTags(QString s) {
  static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
  static const QRegularExpression ass(QStringLiteral("\\{\\\\[^}]*\\}"));
  s.remove(tags);
  s.remove(ass);
  s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
  s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
  s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
  s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
  return s;
}

}  // namespace

std::vector<SubtitleCue> parseSubtitles(const QByteArray& data, QString* error) {
  QString text = decodeText(data);
  text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
  std::vector<SubtitleCue> cues;
  const QStringList lines = text.split(QLatin1Char('\n'));
  int i = 0;
  const int n = int(lines.size());
  while (i < n) {
    const QString line = lines[i].trimmed();
    const int arrow = int(line.indexOf(QStringLiteral("-->")));
    if (arrow < 0) {
      ++i;
      continue;
    }
    // 時刻の行（WebVTT は、時刻のあとに位置などの指定が続くことがある）
    const double start = parseTime(line.left(arrow));
    QString right = line.mid(arrow + 3).trimmed();
    const int space = int(right.indexOf(QRegularExpression(QStringLiteral("\\s"))));
    if (space > 0) right = right.left(space);
    const double end = parseTime(right);
    ++i;
    QStringList body;
    while (i < n && !lines[i].trimmed().isEmpty()) {
      // 空行なしで次の字幕が続いたときに備えて、次の時刻の行が来たら止める（その前の番号の行は、字幕の文字に入れない）
      if (lines[i].contains(QStringLiteral("-->"))) {
        static const QRegularExpression number(QStringLiteral("^\\s*\\d+\\s*$"));
        if (!body.isEmpty() && number.match(body.back()).hasMatch()) body.removeLast();
        break;
      }
      body << lines[i];
      ++i;
    }
    if (start < 0 || end <= start) continue;
    const QString content = stripTags(body.join(QLatin1Char('\n'))).trimmed();
    if (content.isEmpty()) continue;
    cues.push_back({start, end, content});
  }
  std::stable_sort(cues.begin(), cues.end(), [](const SubtitleCue& a, const SubtitleCue& b) { return a.start < b.start; });
  if (cues.empty() && error) *error = QStringLiteral("字幕が見つかりませんでした（SRT か WebVTT のファイルを選んでください）");
  return cues;
}

QByteArray writeSubtitles(const std::vector<SubtitleCue>& cues, bool vtt) {
  QString out;
  if (vtt) out += QStringLiteral("WEBVTT\n\n");
  int index = 1;
  for (const SubtitleCue& c : cues) {
    if (!vtt) out += QString::number(index++) + QLatin1Char('\n');
    out += formatTimestamp(c.start, vtt) + QStringLiteral(" --> ") + formatTimestamp(c.end, vtt) + QLatin1Char('\n');
    // 空行は字幕の区切りなので、字幕の中の空行は詰める
    QStringList lines;
    for (const QString& l : c.text.split(QLatin1Char('\n'))) {
      if (!l.trimmed().isEmpty()) lines << l;
    }
    out += lines.join(QLatin1Char('\n')) + QStringLiteral("\n\n");
  }
  return out.toUtf8();
}

}  // namespace kv
