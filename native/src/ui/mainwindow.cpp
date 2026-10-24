#include "ui/mainwindow.h"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

#include <windows.h>
#include <shobjidl.h>

#include <functional>

#include "ai/account.h"
#include "ai/assistant.h"
#include "core/editor.h"
#include "core/projectio.h"
#include "export/exporter.h"
#include "media/mediamanager.h"
#include "render/previewengine.h"
#include "ui/accountdialog.h"
#include "ui/aipanel.h"
#include "ui/exportdialog.h"
#include "ui/inspector.h"
#include "ui/keymap.h"
#include "ui/librarypanel.h"
#include "ui/perspective.h"
#include "ui/previewwidget.h"
#include "ui/serverstatus.h"
#include "ui/shortcutsdialog.h"
#include "ui/theme.h"
#include "ui/timelinewidget.h"
#include "ui/widgets.h"
#include "ui/workspacebar.h"

namespace kv {

MainWindow::MainWindow() {
  setWindowIcon(QIcon(QStringLiteral(":/icons/film.svg")));
  setAcceptDrops(true);
  resize(1440, 900);
  setMinimumSize(1100, 680);

  engine_ = new PreviewEngine(this);
  preview_ = new PreviewPanel(engine_, this);
  inspector_ = new Inspector(this);
  timeline_ = new TimelineWidget(this);
  inspector_->setMinimumWidth(280);
  // AI アシスタントは右端の欄。はじめはしまっておき、右上の「AI」で出す
  aiPanel_ = new AiPanel(this);

  // 左右の欄のビュー（メディア・オーディオ・テキスト・プロパティ）。パースペクティブで、置く欄・順番・開くかを変えられる
  using Side = PerspectiveManager::Side;
  perspective_ = new PerspectiveManager(this);
  perspective_->addView(QStringLiteral("media"), QStringLiteral("メディア"), LibraryPanel::createMediaView(this), Side::Left);
  perspective_->addView(QStringLiteral("audio"), QStringLiteral("オーディオ"), LibraryPanel::createAudioView(this), Side::Left);
  perspective_->addView(QStringLiteral("text"), QStringLiteral("テキスト"), LibraryPanel::createTextView(this), Side::Left);
  perspective_->addView(QStringLiteral("properties"), QStringLiteral("プロパティ"), inspector_, Side::Right);

  auto* top = new QSplitter(Qt::Horizontal, this);
  top_ = top;
  top->addWidget(perspective_->area(Side::Left));
  top->addWidget(preview_);
  top->addWidget(perspective_->area(Side::Right));
  top->addWidget(aiPanel_);
  top->setStretchFactor(0, 0);
  top->setStretchFactor(1, 1);
  top->setStretchFactor(2, 0);
  top->setStretchFactor(3, 0);
  top->setSizes({264, 860, 316, 0});
  top->setChildrenCollapsible(false);
  aiPanel_->hide();
  // 前回のパースペクティブ（ビューの並びと欄の幅）に戻す
  perspective_->attach(top, 0, 1, 2);
  perspective_->restore();
  connect(aiPanel_, &AiPanel::closeRequested, this, [this] { setAiVisible(false); });
  auto* main = new QSplitter(Qt::Vertical, this);
  main->addWidget(top);
  main->addWidget(timeline_);
  main->setStretchFactor(0, 1);
  main->setStretchFactor(1, 0);
  main->setSizes({580, 300});
  main->setChildrenCollapsible(false);
  timeline_->setMinimumHeight(200);

  // 編集画面の上に、編集場所の切り替えタブ（編集1 / 編集2）を置く
  auto* central = new QWidget(this);
  auto* cv = new QVBoxLayout(central);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(0);
  auto* tabBar = new WorkspaceBar(central);
  connect(tabBar, &WorkspaceBar::closeRequested, this, &MainWindow::closeWorkspace);
  cv->addWidget(tabBar);
  cv->addWidget(main, 1);
  setCentralWidget(central);

  buildMenus();
  // AI サーバーのアドレス（.env の KANTAN_AI_SERVER）が無ければ、AI アシスタントは出さない。
  // .env を書き換えてアプリに戻ってきたら、Account が読み直して changed を出す（起動し直さなくてよい）
  refreshAiAvailability();
  connect(&ai::account(), &ai::Account::changed, this, &MainWindow::refreshAiAvailability);

  // 通知（ステータスバーの左）と、AI サーバーにつながっているか（右下）
  auto* sb = statusBar();
  sb->setSizeGripEnabled(false);
  auto* toast = new QWidget(sb);
  auto* th = new QHBoxLayout(toast);
  th->setContentsMargins(6, 0, 6, 0);
  th->setSpacing(6);
  toastIcon_ = new QLabel(toast);
  toastText_ = new QLabel(toast);
  th->addWidget(toastIcon_);
  th->addWidget(toastText_);
  th->addStretch(1);
  sb->addWidget(toast, 1);
  sb->addPermanentWidget(new ServerStatus(sb));
  toastTimer_.setSingleShot(true);
  connect(&toastTimer_, &QTimer::timeout, this, [this] {
    toastIcon_->clear();
    toastText_->clear();
  });

  dropOverlay_ = new QWidget(this);
  dropOverlay_->setAttribute(Qt::WA_TransparentForMouseEvents);
  dropOverlay_->setStyleSheet(QStringLiteral("background: rgba(38,128,235,30); border: 2px solid #2680eb;"));
  dropOverlay_->hide();

  Editor& ed = editor();
  connect(&ed, &Editor::toastRequested, this, [this](const QString& m, ToastKind k) { showToast(m, int(k)); });
  connect(&ed, &Editor::dirtyChanged, this, &MainWindow::updateTitle);
  connect(&ed, &Editor::workspacesChanged, this, &MainWindow::updateTitle);
  connect(&ed, &Editor::projectChanged, this, [this] {
    updateTitle();
    exportButton_->setEnabled(!editor().project().clips.empty());
    autosave_.start();
  });
  autosave_.setSingleShot(true);
  autosave_.setInterval(1200);
  connect(&autosave_, &QTimer::timeout, this, [] { writeAutosave(); });

  auto focusText = [this] {
    // 文字を入力する欄はプロパティのビューにあるので、裏にいたり閉じていたりしたら前に出す
    perspective_->showView(QStringLiteral("properties"));
    inspector_->focusText();
  };
  connect(timeline_, &TimelineWidget::requestTextFocus, this, focusText);
  connect(preview_->stage(), &PreviewStage::requestTextFocus, this, focusText);

  qApp->installEventFilter(this);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ITaskbarList3* tb = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, reinterpret_cast<void**>(&tb)))) {
    tb->HrInit();
    taskbar_ = tb;
  }
  updateTitle();
  exportButton_->setEnabled(false);
}

