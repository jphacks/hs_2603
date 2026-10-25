#include "ui/workshopdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

#include "ai/account.h"
#include "ai/workshop.h"
#include "core/editor.h"
#include "core/fontlibrary.h"
#include "core/texttemplates.h"
#include "media/audiopreview.h"
#include "media/ffutil.h"
#include "media/mediamanager.h"
#include <QImageReader>
#include "render/textrender.h"
#include "ui/accountdialog.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

using ai::WorkshopKind;
using ai::WorkshopTemplate;

namespace {

/** カード 1 枚の幅の目安（窓の幅に合わせて、横に並べる数を決める） */
constexpr int kCardWidth = 210;
const QSize kCardPreview(188, 74);

QLabel* wrapped(const QString& text, const char* role, QWidget* parent) {
  QLabel* l = makeLabel(text, role, parent);
  l->setWordWrap(true);
  l->setTextFormat(Qt::PlainText);
  return l;
}

/** ログインしていて、ワークショップを使えるようにする（ログインの画面も出す）。使えなければ理由を出して false */
bool ensureReady(QWidget* parent) {
  ai::Account& a = ai::account();
  if (a.server().isEmpty() || a.workshop() == u"off") {
    QMessageBox::information(parent, QStringLiteral("ワークショップ"), ai::workshopProblem());
    return false;
  }
  if (!a.loggedIn()) AccountDialog::run(parent, AccountDialog::Page::Login);
  if (a.loggedIn() && a.info().mustChangePassword) AccountDialog::run(parent, AccountDialog::Page::Password);
  const QString problem = ai::workshopProblem();
  if (!problem.isEmpty()) {
    // ログインの画面を閉じただけのときは、何も出さない
    if (a.loggedIn()) QMessageBox::information(parent, QStringLiteral("ワークショップ"), problem);
    return false;
  }
  return true;
}

/** そのフォントが、このパソコンに無いときの注意（あれば空） */
QString missingFontNote(const TextItem& style) {
  if (style.fontFamily.isEmpty() || FontLibrary::available(style.fontFamily)) return {};
  return QStringLiteral("フォント「%1」がこのパソコンに無いので、ほかのフォントで表示します").arg(style.fontFamily);
}

/** このテンプレートを、もうマイテンプレートに持っているか（追加したもの・自分が公開した元のもの） */
bool haveTemplateLocally(const WorkshopTemplate& t) {
  if (!textTemplates().findBySource(t.id).isEmpty()) return true;
  for (const auto& p : textTemplates().list()) {
    if (textTemplates().workshopInfo(p.id).publishedId == t.id) return true;
  }
  return false;
}

/** このオーディオ・写真が、今の編集場所のオーディオ・メディアの一覧に入っているか */
bool haveFileInList(const WorkshopTemplate& t) {
  const QString file = t.kind == WorkshopKind::Image   ? ai::workshopImageFile(t.id)
                       : t.kind == WorkshopKind::Video ? ai::workshopVideoFile(t.id)
                                                       : ai::workshopAudioFile(t.id);
  if (file.isEmpty()) return false;
  const QString path = QFileInfo(file).absoluteFilePath();
  for (const auto& [id, a] : editor().assets()) {
    if (QFileInfo(a.path).absoluteFilePath().compare(path, Qt::CaseInsensitive) == 0) return true;
  }
  return false;
}

QString statusLabel(const QString& status) {
  if (status == u"pending") return QStringLiteral("管理者の確認待ち");
  if (status == u"hidden") return QStringLiteral("管理者が非公開にしました");
  return {};
}

/** 秒を「1 分 5 秒」「12.3 秒」にする */
QString lengthText(double seconds) {
  if (seconds >= 60) return QStringLiteral("%1 分 %2 秒").arg(int(seconds / 60)).arg(int(std::round(std::fmod(seconds, 60.0))));
  return QStringLiteral("%1 秒").arg(seconds, 0, 'f', 1);
}

QString sizeText(qint64 bytes) {
  if (bytes >= 1024 * 1024) return QStringLiteral("%1 MB").arg(double(bytes) / 1024 / 1024, 0, 'f', 1);
  return QStringLiteral("%1 KB").arg(std::max<qint64>(1, bytes / 1024));
}

QString previewKey(const QString& id) {
  return QStringLiteral("workshop:") + id;
}

// ---- 窓 -------------------------------------------------------------------------------------

/**
 * ワークショップの窓（メニューの「ワークショップ」・テキストの一覧の「ワークショップ」で開く）。
 * ほかの操作を止めない窓で、メインのウィンドウの上に出る。閉じても消さずに、次に開いたときに使う（探す文字などはそのまま）。
 * QDialog にしないのは、探す欄で Enter を押したときに、カードのボタン（既定のボタン）まで押されてしまうため
 */
class WorkshopWindow : public QWidget {
 public:
  explicit WorkshopWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setObjectName(QStringLiteral("WorkshopWindow"));
    setWindowTitle(QStringLiteral("ワークショップ"));
    setMinimumSize(480, 420);
    resize(920, 660);  // カードが横に 4 枚並ぶ幅
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(16, 14, 10, 12);
    v->setSpacing(8);
    // テキストのテンプレートか、オーディオか（ログインの画面の「ログイン」「新規登録」と同じ切り替え）
    kindSwitch_ = new Segmented({{int(WorkshopKind::Text), QStringLiteral("テキストのテンプレート"), QString()},
                                 {int(WorkshopKind::Audio), QStringLiteral("オーディオ"), QString()},
                                 {int(WorkshopKind::Image), QStringLiteral("写真"), QString()},
                                 {int(WorkshopKind::Video), QStringLiteral("動画・GIF"), QString()}},
                                0, this);
    kindSwitch_->setValue(int(kind_));
    kindSwitch_->setMaximumWidth(600);
    connect(kindSwitch_, &Segmented::changed, this, [this](const QVariant& value) { setKind(WorkshopKind(value.toInt())); });
    auto* kinds = new QHBoxLayout();
    kinds->addWidget(kindSwitch_, 1);
    kinds->addStretch(1);
    v->addLayout(kinds);
    // Esc で閉じる（ダイアログと同じ）
    connect(new QShortcut(QKeySequence(Qt::Key_Escape), this), &QShortcut::activated, this, &QWidget::close);
    intro_ = wrapped(QString(), "dim", this);
    v->addWidget(intro_);

