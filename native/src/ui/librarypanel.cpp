#include "ui/librarypanel.h"

#include <QButtonGroup>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QMouseEvent>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <functional>

#include "ai/account.h"
#include "ai/workshop.h"
#include "core/editor.h"
#include "core/textstyles.h"
#include "core/texttemplates.h"
#include "media/audiopreview.h"
#include "media/ffutil.h"
#include "media/mediamanager.h"
#include "render/textrender.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "ui/workshopdialog.h"

namespace kv {

static const QStringList kVideoExts = {"mp4", "mov", "m4v", "avi", "mkv", "webm", "wmv", "mts", "m2ts", "ts", "flv", "3gp", "mpg", "mpeg"};
static const QStringList kImageExts = {"jpg", "jpeg", "png", "webp", "bmp", "gif", "tif", "tiff", "heic", "heif", "avif"};
static const QStringList kAudioExts = {"mp3", "wav", "m4a", "aac", "ogg", "flac", "wma", "opus"};

static QString patterns(const QStringList& exts) {
  QStringList out;
  for (const auto& e : exts) out << QStringLiteral("*.") + e;
  return out.join(QLatin1Char(' '));
}

void LibraryPanel::pickAndImport(QWidget* parent, bool audioOnly, bool addToTimeline) {
  QString filter;
  if (audioOnly) {
    filter = QStringLiteral("オーディオ (%1);;すべてのファイル (*)").arg(patterns(kAudioExts));
  } else {
    filter = QStringLiteral("メディア (%1);;ビデオ (%2);;画像 (%3);;オーディオ (%4);;すべてのファイル (*)")
                 .arg(patterns(kVideoExts + kImageExts + kAudioExts), patterns(kVideoExts), patterns(kImageExts), patterns(kAudioExts));
  }
  static QString lastDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  const QStringList files =
      QFileDialog::getOpenFileNames(parent, audioOnly ? QStringLiteral("オーディオを追加") : QStringLiteral("メディアを追加"), lastDir, filter);
  if (files.isEmpty()) return;
  lastDir = QFileInfo(files.front()).absolutePath();
  // 登録の処理はメディアもオーディオも同じ（同じファイルは 1 つにまとめられる）。違うのは、タイムラインに置くかどうかだけ
  if (audioOnly) media().importPaths(files, addToTimeline, std::nullopt, editor().currentTime());
  else media().importPaths(files, addToTimeline);
}

void LibraryPanel::pickFolderAndImport(QWidget* parent, bool audioOnly) {
  if (media().batchRunning()) {
    editor().toast(QStringLiteral("フォルダから読み込んでいる途中です。終わってから、もう一度選んでください"));
    return;
  }
  static QString lastDir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  QWidget* owner = parent ? parent->window() : nullptr;
  const QString title = audioOnly ? QStringLiteral("オーディオのフォルダを選択") : QStringLiteral("メディアのフォルダを選択");
  const QString dir = QFileDialog::getExistingDirectory(owner, title, lastDir);
  if (dir.isEmpty()) return;
  lastDir = dir;
  // サブフォルダがあるときは、その中のファイルも追加するかを聞く
  bool recursive = false;
  if (!QDir(dir).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
    QMessageBox box(QMessageBox::Question, title,
                    QStringLiteral("「%1」の中には、フォルダがあります。\nサブフォルダの中のファイルも追加しますか？").arg(QDir::toNativeSeparators(dir)),
                    QMessageBox::NoButton, owner);
    QPushButton* only = box.addButton(QStringLiteral("このフォルダだけ"), QMessageBox::AcceptRole);
    QPushButton* sub = box.addButton(QStringLiteral("サブフォルダも含める"), QMessageBox::AcceptRole);
    QPushButton* cancel = box.addButton(QStringLiteral("キャンセル"), QMessageBox::RejectRole);
    box.setDefaultButton(only);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() == sub) recursive = true;
    else if (box.clickedButton() != only) return;
  }
  // 動画・写真はメディアの一覧、音声はオーディオの一覧に入る（ファイル名はそのまま）。対応していない形式は飛ばす
  media().importFolder(dir, recursive, audioOnly ? kAudioExts : kVideoExts + kImageExts + kAudioExts);
}

// ---- AssetList --------------------------------------------------------------

static constexpr int kCardW = 116;
static constexpr int kThumbH = 70;