MainWindow::~MainWindow() {
  qApp->removeEventFilter(this);
  if (taskbar_) static_cast<ITaskbarList3*>(taskbar_)->Release();
}

void MainWindow::buildMenus() {
  Editor& ed = editor();
  // ショートカットのキーは、最後に applyShortcuts() で keymap から付ける
  auto* file = menuBar()->addMenu(QStringLiteral("ファイル(&F)"));
  bindCommand(QStringLiteral("file.new"), file->addAction(QStringLiteral("新しい編集場所"), this, &MainWindow::newProject));
  bindCommand(QStringLiteral("file.open"), file->addAction(QStringLiteral("開く... (新しいタブ)"), this, &MainWindow::openProject));
  bindCommand(QStringLiteral("file.close"),
              file->addAction(QStringLiteral("この編集場所を閉じる"), this, [this] { closeWorkspace(editor().workspaceIndex()); }));
  file->addSeparator();
  bindCommand(QStringLiteral("file.save"), file->addAction(QStringLiteral("保存"), this, [this] { saveProject(false); }));
  bindCommand(QStringLiteral("file.saveAs"), file->addAction(QStringLiteral("名前を付けて保存..."), this, [this] { saveProject(true); }));
  file->addSeparator();
  bindCommand(QStringLiteral("file.import"), file->addAction(QStringLiteral("メディアを追加..."), this, [this] { LibraryPanel::pickAndImport(this); }));
  bindCommand(QStringLiteral("file.export"), file->addAction(QStringLiteral("書き出し..."), this, &MainWindow::openExport));
  file->addSeparator();
  bindCommand(QStringLiteral("file.quit"), file->addAction(QStringLiteral("終了"), this, &QWidget::close));

  auto* edit = menuBar()->addMenu(QStringLiteral("編集(&E)"));
  QAction* undo = edit->addAction(QStringLiteral("元に戻す"), this, [] { editor().undo(); });
  QAction* redo = edit->addAction(QStringLiteral("やり直し"), this, [] { editor().redo(); });
  bindCommand(QStringLiteral("edit.undo"), undo);
  bindCommand(QStringLiteral("edit.redo"), redo);
  edit->addSeparator();
  QAction* split = edit->addAction(QStringLiteral("分割"), this, [] { editor().splitAtPlayhead(); });
  split->setShortcutVisibleInContextMenu(true);
  bindCommand(QStringLiteral("edit.split"), split);
  QAction* dup = edit->addAction(QStringLiteral("複製"), this, [] { editor().duplicateSelection(); });
  bindCommand(QStringLiteral("edit.duplicate"), dup);
  QAction* del = edit->addAction(QStringLiteral("削除"), this, [] { editor().deleteSelection(); });
  bindCommand(QStringLiteral("edit.delete"), del);
  addHiddenCommand(QStringLiteral("edit.deselect"), [] { editor().select({}); }, [] { return bool(editor().selection()); });
  edit->addSeparator();
  // キーフレームのコピー・貼り付け（選んでいるキーフレームを、再生位置へ。値と補間方式は保たれる）
  QAction* copyKeys = edit->addAction(QStringLiteral("キーフレームをコピー"), this, [] {
    Editor& e = editor();
    std::vector<Editor::KeyRef> keys;
    if (e.animation()) {
      // 再生位置にあるキーフレーム（項目ごと）
      const double local = e.currentTime() - e.objectStart();
      for (int pi = 0; pi < kAnimPropCount; ++pi) {
        const AnimProp prop = static_cast<AnimProp>(pi);
        const int idx = findKey(e.animation()->keys(prop), local);
        if (idx >= 0) keys.push_back({prop, idx});
      }
    }
    if (e.copyKeys(keys)) e.toast(QStringLiteral("キーフレームをコピーしました"));
    else e.toast(QStringLiteral("コピーするキーフレームを選んでください"));
  });
  QAction* pasteKeys = edit->addAction(QStringLiteral("キーフレームを貼り付け"), this, [] {
    Editor& e = editor();
    const int n = e.pasteKeys(std::clamp(e.currentTime() - e.objectStart(), 0.0, e.objectDuration()));
    e.toast(n > 0 ? QStringLiteral("キーフレームを%1個貼り付けました").arg(n) : QStringLiteral("貼り付けられるキーフレームがありません"));
  });
  connect(&ed, &Editor::selectionChanged, this, [copyKeys, pasteKeys] {
    copyKeys->setEnabled(editor().animatable());
    pasteKeys->setEnabled(editor().animatable());
  });
  copyKeys->setEnabled(false);
  pasteKeys->setEnabled(false);
  bindCommand(QStringLiteral("edit.copyKeys"), copyKeys);
  bindCommand(QStringLiteral("edit.pasteKeys"), pasteKeys);
  edit->addSeparator();
  // テキストの段を変えるのは、移動や長さの変更とは別の、この明示的な操作だけ
  auto moveTextTrack = [](int delta) {
    const Selection s = editor().selection();
    if (s.type == Selection::Text) editor().moveTextTrack(s.id, delta);
  };
  QAction* trackUp = edit->addAction(QStringLiteral("テキストを上の段へ"), this, [moveTextTrack] { moveTextTrack(-1); });
  QAction* trackDown = edit->addAction(QStringLiteral("テキストを下の段へ"), this, [moveTextTrack] { moveTextTrack(1); });
  bindCommand(QStringLiteral("edit.textUp"), trackUp);
  bindCommand(QStringLiteral("edit.textDown"), trackDown);
  edit->addSeparator();
  bindCommand(QStringLiteral("edit.addText"), edit->addAction(QStringLiteral("テキストを追加"), this, [this] {
    editor().addText(QStringLiteral("basic"));
    perspective_->showView(QStringLiteral("properties"));
    inspector_->focusText();
  }));

  // 再生・コマ送り（メニューには出さない）
  addHiddenCommand(QStringLiteral("play.toggle"), [] { editor().togglePlay(); });
  auto seekBy = [](double sec, bool frames) {
    Editor& e = editor();
    e.pause();
    e.seek(e.currentTime() + (frames ? sec / e.project().settings.fps : sec));
  };
  addHiddenCommand(QStringLiteral("play.prevFrame"), [seekBy] { seekBy(-1, true); });
  addHiddenCommand(QStringLiteral("play.nextFrame"), [seekBy] { seekBy(1, true); });
  addHiddenCommand(QStringLiteral("play.back1s"), [seekBy] { seekBy(-1, false); });
  addHiddenCommand(QStringLiteral("play.forward1s"), [seekBy] { seekBy(1, false); });
  addHiddenCommand(QStringLiteral("play.start"), [] { editor().seek(0); });
  addHiddenCommand(QStringLiteral("play.end"), [] { editor().seek(editor().playEnd()); });
  auto refreshEdit = [undo, redo, dup, del, trackUp, trackDown] {
    undo->setEnabled(editor().canUndo());
    redo->setEnabled(editor().canRedo());
    dup->setEnabled(bool(editor().selection()));
    del->setEnabled(bool(editor().selection()));
    const bool isText = editor().selection().type == Selection::Text;
    trackUp->setEnabled(isText);
    trackDown->setEnabled(isText);
  };
  connect(&ed, &Editor::projectChanged, this, refreshEdit);
  connect(&ed, &Editor::selectionChanged, this, refreshEdit);
  refreshEdit();

  auto* view = menuBar()->addMenu(QStringLiteral("表示(&V)"));
  bindCommand(QStringLiteral("view.zoomIn"),
              view->addAction(QStringLiteral("タイムラインを拡大"), this, [] { editor().setPxPerSec(editor().pxPerSec() * 1.4); }));
  bindCommand(QStringLiteral("view.zoomOut"),
              view->addAction(QStringLiteral("タイムラインを縮小"), this, [] { editor().setPxPerSec(editor().pxPerSec() / 1.4); }));
  bindCommand(QStringLiteral("view.fitAll"), view->addAction(QStringLiteral("全体を表示"), this, [this] { timeline_->view()->fitAll(); }));
  view->addSeparator();
  bindCommand(QStringLiteral("view.fullScreen"),
              view->addAction(QStringLiteral("プレビューを全画面表示"), this, [this] { preview_->toggleFullScreen(); }));
  view->addSeparator();
  aiAction_ = view->addAction(QStringLiteral("AI アシスタント"), this, [this] { setAiVisible(!aiPanel_->isVisible()); });
  aiAction_->setCheckable(true);
  bindCommand(QStringLiteral("view.ai"), aiAction_);
  view->addSeparator();
  // 左右の欄の並び（Eclipse のパースペクティブのように、名前を付けて保存・切り替え・リセットができる）
  QMenu* perspectiveMenu = view->addMenu(QStringLiteral("パースペクティブ"));
  connect(perspectiveMenu, &QMenu::aboutToShow, this, [this, perspectiveMenu] { perspective_->fillPerspectiveMenu(perspectiveMenu); });
  perspective_->fillPerspectiveMenu(perspectiveMenu);
  QMenu* viewsMenu = view->addMenu(QStringLiteral("ビュー"));
  connect(viewsMenu, &QMenu::aboutToShow, this, [this, viewsMenu] { perspective_->fillViewMenu(viewsMenu); });
  perspective_->fillViewMenu(viewsMenu);
  view->addSeparator();
  bindCommand(QStringLiteral("view.nextTab"), view->addAction(QStringLiteral("次のタブ"), this, [] {
    const int n = editor().workspaceCount();
    editor().switchWorkspace((editor().workspaceIndex() + 1) % n);
  }));
  bindCommand(QStringLiteral("view.prevTab"), view->addAction(QStringLiteral("前のタブ"), this, [] {
    const int n = editor().workspaceCount();
    editor().switchWorkspace((editor().workspaceIndex() + n - 1) % n);
  }));

  auto* help = menuBar()->addMenu(QStringLiteral("ヘルプ(&H)"));
  bindCommand(QStringLiteral("help.shortcuts"), help->addAction(QStringLiteral("キーボードショートカット"), this, &MainWindow::showShortcuts));
  help->addAction(QStringLiteral("バージョン情報"), this, [this] {
    QMessageBox::about(this, QStringLiteral("バージョン情報"),
                       QStringLiteral("<b>かんたん動画エディタ</b><br>バージョン %1<br><br>Qt %2 / FFmpeg / miniaudio / Lucide")
                           .arg(QStringLiteral(KV_VERSION), QString::fromLatin1(qVersion())));
  });

  // 右上: 元に戻す・やり直し・書き出し
  auto* corner = new QWidget(menuBar());
  auto* ch = new QHBoxLayout(corner);
  ch->setContentsMargins(0, 3, 8, 3);
  ch->setSpacing(2);
  auto* undoBtn = makeToolButton(QStringLiteral("undo"), QStringLiteral("元に戻す (Ctrl+Z)"), corner);
  auto* redoBtn = makeToolButton(QStringLiteral("redo"), QStringLiteral("やり直し (Ctrl+Y)"), corner);
  undoButton_ = undoBtn;
  redoButton_ = redoBtn;
  exportButton_ = new QPushButton(theme::icon(QStringLiteral("download"), Qt::white), QStringLiteral("書き出し"), corner);
  exportButton_->setProperty("primary", true);
  exportButton_->setFocusPolicy(Qt::NoFocus);
  aiButton_ = makeToolButton(QStringLiteral("message-square"), QStringLiteral("AI アシスタント (Ctrl+Shift+A)"), corner, QStringLiteral("AI"));
  aiButton_->setCheckable(true);
  ch->addWidget(undoBtn);
  ch->addWidget(redoBtn);
  ch->addSpacing(8);
  ch->addWidget(aiButton_);
  ch->addSpacing(4);
  ch->addWidget(exportButton_);
  connect(aiButton_, &QToolButton::clicked, this, [this] { setAiVisible(!aiPanel_->isVisible()); });
  menuBar()->setCornerWidget(corner, Qt::TopRightCorner);
  connect(undoBtn, &QToolButton::clicked, this, [] { editor().undo(); });
  connect(redoBtn, &QToolButton::clicked, this, [] { editor().redo(); });
  connect(exportButton_, &QPushButton::clicked, this, &MainWindow::openExport);
  auto refreshCorner = [undoBtn, redoBtn] {
    undoBtn->setEnabled(editor().canUndo());
    redoBtn->setEnabled(editor().canRedo());
  };
  connect(&ed, &Editor::projectChanged, this, refreshCorner);
  refreshCorner();
  applyShortcuts();
}

