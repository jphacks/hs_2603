#include "ui/aipanel.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QSignalBlocker>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

#include "ai/account.h"
#include "ai/aiconfig.h"
#include "ai/assistant.h"
#include "core/editor.h"
#include "core/projectio.h"
#include "core/timeline.h"
#include "ui/accountdialog.h"
#include "ui/theme.h"
#include "ui/widgets.h"

namespace kv {

struct AiChoiceCard {
  ai::ChoiceSet set;
  std::vector<QPointer<QPushButton>> buttons;
  std::vector<QPointer<QFrame>> frames;
  /** AI の欄の中の、この案の欄（会話を消すと消える） */
  QPointer<QFrame> box;
  /** 大きく見比べる画面（開いていなければ空）と、そこの「これにする」ボタン */
  QPointer<QDialog> dialog;
  std::vector<QPointer<QPushButton>> dialogButtons;
  /** 今タイムラインに当てはまっている案（なければ -1） */
  int applied = -1;
  /** 案を当てはめる前の編集データ。どの案も、この状態に当てはめる（切り替えても、前の案は残らない） */
  std::optional<ProjectData> base;
  QJsonObject baseJson;
  /** それぞれの案を base に当てはめた直後の編集データ（空なら、まだ当てはめていない）。元に戻す・やり直しを見分ける */
  std::vector<QJsonObject> after;
};

namespace {

/** 案のプレビュー画像。幅に合わせて縦横比のまま縮める。onClick を決めると、押せるようになる */
class PreviewThumb : public QWidget {
 public:
  PreviewThumb(QImage image, const QString& caption, QWidget* parent) : QWidget(parent), image_(std::move(image)), caption_(caption) {
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
  }
  void setOnClick(std::function<void()> fn, const QString& tip) {
    onClick = std::move(fn);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tip);
  }

  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override {
    // 縦長（9:16 など）の画面でも欄が伸びすぎないよう、高さには上限をつける
    if (image_.isNull() || image_.width() <= 0) return w * 9 / 16;
    return std::min(int(std::lround(double(w) * image_.height() / image_.width())), kMaxHeight);
  }
  QSize sizeHint() const override { return {240, heightForWidth(240)}; }
  QSize minimumSizeHint() const override { return {60, heightForWidth(60)}; }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect();
    QPainterPath round;
    round.addRoundedRect(r, 4, 4);
    p.fillPath(round, Qt::black);
    if (!image_.isNull()) {
      const QSizeF s = QSizeF(image_.size()).scaled(r.size(), Qt::KeepAspectRatio);
      const QRectF target(r.center().x() - s.width() / 2, r.center().y() - s.height() / 2, s.width(), s.height());
      const qreal dpr = devicePixelRatioF();
      const QSize px = (target.size() * dpr).toSize();
      if (cache_.size() != px) {
        cache_ = QPixmap::fromImage(image_.scaled(px, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        cache_.setDevicePixelRatio(dpr);
      }
      p.save();
      p.setClipPath(round);
      p.drawPixmap(target.topLeft(), cache_);
      p.restore();
    }
    if (!caption_.isEmpty()) {
      // 左下に、どの時間の画面かを出す
      QFont f = font();
      f.setPointSizeF(f.pointSizeF() * 0.85);
      p.setFont(f);
      const QFontMetricsF fm(f);
      const QRectF badge(r.left() + 5, r.bottom() - fm.height() - 9, fm.horizontalAdvance(caption_) + 10, fm.height() + 4);
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(0, 0, 0, 170));
      p.drawRoundedRect(badge, 3, 3);
      p.setPen(Qt::white);
      p.drawText(badge, Qt::AlignCenter, caption_);
    }
    if (hover_ && onClick) {
      p.setPen(QPen(theme::accent, 1.5));
      p.setBrush(Qt::NoBrush);
      p.drawRoundedRect(r.adjusted(0.75, 0.75, -0.75, -0.75), 4, 4);
    }
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && onClick) onClick();
  }
  void enterEvent(QEnterEvent*) override {
    hover_ = true;
    update();
  }
  void leaveEvent(QEvent*) override {
    hover_ = false;
    update();
  }

 private:
  static constexpr int kMaxHeight = 240;
  std::function<void()> onClick;
  QImage image_;
  QString caption_;
  QPixmap cache_;
  bool hover_ = false;
};

/**
 * 同じ幅のタイルを、入るだけ横に並べる（入らなければ次の行へ）。行の高さは、その行でいちばん高いタイルに合わせる。
 * 欄の幅を変えると、並べる数も変わる
 */
class TileLayout : public QLayout {
 public:
  TileLayout(int minTileWidth, QWidget* parent) : QLayout(parent), minWidth_(minTileWidth) {
    setContentsMargins(0, 0, 0, 0);
    setSpacing(8);
  }
  ~TileLayout() override {
    while (QLayoutItem* it = takeAt(0)) delete it;
  }
  void addItem(QLayoutItem* item) override { items_.push_back(item); }
  int count() const override { return int(items_.size()); }
  QLayoutItem* itemAt(int i) const override { return i >= 0 && i < count() ? items_[size_t(i)] : nullptr; }
  QLayoutItem* takeAt(int i) override {
    if (i < 0 || i >= count()) return nullptr;
    QLayoutItem* it = items_[size_t(i)];
    items_.erase(items_.begin() + i);
    return it;
  }
  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override { return arrange(QRect(0, 0, w, 0), false); }
  QSize minimumSize() const override {
    // 高さは heightForWidth で決まるので、ここではいちばん大きいタイル 1 つ分にしておく（Qt の FlowLayout の例と同じ）
    QSize s(minWidth_, 0);
    for (const QLayoutItem* it : items_) s = s.expandedTo(it->minimumSize());
    return s;
  }
  QSize sizeHint() const override {
    const int n = std::max(1, count());
    const int w = minWidth_ * n + spacing() * (n - 1);
    return {w, heightForWidth(w)};
  }
  void setGeometry(const QRect& r) override {
    QLayout::setGeometry(r);
    arrange(r, true);
  }