    // ログインしていない・使えないときの案内
    gate_ = new QWidget(this);
    auto* gv = new QVBoxLayout(gate_);
    gv->setContentsMargins(0, 16, 0, 16);
    gateText_ = wrapped(QString(), "muted", gate_);
    gateText_->setAlignment(Qt::AlignCenter);
    gv->addWidget(gateText_);
    gateLogin_ = new QPushButton(QStringLiteral("ログイン"), gate_);
    gateLogin_->setProperty("primary", true);
    gv->addWidget(gateLogin_, 0, Qt::AlignHCenter);
    gv->addStretch(1);
    v->addWidget(gate_, 1);
    connect(gateLogin_, &QPushButton::clicked, this, [this] {
      ensureReady(this);
      refreshGate();
    });

    content_ = new QWidget(this);
    auto* cv = new QVBoxLayout(content_);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(6);
    search_ = new QLineEdit(content_);
    search_->setPlaceholderText(QStringLiteral("名前・説明・作った人で探す"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(QStringLiteral("ワークショップで探す"));
    cv->addWidget(search_);
    auto* bar = new QHBoxLayout();
    bar->setSpacing(6);
    scope_ = new QComboBox(content_);
    scope_->addItem(QStringLiteral("みんなのもの"));
    scope_->addItem(QStringLiteral("自分が公開したもの"));
    scope_->setAccessibleName(QStringLiteral("一覧に出すもの"));
    disableWheel(scope_);
    bar->addWidget(scope_, 1);
    sort_ = new QComboBox(content_);
    sort_->addItem(QStringLiteral("新しい順"));
    sort_->addItem(QStringLiteral("よく使われている順"));
    sort_->setAccessibleName(QStringLiteral("並べ方"));
    disableWheel(sort_);
    bar->addWidget(sort_, 1);
    auto* reloadButton = new QToolButton(content_);
    reloadButton->setText(QStringLiteral("読み直す"));
    reloadButton->setToolTip(QStringLiteral("一覧を読み直す"));
    reloadButton->setFocusPolicy(Qt::TabFocus);
    bar->addWidget(reloadButton);
    cv->addLayout(bar);

    scroll_ = new QScrollArea(content_);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* host = new QWidget(scroll_);
    auto* hv = new QVBoxLayout(host);
    hv->setContentsMargins(0, 0, 4, 0);
    hv->setSpacing(8);
    grid_ = new QGridLayout();
    grid_->setSpacing(8);
    hv->addLayout(grid_);
    status_ = wrapped(QString(), "dim", host);
    status_->setAlignment(Qt::AlignCenter);
    hv->addWidget(status_);
    more_ = new QPushButton(QStringLiteral("もっと見る"), host);
    hv->addWidget(more_, 0, Qt::AlignHCenter);
    hv->addStretch(1);
    scroll_->setWidget(host);
    cv->addWidget(scroll_, 1);
    v->addWidget(content_, 1);

    // 文字を打ち終わって少ししたら探す（Enter ですぐ探す）
    searchTimer_.setSingleShot(true);
    searchTimer_.setInterval(400);
    connect(&searchTimer_, &QTimer::timeout, this, &WorkshopWindow::reload);
    connect(search_, &QLineEdit::textEdited, this, [this] { searchTimer_.start(); });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
      searchTimer_.stop();
      reload();
    });
    connect(scope_, &QComboBox::currentIndexChanged, this, &WorkshopWindow::reload);
    connect(sort_, &QComboBox::currentIndexChanged, this, &WorkshopWindow::reload);
    connect(reloadButton, &QToolButton::clicked, this, &WorkshopWindow::reload);
    connect(more_, &QPushButton::clicked, this, [this] { load(false); });
    // ログイン・ログアウト・つながりが変わったら、案内と一覧を直す
    connect(&ai::account(), &ai::Account::changed, this, &WorkshopWindow::refreshGate);
    connect(&ai::account(), &ai::Account::linkChanged, this, &WorkshopWindow::refreshGate);
    // マイテンプレート・オーディオの一覧が変わったら、「追加済み」を直す。試聴の状態も
    connect(&textTemplates(), &TextTemplates::changed, this, &WorkshopWindow::refreshButtons);
    connect(&editor(), &Editor::assetsChanged, this, &WorkshopWindow::refreshButtons);
    connect(&editor(), &Editor::workspaceChanged, this, &WorkshopWindow::refreshButtons);
    connect(&audioPreview(), &AudioPreview::changed, this, &WorkshopWindow::refreshButtons);
    // 公開した・公開をやめたら読み直す
    connect(&ai::workshop(), &ai::Workshop::published, this, [this](const WorkshopTemplate& t) {
      if (t.kind == kind_) reload();
    });
    connect(&ai::workshop(), &ai::Workshop::removed, this, [this](const QString&) { reload(); });
    updateIntro();
    refreshGate();
  }

  void setKind(WorkshopKind kind) {
    kindSwitch_->setValue(int(kind));
    if (kind == kind_) return;
    kind_ = kind;
    updateIntro();
    // 閉じているときは、開いたときに読む
    if (isVisible()) reload();
  }

 protected:
  void resizeEvent(QResizeEvent* e) override {
    QWidget::resizeEvent(e);
    relayout();
  }
  void showEvent(QShowEvent* e) override {
    QWidget::showEvent(e);
    if (e->spontaneous()) return;  // 最小化から戻したときなどは、そのまま
    shown_ = true;
    // はじめて開いたとき・アカウントが変わったときは、ここで読む（起動しただけでは読まない）
    const int before = generation_;
    refreshGate();
    // 開き直したときも読み直す（閉じていた間に公開されたものも出す）
    if (generation_ == before && loadedOnce_ && ai::workshopProblem().isEmpty()) reload();
  }
  void hideEvent(QHideEvent* e) override {
    QWidget::hideEvent(e);
    // 閉じたら、ワークショップの試聴を止める（止めるボタンが見えなくなるため）
    if (audioPreview().playingAsset().startsWith(previewKey(QString()))) audioPreview().stop();
  }

 private:
  struct Card {
    WorkshopTemplate t;
    QFrame* frame = nullptr;
    QPushButton* add = nullptr;
    QPushButton* play = nullptr;
  };

  void updateIntro() {
    intro_->setText(kind_ == WorkshopKind::Text
                        ? QStringLiteral("みんなが公開したテキストのテンプレート（フォント・色・大きさ・登場のアニメーションなど）を、マイテンプレートに追加できます。"
                                         "自分のものは、マイテンプレートを右クリックして公開します。")
                    : kind_ == WorkshopKind::Image
                        ? QStringLiteral("みんなが公開した写真（フル HD まで）を、メディアの一覧に追加できます。"
                                         "自分の写真は、メディアの一覧で写真を右クリックして公開します。")
                    : kind_ == WorkshopKind::Video
                        ? QStringLiteral("みんなが公開した短い動画（15 秒・フル HD まで。GIF も動画にして公開されています）を、メディアの一覧に追加できます。"
                                         "自分の動画や GIF は、メディアの一覧で右クリックして公開します。")
                        : QStringLiteral("みんなが公開したオーディオ（BGM・効果音など）を試聴して、オーディオの一覧に追加できます。"
                                         "自分のものは、オーディオの一覧を右クリックして公開します。"));
  }