void MainWindow::bindCommand(const QString& id, QAction* action, std::function<bool()> ready) {
  commands_.insert(id, {action, std::move(ready)});
}

void MainWindow::addHiddenCommand(const QString& id, std::function<void()> run, std::function<bool()> ready) {
  auto* a = new QAction(this);
  connect(a, &QAction::triggered, this, [run = std::move(run)] { run(); });
  addAction(a);  // メニューに無くても、このウィンドウでショートカットが効くように
  bindCommand(id, a, std::move(ready));
}

void MainWindow::applyShortcuts() {
  plainKeys_.clear();
  for (const keymap::Command& c : keymap::commands()) {
    const auto it = commands_.constFind(c.id);
    if (it == commands_.constEnd()) continue;
    // Ctrl・Alt を使うキーは QAction のショートカットに（メニューにも出る）。
    // 使わないキーは、文字の入力中には効かないように eventFilter で受け取る
    QList<QKeySequence> actionKeys;
    for (const QKeySequence& k : keymap::keys(c.id)) {
      if (keymap::isPlain(k)) plainKeys_.insert(k[0].toCombined(), c.id);
      else actionKeys << k;
    }
    it->action->setShortcuts(actionKeys);
  }
  auto tip = [](const QString& label, const QString& id) {
    const QString k = keymap::text(keymap::keys(id));
    return k.isEmpty() ? label : QStringLiteral("%1 (%2)").arg(label, k);
  };
  if (undoButton_) undoButton_->setToolTip(tip(QStringLiteral("元に戻す"), QStringLiteral("edit.undo")));
  if (redoButton_) redoButton_->setToolTip(tip(QStringLiteral("やり直し"), QStringLiteral("edit.redo")));
  if (aiButton_) aiButton_->setToolTip(tip(QStringLiteral("AI アシスタント"), QStringLiteral("view.ai")));
}