static QPixmap cardPixmap(const Asset& a, const QImage* thumb, qreal dpr) {
  QPixmap pm(int(kCardW * dpr), int(kThumbH * dpr));
  pm.setDevicePixelRatio(dpr);
  pm.fill(a.kind == AssetKind::Audio ? QColor(0x16, 0x27, 0x1f) : QColor(0x0c, 0x0c, 0x0c));
  QPainter p(&pm);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF r(0, 0, kCardW, kThumbH);
  if (thumb && !thumb->isNull()) {
    const QSizeF s = thumb->size();
    const double k = std::max(r.width() / s.width(), r.height() / s.height());
    const QSizeF d(s.width() * k, s.height() * k);
    p.drawImage(QRectF((r.width() - d.width()) / 2, (r.height() - d.height()) / 2, d.width(), d.height()), *thumb);
  } else {
    const QString name = a.kind == AssetKind::Audio ? QStringLiteral("music") : a.kind == AssetKind::Image ? QStringLiteral("image") : QStringLiteral("film");
    const QColor c = a.kind == AssetKind::Audio ? QColor(0x5f, 0xbf, 0x8f) : theme::dim;
    p.drawPixmap(QPointF(r.center().x() - 12, r.center().y() - 12), theme::iconPixmap(name, 24, c, dpr));
  }
  if (a.kind != AssetKind::Image && a.duration > 0) {
    QFont f = p.font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    const QString t = formatTime(a.duration, false);
    const QRectF tr = QFontMetricsF(f).boundingRect(t).adjusted(-4, -1, 4, 1);
    const QRectF box(r.right() - tr.width() - 4, r.bottom() - tr.height() - 4, tr.width(), tr.height());
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 190));
    p.drawRoundedRect(box, 2, 2);
    p.setPen(Qt::white);
    p.drawText(box, Qt::AlignCenter, t);
  }
  QString status;
  if (a.status == AssetStatus::Loading && a.kind == AssetKind::Image) status = QStringLiteral("読み込み中");
  if (a.status == AssetStatus::Missing) status = QStringLiteral("見つかりません");
  if (a.status == AssetStatus::Error) status = QStringLiteral("読み込めません");
  if (!status.isEmpty()) {
    p.fillRect(r, QColor(12, 12, 12, 190));
    p.setPen(a.status == AssetStatus::Loading ? theme::text : QColor(0xff, 0x9a, 0x9e));
    p.drawText(r, Qt::AlignCenter, status);
  }
  return pm;
}

// ---- オーディオ一覧の 1 行 ---------------------------------------------------------

static constexpr int kRowH = 54;

/** 行の右端にある 2 つのボタンの場所（試聴 / 一覧から削除）。描画とクリックの判定で同じものを使う */
static QRect playButtonRect(const QRect& row) {
  return QRect(row.right() - 8 - 26 - 4 - 28, row.center().y() - 14, 28, 28);
}
static QRect removeButtonRect(const QRect& row) {
  return QRect(row.right() - 8 - 26, row.center().y() - 13, 26, 26);
}

class AudioRowDelegate : public QStyledItemDelegate {
 public:
  explicit AudioRowDelegate(QListWidget* view) : QStyledItemDelegate(view), view_(view) {}

  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
    return QSize(std::max(80, view_->viewport()->width() - 2), kRowH);
  }

  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
    const Asset* a = editor().asset(index.data(Qt::UserRole).toString());
    if (!a) return;
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hover = opt.state & QStyle::State_MouseOver;
    const bool usable = a->status != AssetStatus::Missing && a->status != AssetStatus::Error;
    const bool playing = audioPreview().playingAsset() == a->id;
    const qreal dpr = p->device()->devicePixelRatioF();
    const QRectF r = QRectF(opt.rect).adjusted(1, 2, -1, -2);

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(selected ? QPen(theme::accent, 1.5) : QPen(theme::border, 1));
    p->setBrush(selected ? QColor(0x23, 0x2a, 0x35) : hover ? theme::panel3 : theme::panel2);
    p->drawRoundedRect(r, 6, 6);

    // 音声のアイコン
    const QRectF tile(r.left() + 8, r.center().y() - 17, 34, 34);
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(0x16, 0x27, 0x1f));
    p->drawRoundedRect(tile, 5, 5);
    p->drawPixmap(QPointF(tile.center().x() - 11, tile.center().y() - 11),
                  theme::iconPixmap(QStringLiteral("music"), 22, usable ? QColor(0x5f, 0xbf, 0x8f) : theme::dim, dpr));

    // ファイル名（長いときは省略）と、長さ（読み込めないときはその理由）
    const int textLeft = int(tile.right()) + 10;
    const int textRight = playButtonRect(opt.rect).left() - 6;
    const QRect nameRect(textLeft, int(r.top()) + 8, std::max(10, textRight - textLeft), 18);
    QFont nf = p->font();
    nf.setPointSizeF(9);
    p->setFont(nf);
    p->setPen(usable ? theme::text : theme::muted);
    p->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(nf).elidedText(a->name, Qt::ElideMiddle, nameRect.width()));
    QString info = a->duration > 0 ? formatTime(a->duration, false) : QString();
    QColor infoColor = theme::muted;
    if (a->status == AssetStatus::Missing) {
      info = QStringLiteral("ファイルが見つかりません");
      infoColor = QColor(0xff, 0x9a, 0x9e);
    } else if (a->status == AssetStatus::Error) {
      info = QStringLiteral("読み込めません");
      infoColor = QColor(0xff, 0x9a, 0x9e);
    } else if (a->status == AssetStatus::Loading) {
      info = QStringLiteral("読み込み中...");
    }
    QFont sf = p->font();
    sf.setPointSizeF(8.5);
    p->setFont(sf);
    p->setPen(infoColor);
    p->drawText(QRect(textLeft, int(r.top()) + 28, std::max(10, textRight - textLeft), 16), Qt::AlignVCenter | Qt::AlignLeft,
                QFontMetrics(sf).elidedText(info, Qt::ElideRight, std::max(10, textRight - textLeft)));

    // 試聴ボタン（再生 ▶ / 停止 ■）
    const QRectF pb = playButtonRect(opt.rect);
    p->setPen(Qt::NoPen);
    p->setBrush(!usable ? QColor(0x2a, 0x2a, 0x2a) : playing ? theme::accent : QColor(0x3a, 0x3a, 0x3a));
    p->drawEllipse(pb);
    p->setBrush(usable ? QColor(Qt::white) : theme::dim);
    if (playing) {
      p->drawRoundedRect(QRectF(pb.center().x() - 5, pb.center().y() - 5, 10, 10), 1.5, 1.5);
    } else {
      QPainterPath tri;
      tri.moveTo(pb.center().x() - 3.5, pb.center().y() - 6);
      tri.lineTo(pb.center().x() + 6.5, pb.center().y());
      tri.lineTo(pb.center().x() - 3.5, pb.center().y() + 6);
      tri.closeSubpath();
      p->drawPath(tri);
    }
    // 一覧から削除するボタン
    const QRectF rb = removeButtonRect(opt.rect);
    p->drawPixmap(QPointF(rb.center().x() - 8, rb.center().y() - 8), theme::iconPixmap(QStringLiteral("trash"), 16, hover || selected ? theme::muted : theme::dim, dpr));
    p->restore();
  }

 private:
  QListWidget* view_;
};

