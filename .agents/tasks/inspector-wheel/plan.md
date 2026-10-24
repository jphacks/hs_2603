# Implementation Plan: no wheel changes on the text font / size controls

User request: 「フォントや文字のサイズを変更する際、押したらスクロールで変更できるようになっているのでそれをなくしてほしい（＝マウスでつかんで変更するだけでいい）一旦それだけ直して」「言語は C++ で」.
The fix is C++ only (Qt 6.8.3 Widgets). Work directly in `c:\git\JPHACK\e-editor`. Never commit or push. Add nothing test-related to the repository.

## 1. Findings

### Target controls (TextPanel, section 「フォント」, native/src/ui/inspector.cpp)

| Line | Member | Widget type | Edits | Wheel path today |
| --- | --- | --- | --- | --- |
| 418 | `font_` | `QComboBox` (7 items from `fontOptions()`) | `TextItem::font` via `QComboBox::activated` (line 516) | `QComboBox::wheelEvent` |
| 426 | `size_` | `SliderRow` → internal `ResetSlider : QSlider` (widgets.cpp:95-105, 122-127) | `TextItem::size` via `SliderRow::changed` (line 520) | `QAbstractSlider::wheelEvent` |

### Root cause
- No app code handles the wheel on these controls. The only wheel code is `TimelineView::wheelEvent` (timelinewidget.cpp:799). `MainWindow::eventFilter` (mainwindow.cpp:477) only handles key events. So Qt's default handlers run.
- `QAbstractSlider::wheelEvent` (Qt 6.8.3) changes the value by `wheelScrollLines() × singleStep` (3 steps per notch) and accepts the event. It does not check focus. SliderRow sets `Qt::TabFocus` (widgets.cpp:125), so clicking the slider doesn't even give it focus. The wheel changes the size whenever the cursor is over the slider, including during or right after a drag.
- `QComboBox::wheelEvent` moves to the previous/next item and emits `activated` (so the inspector writes the new font into the project) whenever `SH_ComboBox_AllowWheelScrolling` is true and the popup is closed. It accepts the event. `QCommonStyle` returns true and Fusion and `QStyleSheetStyle` don't override it. The app uses Fusion plus an app stylesheet (theme.cpp:116-139).
- `QComboBoxPrivate::init()` sets `Qt::WheelFocus` on non-macOS, so the wheel also moves focus to the combo. The stylesheet then shows the blue `QComboBox:focus` border, and focus is taken from the text box.
- Because both handlers accept the event, `QApplication::notify` stops propagating it. The inspector `QScrollArea` (inspector.cpp:687) does not scroll while the cursor is over these controls.
- What 「押したら」 means: the user does not have to click first. The value changes on any wheel turn while the cursor is over the control. The user most likely noticed it right after clicking or dragging, because the cursor was still on the control when they turned the wheel to scroll the panel.
- Sources (Qt v6.8.3): [qabstractslider.cpp](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/widgets/widgets/qabstractslider.cpp), [qcombobox.cpp](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/widgets/widgets/qcombobox.cpp), [qcommonstyle.cpp](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/widgets/styles/qcommonstyle.cpp), [qapplication.cpp](https://raw.githubusercontent.com/qt/qtbase/v6.8.3/src/widgets/kernel/qapplication.cpp). `notify()` re-sends an ignored spontaneous wheel event to each parent in turn. Synthetic (`sendEvent`) wheel events are not propagated. The harness relies on both facts.

### Other controls that keep the wheel behavior after this fix (for the final report)
`SliderRow` (`QAbstractSlider::wheelEvent`):
- ClipPanel: inspector.cpp:154 表示時間 `duration_`, :167 音量 `volume_`, :212 明るさ, :214 コントラスト, :216 彩度, :218 色温度
- TextPanel: :442 縁取りの太さ `strokeWidth_`, :446 背景の不透明度 `boxOpacity_`
- AudioPanel: :612 音量 `volume_`

`SecondsField` → `QDoubleSpinBox` (widgets.cpp:245). Default `Qt::WheelFocus` and `QAbstractSpinBox::wheelEvent`:
- TextPanel: :489 開始, :490 長さ
- AudioPanel: :624 開始

Elsewhere:
- timelinewidget.cpp:886 `zoom_` QSlider (タイムラインの拡大率). The wheel changes the zoom even though it has `Qt::NoFocus`.
- librarypanel.cpp:200 `QTabWidget` tab bar (メディア / テキスト). The wheel switches tabs (`SH_TabBar_AllowWheelScrolling` = true).

These have no wheel behavior: Segmented, QCheckBox, ColorSwatches, QPushButton/QToolButton. Intended wheel use stays as is: TimelineView scroll/zoom, scroll areas, lists.

## 2. Decisions
- D1, scope: 「フォントや文字のサイズ」 means the font selection `font_` plus the text size `size_`. These are the two value controls in the inspector section titled 「フォント」. The bold and shadow checkboxes in that section have no wheel behavior.
- D2, other text-related size controls are excluded:
  - 縁取りの太さ `strokeWidth_` (inspector.cpp:442) is the outline thickness, not the character size. It sits in the 「色」 section, and the user said 「一旦それだけ」.
  - 背景の不透明度 `boxOpacity_` (:446) is not a size.
  - There is no line-spacing or letter-spacing control.
  - Both are listed above for the final report.
- D3, method: an opt-in event filter. Add `void disableWheel(QWidget* w)` to ui/widgets, and `SliderRow::disableWheel()` that applies it to the private slider. Call both only for `font_` and `size_`.
  - The filter calls `e->ignore()` and returns true for `QEvent::Wheel`. The control's own wheel handler never runs, and `notify()` passes the event up to the inspector's QScrollArea, which scrolls.
  - One mechanism covers both widget types. It needs no new subclass for the combo, does not change the shared SliderRow for its other 9 uses, and does not expose `slider_`. It follows the widgets.h small-helper style (`makeLabel`, `makeToolButton`).
  - Only `QEvent::Wheel` is touched. Press, drag, release, groove clicks, double-click reset, the combo popup and keys are unchanged.
- D4, wheel focus: in the same helper, change `Qt::WheelFocus` to `Qt::StrongFocus` (`StrongFocus` = Tab + click, so only the wheel bit is removed). This affects only the combo, because the slider is `TabFocus`.
  - Without it, scrolling over the font box would still focus it, show the blue focus border, and take focus from the text box. That is still wheel interaction with the control, which the user asked to remove. Tab and click focus, and arrow keys once focused, are unchanged.

## 3. Build procedure (verified)
- Toolchain: present at `C:\kantan-dev`. The user folder name has Japanese characters, so tools/common.ps1 chooses `C:\kantan-dev`. Qt 6.8.3 mingw_64, MinGW 13.1, FFmpeg, CMake and Ninja were checked, and Qt6Test exists.
- `native\build` is already configured, and `kantan-configure.txt` matches build.ps1's signature, so builds are incremental. The repo path is ASCII, so build.ps1 builds in place without the robocopy copy.
- Compile only:
  `powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1 -NoDeploy`
  Success means exit code 0 and no errors.
- Build and deploy without launching:
  `powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1`
  Success prints `できあがり: ...\native\dist\kantan-video-editor\kantan-video-editor.exe`. If it says the app is still running (アプリが起動したまま), do not kill the process. Report it so the user can close the app.
- `build.cmd`, the user's double-click option, runs the same script with `-Run`.
- Shell note: the execute_pwsh shell is persistent and ignored the `cwd` parameter after an earlier `cd`. Always use absolute paths and `git -C c:\git\JPHACK\e-editor`.
- Files are UTF-8 without BOM. Working-tree line endings are CRLF and the index is LF. Edit with str_replace and keep the existing line endings.

## 4. Plan

- [ ] 1. Create the throwaway harness outside the repository and record a baseline against the unmodified code.
      Folder: `C:\kantan-dev\tmp\kv-wheelcheck\`. The path must be ASCII because MinGW fails on the user's Japanese %TEMP%.
      Write the three files from Appendix A: `CMakeLists.txt`, `wheelcheck.cpp` (UTF-8, no BOM) and `run.ps1` (ASCII only).
      Do not touch any repository file in this step.
      Files: C:\kantan-dev\tmp\kv-wheelcheck\CMakeLists.txt, wheelcheck.cpp, run.ps1 (temporary)
      Verify: `powershell -NoProfile -ExecutionPolicy Bypass -File C:\kantan-dev\tmp\kv-wheelcheck\run.ps1` (timeout ≥ 20 min; the first build compiles all app sources). A small window shows for about a second.
      Expected baseline exit code: 1.
      - FAIL: `font: focus policy...`, `font: direct wheel event is ignored`, `font: direct wheel does not change...`, `font: real wheel ... scrolls the panel`, `font: real wheel does not change...`, `font: real wheel does not move focus...`, `font: wheel after clicking...`, `size: direct wheel events are ignored`, `size: direct wheel does not change...`, `size: real wheel ... scrolls the panel`, `size: real wheel does not change...`, `size: wheel while dragging...`
      - OK: setup, both `precondition` lines, `font: click opens the list`, `font: choosing from the list...`, `size: dragging...`, `size: clicking the groove...`, `size: arrow key...`, `scope: ...`
      If a line expected OK fails here, the harness is wrong. Fix the harness, not the app, until the baseline matches.

- [ ] 2. Add the opt-in wheel blocker to the shared widgets and apply it to the font and size controls (code in Appendix B).
      - widgets.h: declare `SliderRow::disableWheel()` after `setLabelVisible` (line 52) and the free function `disableWheel(QWidget*)` after `makeToolButton` (line 116).
      - widgets.cpp: add `#include <QEvent>`. Define `WheelBlocker` (anonymous namespace) and `disableWheel()` right after `makeToolButton()`, before the `// ---- Segmented` header. Define `SliderRow::disableWheel()` after `SliderRow::setLabelVisible` (line 150-152).
      - inspector.cpp: insert three lines right after `size_ = new SliderRow(...)` (line 426).
      Change nothing else.
      Files: native/src/ui/widgets.h, native/src/ui/widgets.cpp, native/src/ui/inspector.cpp
      Verify: `powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1 -NoDeploy` exits with 0 and shows no new warnings for widgets.cpp or inspector.cpp.

- [ ] 3. Re-run the harness on the fixed code. Depends on 2.
      Verify: `powershell -NoProfile -ExecutionPolicy Bypass -File C:\kantan-dev\tmp\kv-wheelcheck\run.ps1`. Every line prints `OK`, the last line is `RESULT: ALL OK` and the exit code is 0. Save the full output for the final report.
      If something fails, fix the app code in the three files and repeat 2 and 3. Never change the harness's expectations to make it pass.

- [ ] 4. Build and deploy the real app so the user can check it by hand. Depends on 3.
      Verify: `powershell -NoProfile -ExecutionPolicy Bypass -File c:\git\JPHACK\e-editor\native\build.ps1` prints `できあがり: ...kantan-video-editor.exe`. If the app is running, report it and leave the process alone.

- [ ] 5. Remove all temporary material and confirm the working tree.
      Run `Remove-Item -Recurse -Force -LiteralPath 'C:\kantan-dev\tmp\kv-wheelcheck'`. Delete only this folder, which step 1 created.
      Verify:
      - `Test-Path 'C:\kantan-dev\tmp\kv-wheelcheck'` prints False.
      - `git -C c:\git\JPHACK\e-editor status --short` shows exactly ` M native/src/ui/inspector.cpp`, ` M native/src/ui/widgets.cpp` and ` M native/src/ui/widgets.h`, plus the workflow's own `?? .agents/`.
      - `git -C c:\git\JPHACK\e-editor diff` contains only the Appendix B changes: no printf, qDebug or harness code. Nothing is committed.

## 5. Manual GUI check for the user (put this in the final report, in Japanese)
1. アプリを閉じてから `build.cmd` をダブルクリックします。手順 4 のあとなら `native\dist\kantan-video-editor\kantan-video-editor.exe` を開くだけでも確かめられます。
2. 「テキスト」ボタン（または T キー）でテキストを追加し、右側にテキストの設定を出します。
3. 「フォント」の欄の上でホイールを回します。フォントは変わらず、右側のパネルがスクロールします。欄が青い枠にもなりません。
4. フォントの欄をクリックすると一覧が開き、選んだフォントに変わります。そのあと欄の上でホイールを回しても変わりません。
5. 「サイズ」のスライダーの上でホイールを回します。値は変わらず、パネルがスクロールします。つまみをドラッグすると値とプレビューの文字の大きさが変わります。ドラッグしたあとにホイールを回しても変わりません。
6. Tab キーでサイズのスライダーに移ると、↑ ↓ キーで値が変わります（← → は今までどおり再生位置の移動です）。
7. 比べるために「縁取りの太さ」のスライダーの上でホイールを回します。今回は対象外なので、今までどおり値が変わります。

---

## Appendix A: temporary harness (outside the repo, deleted in step 5)

`C:\kantan-dev\tmp\kv-wheelcheck\CMakeLists.txt`:
```cmake
# Temporary harness for the wheel fix. Delete this folder when done.
cmake_minimum_required(VERSION 3.21)
project(KvWheelCheck LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Svg Network Test)

set(KV "C:/git/JPHACK/e-editor/native")
set(FFMPEG_DIR "C:/kantan-dev/ffmpeg")
add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_DIR}/include")
foreach(lib avformat avcodec avutil swscale swresample avfilter)
  target_link_libraries(ffmpeg INTERFACE "${FFMPEG_DIR}/lib/lib${lib}.dll.a")
endforeach()
add_library(miniaudio STATIC "${KV}/third_party/miniaudio/miniaudio.c")
target_include_directories(miniaudio PUBLIC "${KV}/third_party/miniaudio")
target_compile_definitions(miniaudio PRIVATE MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)

# All app sources except main.cpp, compiled straight from the repo (same as the app target)
file(GLOB_RECURSE KV_FILES CONFIGURE_DEPENDS "${KV}/src/*.cpp" "${KV}/src/*.h")
list(FILTER KV_FILES EXCLUDE REGEX "/src/main\\.cpp$")
qt_add_executable(kv-wheelcheck wheelcheck.cpp ${KV_FILES} "${KV}/resources/app.qrc")
target_include_directories(kv-wheelcheck PRIVATE "${KV}/src")
target_compile_definitions(kv-wheelcheck PRIVATE UNICODE _UNICODE NOMINMAX KV_VERSION="0.1.0")
target_link_libraries(kv-wheelcheck PRIVATE Qt6::Widgets Qt6::Svg Qt6::Network Qt6::Test ffmpeg miniaudio ole32 shell32 uuid)
```

`C:\kantan-dev\tmp\kv-wheelcheck\run.ps1` (ASCII only; runs in a child powershell so env changes don't leak):
```powershell
# Temporary: build and run the wheel check harness. Delete this folder when done.
$ErrorActionPreference = 'Continue'
$d = 'C:\kantan-dev'
$h = Join-Path $d 'tmp\kv-wheelcheck'
$env:PATH = "$d\Qt\Tools\mingw1310_64\bin;$d\Qt\6.8.3\mingw_64\bin;$d\ffmpeg\bin;$env:PATH"
$env:TEMP = $env:TMP = Join-Path $d 'tmp'
$env:QT_PLUGIN_PATH = "$d\Qt\6.8.3\mingw_64\plugins"
$cmake = "$d\cmake\bin\cmake.exe"
if (-not (Test-Path "$h\build\build.ninja")) {
  & $cmake -S $h -B "$h\build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$d\bin\ninja.exe" -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_PREFIX_PATH=$d\Qt\6.8.3\mingw_64" "-DCMAKE_C_COMPILER=$d\Qt\Tools\mingw1310_64\bin\gcc.exe" `
    "-DCMAKE_CXX_COMPILER=$d\Qt\Tools\mingw1310_64\bin\g++.exe"
  if ($LASTEXITCODE -ne 0) { Write-Host "configure failed ($LASTEXITCODE)"; exit 3 }
}
& $cmake --build "$h\build"
if ($LASTEXITCODE -ne 0) { Write-Host "build failed ($LASTEXITCODE)"; exit 3 }
& "$h\build\kv-wheelcheck.exe"
exit $LASTEXITCODE
```

`C:\kantan-dev\tmp\kv-wheelcheck\wheelcheck.cpp` (UTF-8 without BOM; printed text is ASCII so the console shows it correctly):
```cpp
// 一時的な確認用（ホイールの無効化）。確認が終わったらフォルダーごと消す
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTest>
#include <QWheelEvent>
#include <cstdio>

#include "core/editor.h"
#include "ui/inspector.h"
#include "ui/theme.h"

using namespace kv;

static int failures = 0;

static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "OK  " : "FAIL", what);
  std::fflush(stdout);
  if (!ok) ++failures;
}