QString MainWindow::plainCommandFor(const QKeyEvent* e) const {
  const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
  if (mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return {};
  const Qt::Key key = Qt::Key(e->key());
  QString id = plainKeys_.value(QKeyCombination(mods, key).toCombined());
  // Shift 付きで割り当てが無ければ、Shift なしの割り当てを使う（Shift+S でも分割、"?" を Shift で打つキーボードなど）
  if (id.isEmpty() && (mods & Qt::ShiftModifier)) id = plainKeys_.value(QKeyCombination(mods & ~Qt::ShiftModifier, key).toCombined());
  return id;
}

bool MainWindow::runCommand(const QString& id) {
  const auto it = commands_.constFind(id);
  if (it == commands_.constEnd()) return false;
  QAction* a = it->action;
  if (!a->isEnabled() || !a->isVisible() || (it->ready && !it->ready())) return false;
  a->trigger();
  return true;
}

/** 開いているダイアログ（前回の続きを開くかの確認など）が閉じてから fn を行う */
static void whenNoModal(QObject* ctx, std::function<void()> fn) {
  if (!QApplication::activeModalWidget()) {
    fn();
    return;
  }
  QTimer::singleShot(300, ctx, [ctx, fn] { whenNoModal(ctx, fn); });
}

void MainWindow::startupLogin() {
  ai::Account& a = ai::account();
  // .env に AI サーバーのアドレスが無ければ、AI は使わない（ログインの画面も出さない）
  if (a.server().isEmpty()) return;
  if (!a.loadSaved()) {
    // ログインしていない（はじめて・ログアウトした）: 先にログインの画面を出す
    AccountDialog::run(this, AccountDialog::Page::Login, true);
    return;
  }
  // 前回のログインが残っていれば、そのまま使う。サーバーで確かめて、期限が切れていたらログインの画面を出す
  // （サーバーにつながらないときは、つながったら自動で使う）
  a.refresh(this, [this](ai::Account::Refresh result) {
    if (result == ai::Account::Refresh::Expired) {
      whenNoModal(this, [this] { AccountDialog::run(this, AccountDialog::Page::Login, true); });
    } else if (result == ai::Account::Refresh::LoggedIn && ai::account().info().mustChangePassword) {
      whenNoModal(this, [this] { AccountDialog::run(this, AccountDialog::Page::Password, true); });
    }
  });
}

void MainWindow::refreshAiAvailability() {
  aiAvailable_ = !ai::account().server().isEmpty();
  // 隠した項目は、ショートカット（Ctrl+Shift+A）でも出せない
  if (aiButton_) aiButton_->setVisible(aiAvailable_);
  if (aiAction_) aiAction_->setVisible(aiAvailable_);
  if (!aiAvailable_ && aiPanel_->isVisible()) {
    // 使っている途中で .env のアドレスが消えた。返事を待つのをやめて、欄をしまう
    aiPanel_->assistant()->cancel();
    setAiVisible(false);
    editor().toast(QStringLiteral(".env に AI サーバーのアドレス（KANTAN_AI_SERVER）が無くなったので、AI アシスタントを隠しました"), ToastKind::Info);
  }
}

void MainWindow::setAiVisible(bool on) {
  if (!aiAvailable_) on = false;
  if (aiButton_) aiButton_->setChecked(on);
  if (aiAction_) aiAction_->setChecked(on);
  if (on == aiPanel_->isVisible()) {
    if (on) aiPanel_->focusInput();
    return;
  }
  if (!on) {
    aiPanel_->hide();
    return;
  }
  // 出すときは、プレビューの幅を分けて 340px ほどにする
  QList<int> sizes = top_->sizes();
  aiPanel_->show();
  if (sizes.size() == 4) {
    const int want = 340;
    const int take = std::min(want, std::max(0, sizes[1] - 420));
    sizes[1] -= take;
    sizes[3] = std::max(take, aiPanel_->minimumWidth());
    top_->setSizes(sizes);
  }
  aiPanel_->focusInput();
}

void MainWindow::updateTitle() {
  const Editor& ed = editor();
  QString name = ed.workspaceName(ed.workspaceIndex());
  if (!ed.projectPath().isEmpty()) name = QFileInfo(ed.projectPath()).completeBaseName();
  setWindowTitle(QStringLiteral("%1%2 - かんたん動画エディタ").arg(name, ed.dirty() ? QStringLiteral(" *") : QString()));
}

void MainWindow::showToast(const QString& message, int kind) {
  const QString icon = kind == int(ToastKind::Error) ? QStringLiteral("alert") : kind == int(ToastKind::Success) ? QStringLiteral("check") : QString();
  const QColor color = kind == int(ToastKind::Error) ? theme::danger : theme::ok;
  if (icon.isEmpty()) toastIcon_->clear();
  else toastIcon_->setPixmap(theme::iconPixmap(icon, 14, color, devicePixelRatioF()));
  toastText_->setText(message);
  toastText_->setStyleSheet(kind == int(ToastKind::Error) ? QStringLiteral("color:#ff9a9e") : QStringLiteral("color:#e6e6e6"));
  toastTimer_.start(kind == int(ToastKind::Error) ? 8000 : 4000);
}

void MainWindow::setTaskbarProgress(double ratio, bool running) {
  auto* tb = static_cast<ITaskbarList3*>(taskbar_);
  if (!tb) return;
  const HWND hwnd = reinterpret_cast<HWND>(winId());
  if (!running) {
    tb->SetProgressState(hwnd, TBPF_NOPROGRESS);
    return;
  }
  tb->SetProgressState(hwnd, TBPF_NORMAL);
  tb->SetProgressValue(hwnd, ULONGLONG(ratio * 1000), 1000);
}

/** 保存していない変更があるタブについて、保存するか尋ねる。続けてよければ true。保存を選んで保存できなかったときは false */
bool MainWindow::confirmSaveWorkspace(int index) {
  Editor& ed = editor();
  if (!ed.workspaceDirty(index) || !ed.workspaceHasContent(index)) return true;
  // 何についての確認か分かるよう、そのタブを表示してから尋ねる
  // 確認を出している間に、別のウィンドウから開かれるなどでタブが増えたり切り替わったりしても、
  // 同じタブを保存・閉じられるよう、番号ではなく変わらない ID で覚えておく
  const int uid = ed.workspaceUid(index);
  ed.switchWorkspace(index);
  QMessageBox box(QMessageBox::Question, QStringLiteral("かんたん動画エディタ"),
                  QStringLiteral("「%1」の変更を保存しますか？").arg(ed.workspaceName(index)), QMessageBox::NoButton, this);
  QPushButton* save = box.addButton(QStringLiteral("保存"), QMessageBox::AcceptRole);
  QPushButton* dont = box.addButton(QStringLiteral("保存しない"), QMessageBox::DestructiveRole);
  box.addButton(QStringLiteral("キャンセル"), QMessageBox::RejectRole);
  box.setDefaultButton(save);
  box.exec();
  if (box.clickedButton() == save) {
    const int now = ed.indexOfWorkspaceUid(uid);
    if (now < 0) return false;
    ed.switchWorkspace(now);
    return saveProject(false);
  }
  return box.clickedButton() == dont;
}

void MainWindow::closeWorkspace(int index) {
  if (index < 0 || index >= editor().workspaceCount()) return;
  const int uid = editor().workspaceUid(index);
  if (!confirmSaveWorkspace(index)) return;
  // 確認のあとで番号が変わっていても、確認したタブを閉じる
  const int now = editor().indexOfWorkspaceUid(uid);
  if (now >= 0) editor().closeWorkspace(now);
}

void MainWindow::newProject() {
  // 今のタブはそのままにして、新しい空のタブを追加する
  editor().addWorkspace();
}

void MainWindow::openProject() {
  static QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("開く"), dir, QStringLiteral("プロジェクト ファイル (*.kvproj)"));
  if (path.isEmpty()) return;
  dir = QFileInfo(path).absolutePath();
  media().openProject(path);
}