AssetList::AssetList(Kind kind, QWidget* parent) : QListWidget(parent), kind_(kind) {
  if (kind_ == Kind::Audio) {
    // 1 行ずつのカード
    setViewMode(QListView::ListMode);
    setFlow(QListView::TopToBottom);
    setWrapping(false);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setSpacing(0);
    setUniformItemSizes(false);  // 一覧の幅が変わったら、行の幅も合わせ直す
    setItemDelegate(new AudioRowDelegate(this));
    setMouseTracking(true);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // 試聴の状態が変わったら描き直す
    connect(&audioPreview(), &AudioPreview::changed, this, [this] { viewport()->update(); });
  } else {
    setViewMode(QListView::IconMode);
    setFlow(QListView::LeftToRight);
    setWrapping(true);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setIconSize(QSize(kCardW, kThumbH));
    setGridSize(QSize(kCardW + 8, kThumbH + 28));
    setSpacing(0);
    setUniformItemSizes(true);
    setTextElideMode(Qt::ElideMiddle);
    setWordWrap(false);
  }
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setDragEnabled(true);
  setDragDropMode(QAbstractItemView::DragOnly);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  connect(this, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem* item) {
    const QString id = item->data(Qt::UserRole).toString();
    const Asset* a = editor().asset(id);
    if (a && a->status != AssetStatus::Error && a->status != AssetStatus::Missing) editor().addToTimeline({id});
  });
  // 登録・サムネイルの知らせが続けて来ても（フォルダからまとめて登録しているときなど）、作り直すのは少しあとにまとめて 1 回
  rebuildTimer_ = new QTimer(this);
  rebuildTimer_->setSingleShot(true);
  rebuildTimer_->setInterval(60);
  connect(rebuildTimer_, &QTimer::timeout, this, &AssetList::rebuild);
  Editor& ed = editor();
  connect(&ed, &Editor::assetsChanged, this, &AssetList::scheduleRebuild);
  if (kind_ == Kind::Media) connect(&ed, &Editor::thumbsChanged, this, &AssetList::scheduleRebuild);
  rebuild();
}

void AssetList::scheduleRebuild() {
  if (!rebuildTimer_->isActive()) rebuildTimer_->start();
}

void AssetList::rebuild() {
  rebuildTimer_->stop();
  const Editor& ed = editor();
  const qreal dpr = devicePixelRatioF();
  QStringList selected;
  for (auto* it : selectedItems()) selected << it->data(Qt::UserRole).toString();
  const int scroll = verticalScrollBar()->value();
  setUpdatesEnabled(false);
  clear();
  std::map<QString, Card> cards;
  for (const QString& id : ed.assetOrder()) {
    const Asset* a = ed.asset(id);
    if (!a) continue;
    // メディアの一覧には動画と写真、オーディオの一覧には音声だけを並べる
    if ((a->kind == AssetKind::Audio) != (kind_ == Kind::Audio)) continue;
    if (kind_ == Kind::Audio) {
      auto* item = new QListWidgetItem(a->name, this);
      item->setData(Qt::UserRole, id);
      item->setToolTip(a->name);
      item->setSizeHint(QSize(std::max(80, viewport()->width() - 2), kRowH));
      if (selected.contains(id)) item->setSelected(true);
      continue;
    }
    const auto* thumbs = ed.thumbs(id);
    const QImage* thumb = thumbs && !thumbs->empty() ? &thumbs->front() : nullptr;
    // 絵が変わっていないカードは、前に描いたものを使う（たくさん並んでいても、作り直しが重くならない）
    Card card;
    card.status = int(a->status);
    card.duration = a->duration;
    card.thumb = thumb ? thumb->cacheKey() : 0;
    card.dpr = dpr;
    const auto old = cards_.find(id);
    if (old != cards_.end() && old->second.status == card.status && old->second.duration == card.duration && old->second.thumb == card.thumb &&
        old->second.dpr == card.dpr) {
      card.icon = old->second.icon;
    } else {
      card.icon = QIcon(cardPixmap(*a, thumb, dpr));
    }
    auto* item = new QListWidgetItem(card.icon, a->name, this);
    cards[id] = card;
    item->setData(Qt::UserRole, id);
    item->setToolTip(a->name);
    item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    if (selected.contains(id)) item->setSelected(true);
  }
  cards_ = std::move(cards);  // 一覧から無くなったものの絵は手放す
  // 作り直しても、見ていた位置のままにする（並べ直してから、スクロールの位置を戻す）
  doItemsLayout();
  verticalScrollBar()->setValue(scroll);
  setUpdatesEnabled(true);
}