  void refreshGate() {
    const QString problem = ai::workshopProblem();
    const bool ready = problem.isEmpty();
    gate_->setVisible(!ready);
    content_->setVisible(ready);
    gateText_->setText(problem);
    const ai::Account& a = ai::account();
    gateLogin_->setVisible(!ready && !a.loggedIn() && !a.server().isEmpty() && a.workshop() != u"off" && !a.workshop().isEmpty());
    // はじめて使えるようになったとき・別のアカウントでログインし直したとき（「自分のもの」が変わる）は読み直す
    const QString who = a.server() + QLatin1Char('\n') + a.info().email;
    if (ready && shown_ && (!loadedOnce_ || who != loadedFor_)) {
      loadedFor_ = who;
      reload();
    }
  }

  void reload() {
    load(true);
  }

  void load(bool fresh) {
    if (!ai::workshopProblem().isEmpty()) return;
    loadedOnce_ = true;
    if (fresh) {
      ++generation_;
      clearCards();
    }
    const int gen = generation_;
    const WorkshopKind kind = kind_;
    more_->hide();
    status_->setText(QStringLiteral("読み込んでいます…"));
    status_->show();
    ai::workshop().list(kind, search_->text(), sort_->currentIndex() == 1, scope_->currentIndex() == 1, int(cards_.size()), this,
                        [this, gen, kind](const QString& error, const ai::WorkshopPage& page) {
                          if (gen != generation_ || kind != kind_) return;  // 条件を変えた
                          if (!error.isEmpty()) {
                            status_->setText(error);
                            return;
                          }
                          for (const WorkshopTemplate& t : page.items) addCard(t);
                          relayout(true);
                          const bool mine = scope_->currentIndex() == 1;
                          if (cards_.empty()) {
                            status_->setText(!search_->text().trimmed().isEmpty() ? QStringLiteral("見つかりませんでした")
                                             : !mine                              ? QStringLiteral("まだだれも公開していません")
                                             : kind_ == WorkshopKind::Text
                                                 ? QStringLiteral("まだ公開していません。マイテンプレートを右クリックして「ワークショップに公開」で公開できます")
                                             : kind_ == WorkshopKind::Image
                                                 ? QStringLiteral("まだ公開していません。メディアの一覧で写真を右クリックして「ワークショップに公開」で公開できます")
                                             : kind_ == WorkshopKind::Video
                                                 ? QStringLiteral("まだ公開していません。メディアの一覧で動画や GIF を右クリックして「ワークショップに公開」で公開できます")
                                                 : QStringLiteral("まだ公開していません。オーディオの一覧を右クリックして「ワークショップに公開」で公開できます"));
                          } else {
                            status_->setText(QStringLiteral("%1 個中 %2 個").arg(page.total).arg(cards_.size()));
                          }
                          more_->setVisible(int(cards_.size()) < page.total && !page.items.empty());
                        });
  }

  void clearCards() {
    while (QLayoutItem* it = grid_->takeAt(0)) {
      if (QWidget* w = it->widget()) w->deleteLater();
      delete it;
    }
    cards_.clear();
    columns_ = 0;
  }

  /** 窓の幅に合わせて、カードを横に並べる数を決め直す */
  void relayout(bool force = false) {
    const int width = scroll_->viewport()->width();
    const int columns = std::clamp(width / kCardWidth, 1, 4);
    if (!force && columns == columns_) return;
    columns_ = columns;
    while (grid_->takeAt(0)) {
    }
    for (int c = 0; c < 4; ++c) grid_->setColumnStretch(c, c < columns ? 1 : 0);
    for (size_t i = 0; i < cards_.size(); ++i) grid_->addWidget(cards_[i].frame, int(i) / columns, int(i) % columns);
  }