bool MainWindow::saveProject(bool saveAs) {
  Editor& ed = editor();
  QString path = saveAs ? QString() : ed.projectPath();
  if (path.isEmpty()) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    // 保存先を選んでいる間に別のタブへ切り替わっても、選んだときのタブを保存する
    const int uid = ed.workspaceUid(ed.workspaceIndex());
    path = QFileDialog::getSaveFileName(this, QStringLiteral("名前を付けて保存"), dir + QLatin1Char('/') + ed.workspaceName(ed.workspaceIndex()) + QStringLiteral(".kvproj"),
                                        QStringLiteral("プロジェクト ファイル (*.kvproj)"));
    if (path.isEmpty()) return false;
    const int index = ed.indexOfWorkspaceUid(uid);
    if (index < 0) return false;
    ed.switchWorkspace(index);
    if (!path.endsWith(QStringLiteral(".kvproj"), Qt::CaseInsensitive)) path += QStringLiteral(".kvproj");
    // 別のタブが使っているファイルには上書きしない（2 つのタブが同じファイルを指さないように）
    const int other = ed.workspaceWithPath(path);
    if (other >= 0 && other != ed.workspaceIndex()) {
      ed.toast(QStringLiteral("そのファイルは「%1」で開いています。別の名前で保存してください").arg(ed.workspaceName(other)), ToastKind::Error);
      return false;
    }
    const QString name = QFileInfo(path).completeBaseName();
    if (name != ed.project().name) {
      ed.commit([&](ProjectData& p) {
        p.name = name;
        return true;
      });
    }
  }
  // 書き込みに失敗したときに、元のファイルが壊れないよう、できあがってから置き換える
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(serializeProject()) < 0 || !f.commit()) {
    ed.toast(QStringLiteral("保存できませんでした: %1").arg(f.errorString()), ToastKind::Error);
    return false;
  }
  ed.markSaved(path);
  updateTitle();
  ed.toast(QStringLiteral("保存しました"), ToastKind::Success);
  return true;
}