QStringList AssetList::mimeTypes() const {
  return {QString::fromLatin1(kAssetMime)};
}

QMimeData* AssetList::mimeData(const QList<QListWidgetItem*>& items) const {
  auto* m = new QMimeData();
  QStringList ids;
  for (auto* it : items) {
    const QString id = it->data(Qt::UserRole).toString();
    const Asset* a = editor().asset(id);
    if (a && a->status != AssetStatus::Error && a->status != AssetStatus::Missing) ids << id;
  }
  m->setData(QString::fromLatin1(kAssetMime), ids.join(QLatin1Char('\n')).toUtf8());
  return m;
}

void AssetList::removeFromList(const QString& id) {
  // 試聴中の素材なら止めてから外す。タイムラインで使っている素材は removeAsset が断る（理由を知らせる）
  if (audioPreview().playingAsset() == id) audioPreview().stop();
  editor().removeAsset(id);
}

/** 調べたファイルの種類（写真・動画・音声） */
static AssetKind kindOf(const ProbeInfo& info) {
  return info.isImage ? AssetKind::Image : info.hasVideo ? AssetKind::Video : AssetKind::Audio;
}

/** メディア id を、調べ終わったファイル path につなぎ直して、サムネイル・波形を作り直す */
static void relinkTo(const QString& id, const QString& path, const ProbeInfo& info) {
  const AssetKind kind = kindOf(info);
  editor().relinkAsset(id, [&](Asset& x) {
    x.path = QFileInfo(path).absoluteFilePath();
    x.name = QFileInfo(path).fileName();
    x.duration = kind == AssetKind::Image ? 0 : info.duration;
    x.width = info.width;
    x.height = info.height;
    x.hasAudio = kind == AssetKind::Video ? info.hasAudio : kind == AssetKind::Audio;
    x.imagePath.clear();
    x.error.clear();
    x.status = AssetStatus::Loading;
  });
  if (const Asset* now = editor().asset(id)) media().prepare(*now);
}

/**
 * 見つからないメディア（ファイルを動かした・名前を変えた）の場所を指定して、つなぎ直す（リンクし直す）。
 * 同じフォルダに、ほかの見つからないメディアと同じ名前のファイルがあれば、それもまとめてつなぎ直す
 */
static void relinkAssetInteractive(QWidget* parent, const QString& id) {
  const Asset* a = editor().asset(id);
  if (!a) return;
  // ファイルを選ぶ画面を出している間に一覧が変わることがあるので、使う値は先に写しておく
  const QString oldName = QFileInfo(a->path).fileName();
  const QString assetName = a->name;
  const double oldDuration = a->duration;
  const AssetKind kind = a->kind;
  QString dir = QFileInfo(a->path).absolutePath();
  if (!QDir(dir).exists()) dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString filter = kind == AssetKind::Image   ? QStringLiteral("写真 (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.heic *.heif *.tif *.tiff);;すべてのファイル (*)")
                         : kind == AssetKind::Audio ? QStringLiteral("音声 (*.mp3 *.wav *.m4a *.aac *.flac *.ogg *.opus *.wma);;すべてのファイル (*)")
                                                    : QStringLiteral("動画 (*.mp4 *.mov *.m4v *.avi *.mkv *.webm *.wmv *.mts *.m2ts *.gif);;すべてのファイル (*)");
  const QString path = QFileDialog::getOpenFileName(parent, QStringLiteral("「%1」の場所を指定").arg(assetName), QDir(dir).filePath(oldName), filter);
  if (path.isEmpty() || !editor().asset(id)) return;
  ProbeInfo info;
  QString error;
  if (!probeMedia(path, info, error)) {
    editor().toast(QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), error), ToastKind::Error);
    return;
  }
  if (kindOf(info) != kind) {
    editor().toast(QStringLiteral("種類の違うファイルです（動画は動画、写真は写真、音声は音声のファイルを選んでください）"), ToastKind::Error);
    return;
  }
  const bool shorter = kind != AssetKind::Image && oldDuration > 0 && info.duration + 0.05 < oldDuration;
  relinkTo(id, path, info);
  // 同じフォルダにある、ほかの見つからないメディア（同じ名前のファイル）も、つなぎ直す
  const QDir folder = QFileInfo(path).absoluteDir();
  std::vector<std::pair<QString, QString>> more;
  for (const auto& [otherId, other] : editor().assets()) {
    if (otherId == id || other.status != AssetStatus::Missing) continue;
    const QString candidate = folder.filePath(QFileInfo(other.path).fileName());
    if (QFileInfo(candidate).isFile()) more.push_back({otherId, candidate});
  }
  int relinked = 0;
  for (const auto& [otherId, candidate] : more) {
    const Asset* other = editor().asset(otherId);
    ProbeInfo oi;
    QString oe;
    if (!other || !probeMedia(candidate, oi, oe) || kindOf(oi) != other->kind) continue;
    relinkTo(otherId, candidate, oi);
    ++relinked;
  }
  QString message = relinked > 0 ? QStringLiteral("つなぎ直しました（同じフォルダの、見つからなかったファイル %1 個も）").arg(relinked)
                                 : QStringLiteral("つなぎ直しました");
  if (shorter) message += QStringLiteral("。元のファイルより短いので、足りない所は映りません");
  editor().toast(message, ToastKind::Success);
}