  void addCard(const WorkshopTemplate& t) {
    auto* f = new QFrame(grid_->parentWidget());
    f->setObjectName(QStringLiteral("WorkshopCard"));
    f->setStyleSheet(QStringLiteral("QFrame#WorkshopCard{background:#222222;border:1px solid #2f2f2f;border-radius:8px}"));
    auto* v = new QVBoxLayout(f);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(4);
    Card card;
    card.t = t;
    card.frame = f;
    if (t.kind == WorkshopKind::Text) {
      auto* preview = new QLabel(f);
      preview->setPixmap(renderTextPresetCard(TextPreset{QString(), t.name, t.style}, kCardPreview, devicePixelRatioF()));
      preview->setAlignment(Qt::AlignCenter);
      preview->setAccessibleName(QStringLiteral("%1 の見本").arg(t.name));
      v->addWidget(preview);
    } else if (t.kind == WorkshopKind::Image || t.kind == WorkshopKind::Video) {
      // 写真・動画は、サーバーから一覧と一緒に届いた小さな見本を出す（元のファイルは、追加するときに読み込む）
      auto* preview = new QLabel(f);
      const qreal dpr = devicePixelRatioF();
      const QSize box(kCardPreview.width(), 106);
      const QImage img = QImage::fromData(t.thumb, "JPG");
      const bool video = t.kind == WorkshopKind::Video;
      if (!img.isNull()) {
        QPixmap pm = QPixmap::fromImage(img.scaled(box * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        pm.setDevicePixelRatio(dpr);
        if (video) {
          // 動画の印（真ん中に再生の三角）
          QPainter p(&pm);
          p.setRenderHint(QPainter::Antialiasing);
          const QSizeF s = QSizeF(pm.size()) / dpr;
          const QPointF c(s.width() / 2, s.height() / 2);
          p.setPen(Qt::NoPen);
          p.setBrush(QColor(0, 0, 0, 150));
          p.drawEllipse(c, 17, 17);
          p.setBrush(Qt::white);
          const QPointF tri[3] = {c + QPointF(-6, -9), c + QPointF(-6, 9), c + QPointF(10, 0)};
          p.drawPolygon(tri, 3);
        }
        preview->setPixmap(pm);
      } else {
        preview->setPixmap(theme::iconPixmap(video ? QStringLiteral("film") : QStringLiteral("image"), 28, theme::muted, dpr));
      }
      preview->setAlignment(Qt::AlignCenter);
      preview->setMinimumHeight(box.height());
      preview->setStyleSheet(QStringLiteral("background:#111111;border-radius:6px"));
      preview->setAccessibleName(QStringLiteral("%1 の見本").arg(t.name));
      v->addWidget(preview);
    } else {
      auto* icon = new QLabel(f);
      icon->setPixmap(theme::iconPixmap(QStringLiteral("music"), 28, theme::ok, devicePixelRatioF()));
      icon->setAlignment(Qt::AlignCenter);
      icon->setMinimumHeight(44);
      icon->setStyleSheet(QStringLiteral("background:#16271f;border-radius:6px"));
      v->addWidget(icon);
    }
    auto* name = wrapped(t.name, nullptr, f);
    name->setStyleSheet(QStringLiteral("font-weight:600"));
    v->addWidget(name);
    QString meta = QStringLiteral("%1・%2 人が使用").arg(t.mine ? QStringLiteral("自分") : t.author).arg(t.uses);
    if (t.kind == WorkshopKind::Audio) meta += QStringLiteral("\n%1・%2").arg(lengthText(t.duration), sizeText(t.fileSize));
    if (t.kind == WorkshopKind::Image) meta += QStringLiteral("\n%1×%2・%3").arg(t.width).arg(t.height).arg(sizeText(t.fileSize));
    if (t.kind == WorkshopKind::Video) {
      meta += QStringLiteral("\n%1×%2・%3・%4").arg(t.width).arg(t.height).arg(lengthText(t.duration), sizeText(t.fileSize));
    }
    v->addWidget(wrapped(meta, "dim", f));
    if (const QString s = statusLabel(t.status); !s.isEmpty()) v->addWidget(wrapped(s, "note", f));
    if (!t.description.isEmpty()) v->addWidget(wrapped(t.description, "muted", f));
    if (t.kind == WorkshopKind::Text) {
      if (const QString note = missingFontNote(t.style); !note.isEmpty()) v->addWidget(wrapped(note, "dim", f));
    }
    v->addStretch(1);
    const WorkshopTemplate copy = t;
    if (t.kind == WorkshopKind::Audio) {
      card.play = new QPushButton(theme::icon(QStringLiteral("play")), QStringLiteral("試聴"), f);
      card.play->setAccessibleName(QStringLiteral("%1 を試聴").arg(t.name));
      v->addWidget(card.play);
      connect(card.play, &QPushButton::clicked, this, [this, copy] { preview(copy); });
    }
    card.add = new QPushButton(f);
    v->addWidget(card.add);
    connect(card.add, &QPushButton::clicked, this, [this, copy] { add(copy); });
    auto* row = new QHBoxLayout();
    row->setSpacing(6);
    if (t.mine) {
      auto* stop = new QPushButton(QStringLiteral("公開をやめる"), f);
      stop->setProperty("flat", true);
      row->addWidget(stop);
      connect(stop, &QPushButton::clicked, this, [this, copy] { stopPublishing(copy); });
    } else {
      auto* report = new QPushButton(QStringLiteral("報告"), f);
      report->setProperty("flat", true);
      report->setToolTip(QStringLiteral("ふさわしくない名前・説明・音などを、管理者に知らせます"));
      row->addWidget(report);
      connect(report, &QPushButton::clicked, this, [this, copy] { reportItem(copy); });
    }
    row->addStretch(1);
    v->addLayout(row);
    cards_.push_back(card);
    updateButtons(cards_.back());
  }

  void updateButtons(const Card& c) {
    bool have = false;
    QString addText;
    if (c.t.kind == WorkshopKind::Text) {
      have = haveTemplateLocally(c.t);
      addText = have ? QStringLiteral("マイテンプレートにあります") : QStringLiteral("マイテンプレートに追加");
    } else if (c.t.kind == WorkshopKind::Image || c.t.kind == WorkshopKind::Video) {
      have = haveFileInList(c.t);
      addText = have ? QStringLiteral("メディアの一覧にあります") : QStringLiteral("メディアの一覧に追加");
    } else {
      have = haveFileInList(c.t);
      addText = have ? QStringLiteral("オーディオの一覧にあります") : QStringLiteral("オーディオの一覧に追加");
      const bool playing = audioPreview().playingAsset() == previewKey(c.t.id);
      c.play->setText(playing ? QStringLiteral("試聴を止める") : QStringLiteral("試聴"));
      c.play->setIcon(theme::icon(playing ? QStringLiteral("stop") : QStringLiteral("play")));
      c.play->setEnabled(!busy_.contains(c.t.id));
    }
    const bool busy = busy_.contains(c.t.id);
    c.add->setEnabled(!have && !busy);
    c.add->setText(busy ? QStringLiteral("読み込んでいます…") : addText);
    c.add->setProperty("primary", !have && !busy);
    c.add->style()->unpolish(c.add);
    c.add->style()->polish(c.add);
  }

  void refreshButtons() {
    for (const Card& c : cards_) updateButtons(c);
  }

  void add(const WorkshopTemplate& t) {
    if (t.kind == WorkshopKind::Text) {
      const QString id = textTemplates().addFromWorkshop(t.name, t.style, t.id, t.author);
      if (id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ワークショップ"), QStringLiteral("マイテンプレートに追加できませんでした"));
        return;
      }
      ai::workshop().markUsed(t.id);
      editor().toast(QStringLiteral("「%1」をマイテンプレートに追加しました。「テキスト」の一覧からタイムラインへドラッグして使えます").arg(t.name),
                     ToastKind::Success);
      return;
    }
    // オーディオ・写真・動画: 手元に置いてから、今の編集場所のオーディオ・メディアの一覧に入れる（タイムラインには置かない）
    const bool image = t.kind == WorkshopKind::Image || t.kind == WorkshopKind::Video;
    fetch(t, [t, image](const QString& file) {
      media().importPaths({file}, false);
      ai::workshop().markUsed(t.id);
      editor().toast(image ? QStringLiteral("「%1」をメディアの一覧に追加しました。「メディア」の一覧からタイムラインへドラッグして使えます").arg(t.name)
                           : QStringLiteral("「%1」をオーディオの一覧に追加しました。「オーディオ」の一覧からタイムラインへドラッグして使えます").arg(t.name),
                     ToastKind::Success);
    });
  }

  void preview(const WorkshopTemplate& t) {
    if (audioPreview().playingAsset() == previewKey(t.id)) {
      audioPreview().stop();
      return;
    }
    fetch(t, [t](const QString& file) { audioPreview().toggleFile(previewKey(t.id), file, t.duration); });
  }

  /** オーディオを手元に置いてから then を呼ぶ（置いている間は、そのカードのボタンを押せなくする） */
  void fetch(const WorkshopTemplate& t, std::function<void(const QString& file)> then) {
    if (busy_.contains(t.id)) return;
    busy_ << t.id;
    refreshButtons();
    const QString id = t.id;
    ai::workshop().fetchFile(t, this, [this, id, then](const QString& error, const QString& file) {
      busy_.removeAll(id);
      refreshButtons();
      if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ワークショップ"), error);
        return;
      }
      then(file);
    });
  }

  void stopPublishing(const WorkshopTemplate& t) {
    const QString detail = t.kind == WorkshopKind::Text    ? QStringLiteral("（すでに追加した人の手元のテンプレートと、自分のマイテンプレートは、そのまま残ります）")
                           : t.kind == WorkshopKind::Image ? QStringLiteral("（すでに追加した人の手元の写真と、自分のファイルは、そのまま残ります）")
                           : t.kind == WorkshopKind::Video ? QStringLiteral("（すでに追加した人の手元の動画と、自分のファイルは、そのまま残ります）")
                                                           : QStringLiteral("（すでに追加した人の手元のオーディオと、自分のファイルは、そのまま残ります）");
    if (QMessageBox::question(this, QStringLiteral("公開をやめる"), QStringLiteral("「%1」のワークショップへの公開をやめますか？\n%2").arg(t.name, detail)) !=
        QMessageBox::Yes) {
      return;
    }
    const QString id = t.id;
    ai::workshop().remove(id, this, [this, id](const QString& error) {
      if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("ワークショップ"), error);
        return;
      }
      textTemplates().clearPublished(id);
      editor().toast(QStringLiteral("ワークショップへの公開をやめました"), ToastKind::Success);
    });
  }

  void reportItem(const WorkshopTemplate& t) {
    bool ok = false;
    const QString reason = QInputDialog::getText(this, QStringLiteral("管理者に知らせる"),
                                                 QStringLiteral("「%1」のどこに問題があるか（なくてもかまいません）").arg(t.name),
                                                 QLineEdit::Normal, QString(), &ok);
    if (!ok) return;
    ai::workshop().report(t.id, reason, this, [this](const QString& error) {
      if (!error.isEmpty()) QMessageBox::warning(this, QStringLiteral("ワークショップ"), error);
      else editor().toast(QStringLiteral("管理者に知らせました"), ToastKind::Success);
    });
  }

  WorkshopKind kind_ = WorkshopKind::Text;
  Segmented* kindSwitch_ = nullptr;
  QLabel* intro_ = nullptr;
  QWidget* gate_ = nullptr;
  QLabel* gateText_ = nullptr;
  QPushButton* gateLogin_ = nullptr;
  QWidget* content_ = nullptr;
  QLineEdit* search_ = nullptr;
  QComboBox* scope_ = nullptr;
  QComboBox* sort_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QGridLayout* grid_ = nullptr;
  QLabel* status_ = nullptr;
  QPushButton* more_ = nullptr;
  QTimer searchTimer_;
  std::vector<Card> cards_;
  /** 読み込んでいる途中のオーディオ */
  QStringList busy_;
  int columns_ = 0;
  int generation_ = 0;
  bool shown_ = false;
  bool loadedOnce_ = false;
  /** 一覧を読んだときのサーバーとアカウント */
  QString loadedFor_;
};