 private:
  /** r の幅に並べたときの高さ。apply なら、タイルをその位置に置く */
  int arrange(const QRect& r, bool apply) const {
    const int n = count();
    if (n == 0) return 0;
    const int sp = spacing();
    const int cols = std::clamp((r.width() + sp) / (minWidth_ + sp), 1, n);
    const int colWidth = std::max(1, (r.width() - sp * (cols - 1)) / cols);
    int y = r.y();
    for (int first = 0; first < n; first += cols) {
      const int last = std::min(n, first + cols);
      int rowHeight = 0;
      for (int i = first; i < last; ++i) {
        const QLayoutItem* it = items_[size_t(i)];
        rowHeight = std::max(rowHeight, it->hasHeightForWidth() ? it->heightForWidth(colWidth) : it->sizeHint().height());
      }
      if (apply) {
        for (int i = first; i < last; ++i) items_[size_t(i)]->setGeometry(QRect(r.x() + (i - first) * (colWidth + sp), y, colWidth, rowHeight));
      }
      y += rowHeight + sp;
    }
    return y - sp - r.y();
  }

  int minWidth_;
  std::vector<QLayoutItem*> items_;
};

/** 「これにする」ボタンの見せ方（欄と、大きく見比べる画面で同じにする） */
struct PickState {
  QString text;
  bool enabled = false;
  bool chosen = false;
  QString tip;
};

PickState pickState(const AiChoiceCard& card, int index, bool here, bool busy) {
  PickState s;
  s.chosen = card.applied == index;
  s.text = s.chosen ? QStringLiteral("選択中") : card.applied >= 0 ? QStringLiteral("こちらにする") : QStringLiteral("これにする");
  s.enabled = here && !busy && !s.chosen;
  s.tip = !here   ? QStringLiteral("この案を作ったタブに切り替えると選べます")
          : busy  ? QStringLiteral("AI の返事が終わると選べます")
          : s.chosen ? QStringLiteral("Ctrl+Z で戻せます")
                     : QString();
  return s;
}

void applyPickState(QPushButton* b, const PickState& s) {
  b->setText(s.text);
  b->setIcon(s.chosen ? theme::icon(QStringLiteral("check"), theme::ok) : QIcon());
  b->setEnabled(s.enabled);
  b->setToolTip(s.tip);
}

/** 案の記号の丸 */
QLabel* choiceBadge(const QString& letter, QWidget* parent) {
  auto* badge = new QLabel(letter, parent);
  badge->setAlignment(Qt::AlignCenter);
  badge->setFixedSize(20, 20);
  badge->setStyleSheet(QStringLiteral("QLabel{background:#2680eb;color:#ffffff;border-radius:10px;font-weight:600;}"));
  badge->setAccessibleName(QStringLiteral("案 %1").arg(letter));
  return badge;
}

QLabel* wrapLabel(const QString& text, const char* role, QWidget* parent) {
  auto* l = makeLabel(text, role, parent);
  l->setWordWrap(true);
  l->setTextFormat(Qt::PlainText);
  l->setTextInteractionFlags(Qt::TextSelectableByMouse);
  l->setCursor(Qt::IBeamCursor);
  return l;
}

/** 左にアイコン、右に文の 1 行 */
QWidget* iconRow(const QString& icon, const QColor& color, QLabel* label, QWidget* parent) {
  auto* row = new QWidget(parent);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(6);
  auto* ic = new QLabel(row);
  ic->setPixmap(theme::iconPixmap(icon, 13, color, row->devicePixelRatioF()));
  ic->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
  ic->setFixedWidth(14);
  ic->setContentsMargins(0, 2, 0, 0);
  h->addWidget(ic, 0, Qt::AlignTop);
  label->setParent(row);
  h->addWidget(label, 1);
  return row;
}

void setPrimary(QPushButton* b, bool primary) {
  b->setProperty("primary", primary);
  b->style()->unpolish(b);
  b->style()->polish(b);
}

/** 考える深さの、画面に出す名前 */
QString effortName(const QString& id) {
  if (id == u"auto") return QStringLiteral("自動");
  if (id == u"low") return QStringLiteral("低");
  if (id == u"medium") return QStringLiteral("中");
  if (id == u"high") return QStringLiteral("高");
  if (id == u"max") return QStringLiteral("最大");
  return id;
}

/** トークンの数（3 桁ごとにカンマ） */
QString tokenCount(qint64 n) {
  return QLocale(QLocale::Japanese, QLocale::Japan).toString(n);
}

}  // namespace