void AssetList::contextMenuEvent(QContextMenuEvent* e) {
  QListWidgetItem* item = itemAt(e->pos());
  if (!item) return;
  const QString id = item->data(Qt::UserRole).toString();
  const Asset* a = editor().asset(id);
  const bool usable = a && a->status != AssetStatus::Error && a->status != AssetStatus::Missing;
  QMenu menu(this);
  QAction* play = nullptr;
  if (kind_ == Kind::Audio) {
    play = menu.addAction(audioPreview().playingAsset() == id ? QStringLiteral("試聴を止める") : QStringLiteral("試聴する"));
    play->setEnabled(usable);
  }
  QAction* add = menu.addAction(QStringLiteral("タイムラインに追加"));
  add->setEnabled(usable);
  // 動画・写真は、メインの映像の上に重ねることもできる（再生位置から。タイムラインの「重ねる」の段へドラッグしても同じ）
  QAction* overlay = nullptr;
  if (kind_ == Kind::Media) {
    overlay = menu.addAction(QStringLiteral("重ねて追加（再生位置から）"));
    overlay->setEnabled(usable);
  }
  QAction* remove = menu.addAction(QStringLiteral("一覧から削除"));
  // ファイルを動かした・名前を変えたとき: 場所を指定して、つなぎ直す（見つからないときは、目立つように先頭の言い方にする）
  QAction* relink = menu.addAction(a && a->status == AssetStatus::Missing ? QStringLiteral("ファイルの場所を指定（リンクし直す）…")
                                                                          : QStringLiteral("別のファイルにリンクし直す…"));
  relink->setEnabled(a != nullptr);
  // オーディオ・写真・動画（動く GIF も）は、ワークショップに公開できる（AI サーバーで使えるときだけ。ワークショップから追加したものは、作った人のものなので出さない）
  QAction* publish = nullptr;
  const bool image = a && a->kind == AssetKind::Image;
  const bool video = kind_ == Kind::Media && a && a->kind == AssetKind::Video;
  if ((kind_ == Kind::Audio || image || video) && ai::workshopVisible()) {
    menu.addSeparator();
    if (a && (image ? ai::isWorkshopImage(a->path) : video ? ai::isWorkshopVideo(a->path) : ai::isWorkshopAudio(a->path))) {
      menu.addAction(image   ? QStringLiteral("ワークショップから追加した写真")
                     : video ? QStringLiteral("ワークショップから追加した動画")
                             : QStringLiteral("ワークショップから追加したオーディオ"))
          ->setEnabled(false);
    } else {
      publish = menu.addAction(theme::icon(QStringLiteral("upload")), QStringLiteral("ワークショップに公開…"));
      publish->setEnabled(usable);
    }
  }
  QAction* chosen = menu.exec(e->globalPos());
  if (!chosen) return;
  if (chosen == play) audioPreview().toggle(id);
  else if (chosen == add) editor().addToTimeline({id});
  else if (overlay && chosen == overlay) editor().addOverlays({id});
  else if (chosen == remove) removeFromList(id);
  else if (chosen == relink) relinkAssetInteractive(this, id);
  else if (publish && chosen == publish) {
    if (image) publishImageAsset(this, id);
    else if (video) publishVideoAsset(this, id);
    else publishAudioAsset(this, id);
  }
}

void AssetList::mousePressEvent(QMouseEvent* e) {
  if (kind_ == Kind::Audio && e->button() == Qt::LeftButton) {
    if (QListWidgetItem* item = itemAt(e->pos())) {
      const QRect row = visualItemRect(item);
      const QString id = item->data(Qt::UserRole).toString();
      // 行の右端のボタン: 試聴 / 一覧から削除（ドラッグや選択の動きにはしない）
      if (playButtonRect(row).contains(e->pos())) {
        audioPreview().toggle(id);
        e->accept();
        return;
      }
      if (removeButtonRect(row).contains(e->pos())) {
        removeFromList(id);
        e->accept();
        return;
      }
    }
  }
  QListWidget::mousePressEvent(e);
}

void AssetList::paintEvent(QPaintEvent* e) {
  QListWidget::paintEvent(e);
  // 何も登録していないオーディオ一覧には、使い方を出す
  if (kind_ == Kind::Audio && count() == 0) {
    QPainter p(viewport());
    p.setPen(theme::dim);
    p.drawText(viewport()->rect().adjusted(16, 24, -16, -16), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
               QStringLiteral("音楽・効果音・ナレーションを追加すると、ここに並びます。\n\nタイムラインへドラッグして、好きな位置に置けます。何度でも使えます。"));
  }
}