void MainWindow::openExport() {
  editor().pause();
  const ExportCheck check = checkExportable();
  if (!check.ok) {
    editor().toast(check.message, ToastKind::Error);
    return;
  }
  if (!exportDialog_) {
    exportDialog_ = new ExportDialog(this);
    connect(exportDialog_, &ExportDialog::progressChanged, this, &MainWindow::setTaskbarProgress);
  }
  exportDialog_->show();
  exportDialog_->raise();
}

void MainWindow::showShortcuts() {
  // AI アシスタントを使えないときは、その操作も載せない
  QStringList hidden;
  if (!aiAvailable_) hidden << QStringLiteral("view.ai");
  ShortcutsDialog dlg(hidden, this);
  connect(&dlg, &ShortcutsDialog::changed, this, &MainWindow::applyShortcuts);
  dlg.exec();
}

void MainWindow::offerRestore() {
  auto file = readAutosave();
  if (!file) return;
  QMessageBox box(QMessageBox::Question, QStringLiteral("かんたん動画エディタ"), QStringLiteral("前回の編集内容が残っています。復元しますか？"),
                  QMessageBox::NoButton, this);
  QPushButton* restore = box.addButton(QStringLiteral("復元"), QMessageBox::AcceptRole);
  box.addButton(QStringLiteral("破棄"), QMessageBox::DestructiveRole);
  box.setDefaultButton(restore);
  box.exec();
  if (box.clickedButton() == restore) {
    media().loadSession(*file);
  } else {
    clearAutosave();
  }
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (exportDialog_ && exportDialog_->isRunning()) {
    const auto r = QMessageBox::question(this, QStringLiteral("かんたん動画エディタ"), QStringLiteral("書き出し中です。中止して終了しますか？"));
    if (r != QMessageBox::Yes) {
      e->ignore();
      return;
    }
  }
  // 保存していない変更があるタブを順に確認する。途中でキャンセルしたら終了しない
  Editor& ed = editor();
  for (int i = 0; i < ed.workspaceCount(); ++i) {
    if (!confirmSaveWorkspace(i)) {
      e->ignore();
      return;
    }
  }
  // 全部のタブについて保存するかどうかを決めたので、前回の編集内容の自動保存は残さない
  clearAutosave();
  ed.pause();
  // 左右の欄の並びと幅を、次に起動したときのために残す
  perspective_->storeNow();
  e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (!e->mimeData()->hasUrls()) return;
  e->acceptProposedAction();
  dropOverlay_->setGeometry(rect());
  dropOverlay_->raise();
  dropOverlay_->show();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent*) {
  dropOverlay_->hide();
}

