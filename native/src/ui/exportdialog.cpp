#include "ui/exportdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QSettings>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

#include "core/editor.h"
#include "core/timeline.h"
#include "export/exporter.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

// AI の設定・キーの割り当てと同じ settings.ini（最後に選んだ書き出しの形式を覚える）
static QString exportSettingsPath() {
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("settings.ini"));
}

static QString formatEta(double sec) {
  if (sec < 0 || !std::isfinite(sec)) return {};
  if (sec < 60) return QStringLiteral("残り約%1秒").arg(std::max<long>(1, std::lround(sec)));
  return QStringLiteral("残り約%1分%2秒").arg(long(sec) / 60).arg(std::lround(std::fmod(sec, 60)));
}

ExportDialog::ExportDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("書き出し"));
  setModal(true);
  setMinimumWidth(460);
  exporter_ = new Exporter(this);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 16);
  pages_ = new QStackedWidget(this);
  v->addWidget(pages_);

  // 設定
  auto* settings = new QWidget(pages_);
  auto* sv = new QVBoxLayout(settings);
  sv->setContentsMargins(0, 0, 0, 0);
  sv->setSpacing(14);
  auto* sum = new QFrame(settings);
  sum->setStyleSheet(QStringLiteral("QFrame{background:#222222;border:1px solid #2a2a2a;border-radius:4px}QLabel{background:transparent;border:none}"));
  auto* sg = new QGridLayout(sum);
  sg->setContentsMargins(10, 8, 10, 8);
  sg->setHorizontalSpacing(24);
  const QStringList heads{QStringLiteral("長さ"), QStringLiteral("解像度"), QStringLiteral("形式")};
  for (int i = 0; i < 3; ++i) {
    sg->addWidget(makeLabel(heads[i], "muted", sum), 0, i);
    summary_[i] = new QLabel(sum);
    sg->addWidget(summary_[i], 1, i);
  }
  sv->addWidget(sum);
  // 書き出す部分。タイムラインで範囲を決めてあるときだけ出す（I / O キー、または目盛りを Shift を押しながらドラッグ）
  partBox_ = new QWidget(settings);
  auto* pv = new QVBoxLayout(partBox_);
  pv->setContentsMargins(0, 0, 0, 0);
  pv->setSpacing(6);
  pv->addWidget(makeLabel(QStringLiteral("書き出す部分"), "muted", partBox_));
  part_ = new Segmented({{0, QStringLiteral("全体")}, {1, QStringLiteral("指定した範囲")}}, 0, partBox_);
  part_->setValue(1);
  pv->addWidget(part_);
  partNote_ = makeLabel(QString(), "muted", partBox_);
  pv->addWidget(partNote_);
  sv->addWidget(partBox_);
  // 形式（MP4 のほか、H.265・WebM・ProRes・GIF・音声だけ）。最後に選んだものを覚える
  sv->addWidget(makeLabel(QStringLiteral("形式"), "muted", settings));
  format_ = new QComboBox(settings);
  format_->setObjectName(QStringLiteral("ExportFormat"));  // 操作の案内（ai/guidecatalog.cpp）で探す名前
  format_->setAccessibleName(QStringLiteral("書き出す形式"));
  for (const ExportFormatInfo& f : exportFormats()) format_->addItem(f.label, int(f.format));
  {
    ExportFormat last = ExportFormat::Mp4H264;
    exportFormatFromString(QSettings(exportSettingsPath(), QSettings::IniFormat).value(QStringLiteral("export/format")).toString(), &last);
    format_->setCurrentIndex(std::max(0, format_->findData(int(last))));
  }
  sv->addWidget(format_);
  formatHint_ = makeLabel(QString(), "dim", settings);
  formatHint_->setWordWrap(true);
  sv->addWidget(formatHint_);
  resBox_ = new QWidget(settings);
  auto* resv = new QVBoxLayout(resBox_);
  resv->setContentsMargins(0, 0, 0, 0);
  resv->setSpacing(6);
  resv->addWidget(makeLabel(QStringLiteral("解像度"), "muted", resBox_));
  resolution_ = new Segmented({{720, QStringLiteral("720p")}, {1080, QStringLiteral("1080p")}, {2160, QStringLiteral("4K")}}, 0, resBox_);
  resolution_->setValue(1080);
  resolution_->setProperty("kvGuide", QStringLiteral("ExportResolution"));  // Segmented の objectName は見た目に使うので、別に持たせる
  resv->addWidget(resolution_);
  // GIF は大きくすると、ファイルがとても大きくなるので、小さめの大きさから選ぶ
  gifRes_ = new Segmented({{240, QStringLiteral("240p")}, {360, QStringLiteral("360p")}, {480, QStringLiteral("480p")}}, 0, resBox_);
  gifRes_->setValue(360);
  resv->addWidget(gifRes_);
  sv->addWidget(resBox_);
  sv->addWidget(makeLabel(QStringLiteral("品質"), "muted", settings));
  quality_ = new Segmented({{int(ExportQuality::Fast), QStringLiteral("高速")},
                            {int(ExportQuality::Standard), QStringLiteral("標準")},
                            {int(ExportQuality::High), QStringLiteral("高画質")}},
                           0, settings);
  quality_->setValue(int(ExportQuality::Standard));
  sv->addWidget(quality_);
  connect(quality_, &Segmented::changed, this, &ExportDialog::refreshSummary);
  auto* sb = new QHBoxLayout();
  sb->addStretch(1);
  auto* cancel = new QPushButton(QStringLiteral("キャンセル"), settings);
  auto* go = new QPushButton(theme::icon(QStringLiteral("download"), Qt::white), QStringLiteral("書き出し"), settings);
  go->setObjectName(QStringLiteral("ExportStart"));
  go->setProperty("primary", true);
  go->setDefault(true);
  sb->addWidget(cancel);
  sb->addWidget(go);
  sv->addLayout(sb);
  pages_->addWidget(settings);

  // 書き出し中
  auto* running = new QWidget(pages_);
  auto* rv = new QVBoxLayout(running);
  rv->setContentsMargins(0, 0, 0, 0);
  rv->setSpacing(12);
  auto* rh = new QHBoxLayout();
  rh->addWidget(makeLabel(QStringLiteral("書き出し中…"), "muted", running));
  rh->addStretch(1);
  eta_ = makeLabel(QString(), "muted", running);
  rh->addWidget(eta_);
  rv->addLayout(rh);
  progress_ = new QProgressBar(running);
  progress_->setRange(0, 1000);
  progress_->setTextVisible(false);
  progress_->setFixedHeight(6);
  progress_->setAccessibleName(QStringLiteral("書き出しの進捗"));
  rv->addWidget(progress_);
  auto* rb = new QHBoxLayout();
  pct_ = makeLabel(QStringLiteral("0%"), "muted", running);
  rb->addWidget(pct_);
  rb->addStretch(1);
  auto* stop = new QPushButton(QStringLiteral("中止"), running);
  rb->addWidget(stop);
  rv->addLayout(rb);
  pages_->addWidget(running);

  // 完了
  auto* done = new QWidget(pages_);
  auto* dv = new QVBoxLayout(done);
  dv->setContentsMargins(0, 0, 0, 0);
  dv->setSpacing(8);
  auto* icon = new QLabel(done);
  icon->setPixmap(theme::iconPixmap(QStringLiteral("check"), 36, theme::ok, devicePixelRatioF()));
  icon->setAlignment(Qt::AlignCenter);
  dv->addWidget(icon);
  auto* doneTitle = makeLabel(QStringLiteral("書き出しが完了しました"), "title", done);
  doneTitle->setAlignment(Qt::AlignCenter);
  dv->addWidget(doneTitle);
  donePath_ = makeLabel(QString(), "muted", done);
  donePath_->setAlignment(Qt::AlignCenter);
  donePath_->setWordWrap(true);
  donePath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  dv->addWidget(donePath_);
  auto* db = new QHBoxLayout();
  db->addStretch(1);
  auto* folder = new QPushButton(theme::icon(QStringLiteral("folder-open")), QStringLiteral("フォルダを開く"), done);
  auto* play = new QPushButton(theme::icon(QStringLiteral("play"), Qt::white), QStringLiteral("再生"), done);
  play->setProperty("primary", true);
  db->addWidget(folder);
  db->addWidget(play);
  db->addStretch(1);
  dv->addSpacing(6);
  dv->addLayout(db);
  pages_->addWidget(done);

  // 失敗
  auto* failed = new QWidget(pages_);
  auto* fv = new QVBoxLayout(failed);
  fv->setContentsMargins(0, 0, 0, 0);
  fv->setSpacing(10);
  fv->addWidget(makeLabel(QStringLiteral("書き出しに失敗しました"), "error", failed));
  errorDetail_ = makeLabel(QString(), "muted", failed);
  errorDetail_->setWordWrap(true);
  errorDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  errorDetail_->setStyleSheet(QStringLiteral("background:#0f0f0f;border:1px solid #2a2a2a;border-radius:4px;padding:8px;font-size:8pt"));
  fv->addWidget(errorDetail_);
  auto* fb = new QHBoxLayout();
  fb->addStretch(1);
  auto* close = new QPushButton(QStringLiteral("閉じる"), failed);
  auto* retry = new QPushButton(QStringLiteral("再試行"), failed);
  retry->setProperty("primary", true);
  fb->addWidget(close);
  fb->addWidget(retry);
  fv->addLayout(fb);
  pages_->addWidget(failed);

  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(resolution_, &Segmented::changed, this, &ExportDialog::refreshSummary);
  connect(gifRes_, &Segmented::changed, this, &ExportDialog::refreshSummary);
  connect(part_, &Segmented::changed, this, &ExportDialog::refreshSummary);
  connect(format_, &QComboBox::currentIndexChanged, this, [this] {
    QSettings(exportSettingsPath(), QSettings::IniFormat).setValue(QStringLiteral("export/format"), exportFormatInfo(format()).id);
    refreshSummary();
  });
  // 開いている間にタイムラインで範囲を変えたら、表示を合わせる（書き出し中は変えない）
  connect(&editor(), &Editor::rangeChanged, this, [this] {
    if (!exporter_->running()) refreshSummary();
  });
  connect(go, &QPushButton::clicked, this, [this] {
    static QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    // 保存したプロジェクトならそのファイル名、まだなら今のタブの名前（タブごとに別のファイルになるように）
    QString name = editor().projectPath().isEmpty() ? editor().workspaceName(editor().workspaceIndex())
                                                    : QFileInfo(editor().projectPath()).completeBaseName();
    if (name.isEmpty()) name = QStringLiteral("無題");
    // 保存先を選んでいる間に別のタブへ切り替わっても（別のウィンドウから開かれたときなど）、選んだときのタブを書き出す
    const int uid = editor().workspaceUid(editor().workspaceIndex());
    const ExportFormatInfo& info = exportFormatInfo(format());
    const QString ext = QLatin1Char('.') + info.extension;
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("書き出し"), dir + QLatin1Char('/') + name + ext, info.fileFilter);
    if (path.isEmpty()) return;
    const int index = editor().indexOfWorkspaceUid(uid);
    if (index < 0) return;
    editor().switchWorkspace(index);
    refreshSummary();
    if (!path.endsWith(ext, Qt::CaseInsensitive)) path += ext;
    dir = QFileInfo(path).absolutePath();
    begin(path);
  });
  connect(stop, &QPushButton::clicked, exporter_, &Exporter::cancel);
  connect(folder, &QPushButton::clicked, this, [this] {
    QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(result_)});
  });
  connect(play, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(result_)); });
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(retry, &QPushButton::clicked, this, [this] { pages_->setCurrentIndex(0); });
  connect(exporter_, &Exporter::progress, this, [this](double ratio, double, double eta) {
    progress_->setValue(int(std::lround(ratio * 1000)));
    pct_->setText(QStringLiteral("%1%").arg(std::lround(ratio * 100)));
    eta_->setText(formatEta(eta));
    emit progressChanged(ratio, true);
  });
  connect(exporter_, &Exporter::finished, this, [this](bool ok, bool canceled, const QString& out, const QString& err, double) {
    emit progressChanged(0, false);
    if (ok) {
      result_ = out;
      donePath_->setText(out);
      pages_->setCurrentIndex(2);
      QApplication::alert(parentWidget());
    } else if (canceled) {
      pages_->setCurrentIndex(0);
      editor().toast(QStringLiteral("書き出しを中止しました"));
    } else {
      errorDetail_->setText(err);
      pages_->setCurrentIndex(3);
    }
  });
  refreshSummary();
}