// ---- LibraryPanel -----------------------------------------------------------

// 欄の幅をこれより狭くしない（メディアのカードが 1 列は入る幅）
static constexpr int kViewMinWidth = 220;

/**
 * 幅が足りないときは短い名前に、さらに足りなければアイコンだけにするボタン（名前が切れて読めなくならないように）。
 * 幅は並べる側で決める（欄が狭くても、横に並べられる）
 */
class FitButton : public QPushButton {
 public:
  FitButton(const QIcon& icon, const QString& longText, const QString& shortText, QWidget* parent)
      : QPushButton(icon, longText, parent), long_(longText), short_(shortText) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setMinimumWidth(40);
    // 左右の余白は少なめにして、なるべく元の名前のまま出す（名前は中央に置くので、広いときの見た目は変わらない）
    setStyleSheet(QStringLiteral("QPushButton{padding-left:4px;padding-right:4px}"));
    setAccessibleName(longText);  // アイコンだけのときも、読み上げでは名前がわかる
  }

 protected:
  void resizeEvent(QResizeEvent* e) override {
    QPushButton::resizeEvent(e);
    fit();
  }

 private:
  void fit() {
    const QFontMetrics fm(font());
    // 名前のほかに要る幅（アイコン・余白・枠）。名前が出ているときに測っておく
    if (!text().isEmpty()) overhead_ = std::max(overhead_, QPushButton::sizeHint().width() - fm.horizontalAdvance(text()));
    QString next;
    if (width() >= overhead_ + fm.horizontalAdvance(long_)) next = long_;
    else if (width() >= overhead_ + fm.horizontalAdvance(short_)) next = short_;
    if (next != text()) setText(next);
  }
  QString long_;
  QString short_;
  int overhead_ = 0;
};

/** 「ファイルを選択」「フォルダを選択」のボタンを横に並べた行。name は操作の案内で探す名前の始まり（Media・Audio） */
static QWidget* makeAddRow(QWidget* parent, const QString& name, const QString& fileTip, const QString& folderTip, std::function<void()> pickFiles,
                           std::function<void()> pickFolder) {
  auto* row = new QWidget(parent);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(4);
  auto* files = new FitButton(theme::icon(QStringLiteral("plus")), QStringLiteral("ファイルを選択"), QStringLiteral("ファイル"), row);
  auto* folder = new FitButton(theme::icon(QStringLiteral("folder-open")), QStringLiteral("フォルダを選択"), QStringLiteral("フォルダ"), row);
  files->setObjectName(name + QStringLiteral("AddFiles"));
  folder->setObjectName(name + QStringLiteral("AddFolder"));
  files->setToolTip(fileTip);
  folder->setToolTip(folderTip);
  for (QPushButton* b : {static_cast<QPushButton*>(files), static_cast<QPushButton*>(folder)}) {
    b->setFocusPolicy(Qt::TabFocus);
    b->setAccessibleDescription(b->toolTip());
    h->addWidget(b, 1);
  }
  QObject::connect(files, &QPushButton::clicked, row, std::move(pickFiles));
  QObject::connect(folder, &QPushButton::clicked, row, std::move(pickFolder));
  return row;
}

/** フォルダからまとめて読み込んでいる間の、進み具合（件数・バー・やめるボタン） */
static QWidget* makeImportProgress(QWidget* parent) {
  auto* box = new QWidget(parent);
  auto* v = new QVBoxLayout(box);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(4);
  auto* top = new QHBoxLayout();
  top->setSpacing(4);
  auto* label = makeLabel(QString(), "muted", box);
  auto* stop = new QToolButton(box);
  stop->setIcon(theme::icon(QStringLiteral("x"), theme::muted));
  stop->setIconSize(QSize(12, 12));
  stop->setToolTip(QStringLiteral("読み込みをやめる（そこまでに登録したものは残ります）"));
  stop->setAccessibleName(QStringLiteral("フォルダからの読み込みをやめる"));
  top->addWidget(label, 1);
  top->addWidget(stop);
  v->addLayout(top);
  auto* bar = new QProgressBar(box);
  bar->setTextVisible(false);
  bar->setAccessibleName(QStringLiteral("フォルダからの読み込みの進み具合"));
  v->addWidget(bar);
  box->hide();
  QObject::connect(&media(), &MediaManager::importProgress, box, [box, label, bar](int done, int total) {
    box->show();
    if (total <= 0) {
      label->setText(QStringLiteral("フォルダの中を調べています…"));
      bar->setRange(0, 0);  // 数がわかるまでは、動き続けるバー
      return;
    }
    label->setText(QStringLiteral("読み込み中… %1 / %2 件").arg(done).arg(total));
    bar->setRange(0, total);
    bar->setValue(done);
  });
  QObject::connect(&media(), &MediaManager::importFinished, box, [box] { box->hide(); });
  QObject::connect(stop, &QToolButton::clicked, box, [] { media().cancelImport(); });
  return box;
}