QPointer<WorkshopWindow> g_window;

/** 公開の画面の共通の注意（だれに見えるか・作った人の名前・確認待ち） */
QStringList publishNotes(const QString& what) {
  const ai::AccountInfo& me = ai::account().info();
  QStringList notes;
  notes << QStringLiteral("公開すると、この AI サーバーにログインできる人がみんな、この%1を見て使えるようになります。").arg(what);
  notes << (me.displayName.trimmed().isEmpty()
                ? QStringLiteral("作った人の名前は「名前なし」と出ます（メールアドレスは出ません）。")
                : QStringLiteral("作った人の名前として「%1」が出ます（メールアドレスは出ません）。").arg(me.displayName.trimmed()));
  if (ai::account().workshop() == u"approval") notes << QStringLiteral("このサーバーでは、管理者が確認してから、みんなの一覧に出ます。");
  return notes;
}

}  // namespace

void openWorkshop(QWidget* parent, WorkshopKind kind) {
  if (!g_window) {
    // メインのウィンドウの上に出す（メインのウィンドウを閉じると一緒に閉じる）。はじめはメインのウィンドウの真ん中に
    QWidget* owner = parent ? parent->window() : nullptr;
    g_window = new WorkshopWindow(owner);
    if (owner) g_window->move(owner->frameGeometry().center() - g_window->rect().center());
  }
  g_window->setKind(kind);
  if (g_window->isMinimized()) g_window->showNormal();
  else g_window->show();
  g_window->raise();
  g_window->activateWindow();
}

void publishTemplate(QWidget* parent, const QString& templateId) {
  QWidget* owner = parent ? parent->window() : nullptr;
  const TextPreset* found = textTemplates().find(templateId);
  if (!found) return;
  const TextPreset preset = *found;
  const TemplateWorkshopInfo info = textTemplates().workshopInfo(templateId);
  if (!info.sourceId.isEmpty()) {
    QMessageBox::information(owner, QStringLiteral("ワークショップに公開"),
                             QStringLiteral("ワークショップから追加したテンプレートは、作った人のものなので公開できません。"
                                            "見た目を変えて「テンプレートに保存」し直したものは、公開できます"));
    return;
  }
  if (!info.publishedId.isEmpty()) {
    QMessageBox::information(owner, QStringLiteral("ワークショップに公開"), QStringLiteral("このテンプレートは、もう公開しています"));
    return;
  }
  if (!ensureReady(owner)) return;

  QDialog dlg(owner);
  dlg.setWindowTitle(QStringLiteral("ワークショップに公開"));
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(8);
  auto* preview = new QLabel(&dlg);
  preview->setPixmap(renderTextPresetCard(preset, QSize(240, 96), dlg.devicePixelRatioF()));
  preview->setAlignment(Qt::AlignCenter);
  v->addWidget(preview);
  v->addWidget(makeLabel(QStringLiteral("名前"), "muted", &dlg));
  auto* name = new QLineEdit(preset.label, &dlg);
  name->setMaxLength(30);
  name->setAccessibleName(QStringLiteral("公開する名前"));
  v->addWidget(name);
  v->addWidget(makeLabel(QStringLiteral("説明（なくてもかまいません）"), "muted", &dlg));
  auto* description = new QLineEdit(&dlg);
  description->setMaxLength(200);
  description->setPlaceholderText(QStringLiteral("例: バラエティ番組のツッコミに"));
  description->setAccessibleName(QStringLiteral("説明"));
  v->addWidget(description);
  QStringList notes = publishNotes(QStringLiteral("テンプレート"));
  notes.insert(1, QStringLiteral("文字そのものは公開されません（見た目だけです）。"));
  // 自分で読み込んだフォント・パソコンのフォントは、ほかの人のパソコンには無いことがある
  if (!preset.style.fontFamily.isEmpty()) {
    bool bundled = false;
    for (const FontEntry& e : fontLibrary().bundled()) bundled = bundled || e.family == preset.style.fontFamily;
    if (!bundled) {
      notes << QStringLiteral("フォント「%1」は、ほかの人のパソコンには無いことがあります。無いときは、ほかのフォントで表示されます。")
                   .arg(fontLibrary().labelOf(preset.style.fontFamily));
    }
  }
  v->addWidget(wrapped(notes.join(QLatin1Char('\n')), "dim", &dlg));
  auto* error = wrapped(QString(), "error", &dlg);
  error->hide();
  v->addWidget(error);
  auto* buttons = new QDialogButtonBox(&dlg);
  auto* go = buttons->addButton(QStringLiteral("公開する"), QDialogButtonBox::AcceptRole);
  go->setProperty("primary", true);
  go->setDefault(true);
  buttons->addButton(QStringLiteral("やめる"), QDialogButtonBox::RejectRole);
  v->addWidget(buttons);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
    if (name->text().trimmed().isEmpty()) {
      error->setText(QStringLiteral("名前を入れてください"));
      error->show();
      return;
    }
    go->setEnabled(false);
    error->hide();
    ai::workshop().publish(name->text(), description->text(), preset.style, &dlg,
                           [&dlg, go, error, templateId](const QString& err, const WorkshopTemplate& t, const QString& message) {
                             if (!err.isEmpty()) {
                               go->setEnabled(true);
                               error->setText(err);
                               error->show();
                               return;
                             }
                             textTemplates().setPublished(templateId, t.id);
                             editor().toast(message, ToastKind::Success);
                             dlg.accept();
                           });
  });
  dlg.exec();
}