void ExportDialog::showEvent(QShowEvent* e) {
  QDialog::showEvent(e);
  // 書き出し中に再表示されたときは、進行中の内容を変えない
  if (exporter_->running()) return;
  refreshSummary();
}

void ExportDialog::refreshSummary() {
  const Editor& ed = editor();
  setWindowTitle(QStringLiteral("書き出し - %1").arg(ed.workspaceName(ed.workspaceIndex())));
  const ExportFormatInfo& info = exportFormatInfo(format());
  formatHint_->setText(info.hint);
  // 音だけの形式では解像度を選ばない。GIF は小さめの大きさから選ぶ
  resBox_->setVisible(info.video);
  resolution_->setVisible(info.format != ExportFormat::Gif);
  gifRes_->setVisible(info.format == ExportFormat::Gif);
  const QSize size = exportSize(ed.project().settings.aspect, shortSide());
  const std::optional<Editor::TimeRange> range = ed.exportRange();
  partBox_->setVisible(range.has_value());
  if (range) {
    partNote_->setText(QStringLiteral("範囲: %1 〜 %2").arg(formatTime(range->start, true), formatTime(range->end, true)));
  }
  // 書き出す動画の長さ（動画全体の再生速度を変えていれば、その速さでの長さ）
  const double speed = ed.project().settings.speed;
  const double length = (rangeSelected() ? range->end - range->start : ed.layout().total) / speed;
  summary_[0]->setText(std::abs(speed - 1) > 1e-9 ? QStringLiteral("%1（%2倍速）").arg(formatDuration(length), QString::number(speed, 'g', 4))
                                                  : formatDuration(length));
  if (!info.video) summary_[1]->setText(QStringLiteral("（音声だけ）"));
  else if (info.format == ExportFormat::Gif)
    summary_[1]->setText(QStringLiteral("%1 × %2・%3fps").arg(size.width()).arg(size.height()).arg(gifFps(quality(), ed.project().settings.fps)));
  else summary_[1]->setText(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()));
  summary_[2]->setText(info.extension.toUpper());
}

ExportFormat ExportDialog::format() const {
  return ExportFormat(format_->currentData().toInt());
}

ExportQuality ExportDialog::quality() const {
  return ExportQuality(quality_->value().toInt());
}

int ExportDialog::shortSide() const {
  return format() == ExportFormat::Gif ? gifRes_->value().toInt() : resolution_->value().toInt();
}

bool ExportDialog::rangeSelected() const {
  return editor().exportRange().has_value() && part_->value().toInt() == 1;
}

void ExportDialog::begin(const QString& path) {
  QString error;
  progress_->setValue(0);
  pct_->setText(QStringLiteral("0%"));
  eta_->clear();
  pages_->setCurrentIndex(1);
  if (!exporter_->start(path, quality(), shortSide(), &error, rangeSelected(), format())) {
    errorDetail_->setText(error);
    pages_->setCurrentIndex(3);
    return;
  }
  emit progressChanged(0, true);
}

void ExportDialog::startWithPath(const QString& path) {
  refreshSummary();
  begin(path);
}

bool ExportDialog::isRunning() const {
  return exporter_->running();
}

void ExportDialog::reject() {
  if (exporter_->running()) return;
  pages_->setCurrentIndex(0);
  QDialog::reject();
}

}  // namespace kv