AiPanel::AiPanel(QWidget* parent) : QWidget(parent), assistant_(new ai::Assistant(this)) {
  setAutoFillBackground(true);
  // ウィンドウの最小の幅（1100px）でも、メディア一覧・プレビュー・右側の設定と並べられる幅
  setMinimumWidth(260);
  setAccessibleName(QStringLiteral("AI アシスタント"));
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);

  // 見出し
  auto* head = new QWidget(this);
  auto* hh = new QHBoxLayout(head);
  hh->setContentsMargins(12, 8, 6, 8);
  hh->setSpacing(4);
  auto* icon = new QLabel(head);
  icon->setPixmap(theme::iconPixmap(QStringLiteral("message-square"), 15, theme::text, devicePixelRatioF()));
  hh->addWidget(icon);
  hh->addSpacing(4);
  hh->addWidget(makeLabel(QStringLiteral("AI アシスタント"), "title", head));
  hh->addStretch(1);
  auto* fresh = new QPushButton(QStringLiteral("新しい会話"), head);
  fresh->setProperty("flat", true);
  fresh->setToolTip(QStringLiteral("会話を最初からにする（タイムラインはそのまま）"));
  fresh->setFocusPolicy(Qt::NoFocus);
  hh->addWidget(fresh);
  auto* close = makeToolButton(QStringLiteral("x"), QStringLiteral("閉じる"), head);
  hh->addWidget(close);
  v->addWidget(head);
  v->addWidget(makeSeparator(this));

  // 会話
  // AI サーバーのアカウント（ログインしているとき）: 名前・今月の残り・アカウントのメニュー
  accountBar_ = new QWidget(this);
  accountBar_->setObjectName(QStringLiteral("AiAccountBar"));
  // 1 行目: 名前とメニュー、2 行目: 今月の残り（パネルの幅いっぱいに使う。とても狭いときだけ折り返す）
  auto* ab = new QVBoxLayout(accountBar_);
  ab->setContentsMargins(12, 6, 6, 6);
  ab->setSpacing(0);
  auto* top = new QHBoxLayout();
  top->setSpacing(6);
  accountName_ = makeLabel(QString(), nullptr, accountBar_);
  accountName_->setObjectName(QStringLiteral("AiAccountName"));
  accountQuota_ = makeLabel(QString(), "dim", accountBar_);
  accountQuota_->setObjectName(QStringLiteral("AiAccountQuota"));
  accountQuota_->setWordWrap(true);
  top->addWidget(accountName_, 1);
  ab->addLayout(top);
  ab->addWidget(accountQuota_);
  auto* accountMenuButton = new QToolButton(accountBar_);
  accountMenuButton->setText(QStringLiteral("アカウント"));
  accountMenuButton->setObjectName(QStringLiteral("AiAccountMenu"));
  accountMenuButton->setPopupMode(QToolButton::InstantPopup);
  accountMenuButton->setAccessibleName(QStringLiteral("アカウントのメニュー"));
  auto* accountMenu = new QMenu(accountMenuButton);
  accountMenu->addAction(QStringLiteral("今月の残りを読み直す"), this, [] { ai::account().refresh(); });
  accountMenu->addAction(QStringLiteral("パスワードを変える..."), this, [this] { AccountDialog::run(window(), AccountDialog::Page::Password); });
  accountMenu->addSeparator();
  accountMenu->addAction(QStringLiteral("ログアウト"), this, [this] {
    if (assistant_->busy()) assistant_->cancel();
    clearConversation();
    ai::account().logout();
  });
  accountMenuButton->setMenu(accountMenu);
  top->addWidget(accountMenuButton, 0, Qt::AlignVCenter);
  v->addWidget(accountBar_);
  accountSeparator_ = makeSeparator(this);
  v->addWidget(accountSeparator_);

  // ログインしていないときの案内（会話の代わりに出す）
  gate_ = buildGate();
  v->addWidget(gate_, 1);

  // 会話と入力（ログインしていれば出す）
  chat_ = new QWidget(this);
  auto* cv = new QVBoxLayout(chat_);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(0);
  v->addWidget(chat_, 1);

  scroll_ = new QScrollArea(chat_);
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* list = new QWidget(scroll_);
  items_ = new QVBoxLayout(list);
  items_->setContentsMargins(12, 12, 12, 12);
  items_->setSpacing(10);

  intro_ = new QWidget(list);
  auto* iv = new QVBoxLayout(intro_);
  iv->setContentsMargins(0, 0, 0, 0);
  iv->setSpacing(10);
  iv->addWidget(wrapLabel(QStringLiteral("編集を頼んだり、タイトルやテロップの案を出してもらったりできます。"
                                         "見た目をおまかせすると、いくつかの案をプレビューで見比べて選べます。\n\n"
                                         "例:\n"
                                         "・1本目を2倍速にして\n"
                                         "・最後に「ご視聴ありがとうございました」と入れて\n"
                                         "・最初に「夏の思い出」とタイトルを入れて。見た目はおまかせ\n"
                                         "・BGM を動画の長さに合わせて、最後はフェードアウトして"),
                          "muted", intro_));
  introNote_ = wrapLabel(QString(), "dim", intro_);
  iv->addWidget(introNote_);
  items_->addWidget(intro_);
  items_->addStretch(1);
  scroll_->setWidget(list);
  cv->addWidget(scroll_, 1);
  // 新しい行が増えたら、いちばん下を見せる
  connect(scroll_->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int max) { scroll_->verticalScrollBar()->setValue(max); });

  // 入力
  auto* bottom = new QWidget(chat_);
  auto* bv = new QVBoxLayout(bottom);
  bv->setContentsMargins(12, 6, 12, 12);
  bv->setSpacing(8);
  status_ = makeLabel(QString(), "dim", bottom);
  status_->hide();
  bv->addWidget(status_);
  auto* quickRow = new QHBoxLayout();
  quickRow->setSpacing(6);
  struct Quick {
    QString label;
    QString request;
  };
  const std::vector<Quick> quicks = {
      {QStringLiteral("タイトル案"),
       QStringLiteral("この動画に合うタイトルの案を 5 つ考えて、propose_texts（kind: title）で見せてください。先に view_frames で動画の中身を確かめてください。")},
      {QStringLiteral("テロップ案"),
       QStringLiteral("動画の場面ごとに合うテロップの案を考えて、開始時間つきで propose_texts（kind: telop）で見せてください。先に view_frames で場面を確かめてください。")},
  };
  for (const Quick& q : quicks) {
    auto* b = new QPushButton(q.label + QStringLiteral("を考える"), bottom);
    b->setToolTip(QStringLiteral("動画の画面を見て、%1を出します（追加は選んでから）").arg(q.label));
    b->setFocusPolicy(Qt::NoFocus);
    const QString request = q.request;
    const QString shown = q.label + QStringLiteral("を考えて");
    connect(b, &QPushButton::clicked, this, [this, request, shown] { ask(request, shown); });
    quickRow->addWidget(b);
    quick_.push_back(b);
  }
  quickRow->addStretch(1);
  bv->addLayout(quickRow);
  // 考える深さと、この会話で使ったトークン
  auto* effortRow = new QHBoxLayout();
  effortRow->setSpacing(6);
  auto* effortLabelWidget = makeLabel(QStringLiteral("考える深さ"), "muted", bottom);
  effortRow->addWidget(effortLabelWidget);
  effort_ = new QComboBox(bottom);
  effort_->setObjectName(QStringLiteral("AiEffort"));
  effort_->setAccessibleName(QStringLiteral("考える深さ"));
  effort_->setFocusPolicy(Qt::TabFocus);
  effortLabelWidget->setBuddy(effort_);
  const std::vector<std::pair<QString, QString>> tips = {
      {QStringLiteral("auto"), QStringLiteral("依頼に合わせて決めます（ふつうの編集は「低」、案を出す・見た目を決める依頼は「中」）")},
      {QStringLiteral("low"), QStringLiteral("考えるのは最小限。速く、トークンも少なめ")},
      {QStringLiteral("medium"), QStringLiteral("ほどほどに考える")},
      {QStringLiteral("high"), QStringLiteral("いつもじっくり考える。難しい依頼向け（遅くなり、トークンも増える）")},
      {QStringLiteral("max"), QStringLiteral("いちばん深く考える（とても遅くなることがあり、トークンも多く使う）")},
  };
  for (const auto& [id, tip] : tips) {
    effort_->addItem(effortName(id), id);
    effort_->setItemData(effort_->count() - 1, tip, Qt::ToolTipRole);
  }
  effortTips_ = tips;
  effort_->setCurrentIndex(std::max(0, effort_->findData(ai::savedEffort())));
  assistant_->setEffort(effort_->currentData().toString());
  // パネルの下でホイールを回しても、深さは変わらないように
  disableWheel(effort_);
  effortRow->addWidget(effort_);
  effortRow->addStretch(1);
  usage_ = makeLabel(QString(), "dim", bottom);
  usage_->setObjectName(QStringLiteral("AiUsageTotal"));
  usage_->hide();
  effortRow->addWidget(usage_);
  bv->addLayout(effortRow);
  input_ = new QPlainTextEdit(bottom);
  input_->setPlaceholderText(QStringLiteral("頼みたいことを入力（Enter で送信、Shift+Enter で改行）"));
  input_->setTabChangesFocus(true);
  input_->setFixedHeight(input_->fontMetrics().lineSpacing() * 3 + 18);
  input_->setAccessibleName(QStringLiteral("AI への依頼"));
  input_->installEventFilter(this);
  bv->addWidget(input_);
  auto* sendRow = new QHBoxLayout();
  sendRow->setSpacing(6);
  auto* tip = makeLabel(QStringLiteral("AI の変更は Ctrl+Z でまとめて戻せます"), "dim", bottom);
  sendRow->addWidget(tip, 1);
  send_ = new QPushButton(theme::icon(QStringLiteral("send"), Qt::white), QStringLiteral("送信"), bottom);
  send_->setProperty("primary", true);
  send_->setFocusPolicy(Qt::NoFocus);
  sendRow->addWidget(send_);
  bv->addLayout(sendRow);
  cv->addWidget(makeSeparator(chat_));
  cv->addWidget(bottom);

  statusTimer_.setInterval(500);
  connect(&statusTimer_, &QTimer::timeout, this, &AiPanel::refreshStatus);

  connect(close, &QToolButton::clicked, this, &AiPanel::closeRequested);
  connect(fresh, &QPushButton::clicked, this, &AiPanel::clearConversation);
  connect(effort_, &QComboBox::activated, this, [this](int index) {
    // 次に送る依頼から使う（返事を待っている途中の依頼は、そのままの深さで続ける）
    const QString id = effort_->itemData(index).toString();
    assistant_->setEffort(id);
    ai::saveEffort(id);
  });
  connect(assistant_, &ai::Assistant::usageChanged, this, &AiPanel::showUsage);
  connect(assistant_, &ai::Assistant::turnFinished, this, &AiPanel::addUsage);
  // AI サーバーのアカウントとつながり
  connect(&ai::account(), &ai::Account::changed, this, &AiPanel::refreshAccount);
  connect(&ai::account(), &ai::Account::linkChanged, this, &AiPanel::refreshAccount);
  connect(&ai::account(), &ai::Account::failed, this, [this](const QString& message) {
    if (isVisible() && chat_->isVisible()) addError(message);
  });
  connect(send_, &QPushButton::clicked, this, [this] {
    if (assistant_->busy()) {
      assistant_->cancel();
      auto* note = makeLabel(QStringLiteral("中止しました（それまでの変更は残っています）"), "dim", this);
      addItem(note);
      return;
    }
    sendInput();
  });
  connect(assistant_, &ai::Assistant::busyChanged, this, &AiPanel::setBusy);
  connect(assistant_, &ai::Assistant::statusChanged, this, [this](const QString& s) {
    statusText_ = s;
    refreshStatus();
  });
  connect(assistant_, &ai::Assistant::replied, this, &AiPanel::addReply);
  connect(assistant_, &ai::Assistant::acted, this, &AiPanel::addAction);
  connect(assistant_, &ai::Assistant::proposed, this, &AiPanel::addIdeas);
  connect(assistant_, &ai::Assistant::choicesProposed, this, &AiPanel::addChoices);
  connect(assistant_, &ai::Assistant::failed, this, &AiPanel::addError);
  connect(&editor(), &Editor::projectChanged, this, &AiPanel::trackChoices);
  connect(&editor(), &Editor::workspaceChanged, this, &AiPanel::refreshChoices);
  refreshAccount();
}