void MainWindow::dropEvent(QDropEvent* e) {
  dropOverlay_->hide();
  QStringList paths;
  for (const QUrl& u : e->mimeData()->urls()) {
    if (u.isLocalFile()) paths << u.toLocalFile();
  }
  if (paths.isEmpty()) return;
  e->acceptProposedAction();
  media().importPaths(paths, true);
}

static bool isTextInput(QObject* o) {
  // プレビュー上で文字を直接編集している間も、文字の入力欄と同じ（Space・Delete・S・T などを、編集の操作に使わない）
  if (auto* stage = qobject_cast<PreviewStage*>(o)) return stage->isEditingText();
  return qobject_cast<QLineEdit*>(o) || qobject_cast<QPlainTextEdit*>(o) || qobject_cast<QTextEdit*>(o) || qobject_cast<QAbstractSpinBox*>(o) ||
         (qobject_cast<QComboBox*>(o) && static_cast<QComboBox*>(o)->isEditable());
}

/** 数値の部品（SliderRow・NumberSpin、その中の部品）を選んでいるか */
static bool isNumericFocus(const QWidget* w) {
  for (; w; w = w->parentWidget()) {
    if (w->property("kvNumeric").toBool()) return true;
  }
  return false;
}

bool MainWindow::handleKey(QKeyEvent* e) {
  // Ctrl・Alt を使わないキー（既定は Space・S・T・←→・Home・End・Delete・Esc・? など）。割り当ては keymap で変えられる
  const QString id = plainCommandFor(e);
  return !id.isEmpty() && runCommand(id);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* e) {
  if (e->type() == QEvent::KeyPress || e->type() == QEvent::ShortcutOverride) {
    // このウィンドウの中だけで、文字入力欄以外にフォーカスがあるときに使う
    QWidget* w = qobject_cast<QWidget*>(obj);
    if (!w || w->window() != this || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
    auto* ke = static_cast<QKeyEvent*>(e);
    // 数値の欄（スライダーの数値・数値入力欄）を選んでいるときは、矢印キー（Shift・Alt つきも）は数値の調整に使う。
    // タイムラインのコマ送りや、メニューの Alt+↑↓ のショートカットより先に、その欄へ渡す
    if (isNumericFocus(QApplication::focusWidget())) {
      const int k = ke->key();
      if ((k == Qt::Key_Left || k == Qt::Key_Right || k == Qt::Key_Up || k == Qt::Key_Down) && !(ke->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) {
        if (e->type() == QEvent::ShortcutOverride) e->accept();
        return false;
      }
    }
    // タイムラインでクリップなどをドラッグしている最中の Esc は、ドラッグをやめて元の位置・長さに戻す
    if (ke->key() == Qt::Key_Escape && !(ke->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && timeline_->view()->isDragging()) {
      if (e->type() == QEvent::ShortcutOverride) {
        e->accept();
        return false;
      }
      timeline_->view()->cancelDrag();
      return true;
    }
    if (isTextInput(QApplication::focusWidget())) return false;
    if (e->type() == QEvent::ShortcutOverride) {
      // 矢印キーなどをスライダーより先に受け取る
      if (!plainCommandFor(ke).isEmpty()) e->accept();
      return false;
    }
    if (obj != QApplication::focusWidget() && QApplication::focusWidget()) return false;
    if (handleKey(ke)) return true;
  }
  return QMainWindow::eventFilter(obj, e);
}

}  // namespace kv