void unpublishTemplate(QWidget* parent, const QString& templateId) {
  QWidget* owner = parent ? parent->window() : nullptr;
  const TextPreset* found = textTemplates().find(templateId);
  const QString publishedId = textTemplates().workshopInfo(templateId).publishedId;
  if (!found || publishedId.isEmpty()) return;
  if (QMessageBox::question(owner, QStringLiteral("公開をやめる"),
                            QStringLiteral("「%1」のワークショップへの公開をやめますか？\n（このマイテンプレートと、すでに追加した人の手元のテンプレートは、そのまま残ります）")
                                .arg(found->label)) != QMessageBox::Yes) {
    return;
  }
  if (!ensureReady(owner)) return;
  QPointer<QWidget> guard(owner);
  ai::workshop().remove(publishedId, owner, [guard, publishedId](const QString& error) {
    // サーバーで見つからない（管理者が消した・別の PC で公開をやめた）ときも、手元の印は外す
    if (!error.isEmpty() && !error.contains(QStringLiteral("見つかりません"))) {
      QMessageBox::warning(guard, QStringLiteral("ワークショップ"), error);
      return;
    }
    textTemplates().clearPublished(publishedId);
    editor().toast(QStringLiteral("ワークショップへの公開をやめました"), ToastKind::Success);
  });
}

void publishAudioAsset(QWidget* parent, const QString& assetId) {
  QWidget* owner = parent ? parent->window() : nullptr;
  const Asset* found = editor().asset(assetId);
  if (!found || found->kind != AssetKind::Audio) return;
  const Asset asset = *found;
  if (ai::isWorkshopAudio(asset.path)) {
    QMessageBox::information(owner, QStringLiteral("ワークショップに公開"),
                             QStringLiteral("ワークショップから追加したオーディオは、作った人のものなので公開できません"));
    return;
  }
  const QFileInfo file(asset.path);
  QString problem;
  if (asset.status == AssetStatus::Missing || asset.status == AssetStatus::Error || !file.isFile()) {
    problem = QStringLiteral("ファイルが見つからないか、読み込めないので公開できません");
  } else if (!ai::workshopAudioExtensions().contains(file.suffix().toLower())) {
    problem = QStringLiteral("この形式（%1）は公開できません。MP3・WAV・M4A・AAC・OGG・Opus・FLAC・WMA を公開できます").arg(file.suffix().toUpper());
  } else if (file.size() > ai::kWorkshopAudioMaxBytes) {
    problem = QStringLiteral("ファイルが大きすぎます（%1）。20MB まで公開できます").arg(sizeText(file.size()));
  } else if (asset.duration <= 0 || asset.duration > ai::kWorkshopAudioMaxSeconds) {
    problem = QStringLiteral("長さが 30 分までのオーディオだけ公開できます");
  }
  if (!problem.isEmpty()) {
    QMessageBox::information(owner, QStringLiteral("ワークショップに公開"), problem);
    return;
  }
  if (!ensureReady(owner)) return;

  QDialog dlg(owner);
  dlg.setWindowTitle(QStringLiteral("オーディオをワークショップに公開"));
  dlg.setMinimumWidth(420);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(8);
  v->addWidget(wrapped(QStringLiteral("%1（%2・%3）").arg(asset.name, lengthText(asset.duration), sizeText(file.size())), "muted", &dlg));
  v->addWidget(makeLabel(QStringLiteral("名前"), "muted", &dlg));
  auto* name = new QLineEdit(file.completeBaseName().left(30), &dlg);
  name->setMaxLength(30);
  name->setAccessibleName(QStringLiteral("公開する名前"));
  v->addWidget(name);
  v->addWidget(makeLabel(QStringLiteral("説明（なくてもかまいません）"), "muted", &dlg));
  auto* description = new QLineEdit(&dlg);
  description->setMaxLength(200);
  description->setPlaceholderText(QStringLiteral("例: 決定ボタンを押したときの音"));
  description->setAccessibleName(QStringLiteral("説明"));
  v->addWidget(description);
  // ほかの人の曲などを勝手に配らないよう、公開してよい音かを確かめてもらう
  auto* rights = new QCheckBox(QStringLiteral("自分で作った音か、公開してよいと許可されている音です"), &dlg);
  rights->setAccessibleName(QStringLiteral("公開してよい音であることを確かめる"));
  v->addWidget(rights);
  QStringList notes = publishNotes(QStringLiteral("オーディオ"));
  notes.insert(1, QStringLiteral("市販の曲・テレビや動画から録った音など、ほかの人が作った音は公開しないでください。"));
  v->addWidget(wrapped(notes.join(QLatin1Char('\n')), "dim", &dlg));
  auto* error = wrapped(QString(), "error", &dlg);
  error->hide();
  v->addWidget(error);
  auto* buttons = new QDialogButtonBox(&dlg);
  auto* go = buttons->addButton(QStringLiteral("公開する"), QDialogButtonBox::AcceptRole);
  go->setProperty("primary", true);
  go->setDefault(true);
  go->setEnabled(false);
  buttons->addButton(QStringLiteral("やめる"), QDialogButtonBox::RejectRole);
  v->addWidget(buttons);
  QObject::connect(rights, &QCheckBox::toggled, go, &QPushButton::setEnabled);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
    if (!rights->isChecked()) return;
    if (name->text().trimmed().isEmpty()) {
      error->setText(QStringLiteral("名前を入れてください"));
      error->show();
      return;
    }
    go->setEnabled(false);
    go->setText(QStringLiteral("送っています…"));
    error->hide();
    ai::workshop().publishAudio(name->text(), description->text(), asset.path, asset.duration, &dlg,
                                [&dlg, go, error](const QString& err, const WorkshopTemplate&, const QString& message) {
                                  if (!err.isEmpty()) {
                                    go->setEnabled(true);
                                    go->setText(QStringLiteral("公開する"));
                                    error->setText(err);
                                    error->show();
                                    return;
                                  }
                                  editor().toast(message, ToastKind::Success);
                                  dlg.accept();
                                });
  });
  dlg.exec();
}