QWidget* AiPanel::buildGate() {
  auto* gate = new QScrollArea(this);
  gate->setObjectName(QStringLiteral("AiGate"));
  gate->setWidgetResizable(true);
  gate->setFrameShape(QFrame::NoFrame);
  gate->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* inner = new QWidget(gate);
  auto* v = new QVBoxLayout(inner);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(8);
  gateTitle_ = makeLabel(QString(), "title", inner);
  gateTitle_->setObjectName(QStringLiteral("AiGateTitle"));
  v->addWidget(gateTitle_);
  gateText_ = wrapLabel(QString(), "muted", inner);
  gateText_->setObjectName(QStringLiteral("AiGateText"));
  // .env の場所やサーバーのアドレスを写せるように
  gateText_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(gateText_);
  v->addSpacing(4);
  gateButton_ = new QPushButton(inner);
  gateButton_->setObjectName(QStringLiteral("AiGateButton"));
  gateButton_->setProperty("primary", true);
  v->addWidget(gateButton_, 0, Qt::AlignLeft);
  gateRetry_ = new QPushButton(QStringLiteral("もう一度つなぐ"), inner);
  gateRetry_->setObjectName(QStringLiteral("AiGateRetry"));
  gateRetry_->setToolTip(QStringLiteral("前回のログインのまま、AI サーバーにもう一度つなぎます"));
  v->addWidget(gateRetry_, 0, Qt::AlignLeft);
  v->addStretch(1);
  connect(gateButton_, &QPushButton::clicked, this, [this] {
    AccountDialog::run(window(), gatePassword_ ? AccountDialog::Page::Password : AccountDialog::Page::Login);
  });
  connect(gateRetry_, &QPushButton::clicked, this, [] {
    ai::account().checkLink();
    ai::account().refresh();
  });
  gate->setWidget(inner);
  return gate;
}