// QSlider::initStyleOption は protected なので、派生クラス経由のメンバーポインターで呼ぶ
struct SliderAccess : QSlider {
  static QRect handleRect(const QSlider* s) {
    QStyleOptionSlider opt;
    const auto init = &SliderAccess::initStyleOption;
    (s->*init)(&opt);
    return s->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, s);
  }
};

// 直接送るホイール（spontaneous ではないので親には回らない）。受け取られたら true
static bool sendWheel(QWidget* w, int dy) {
  const QPointF pos = QRectF(w->rect()).center();
  QWheelEvent ev(pos, w->mapToGlobal(pos), QPoint(), QPoint(0, dy), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(w, &ev);
  return ev.isAccepted();
}

// OS から来るのと同じ経路（spontaneous）でホイールを回す。受け取られなければ親へ回る
static void realWheel(QWidget* w, int dy) {
  QWidget* top = w->window();
  const QPoint pos = w->mapTo(top, w->rect().center());
  QTest::wheelEvent(top->windowHandle(), QPointF(pos), QPoint(0, dy));
  QApplication::processEvents();
}

static void mouse(QWidget* w, QEvent::Type type, QPoint pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
  QMouseEvent ev(type, QPointF(pos), w->mapToGlobal(QPointF(pos)), button, buttons, Qt::NoModifier);
  QApplication::sendEvent(w, &ev);
}

static void key(QWidget* w, QEvent::Type type, int k) {
  QKeyEvent ev(type, k, Qt::NoModifier);
  QApplication::sendEvent(w, &ev);
}

int main(int argc, char** argv) {
  QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
  QApplication app(argc, argv);
  theme::apply(app);
  Inspector insp;
  insp.resize(340, 420);
  insp.show();
  if (!QTest::qWaitForWindowExposed(&insp)) std::printf("note: window was not exposed\n");
  const QString id = editor().addText(QStringLiteral("basic"));  // テキストを選んだ状態にする
  QApplication::processEvents();

  auto* font = insp.findChild<QComboBox*>();
  auto* edit = insp.findChild<QPlainTextEdit*>();
  QSlider* size = nullptr;
  QSlider* stroke = nullptr;
  for (QSlider* s : insp.findChildren<QSlider*>()) {
    if (s->accessibleName() == QStringLiteral("サイズ")) size = s;
    if (s->accessibleName() == QStringLiteral("縁取りの太さ")) stroke = s;
  }
  check(font && edit && size && stroke && font->isVisible() && size->isVisible(), "setup: text panel controls found");
  if (!font || !edit || !size || !stroke) return 2;
  QScrollBar* bar = insp.verticalScrollBar();
  auto item = [&] { return editor().text(id); };
  auto show = [&](QWidget* w) {
    bar->setValue(0);
    insp.ensureWidgetVisible(w);
    QApplication::processEvents();
  };

  // ---- フォント ----
  check(font->focusPolicy() == Qt::StrongFocus, "font: focus policy is StrongFocus (wheel does not take focus)");
  {
    const int i0 = font->currentIndex();
    const FontId f0 = item()->font;
    const bool accepted = sendWheel(font, -120);
    check(!accepted, "font: direct wheel event is ignored");
    check(font->currentIndex() == i0 && item()->font == f0, "font: direct wheel does not change the font");
  }
  {
    show(font);
    edit->setFocus();
    const int v0 = bar->value();
    const int i0 = font->currentIndex();
    check(v0 < bar->maximum(), "font: precondition - the panel can scroll further");
    realWheel(font, -120);
    check(bar->value() > v0, "font: real wheel over the combo scrolls the panel");
    check(font->currentIndex() == i0, "font: real wheel does not change the font");
    check(insp.focusWidget() == edit, "font: real wheel does not move focus to the combo");
  }
  {
    show(font);
    const int target = (font->currentIndex() + 1) % font->count();
    mouse(font, QEvent::MouseButtonPress, font->rect().center(), Qt::LeftButton, Qt::LeftButton);
    QApplication::processEvents();
    check(font->view()->isVisible(), "font: click opens the list");
    font->view()->setCurrentIndex(font->model()->index(target, 0));
    key(font->view(), QEvent::ShortcutOverride, Qt::Key_Return);  // 一覧で Enter（Qt 6.8 は ShortcutOverride で選ぶ）
    QApplication::processEvents();
    font->hidePopup();
    QApplication::processEvents();
    check(font->currentIndex() == target && int(item()->font) == font->itemData(target).toInt(),
          "font: choosing from the list changes the font");
  }
  {
    font->setFocus(Qt::MouseFocusReason);
    const int i0 = font->currentIndex();
    const bool accepted = sendWheel(font, 120);
    check(!accepted && font->currentIndex() == i0, "font: wheel after clicking (focused) is still ignored");
  }

  // ---- 文字のサイズ ----
  {
    const int s0 = size->value();
    const double t0 = item()->size;
    const bool down = sendWheel(size, -120);
    const bool up = sendWheel(size, 120);
    check(!down && !up, "size: direct wheel events are ignored");
    check(size->value() == s0 && item()->size == t0, "size: direct wheel does not change the size");
  }
  {
    show(size);
    const int v0 = bar->value();
    const int s0 = size->value();
    check(v0 < bar->maximum(), "size: precondition - the panel can scroll further");
    realWheel(size, -120);
    check(bar->value() > v0, "size: real wheel over the slider scrolls the panel");
    check(size->value() == s0, "size: real wheel does not change the size");
  }
  {
    show(size);
    const int s0 = size->value();
    const double t0 = item()->size;
    const QPoint p = SliderAccess::handleRect(size).center();
    const QPoint q = p + QPoint(40, 0);
    mouse(size, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
    mouse(size, QEvent::MouseMove, q, Qt::NoButton, Qt::LeftButton);
    const bool accepted = sendWheel(size, -120);  // ドラッグ中のホイール
    const int s1 = size->value();
    mouse(size, QEvent::MouseButtonRelease, q, Qt::LeftButton, Qt::NoButton);
    check(s1 > s0 && item()->size > t0, "size: dragging the handle changes the size");
    check(!accepted && size->value() == s1, "size: wheel while dragging is ignored");
  }
  {
    const int s0 = size->value();
    const QPoint p(size->width() - 2, size->height() / 2);
    mouse(size, QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton);
    mouse(size, QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton);
    check(size->value() > s0, "size: clicking the groove still moves the slider");
  }
  {
    const int s0 = size->value();
    key(size, QEvent::KeyPress, Qt::Key_Down);
    check(size->value() == s0 - 1, "size: arrow key still changes the size");
  }

  // ---- 対象外のスライダーは今までどおり（変更の範囲の確認） ----
  {
    const int s0 = stroke->value();
    const bool accepted = sendWheel(stroke, s0 < stroke->maximum() ? 120 : -120);
    check(accepted && stroke->value() != s0, "scope: stroke-width slider still reacts to the wheel");
  }

  if (failures) std::printf("RESULT: %d check(s) FAILED\n", failures);
  else std::printf("RESULT: ALL OK\n");
  return failures ? 1 : 0;
}
```
The harness never creates a MainWindow, so it cannot write the user's autosave (`writeAutosave` only runs from MainWindow's timer).

## Appendix B: exact code changes

native/src/ui/widgets.h. In `class SliderRow`, after `void setLabelVisible(bool v);`:
```cpp
  /** ホイールでは値を変えない（ドラッグとキー操作では変えられる） */
  void disableWheel();
```
After the `makeToolButton(...)` declaration, before `}  // namespace kv`:
```cpp
/** ホイールでは値を変えない。ホイールは親に渡すので、パネルはそのままスクロールできる */
void disableWheel(QWidget* w);
```

native/src/ui/widgets.cpp. Includes: add `#include <QEvent>` between `<QColorDialog>` and `<QGridLayout>`. Right after the closing `}` of `makeToolButton()`:
```cpp
namespace {
// ホイールを受け取らずに親（スクロールエリアなど）へ渡す
class WheelBlocker : public QObject {
 public:
  using QObject::QObject;

 protected:
  bool eventFilter(QObject* obj, QEvent* e) override {
    if (e->type() != QEvent::Wheel) return QObject::eventFilter(obj, e);
    e->ignore();
    return true;
  }
};
}  // namespace

void disableWheel(QWidget* w) {
  w->installEventFilter(new WheelBlocker(w));
  // ホイールではフォーカスも移さない（クリックと Tab ではこれまでどおり移る）
  if (w->focusPolicy() == Qt::WheelFocus) w->setFocusPolicy(Qt::StrongFocus);
}
```
After `SliderRow::setLabelVisible`:
```cpp
void SliderRow::disableWheel() {
  kv::disableWheel(slider_);
}
```
The `kv::` qualification is required: inside the member, an unqualified call would find the member itself.

native/src/ui/inspector.cpp, TextPanel constructor. Right after line 426 (`size_ = new SliderRow(QStringLiteral("サイズ"), ...);`):
```cpp
    // フォントとサイズはホイールでは変えない（ホイールはパネルのスクロールに使う）
    disableWheel(font_);
    size_->disableWheel();
```