void publishImageAsset(QWidget* parent, const QString& assetId) {
  QWidget* owner = parent ? parent->window() : nullptr;
  const Asset* found = editor().asset(assetId);
  if (!found || found->kind != AssetKind::Image) return;
  const Asset asset = *found;
  if (ai::isWorkshopImage(asset.path)) {
    QMessageBox::information(owner, QStringLiteral("ワークショップに公開"),
                             QStringLiteral("ワークショップから追加した写真は、撮った人・作った人のものなので公開できません"));
    return;
  }
  // 読み込んだときに作った画像（向きを直し、透明な所は黒にしたもの。長い辺 2048px まで）を使う。無ければファイルから読む
  QImage image = media().previewImage(asset.id);
  if (image.isNull() && QFileInfo(asset.path).isFile()) {
    QImageReader reader(asset.path);
    reader.setAutoTransform(true);
    image = reader.read();
  }
  if (asset.status == AssetStatus::Missing || asset.status == AssetStatus::Error || image.isNull()) {
    QMessageBox::information(owner, QStringLiteral("ワークショップに公開"), QStringLiteral("ファイルが見つからないか、読み込めないので公開できません"));
    return;
  }
  if (!ensureReady(owner)) return;
  // 元の大きさと、公開する大きさ（フル HD に収める）
  const QSize original = asset.width > 0 && asset.height > 0 ? QSize(asset.width, asset.height) : image.size();
  const QSize sent = ai::workshopImageSize(original);

  QDialog dlg(owner);
  dlg.setWindowTitle(QStringLiteral("写真をワークショップに公開"));
  dlg.setMinimumWidth(440);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(8);
  auto* preview = new QLabel(&dlg);
  const qreal dpr = dlg.devicePixelRatioF();
  QPixmap pm = QPixmap::fromImage(image.scaled(QSize(400, 225) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  pm.setDevicePixelRatio(dpr);
  preview->setPixmap(pm);
  preview->setAlignment(Qt::AlignCenter);
  preview->setStyleSheet(QStringLiteral("background:#111111;border-radius:6px"));
  preview->setAccessibleName(QStringLiteral("公開する写真"));
  v->addWidget(preview);
  v->addWidget(wrapped(QStringLiteral("%1（%2×%3）").arg(asset.name).arg(original.width()).arg(original.height()), "muted", &dlg));
  v->addWidget(makeLabel(QStringLiteral("名前"), "muted", &dlg));
  auto* name = new QLineEdit(QFileInfo(asset.path).completeBaseName().left(30), &dlg);
  name->setMaxLength(30);
  name->setAccessibleName(QStringLiteral("公開する名前"));
  v->addWidget(name);
  v->addWidget(makeLabel(QStringLiteral("説明（なくてもかまいません）"), "muted", &dlg));
  auto* description = new QLineEdit(&dlg);
  description->setMaxLength(200);
  description->setPlaceholderText(QStringLiteral("例: 夕方の海（背景に使えます）"));
  description->setAccessibleName(QStringLiteral("説明"));
  v->addWidget(description);
  // ほかの人の写真などを勝手に配らないよう、公開してよい写真かを確かめてもらう
  auto* rights = new QCheckBox(QStringLiteral("自分で撮った・作った写真か、公開してよいと許可されている写真です"), &dlg);
  rights->setAccessibleName(QStringLiteral("公開してよい写真であることを確かめる"));
  v->addWidget(rights);
  QStringList notes = publishNotes(QStringLiteral("写真"));
  notes.insert(1, QStringLiteral("人の顔・名札・住所・車のナンバーなど、だれかが分かるものが写っていないか確かめてください。"));
  notes.insert(2, QStringLiteral("撮った場所などの写真の情報（Exif）は、消してから送ります。"));
  if (sent != original) notes.insert(3, QStringLiteral("フル HD より大きいので、%1×%2 に縮めて公開します。").arg(sent.width()).arg(sent.height()));
  v->addWidget(wrapped(notes.join(QLatin1Char('\n')), "dim", &dlg));
  auto* error = wrapped(QString(), "error", &dlg);
  error->hide();
  v->addWidget(error);
  auto* buttons = new QDialogButtonBox(&dlg);
  auto* go = buttons->addButton(QStringLiteral("公開する"), QDialogButtonBox::AcceptRole);
  go->setProperty("primary", true);
  go->setDefault(true);
  go->setEnabled(false);
  buttons->addButton(QStringLiteral("やめる"), QDialogButtonBox::RejectRole);
  v->addWidget(buttons);
  QObject::connect(rights, &QCheckBox::toggled, go, &QPushButton::setEnabled);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
    if (!rights->isChecked()) return;
    if (name->text().trimmed().isEmpty()) {
      error->setText(QStringLiteral("名前を入れてください"));
      error->show();
      return;
    }
    go->setEnabled(false);
    go->setText(QStringLiteral("送っています…"));
    error->hide();
    ai::workshop().publishImage(name->text(), description->text(), image, &dlg,
                                [&dlg, go, error](const QString& err, const WorkshopTemplate&, const QString& message) {
                                  if (!err.isEmpty()) {
                                    go->setEnabled(true);
                                    go->setText(QStringLiteral("公開する"));
                                    error->setText(err);
                                    error->show();
                                    return;
                                  }
                                  editor().toast(message, ToastKind::Success);
                                  dlg.accept();
                                });
  });
  dlg.exec();
}