void AiPanel::refreshAccount() {
  const ai::Account& a = ai::account();
  using Link = ai::Account::Link;
  const bool ready = a.loggedIn() && !a.info().mustChangePassword;
  gate_->setVisible(!ready);
  chat_->setVisible(ready);
  accountBar_->setVisible(a.loggedIn());
  accountSeparator_->setVisible(accountBar_->isVisible());
  gatePassword_ = false;
  gateRetry_->hide();
  gateButton_->show();
  if (a.server().isEmpty()) {
    // アドレスはアプリの中では決めない（.env に書いてもらう）
    const QString env = ai::envFile().isEmpty() ? ai::suggestedEnvPath() : ai::envFile();
    gateTitle_->setText(QStringLiteral("AI サーバーのアドレスが決まっていません"));
    gateText_->setText(QStringLiteral("AI アシスタントを使うには、%1 に\nKANTAN_AI_SERVER=（AI サーバーのアドレス）\nを書いて、アプリに戻ってきてください。"
                                      "アドレスは、AI サーバーを動かしている人（管理者）に教えてもらってください。")
                           .arg(env));
    gateButton_->hide();
  } else if (a.hasToken() && !a.loggedIn()) {
    // 前回のログインは残っているが、サーバーにつながらず、まだ確かめられていない（アドレスは出さない）
    gateTitle_->setText(QStringLiteral("AI サーバーにつながりません"));
    gateText_->setText(a.linkDetail().isEmpty() ? QStringLiteral("前回のログインのまま、つながるのを待っています。") : a.linkDetail());
    gateButton_->setText(QStringLiteral("ログインし直す"));
    gateRetry_->show();
  } else if (!a.loggedIn()) {
    gateTitle_->setText(QStringLiteral("ログインしてください"));
    // つながらないときだけ理由を出す（アドレスは出さない）
    QStringList text;
    if (a.link() == Link::Offline || a.link() == Link::Wrong || a.link() == Link::NoDatabase) text << a.linkDetail();
    text << (a.registration() == u"off" ? QStringLiteral("アカウントは、管理者に作ってもらってください。")
                                        : QStringLiteral("アカウントが無いときは、ログインの画面の「新規登録」から作れます。"));
    gateText_->setText(text.join(QLatin1Char('\n')));
    gateButton_->setText(QStringLiteral("ログイン"));
  } else if (!ready) {
    gateTitle_->setText(QStringLiteral("パスワードを変えてください"));
    gateText_->setText(QStringLiteral("管理者が作った仮のパスワードでログインしています。AI を使う前に、自分だけのパスワードに変えてください。"));
    gateButton_->setText(QStringLiteral("パスワードを変える"));
    gatePassword_ = true;
  }
  if (a.loggedIn()) {
    const ai::AccountInfo& info = a.info();
    accountName_->setText(ai::accountLabel(info));
    accountName_->setToolTip(info.email);
    bool warn = false;
    accountQuota_->setText(ai::quotaSummary(info, &warn));
    accountQuota_->setProperty("role", warn ? "error" : "dim");
    accountQuota_->style()->unpolish(accountQuota_);
    accountQuota_->style()->polish(accountQuota_);
  }
  refreshEffortChoices();
  refreshHints();
}

void AiPanel::refreshEffortChoices() {
  ai::Account& a = ai::account();
  // そのアカウントの上限までの深さだけを並べる
  const bool limited = a.loggedIn();
  const QString max = limited ? a.info().maxEffort : QStringLiteral("max");
  QString want = effort_->currentData().toString();
  if (limited && a.freshLogin()) {
    // ログインした直後は、管理者が決めた最初の深さにする
    want = a.info().defaultEffort;
    a.clearFreshLogin();
    ai::saveEffort(want);
  }
  if (want != u"auto") want = ai::clampEffort(want.isEmpty() ? QStringLiteral("auto") : want, max);
  {
    const QSignalBlocker block(effort_);
    effort_->clear();
    for (const auto& [id, tip] : effortTips_) {
      if (id != u"auto" && ai::clampEffort(id, max) != id) continue;
      effort_->addItem(effortName(id), id);
      effort_->setItemData(effort_->count() - 1, tip, Qt::ToolTipRole);
    }
    effort_->setCurrentIndex(std::max(0, effort_->findData(want)));
  }
  assistant_->setEffort(effort_->currentData().toString());
  // 深さを変えられないモデルでは、選べないようにする
  ai::Config serverModel;
  serverModel.model = a.info().model;
  const bool canChoose = limited && serverModel.supportsEffort();
  effort_->setEnabled(canChoose);
  effort_->setToolTip(canChoose ? (limited ? QStringLiteral("AI が答える前に、どれくらい考えるか（このアカウントは「%1」まで）").arg(effortName(max))
                                           : QStringLiteral("AI が答える前に、どれくらい考えるか。次に送る依頼から使います"))
                                : QStringLiteral("このモデルでは、考える深さを変えられません（モデルの標準のまま）"));
}

void AiPanel::focusInput() {
  input_->setFocus();
}

void AiPanel::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  // 開くたびに、今のサーバーとログインに合わせる
  refreshAccount();
}

bool AiPanel::eventFilter(QObject* obj, QEvent* e) {
  if (obj == input_ && e->type() == QEvent::KeyPress) {
    auto* ke = static_cast<QKeyEvent*>(e);
    if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) && !(ke->modifiers() & Qt::ShiftModifier)) {
      if (!assistant_->busy()) sendInput();
      return true;
    }
  }
  return QWidget::eventFilter(obj, e);
}

void AiPanel::addUsage(const ai::Usage& turn, const QString& effort, bool automatic) {
  const QString depth = effort.isEmpty() ? QStringLiteral("モデルの標準")
                        : automatic       ? QStringLiteral("自動（%1）").arg(effortName(effort))
                                          : effortName(effort);
  auto* line = makeLabel(QStringLiteral("使ったトークン: 入力 %1・出力 %2 ／ 考える深さ: %3").arg(tokenCount(turn.input), tokenCount(turn.output), depth), "dim", this);
  line->setObjectName(QStringLiteral("AiUsageLine"));
  line->setWordWrap(true);
  line->setToolTip(QStringLiteral("AI に %1 回送りました。入力のうち %2 はキャッシュの分です。出力には、AI が考えた分も入ります")
                       .arg(turn.calls)
                       .arg(tokenCount(turn.cached)));
  addItem(line);
}

void AiPanel::showUsage(const ai::Usage& session) {
  usage_->setVisible(session.calls > 0);
  usage_->setText(QStringLiteral("この会話 %1 トークン").arg(tokenCount(session.input + session.output)));
  usage_->setToolTip(QStringLiteral("入力 %1（うちキャッシュ %2）・出力 %3（「新しい会話」で 0 に戻ります）")
                         .arg(tokenCount(session.input), tokenCount(session.cached), tokenCount(session.output)));
}

void AiPanel::refreshHints() {
  // サーバーのアドレスは出さない
  introNote_->setText(QStringLiteral("送った文、プロジェクトの内容、画面の一部が、AI サーバーを通して Amazon Bedrock に送られます。"
                                     "使ったトークンは、アカウントごとに記録されます。"));
}

void AiPanel::addItem(QWidget* w) {
  if (!hasMessages_) {
    hasMessages_ = true;
    intro_->hide();
  }
  // 最後の伸縮の手前に入れる
  items_->insertWidget(items_->count() - 1, w);
}

void AiPanel::addUser(const QString& text) {
  auto* row = new QWidget(this);
  auto* h = new QHBoxLayout(row);
  h->setContentsMargins(36, 0, 0, 0);
  h->addStretch(1);
  auto* bubble = wrapLabel(text, nullptr, row);
  bubble->setStyleSheet(QStringLiteral("QLabel{background:#2c2c2c;border-radius:6px;padding:6px 9px;}"));
  h->addWidget(bubble);
  addItem(row);
}

