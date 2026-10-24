# Verification: no wheel changes on the text font / size controls

Iteration 1. No `review.json` existed, so the change was implemented from scratch, following plan.md.
Nothing is committed or pushed.

## Changed files

`git -C c:\git\JPHACK\e-editor status --short`:
```
 M native/src/ui/inspector.cpp
 M native/src/ui/widgets.cpp
 M native/src/ui/widgets.h
?? .agents/
```
`git diff --numstat` shows 3, 26 and 4 lines added and none removed. `git diff --check` is clean. The diff has no printf, qDebug, qWarning or QTest code. The files stay UTF-8 without BOM with CRLF line endings.

- `native/src/ui/widgets.h`: declares `SliderRow::disableWheel()` and the free function `kv::disableWheel(QWidget*)`, both with Japanese doc comments.
- `native/src/ui/widgets.cpp`:
  - Adds `#include <QEvent>`.
  - Adds `WheelBlocker`, an event filter in an anonymous namespace. For `QEvent::Wheel` it calls `e->ignore()` and returns true, so the widget's own wheel handler never runs and `QApplication::notify` passes the event up to the parents. Other events pass through untouched.
  - Adds `disableWheel(QWidget*)`. It installs the filter, parented to the widget, and changes `Qt::WheelFocus` to `Qt::StrongFocus`. Tab and click focus stay; only focus-by-wheel is removed.
  - Adds `SliderRow::disableWheel()`, which applies the same to the private slider.
- `native/src/ui/inspector.cpp` (TextPanel, section 「フォント」): three lines right after `size_` is created: `disableWheel(font_);` and `size_->disableWheel();`, with a comment.

The change is opt-in. `SliderRow`'s constructor and every other use of it are unchanged.

## Target controls
- `font_`: the `QComboBox` 「フォント」 (inspector.cpp:418).
- `size_`: the `SliderRow` 「サイズ」 (inspector.cpp:426). This is its internal `QSlider`.

## Controls that still react to the wheel (intentionally unchanged)
- `SliderRow` (QAbstractSlider wheel):
  - ClipPanel: 表示時間 :154, 音量 :167, 明るさ :212, コントラスト :214, 彩度 :216, 色温度 :218
  - TextPanel: 縁取りの太さ :445, 背景の不透明度 :449
  - AudioPanel: 音量 :615
- `SecondsField` (QDoubleSpinBox, which keeps WheelFocus):
  - TextPanel: 開始 :492, 長さ :493
  - AudioPanel: 開始 :627
- Elsewhere: the timeline zoom slider (timelinewidget.cpp:886) and the メディア / テキスト tab bar (librarypanel.cpp:200).
- Intended wheel use is unchanged: timeline scroll and zoom, scroll areas and lists.

Line numbers are for the modified files.

## Build
1. `powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1 -NoDeploy`: exit 0.
   - Ninja rebuilt widgets.cpp, inspector.cpp, the other files that include widgets.h (exportdialog, previewwidget, timelinewidget, mainwindow) and mocs, then linked `native\build\kantan-video-editor.exe`.
   - The output had no warnings or errors. The app target builds with `-Wall`.
2. `powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1`: compile printed `ninja: no work to do`, then deploy stopped with exit 1:
   `アプリが起動したままなので上書きできません。閉じてからもう一度実行してください: C:\git\JPHACK\e-editor\native\dist\kantan-video-editor\kantan-video-editor.exe`.
   - The app was already running (PID 23992, started 2026-10-21 11:14:36, before this step). Per the plan it was not killed.
   - So `native\dist` still has the old exe. The fixed exe is `native\build\kantan-video-editor.exe`. The user needs to close the app and run `build.cmd`.

## Harness (temporary, outside the repo, now deleted)
Location: `C:\kantan-dev\tmp\kv-wheelcheck\`, built from Appendix A of plan.md. It compiles every app source except main.cpp straight from the repo, with Qt6::Test. It shows an `Inspector` with a text item selected, then sends:
- direct `QWheelEvent`s through `sendEvent`, checking accepted/ignored and the values;
- OS-path (spontaneous) wheel events through `qt_handleWheelEvent`, which go through `QGuiApplication` → `QWidgetWindow` → `QApplication::notify` propagation;
- mouse press → move → release, groove clicks, double-clicks and keys.

Harness corrections (no expectation was changed):
1. Added `#include <QAbstractItemView>`. The first build failed because `font->view()` was an incomplete type.
2. `realWheel()` now calls `qt_handleWheelEvent` directly, with the position multiplied by the window's devicePixelRatio, and adds `#include <QWindow>` and a `note: device pixel ratio` line.
   - Reason: in Qt 6.8.3, `qt_handleWheelEvent`, which `QTest::wheelEvent` uses, passes the logical position to `QWindowSystemInterface::handleWheelEvent`. That function treats it as native pixels and divides by the scale factor.
   - This machine runs at 125% (DPR 1.25), so the wheel landed up and to the left of the control. On the unmodified code the five "real wheel" checks therefore passed falsely.
   - With the correction, the baseline matched the plan's expectation exactly.
