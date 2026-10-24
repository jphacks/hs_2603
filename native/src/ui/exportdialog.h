#pragma once

#include <QDialog>

class QLabel;
class QProgressBar;
class QStackedWidget;
class QPlainTextEdit;

namespace kv {

class Exporter;
class Segmented;

class ExportDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ExportDialog(QWidget* parent = nullptr);
  /** 保存先を指定して、すぐ書き出す（確認用） */
  void startWithPath(const QString& path);
  bool isRunning() const;

 signals:
  void progressChanged(double ratio, bool running);

 protected:
  void reject() override;
  /** 開くたびに、今の編集場所の長さや解像度を表示し直す */
  void showEvent(QShowEvent* e) override;

 private:
  void refreshSummary();
  void begin(const QString& path);
  /** 範囲を決めてあって、「指定した範囲」を選んでいるか */
  bool rangeSelected() const;
  QStackedWidget* pages_;
  /** 書き出す部分（全体 / 指定した範囲）。範囲を決めてあるときだけ出す */
  QWidget* partBox_;
  Segmented* part_;
  QLabel* partNote_;
  Segmented* resolution_;
  Segmented* quality_;
  QLabel* summary_[3];
  QProgressBar* progress_;
  QLabel* eta_;
  QLabel* pct_;
  QLabel* donePath_;
  QLabel* errorDetail_;
  QString result_;
  Exporter* exporter_;
};

}  // namespace kv