void AiPanel::addReply(const QString& text) {
  addItem(wrapLabel(text, nullptr, this));
}

void AiPanel::addAction(const QString& text) {
  auto* label = wrapLabel(text, "muted", this);
  addItem(iconRow(QStringLiteral("check"), theme::ok, label, this));
}

void AiPanel::addError(const QString& text) {
  auto* label = wrapLabel(text, "error", this);
  addItem(iconRow(QStringLiteral("alert"), theme::danger, label, this));
}

void AiPanel::addIdeas(const QString& kind, const std::vector<ai::TextIdea>& ideas) {
  const bool telop = kind == u"telop";
  auto* card = new QFrame(this);
  card->setObjectName(QStringLiteral("AiIdeas"));
  card->setStyleSheet(QStringLiteral("#AiIdeas{background:#202020;border:1px solid #2e2e2e;border-radius:6px;}"));
  auto* cv = new QVBoxLayout(card);
  cv->setContentsMargins(10, 8, 10, 10);
  cv->setSpacing(6);
  cv->addWidget(makeLabel(telop ? QStringLiteral("テロップ案（追加を押したものだけが入ります）") : QStringLiteral("タイトル案（追加を押したものが入ります）"),
                          "muted", card));
  auto pending = std::make_shared<std::vector<int>>();
  auto buttons = std::make_shared<std::vector<QPushButton*>>();
  for (size_t i = 0; i < ideas.size(); ++i) {
    const ai::TextIdea idea = ideas[i];
    auto* row = new QWidget(card);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    if (idea.start) {
      auto* time = makeLabel(formatTime(*idea.start), "dim", row);
      time->setMinimumWidth(time->fontMetrics().horizontalAdvance(QStringLiteral("0:00.0")) + 4);
      h->addWidget(time, 0, Qt::AlignTop);
    }
    auto* text = wrapLabel(idea.text, nullptr, row);
    h->addWidget(text, 1);
    auto* add = new QPushButton(QStringLiteral("追加"), row);
    add->setFocusPolicy(Qt::NoFocus);
    h->addWidget(add, 0, Qt::AlignTop);
    cv->addWidget(row);
    buttons->push_back(add);
    pending->push_back(int(i));
    connect(add, &QPushButton::clicked, this, [idea, kind, add, pending, i] {
      const QStringList ids = ai::addIdeas({idea}, kind);
      if (ids.isEmpty()) return;
      add->setText(QStringLiteral("追加済み"));
      add->setEnabled(false);
      pending->erase(std::remove(pending->begin(), pending->end(), int(i)), pending->end());
      // 追加したテキストを選んで、その時間を表示する
      Editor& ed = editor();
      ed.select({Selection::Text, ids.front()});
      if (const TextItem* t = ed.text(ids.front())) ed.seek(t->start);
    });
  }
  if (telop && ideas.size() > 1) {
    auto* all = new QPushButton(QStringLiteral("すべて追加"), card);
    all->setFocusPolicy(Qt::NoFocus);
    cv->addWidget(all, 0, Qt::AlignRight);
    connect(all, &QPushButton::clicked, this, [ideas, kind, pending, buttons, all] {
      std::vector<ai::TextIdea> rest;
      for (int i : *pending) rest.push_back(ideas[size_t(i)]);
      if (rest.empty()) return;
      const QStringList ids = ai::addIdeas(rest, kind);
      if (ids.isEmpty()) return;
      for (int i : *pending) {
        (*buttons)[size_t(i)]->setText(QStringLiteral("追加済み"));
        (*buttons)[size_t(i)]->setEnabled(false);
      }
      pending->clear();
      all->setEnabled(false);
      editor().select({Selection::Text, ids.front()});
      if (const TextItem* t = editor().text(ids.front())) editor().seek(t->start);
    });
  }
  addItem(card);
}

void AiPanel::addChoices(const ai::ChoiceSet& set) {
  if (set.choices.empty()) return;
  auto card = std::make_shared<AiChoiceCard>();
  card->set = set;
  auto* box = new QFrame(this);
  box->setObjectName(QStringLiteral("AiChoices"));
  box->setProperty("choiceSetId", set.id);
  box->setStyleSheet(QStringLiteral("#AiChoices{background:#202020;border:1px solid #2e2e2e;border-radius:6px;}"
                                    "#AiChoice{background:#262626;border:1px solid #333333;border-radius:6px;}"
                                    "#AiChoice[chosen=\"true\"]{border:1px solid #2680eb;}"));
  auto* v = new QVBoxLayout(box);
  v->setContentsMargins(10, 8, 10, 10);
  v->setSpacing(8);
  card->box = box;
  v->addWidget(wrapLabel(set.question.isEmpty() ? QStringLiteral("どれにしますか？") : set.question, "title", box));
  v->addWidget(wrapLabel(QStringLiteral("「これにする」を押した案だけが入ります。選んだあとでも、ほかの案に切り替えられます。"), "dim", box));
  // 欄が広ければ案を横に並べ、狭ければ縦に重ねる
  auto* tiles = new QWidget(box);
  auto* grid = new TileLayout(190, tiles);
  for (size_t i = 0; i < set.choices.size(); ++i) {
    const ai::Choice& c = set.choices[i];
    const QString letter = ai::choiceLetter(int(i));
    auto* f = new QFrame(tiles);
    f->setObjectName(QStringLiteral("AiChoice"));
    auto* fv = new QVBoxLayout(f);
    fv->setContentsMargins(8, 8, 8, 8);
    fv->setSpacing(6);
    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    head->addWidget(choiceBadge(letter, f), 0, Qt::AlignTop);
    head->addWidget(wrapLabel(c.label, "title", f), 1);
    fv->addLayout(head);
    for (size_t j = 0; j < c.previews.size() && j < set.times.size(); ++j) {
      const QImage img = QImage::fromData(c.previews[j], "JPG");
      if (img.isNull()) continue;
      const QString time = formatTime(set.times[j]);
      auto* thumb = new PreviewThumb(img, time, f);
      thumb->setAccessibleName(QStringLiteral("案 %1 のプレビュー（%2）").arg(letter, time));
      thumb->setOnClick([this, card] { showComparison(card); }, QStringLiteral("押すと、案を大きく並べて見比べられます"));
      fv->addWidget(thumb);
    }
    // AI の説明が無ければ、何をするか（道具の結果の 1 行ずつ）を出す
    const QString about = !c.description.isEmpty() ? c.description : c.log.join(QLatin1Char('\n'));
    if (!about.isEmpty()) fv->addWidget(wrapLabel(about, "muted", f));
    // 横に並んだ案どうしで、ボタンの高さをそろえる
    fv->addStretch(1);
    auto* pick = new QPushButton(QStringLiteral("これにする"), f);
    pick->setObjectName(QStringLiteral("AiChoiceButton"));
    pick->setProperty("choiceIndex", int(i));
    // クリックで入力欄のフォーカスは奪わない。Tab では選べる
    pick->setFocusPolicy(Qt::TabFocus);
    pick->setAccessibleName(QStringLiteral("案 %1「%2」にする").arg(letter, c.label));
    fv->addWidget(pick, 0, Qt::AlignRight);
    // ボタンが消えるとつなぎも消えるので、欄の状態は欄と一緒に消える
    connect(pick, &QPushButton::clicked, this, [this, card, i] { chooseOption(card, int(i)); });
    card->buttons.push_back(pick);
    card->frames.push_back(f);
    grid->addWidget(f);
  }
  v->addWidget(tiles);
  // 新しい欄は下が見えるので、見比べるボタンは下に置く
  auto* compare = new QPushButton(theme::icon(QStringLiteral("maximize")), QStringLiteral("大きく並べて見比べる"), box);
  compare->setObjectName(QStringLiteral("AiCompareButton"));
  compare->setFocusPolicy(Qt::TabFocus);
  compare->setToolTip(QStringLiteral("案を大きく横に並べて見比べます（そこからも選べます）"));
  v->addWidget(compare, 0, Qt::AlignRight);
  connect(compare, &QPushButton::clicked, this, [this, card] { showComparison(card); });
  choiceCards_.push_back(card);
  if (QFrame* old = turnChoices_; old && items_->indexOf(old) >= 0) {
    // 同じ回の出し直し（プレビューを見た AI が直した案）。前の案の欄と、その場で入れ替える
    const int at = items_->indexOf(old);
    items_->removeWidget(old);
    old->hide();
    old->deleteLater();
    items_->insertWidget(at, box);
  } else {
    addItem(box);
  }
  turnChoices_ = box;
  refreshChoices();
}