3. After the baseline, three extra non-wheel checks were added (`extra:` lines). They ran only on the fixed code.

Baseline on the unmodified code: exit 1, the 12 FAILs the plan predicts.
```
note: device pixel ratio 1.25
OK   setup: text panel controls found
FAIL font: focus policy is StrongFocus (wheel does not take focus)
FAIL font: direct wheel event is ignored
FAIL font: direct wheel does not change the font
OK   font: precondition - the panel can scroll further
FAIL font: real wheel over the combo scrolls the panel
FAIL font: real wheel does not change the font
FAIL font: real wheel does not move focus to the combo
OK   font: click opens the list
OK   font: choosing from the list changes the font
FAIL font: wheel after clicking (focused) is still ignored
FAIL size: direct wheel events are ignored
FAIL size: direct wheel does not change the size
OK   size: precondition - the panel can scroll further
FAIL size: real wheel over the slider scrolls the panel
FAIL size: real wheel does not change the size
OK   size: dragging the handle changes the size
FAIL size: wheel while dragging is ignored
OK   size: clicking the groove still moves the slider
OK   size: arrow key still changes the size
OK   scope: stroke-width slider still reacts to the wheel
RESULT: 12 check(s) FAILED
```

Fixed code, final run: exit 0. The same 21 checks also all passed in a run before the extra checks were added.
```
note: device pixel ratio 1.25
OK   setup: text panel controls found
OK   font: focus policy is StrongFocus (wheel does not take focus)
OK   font: direct wheel event is ignored
OK   font: direct wheel does not change the font
OK   font: precondition - the panel can scroll further
OK   font: real wheel over the combo scrolls the panel
OK   font: real wheel does not change the font
OK   font: real wheel does not move focus to the combo
OK   font: click opens the list
OK   font: choosing from the list changes the font
OK   font: wheel after clicking (focused) is still ignored
OK   size: direct wheel events are ignored
OK   size: direct wheel does not change the size
OK   size: precondition - the panel can scroll further
OK   size: real wheel over the slider scrolls the panel
OK   size: real wheel does not change the size
OK   size: dragging the handle changes the size
OK   size: wheel while dragging is ignored
OK   size: clicking the groove still moves the slider
OK   size: arrow key still changes the size
OK   extra: size slider focus policy is still TabFocus
OK   extra: arrow key on the focused combo still changes the font
OK   extra: double-click on the size slider still resets it
OK   scope: stroke-width slider still reacts to the wheel
RESULT: ALL OK
```

Cleanup:
- `Remove-Item -Recurse -Force -LiteralPath 'C:\kantan-dev\tmp\kv-wheelcheck'`; afterwards `Test-Path` returns False.
- No harness leftovers in `%LOCALAPPDATA%`, `%APPDATA%` or `HKCU\Software`. The harness never creates a MainWindow, so it never writes an autosave.
- No throwaway target was added to the repo's CMake, and no repo file was touched by the harness.

## Not verified
- No physical mouse wheel or touchpad was used on the real app. I cannot operate the GUI. The harness events enter Qt at `QWindowSystemInterface`, which is the same path real input takes after the Windows backend. The Windows `WM_MOUSEWHEEL` handling itself was not exercised.
- `native\dist` was not refreshed, because the running app blocked the deploy (see Build).
- Only 125% display scaling was tested. The fix itself doesn't depend on DPI.
- Not separately tested:
  - Touchpad pixel-delta scrolling. It arrives as the same `QEvent::Wheel`, so the filter covers it.
  - Wheel inside the opened font list. The filter sits on the combo only, not on its popup view.
- The three `extra:` checks were not run against the unmodified code.

## 手動での確認手順（GUI）
1. アプリを閉じてから `build.cmd` をダブルクリックします（今回はアプリが起動したままだったので `native\dist` はまだ古いままです）。
2. 「テキスト」ボタン（または T キー）でテキストを追加し、右側にテキストの設定を出します。
3. 「フォント」の欄の上でホイールを回します。フォントは変わらず、右側のパネルがスクロールします。欄が青い枠にもなりません。
4. フォントの欄をクリックすると一覧が開き、選んだフォントに変わります。そのあと欄の上でホイールを回しても変わりません。
5. 「サイズ」のスライダーの上でホイールを回します。値は変わらず、パネルがスクロールします。つまみをドラッグすると値とプレビューの文字の大きさが変わります。ドラッグしたあとにホイールを回しても変わりません。
6. Tab キーでサイズのスライダーに移ると、↑ ↓ キーで値が変わります（← → は今までどおり再生位置の移動です）。
7. 比べるために「縁取りの太さ」のスライダーの上でホイールを回します。今回は対象外なので、今までどおり値が変わります。