/**
 * ワークショップ（みんなが公開したものを探して追加する窓）を kind の一覧で開くボタンの行（テキストの一覧の「ワークショップ」と同じ）。
 * AI サーバーでワークショップを使えるときだけ出す
 */
static QWidget* makeWorkshopRow(QWidget* parent, ai::WorkshopKind kind, const QString& name, const QString& tip) {
  auto* row = new QWidget(parent);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  h->addStretch(1);
  auto* button = new QPushButton(theme::icon(QStringLiteral("download")), QStringLiteral("ワークショップ"), row);
  button->setObjectName(name);
  button->setToolTip(tip);
  button->setAccessibleDescription(tip);
  button->setFocusPolicy(Qt::TabFocus);
  h->addWidget(button);
  QObject::connect(button, &QPushButton::clicked, button, [button, kind] { openWorkshop(button, kind); });
  auto sync = [row] { row->setVisible(ai::workshopVisible()); };
  sync();
  QObject::connect(&ai::account(), &ai::Account::changed, row, sync);
  QObject::connect(&ai::account(), &ai::Account::linkChanged, row, sync);
  return row;
}

QWidget* LibraryPanel::createMediaView(QWidget* parent) {
  auto* mediaPage = new QWidget(parent);
  mediaPage->setMinimumWidth(kViewMinWidth);
  auto* mv = new QVBoxLayout(mediaPage);
  mv->setContentsMargins(10, 10, 6, 0);
  mv->setSpacing(10);
  // ファイルを選ぶ（これまでどおり、選んだものはタイムラインにも置く）/ フォルダを選ぶ（中のものをまとめて一覧に登録する）
  mv->addWidget(makeAddRow(mediaPage, QStringLiteral("Media"), QStringLiteral("動画・写真・音声のファイルを選んで追加します（タイムラインにも置きます）"),
                           QStringLiteral("フォルダの中の動画・写真・音声を、まとめて一覧に登録します（動画・写真はメディア、音声はオーディオの一覧へ）"),
                           [mediaPage] { pickAndImport(mediaPage); }, [mediaPage] { pickFolderAndImport(mediaPage, false); }));
  mv->addWidget(makeWorkshopRow(mediaPage, ai::WorkshopKind::Image, QStringLiteral("MediaWorkshopButton"),
                                QStringLiteral("みんなが公開した写真・短い動画（GIF も）を探して、メディアの一覧に追加します\n"
                                               "自分の写真・動画は、一覧で右クリックして「ワークショップに公開」で公開できます")));
  mv->addWidget(makeImportProgress(mediaPage));
  auto* list = new AssetList(AssetList::Kind::Media, mediaPage);
  list->setObjectName(QStringLiteral("MediaList"));  // 操作の案内で探す名前
  mv->addWidget(list, 1);
  return mediaPage;
}

QWidget* LibraryPanel::createAudioView(QWidget* parent) {
  // メディアと同じ構成で、一覧に登録して何度でも使える
  auto* audioPage = new QWidget(parent);
  audioPage->setMinimumWidth(kViewMinWidth);
  auto* av = new QVBoxLayout(audioPage);
  av->setContentsMargins(10, 10, 6, 0);
  av->setSpacing(10);
  // 一覧に登録するだけ（タイムラインには置かない）。置くのは、一覧からドラッグするか、ダブルクリック
  av->addWidget(makeAddRow(audioPage, QStringLiteral("Audio"), QStringLiteral("音楽・効果音・ナレーションのファイルを選んで、一覧に登録します"),
                           QStringLiteral("フォルダの中の音声ファイルを、まとめて一覧に登録します"),
                           [audioPage] { pickAndImport(audioPage, true, false); }, [audioPage] { pickFolderAndImport(audioPage, true); }));
  av->addWidget(makeWorkshopRow(audioPage, ai::WorkshopKind::Audio, QStringLiteral("AudioWorkshopButton"),
                                QStringLiteral("みんなが公開したオーディオ（BGM・効果音など）を試聴して、オーディオの一覧に追加します\n"
                                               "自分のものは、一覧で右クリックして「ワークショップに公開」で公開できます")));
  av->addWidget(makeImportProgress(audioPage));
  auto* list = new AssetList(AssetList::Kind::Audio, audioPage);
  list->setObjectName(QStringLiteral("AudioList"));  // 操作の案内で探す名前
  av->addWidget(list, 1);
  return audioPage;
}

