#pragma once

#include <QHash>
#include <QMainWindow>
#include <QTimer>

#include <functional>

class QAction;
class QLabel;
class QPushButton;
class QSplitter;
class QToolButton;

namespace kv {

class PreviewEngine;
class PreviewPanel;
class TimelineWidget;
class Inspector;
class PerspectiveManager;
class ExportDialog;
class AiPanel;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow();
  ~MainWindow() override;

  void newProject();
  void openProject();
  bool saveProject(bool saveAs);
  void openExport();
  void showShortcuts();
  void offerRestore();
  /**
   * 起動したときのログイン。前回のログインが残っていればそのまま使い（期限が切れていたらログインの画面を出す）、
   * 残っていなければ、先にログインの画面を出す（「ログインしないで使う」で閉じられる）
   */
  void startupLogin();

 protected:
  void closeEvent(QCloseEvent*) override;
  void dragEnterEvent(QDragEnterEvent*) override;
  void dragLeaveEvent(QDragLeaveEvent*) override;
  void dropEvent(QDropEvent*) override;
  bool eventFilter(QObject* obj, QEvent* e) override;

 private:
  void buildMenus();
  void updateTitle();
  void showToast(const QString& message, int kind);
  void setTaskbarProgress(double ratio, bool running);
  bool handleKey(QKeyEvent* e);
  /** キーボードショートカットで動かす操作（keymap の id）と、その QAction を結び付ける。ready が false の間はキーを横取りしない */
  void bindCommand(const QString& id, QAction* action, std::function<bool()> ready = {});
  /** メニューに出さない操作（再生・コマ送りなど）を、ショートカット用の QAction として足す */
  void addHiddenCommand(const QString& id, std::function<void()> run, std::function<bool()> ready = {});
  /** keymap の割り当てを、メニューの QAction と、Ctrl・Alt を使わないキーの表に反映する */
  void applyShortcuts();
  /** Ctrl・Alt を使わないキーで動かす操作の id（無ければ空） */
  QString plainCommandFor(const QKeyEvent* e) const;
  /** 操作を行う。いま行えなければ false（キーはほかの部品に渡す） */
  bool runCommand(const QString& id);
  /** 保存していない変更があれば保存するか尋ねる。続けてよければ true */
  bool confirmSaveWorkspace(int index);
  void closeWorkspace(int index);
  /** AI アシスタントの欄を出す・しまう（ログインしていなければ、欄にログインの案内が出る） */
  void setAiVisible(bool on);
  /** AI サーバーのアドレス（.env の KANTAN_AI_SERVER）が決まっていなければ、AI アシスタントを隠す */
  void refreshAiAvailability();

  PreviewEngine* engine_;
  AiPanel* aiPanel_;
  QSplitter* top_;
  QToolButton* aiButton_ = nullptr;
  QAction* aiAction_ = nullptr;
  bool aiAvailable_ = false;
  struct BoundCommand {
    QAction* action = nullptr;
    std::function<bool()> ready;
  };
  QHash<QString, BoundCommand> commands_;
  /** Ctrl・Alt を使わないキー（QKeyCombination::toCombined()）→ 操作の id */
  QHash<int, QString> plainKeys_;
  QToolButton* undoButton_ = nullptr;
  QToolButton* redoButton_ = nullptr;
  PreviewPanel* preview_;
  TimelineWidget* timeline_;
  Inspector* inspector_;
  PerspectiveManager* perspective_ = nullptr;
  ExportDialog* exportDialog_ = nullptr;
  QPushButton* exportButton_;
  QLabel* toastIcon_;
  QLabel* toastText_;
  QTimer toastTimer_;
  QTimer autosave_;
  QWidget* dropOverlay_;
  void* taskbar_ = nullptr;
};

}  // namespace kv
