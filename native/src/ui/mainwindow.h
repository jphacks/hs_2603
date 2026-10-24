#pragma once

#include <QMainWindow>
#include <QTimer>

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
class LibraryPanel;
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
  PreviewPanel* preview_;
  TimelineWidget* timeline_;
  Inspector* inspector_;
  LibraryPanel* library_;
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