/** テキストのスタイル・テンプレートのカード（タイムラインへドラッグして追加する） */
static QToolButton* makeTextCard(const TextPreset& preset, QWidget* parent, QButtonGroup* group) {
  const QSize presetSize(112, 56);
  auto* b = makeTextPresetButton(preset, presetSize, parent);
  const bool user = TextTemplates::isUserId(preset.id);
  b->setAccessibleName(QStringLiteral("テキストスタイル: %1（タイムラインへドラッグして追加）").arg(preset.label));
  if (user) {
    // ワークショップから追加したもの・公開したものは、そのことも出す
    const TemplateWorkshopInfo info = textTemplates().workshopInfo(preset.id);
    QString origin;
    if (!info.sourceId.isEmpty()) origin = QStringLiteral("\nワークショップから追加（作った人: %1）").arg(info.author);
    else if (!info.publishedId.isEmpty()) origin = QStringLiteral("\nワークショップに公開中");
    b->setToolTip(QStringLiteral("%1（マイテンプレート）%2\nタイムラインへドラッグして追加。右クリックで名前の変更・削除・ワークショップへの公開").arg(preset.label, origin));
  } else {
    b->setToolTip(QStringLiteral("%1\nタイムラインへドラッグして追加").arg(preset.label));
  }
  // クリックでは追加しない。選んだスタイルの枠を強調して確認できるようにするだけ
  b->setCheckable(true);
  group->addButton(b);
  b->setStyleSheet(b->styleSheet() + QStringLiteral("QToolButton:checked{border-color:#2680eb;color:#ffffff}"));
  makeTextPresetDraggable(b, preset);
  const QString label = preset.label;
  QObject::connect(b, &QToolButton::clicked, b, [label] {
    editor().toast(QStringLiteral("「%1」をタイムラインへドラッグすると、好きな位置に追加できます").arg(label));
  });
  if (user) {
    const QString id = preset.id;
    b->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(b, &QWidget::customContextMenuRequested, b, [b, id](const QPoint& pos) { execTemplateMenu(b, id, b->mapToGlobal(pos)); });
  }
  return b;
}

QWidget* LibraryPanel::createTextView(QWidget* parent) {
  auto* scroll = new QScrollArea(parent);
  scroll->setMinimumWidth(kViewMinWidth);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* textPage = new QWidget(scroll);
  auto* v = new QVBoxLayout(textPage);
  v->setContentsMargins(10, 10, 10, 10);
  v->setSpacing(8);
  // カードは 1 つだけ選んだ見た目にする（組み込みのスタイルとテンプレートをまたいで）
  auto* group = new QButtonGroup(textPage);
  group->setExclusive(true);
  auto* grid = new QGridLayout();
  grid->setSpacing(6);
  int i = 0;
  for (const auto& preset : textPresets()) {
    QToolButton* card = makeTextCard(preset, textPage, group);
    card->setObjectName(QStringLiteral("TextPresetCard"));  // 操作の案内で探す名前（最初のカードを指す）
    grid->addWidget(card, i / 2, i % 2);
    ++i;
  }
  v->addLayout(grid);
  // 自分で保存したテンプレート（右の設定欄の「テンプレートに保存」・テキストの右クリックで増える）
  v->addWidget(makeSeparator(textPage));
  auto* userHead = new QHBoxLayout();
  userHead->setSpacing(6);
  userHead->addWidget(makeLabel(QStringLiteral("マイテンプレート"), "muted", textPage));
  userHead->addStretch(1);
  // ワークショップ（みんなが公開したテンプレートを探す・自分のものを公開する）。AI サーバーで使えるときだけ出す
  auto* workshopButton = new QPushButton(theme::icon(QStringLiteral("download")), QStringLiteral("ワークショップ"), textPage);
  workshopButton->setObjectName(QStringLiteral("WorkshopButton"));
  workshopButton->setToolTip(QStringLiteral("みんなが公開したテキストのテンプレートを探して、マイテンプレートに追加します\n"
                                            "自分のテンプレートは、右クリックの「ワークショップに公開」で公開できます"));
  workshopButton->setFocusPolicy(Qt::TabFocus);
  userHead->addWidget(workshopButton);
  v->addLayout(userHead);
  QObject::connect(workshopButton, &QPushButton::clicked, workshopButton, [workshopButton] { openWorkshop(workshopButton, ai::WorkshopKind::Text); });
  auto syncWorkshop = [workshopButton] { workshopButton->setVisible(ai::workshopVisible()); };
  syncWorkshop();
  QObject::connect(&ai::account(), &ai::Account::changed, workshopButton, syncWorkshop);
  QObject::connect(&ai::account(), &ai::Account::linkChanged, workshopButton, syncWorkshop);
  auto* userBox = new QWidget(textPage);
  auto* userGrid = new QGridLayout(userBox);
  userGrid->setContentsMargins(0, 0, 0, 0);
  userGrid->setSpacing(6);
  v->addWidget(userBox);
  auto* hint = makeLabel(QStringLiteral("テキストを選んで、右の設定欄の「今の見た目をテンプレートに保存」を押すと、ここに並びます。"
                                        "フォント・色・登場のアニメーション・キーフレームを、何度でも使えます"),
                         "dim", textPage);
  hint->setWordWrap(true);
  v->addWidget(hint);
  v->addStretch(1);
  auto rebuild = [userBox, userGrid, group, hint] {
    while (QLayoutItem* it = userGrid->takeAt(0)) {
      if (QWidget* w = it->widget()) {
        if (auto* b = qobject_cast<QAbstractButton*>(w)) group->removeButton(b);
        w->deleteLater();
      }
      delete it;
    }
    int k = 0;
    for (const auto& p : textTemplates().list()) {
      userGrid->addWidget(makeTextCard(p, userBox, group), k / 2, k % 2);
      ++k;
    }
    hint->setVisible(k == 0);
  };
  rebuild();
  QObject::connect(&textTemplates(), &TextTemplates::changed, userBox, rebuild);
  scroll->setWidget(textPage);
  return scroll;
}

}  // namespace kv
