#pragma once
// 一時的: 動作確認（確認が終わったら消す）

#include <QString>

namespace kv {

class MainWindow;

void prepareUiTest(const QString& outDir, const QString& mode);
void runUiTest(MainWindow* window, const QString& outDir, const QString& mode);

}  // namespace kv