void AiPanel::showComparison(const std::shared_ptr<AiChoiceCard>& card) {
  if (card->dialog) {
    card->dialog->raise();
    card->dialog->activateWindow();
    return;
  }
  const ai::ChoiceSet& set = card->set;
  const int cols = int(set.choices.size());
  const int rows = std::max(1, int(set.times.size()));
  auto* dlg = new QDialog(window());
  dlg->setAttribute(Qt::WA_DeleteOnClose);
  dlg->setWindowTitle(QStringLiteral("案を見比べる"));
  dlg->setAccessibleName(QStringLiteral("案を見比べる"));
  card->dialog = dlg;
  card->dialogButtons.clear();

  // 画面に収まる大きさ（作った画像の大きさ 1280px の半分より大きくはしない）
  const QImage first = set.choices.front().previews.empty() ? QImage() : QImage::fromData(set.choices.front().previews.front(), "JPG");
  const double ratio = first.isNull() || first.width() <= 0 ? 9.0 / 16 : double(first.height()) / first.width();
  const QSize avail = screen() ? screen()->availableSize() : QSize(1280, 800);
  constexpr int kGap = 16;
  int cellW = std::min(640, (int(avail.width() * 0.88) - 2 * kGap - (cols - 1) * kGap) / cols);
  int cellH = int(std::lround(cellW * ratio));
  const int maxH = (int(avail.height() * 0.85) - 260) / rows;
  if (cellH > maxH) {
    cellH = std::max(90, maxH);
    cellW = int(std::lround(cellH / ratio));
  }

  auto* v = new QVBoxLayout(dlg);
  v->setContentsMargins(kGap, kGap, kGap, kGap);
  v->setSpacing(12);
  v->addWidget(wrapLabel(set.question.isEmpty() ? QStringLiteral("どれにしますか？") : set.question, "title", dlg));
  auto* row = new QHBoxLayout();
  row->setSpacing(kGap);
  for (int i = 0; i < cols; ++i) {
    const ai::Choice& c = set.choices[size_t(i)];
    const QString letter = ai::choiceLetter(i);
    auto* col = new QVBoxLayout();
    col->setSpacing(8);
    auto* head = new QHBoxLayout();
    head->setSpacing(8);
    head->addWidget(choiceBadge(letter, dlg), 0, Qt::AlignTop);
    auto* name = wrapLabel(c.label, "title", dlg);
    name->setMaximumWidth(cellW - 28);
    head->addWidget(name, 1);
    col->addLayout(head);
    for (size_t j = 0; j < c.previews.size() && j < set.times.size(); ++j) {
      const QImage img = QImage::fromData(c.previews[j], "JPG");
      if (img.isNull()) continue;
      const QString time = formatTime(set.times[j]);
      auto* view = new PreviewThumb(img, time, dlg);
      view->setFixedSize(cellW, cellH);
      view->setAccessibleName(QStringLiteral("案 %1 のプレビュー（%2）").arg(letter, time));
      col->addWidget(view);
    }
    const QString about = !c.description.isEmpty() ? c.description : c.log.join(QLatin1Char('\n'));
    if (!about.isEmpty()) {
      auto* text = wrapLabel(about, "muted", dlg);
      text->setMaximumWidth(cellW);
      col->addWidget(text);
    }
    col->addStretch(1);
    auto* pick = new QPushButton(QStringLiteral("これにする"), dlg);
    pick->setAccessibleName(QStringLiteral("案 %1「%2」にする").arg(letter, c.label));
    col->addWidget(pick, 0, Qt::AlignRight);
    // 選んだら閉じる（選んだ案はプレビューに出る）
    connect(pick, &QPushButton::clicked, this, [this, card, i, dlg] {
      chooseOption(card, i);
      if (card->applied == i) dlg->close();
    });
    card->dialogButtons.push_back(pick);
    row->addLayout(col);
  }
  v->addLayout(row);
  auto* bottom = new QHBoxLayout();
  bottom->addWidget(wrapLabel(QStringLiteral("選んだあとでも、AI の欄の「こちらにする」でほかの案に切り替えられます（Ctrl+Z で戻せます）。"), "dim", dlg), 1);
  auto* close = new QPushButton(QStringLiteral("閉じる"), dlg);
  bottom->addWidget(close);
  v->addLayout(bottom);
  connect(close, &QPushButton::clicked, dlg, &QDialog::close);
  // 「新しい会話」で欄が消えたら、この画面も閉じる
  if (card->box) connect(card->box, &QObject::destroyed, dlg, &QWidget::close);
  refreshChoices();
  dlg->show();
}

