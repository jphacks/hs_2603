#include "ui/librarypanel.h"

#include <QContextMenuEvent>
#include <QFileDialog>
#include <QGridLayout>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QMouseEvent>
#include <QPainterPath>
#include <QScrollArea>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "core/editor.h"
#include "core/textstyles.h"
#include "media/audiopreview.h"
#include "media/mediamanager.h"
#include "render/textrender.h"
#include "ui/theme.h"
#include "ui/widgets.h"

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
  Editor& ed = editor();
  connect(&ed, &Editor::assetsChanged, this, &AssetList::rebuild);
  if (kind_ == Kind::Media) connect(&ed, &Editor::thumbsChanged, this, &AssetList::rebuild);
}

void AssetList::rebuild() {
  const Editor& ed = editor();
  const qreal dpr = devicePixelRatioF();
  QStringList selected;
  for (auto* it : selectedItems()) selected << it->data(Qt::UserRole).toString();
  clear();
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
    auto* item = new QListWidgetItem(QIcon(cardPixmap(*a, thumb, dpr)), a->name, this);
    item->setData(Qt::UserRole, id);
    item->setToolTip(a->name);
    item->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    if (selected.contains(id)) item->setSelected(true);
  }
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
  QAction* remove = menu.addAction(QStringLiteral("一覧から削除"));
  QAction* chosen = menu.exec(e->globalPos());
  if (!chosen) return;
  if (chosen == play) audioPreview().toggle(id);
  else if (chosen == add) editor().addToTimeline({id});
  else if (chosen == remove) removeFromList(id);
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

namespace {
/** 3 つのタブを同じ幅にする */
class EvenTabBar : public QTabBar {
 public:
  using QTabBar::QTabBar;

 protected:
  QSize tabSizeHint(int index) const override {
    QSize s = QTabBar::tabSizeHint(index);
    if (count() > 0) s.setWidth(width() / count());
    return s;
  }
  void resizeEvent(QResizeEvent* e) override {
    QTabBar::resizeEvent(e);
    // 幅が変わったら、タブの並びを引き直す
    if (count() > 0) setTabText(0, tabText(0));
  }
};

class LibraryTabs : public QTabWidget {
 public:
  explicit LibraryTabs(QWidget* parent) : QTabWidget(parent) { setTabBar(new EvenTabBar(this)); }
};
}  // namespace

LibraryPanel::LibraryPanel(QWidget* parent) : QWidget(parent) {
  setAutoFillBackground(true);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  auto* tabs = new LibraryTabs(this);
  tabs->setDocumentMode(true);
  tabs->tabBar()->setExpanding(true);
  v->addWidget(tabs);

  // メディア（動画・写真）
  auto* mediaPage = new QWidget(tabs);
  auto* mv = new QVBoxLayout(mediaPage);
  mv->setContentsMargins(10, 10, 6, 0);
  mv->setSpacing(10);
  auto* addBtn = new QPushButton(theme::icon(QStringLiteral("plus")), QStringLiteral("メディアを追加"), mediaPage);
  addBtn->setFocusPolicy(Qt::TabFocus);
  connect(addBtn, &QPushButton::clicked, this, [this] { pickAndImport(this); });
  mv->addWidget(addBtn);
  list_ = new AssetList(AssetList::Kind::Media, mediaPage);
  mv->addWidget(list_, 1);
  tabs->addTab(mediaPage, QStringLiteral("メディア"));

  // オーディオ（BGM・効果音・ナレーションなど）。メディアと同じ構成で、一覧に登録して何度でも使える
  auto* audioPage = new QWidget(tabs);
  auto* av = new QVBoxLayout(audioPage);
  av->setContentsMargins(10, 10, 6, 0);
  av->setSpacing(10);
  auto* addAudio = new QPushButton(theme::icon(QStringLiteral("plus")), QStringLiteral("オーディオを追加"), audioPage);
  addAudio->setFocusPolicy(Qt::TabFocus);
  // 一覧に登録するだけ（タイムラインには置かない）。置くのは、一覧からドラッグするか、ダブルクリック
  connect(addAudio, &QPushButton::clicked, this, [this] { pickAndImport(this, true, false); });
  av->addWidget(addAudio);
  audioList_ = new AssetList(AssetList::Kind::Audio, audioPage);
  av->addWidget(audioList_, 1);
  tabs->addTab(audioPage, QStringLiteral("オーディオ"));

  // テキスト
  auto* scroll = new QScrollArea(tabs);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* textPage = new QWidget(scroll);
  auto* grid = new QGridLayout(textPage);
  grid->setContentsMargins(10, 10, 10, 10);
  grid->setSpacing(6);
  const QSize presetSize(112, 56);
  int i = 0;
  for (const auto& preset : textPresets()) {
    auto* b = makeTextPresetButton(preset, presetSize, textPage);
    b->setAccessibleName(QStringLiteral("テキストスタイル: %1（タイムラインへドラッグして追加）").arg(preset.label));
    b->setToolTip(QStringLiteral("%1\nタイムラインへドラッグして追加").arg(preset.label));
    // クリックでは追加しない。選んだスタイルの枠を強調して確認できるようにするだけ
    b->setCheckable(true);
    b->setAutoExclusive(true);
    b->setStyleSheet(b->styleSheet() + QStringLiteral("QToolButton:checked{border-color:#2680eb;color:#ffffff}"));
    makeTextPresetDraggable(b, preset);
    const QString label = preset.label;
    connect(b, &QToolButton::clicked, this, [label] {
      editor().toast(QStringLiteral("「%1」をタイムラインへドラッグすると、好きな位置に追加できます").arg(label));
    });
    grid->addWidget(b, i / 2, i % 2);
    ++i;
  }
  grid->setRowStretch(i / 2 + 1, 1);
  scroll->setWidget(textPage);
  tabs->addTab(scroll, QStringLiteral("テキスト"));
}

}  // namespace kv