void publishVideoAsset(QWidget* parent, const QString& assetId) {
  QWidget* owner = parent ? parent->window() : nullptr;
  const Asset* found = editor().asset(assetId);
  if (!found || found->kind != AssetKind::Video) return;
  const Asset asset = *found;
  if (ai::isWorkshopVideo(asset.path)) {
    QMessageBox::information(owner, QStringLiteral("ワークショップに公開"),
                             QStringLiteral("ワークショップから追加した動画は、撮った人・作った人のものなので公開できません"));
    return;
  }
  const bool gif = QFileInfo(asset.path).suffix().compare(QStringLiteral("gif"), Qt::CaseInsensitive) == 0;
  QString problem;
  if (asset.status == AssetStatus::Missing || asset.status == AssetStatus::Error || !QFileInfo(asset.path).isFile()) {
    problem = QStringLiteral("ファイルが見つからないか、読み込めないので公開できません");
  } else if (asset.duration <= 0 || asset.duration > ai::kWorkshopVideoMaxSeconds + ai::kWorkshopVideoSlack) {
    problem = QStringLiteral("公開できるのは、15 秒までの動画です（この動画は %1）。長い動画は、使いたい所だけを書き出してから公開してください")
                  .arg(lengthText(asset.duration));
  }
  if (!problem.isEmpty()) {
    QMessageBox::information(owner, QStringLiteral("ワークショップに公開"), problem);
    return;
  }
  if (!ensureReady(owner)) return;
  const QSize original(asset.width, asset.height);
  const QSize sent = ai::workshopImageSize(original);

  QDialog dlg(owner);
  dlg.setWindowTitle(gif ? QStringLiteral("GIF をワークショップに公開") : QStringLiteral("動画をワークショップに公開"));
  dlg.setMinimumWidth(440);
  auto* v = new QVBoxLayout(&dlg);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(8);
  // 見本（はじめのコマ）
  const QImage first = decodeFirstFrame(asset.path, 800);
  if (!first.isNull()) {
    auto* preview = new QLabel(&dlg);
    const qreal dpr = dlg.devicePixelRatioF();
    QPixmap pm = QPixmap::fromImage(first.scaled(QSize(400, 225) * dpr, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pm.setDevicePixelRatio(dpr);
    preview->setPixmap(pm);
    preview->setAlignment(Qt::AlignCenter);
    preview->setStyleSheet(QStringLiteral("background:#111111;border-radius:6px"));
    preview->setAccessibleName(QStringLiteral("公開する動画のはじめの画面"));
    v->addWidget(preview);
  }
  v->addWidget(wrapped(QStringLiteral("%1（%2×%3・%4）").arg(asset.name).arg(original.width()).arg(original.height()).arg(lengthText(asset.duration)), "muted",
                       &dlg));
  v->addWidget(makeLabel(QStringLiteral("名前"), "muted", &dlg));
  auto* name = new QLineEdit(QFileInfo(asset.path).completeBaseName().left(30), &dlg);
  name->setMaxLength(30);
  name->setAccessibleName(QStringLiteral("公開する名前"));
  v->addWidget(name);
  v->addWidget(makeLabel(QStringLiteral("説明（なくてもかまいません）"), "muted", &dlg));
  auto* description = new QLineEdit(&dlg);
  description->setMaxLength(200);
  description->setPlaceholderText(gif ? QStringLiteral("例: 拍手しているアニメーション") : QStringLiteral("例: 波打ちぎわ（オープニングに使えます）"));
  description->setAccessibleName(QStringLiteral("説明"));
  v->addWidget(description);
  // 音が入っている動画は、音を入れずに公開することもできる（BGM などが入っているとき）
  QCheckBox* mute = nullptr;
  if (asset.hasAudio && !gif) {
    mute = new QCheckBox(QStringLiteral("音を入れずに公開する"), &dlg);
    mute->setAccessibleName(QStringLiteral("音を入れずに公開する"));
    v->addWidget(mute);
  }
  // ほかの人の動画などを勝手に配らないよう、公開してよい動画かを確かめてもらう
  auto* rights = new QCheckBox(QStringLiteral("自分で撮った・作った動画か、公開してよいと許可されている動画です"), &dlg);
  rights->setAccessibleName(QStringLiteral("公開してよい動画であることを確かめる"));
  v->addWidget(rights);
  QStringList notes = publishNotes(QStringLiteral("動画"));
  notes.insert(1, QStringLiteral("人の顔・名札・住所・車のナンバーなど、だれかが分かるものが映っていないか確かめてください。"));
  if (asset.hasAudio && !gif) notes.insert(2, QStringLiteral("市販の曲・テレビの音など、ほかの人が作った音が入っていないか確かめてください（入っていれば「音を入れずに公開する」に）。"));
  notes << QStringLiteral("撮った場所などの情報は消して、MP4 に作り直してから送ります。");
  if (sent != original) notes << QStringLiteral("フル HD より大きいので、%1×%2 に縮めて公開します。").arg(sent.width()).arg(sent.height());
  if (gif) notes << QStringLiteral("GIF は動画（MP4）にして公開します。透明な所は黒になります。");
  v->addWidget(wrapped(notes.join(QLatin1Char('\n')), "dim", &dlg));
  auto* error = wrapped(QString(), "error", &dlg);
  error->hide();
  v->addWidget(error);
  auto* buttons = new QDialogButtonBox(&dlg);
  auto* go = buttons->addButton(QStringLiteral("公開する"), QDialogButtonBox::AcceptRole);
  go->setProperty("primary", true);
  go->setDefault(true);
  go->setEnabled(false);
  buttons->addButton(QStringLiteral("やめる"), QDialogButtonBox::RejectRole);
  v->addWidget(buttons);
  QObject::connect(rights, &QCheckBox::toggled, go, &QPushButton::setEnabled);
  QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
  QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
    if (!rights->isChecked()) return;
    if (name->text().trimmed().isEmpty()) {
      error->setText(QStringLiteral("名前を入れてください"));
      error->show();
      return;
    }
    go->setEnabled(false);
    go->setText(QStringLiteral("作り直しています…"));
    error->hide();
    const bool withAudio = asset.hasAudio && !gif && !(mute && mute->isChecked());
    ai::workshop().publishVideo(
        name->text(), description->text(), asset.path, original, asset.duration, withAudio, &dlg,
        [&dlg, go, error](const QString& err, const WorkshopTemplate&, const QString& message) {
          if (!err.isEmpty()) {
            go->setEnabled(true);
            go->setText(QStringLiteral("公開する"));
            error->setText(err);
            error->show();
            return;
          }
          editor().toast(message, ToastKind::Success);
          dlg.accept();
        },
        [go](const QString& stage) { go->setText(stage); });
  });
  dlg.exec();
}

}  // namespace kv