void AiPanel::chooseOption(const std::shared_ptr<AiChoiceCard>& card, int index) {
  Editor& ed = editor();
  if (assistant_->busy() || index < 0 || index >= int(card->set.choices.size()) || card->applied == index) return;
  const ai::Choice& choice = card->set.choices[size_t(index)];
  const QString letter = ai::choiceLetter(index);
  if (ed.workspaceUid(ed.workspaceIndex()) != card->set.workspaceUid) {
    addError(QStringLiteral("この案は、案を作ったときのタブのものです。そのタブに切り替えてから選んでください。"));
    return;
  }
  ed.pause();
  const QJsonObject now = projectToJson(ed.project());
  const ProjectData* start = nullptr;
  if (card->applied >= 0) {
    // ほかの案が当てはまっているときは、案を当てはめる前の状態に、この案を当てはめる（1 回の操作で切り替わる）
    if (now != card->after[size_t(card->applied)]) {
      addError(QStringLiteral("案 %1 を選んだあとにほかの変更をしたので、切り替えられません。Ctrl+Z で案 %1 を戻してから選んでください。")
                   .arg(ai::choiceLetter(card->applied)));
      return;
    }
    start = &*card->base;
  } else if (!card->base || now != card->baseJson) {
    // はじめて選ぶとき、または元に戻したあとにほかの変更をしたときは、今の状態を「案を当てはめる前」にする
    card->base = ed.project();
    card->baseJson = now;
    card->after.assign(card->set.choices.size(), QJsonObject());
  }
  choosing_ = true;
  const ai::ChoiceApplied res = ai::applyChoice(choice, QStringLiteral("ai-choice-") + newId(), start);
  choosing_ = false;
  if (!res.ok) {
    addError(QStringLiteral("案 %1 を今のタイムラインに当てはめられませんでした（%2）。もう一度頼んでください。").arg(letter, res.error));
    return;
  }
  card->after[size_t(index)] = projectToJson(ed.project());
  card->applied = index;
  addAction(QStringLiteral("案 %1「%2」にしました").arg(letter, choice.label));
  assistant_->noteChoice(card->set.id, QStringLiteral("%1「%2」").arg(letter, choice.label));
  // 変えたところを選び、プレビューと同じ時間を表示する
  if (!res.focusText.isEmpty()) ed.select({Selection::Text, res.focusText});
  else if (!res.focusClip.isEmpty()) ed.select({Selection::Clip, res.focusClip});
  if (!card->set.times.empty()) ed.seek(card->set.times.front());
  refreshChoices();
}

void AiPanel::trackChoices() {
  if (choosing_) return;
  const Editor& ed = editor();
  const int uid = ed.workspaceUid(ed.workspaceIndex());
  std::optional<QJsonObject> now;
  bool changed = false;
  for (const auto& weak : choiceCards_) {
    const auto card = weak.lock();
    if (!card || !card->base || card->set.workspaceUid != uid) continue;
    if (!now) now = projectToJson(ed.project());
    // 案を当てはめる前の状態なら外れた（元に戻した）、どれかの案を当てはめた直後の状態ならその案。それ以外はそのまま
    int applied = card->applied;
    if (*now == card->baseJson) {
      applied = -1;
    } else {
      for (size_t i = 0; i < card->after.size(); ++i) {
        if (!card->after[i].isEmpty() && *now == card->after[i]) applied = int(i);
      }
    }
    if (applied != card->applied) {
      card->applied = applied;
      changed = true;
    }
  }
  if (changed) refreshChoices();
}

void AiPanel::refreshChoices() {
  const Editor& ed = editor();
  const int uid = ed.workspaceUid(ed.workspaceIndex());
  const bool busy = assistant_->busy();
  for (auto it = choiceCards_.begin(); it != choiceCards_.end();) {
    const auto card = it->lock();
    if (!card) {
      it = choiceCards_.erase(it);
      continue;
    }
    ++it;
    const bool here = card->set.workspaceUid == uid;
    for (size_t i = 0; i < card->buttons.size(); ++i) {
      const PickState s = pickState(*card, int(i), here, busy);
      if (QPushButton* b = card->buttons[i]) applyPickState(b, s);
      if (i < card->dialogButtons.size()) {
        if (QPushButton* b = card->dialogButtons[i]) applyPickState(b, s);
      }
      if (QFrame* f = card->frames[i]; f && f->property("chosen").toBool() != s.chosen) {
        f->setProperty("chosen", s.chosen);
        f->style()->unpolish(f);
        f->style()->polish(f);
      }
    }
  }
}

void AiPanel::setBusy(bool busy) {
  send_->setText(busy ? QStringLiteral("中止") : QStringLiteral("送信"));
  send_->setIcon(busy ? theme::icon(QStringLiteral("stop")) : theme::icon(QStringLiteral("send"), Qt::white));
  setPrimary(send_, !busy);
  for (auto* b : quick_) b->setEnabled(!busy);
  if (busy) {
    // 新しい依頼では、前の回の案の欄は置き換えない
    turnChoices_ = nullptr;
    busyClock_.start();
    statusTimer_.start();
  } else {
    statusTimer_.stop();
  }
  refreshStatus();
  refreshChoices();
}

void AiPanel::refreshStatus() {
  const bool show = assistant_->busy() && !statusText_.isEmpty();
  status_->setVisible(show);
  if (!show) return;
  const qint64 sec = busyClock_.elapsed() / 1000;
  const int dots = int((busyClock_.elapsed() / 500) % 3) + 1;
  status_->setText(QStringLiteral("%1%2（%3 秒）").arg(statusText_, QString(dots, QChar(u'.')), QString::number(sec)));
}

bool AiPanel::ask(const QString& request, const QString& shown) {
  if (assistant_->busy()) return false;
  addUser(shown.isEmpty() ? request : shown);
  return assistant_->send(request);
}

void AiPanel::sendInput() {
  const QString text = input_->toPlainText().trimmed();
  if (text.isEmpty()) return;
  if (ask(text)) input_->clear();
}

void AiPanel::clearConversation() {
  assistant_->reset();
  // はじめの説明と最後の伸縮だけを残す
  while (items_->count() > 2) {
    QLayoutItem* it = items_->takeAt(1);
    if (QWidget* w = it->widget()) w->deleteLater();
    delete it;
  }
  hasMessages_ = false;
  intro_->show();
  refreshHints();
}

}  // namespace kv
