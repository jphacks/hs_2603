# Review: no wheel changes on the text font / size controls (pass 1)

Verdict: APPROVED. No blockers.

Reviewed: the user request, plan.md, `git diff` / `git status`, verification.md, and the unchanged code the change depends on: the SliderRow constructor (widgets.cpp:131-160), TextPanel and the Inspector scroll area (inspector.cpp:417-441, 690-713), and the app-wide filter (mainwindow.cpp:477-497). This is the first pass, so there was no prior review. I did not rerun the build or the harness.

## Checks
1. The wheel no longer changes the font or the size, whatever the focus. `WheelBlocker` (widgets.cpp:46-59) returns true for every `QEvent::Wheel`, so `QComboBox::wheelEvent` and `QAbstractSlider::wheelEvent` never run. The filter has no focus or drag condition. It is installed on `font_` and on the private slider inside `size_` (inspector.cpp:427-429, widgets.cpp:176-178). Harness evidence: the baseline has exactly the 12 predicted FAILs, and on the fixed code all checks pass, including "wheel after clicking (focused)" and "wheel while dragging".
2. Behavior other than the wheel is unchanged:
   - Every other event goes to `QObject::eventFilter`, which returns false.
   - The focus change only clears the wheel bit (WheelFocus → StrongFocus, so Tab and click focus stay). It only affects the combo, because the slider stays `TabFocus` (widgets.cpp:147).
   - The app filter handles only key events (mainwindow.cpp:479), so key routing is the same.
   - Harness checks for drag, groove click, arrow keys on the slider and the combo, choosing from the list, and double-click reset all pass.
3. Wheel events reach the panel. `e->ignore()` plus `return true` means `notify_helper` returns true but the event is not accepted, so `QApplication::notify` passes spontaneous wheel events on to the parent. The parent chain is fontSec → TextPanel → container → viewport → `Inspector : QScrollArea` (inspector.cpp:690). None of these override `wheelEvent`, and grep finds no `WA_NoMousePropagation`. The harness's real-wheel checks (spontaneous path, DPR-corrected) show the panel scrolling; they fail on the baseline.
4. Scope is limited to these two controls. The SliderRow constructor is unchanged, and the only callers of `disableWheel` are inspector.cpp:428-429 (grep). The harness scope check confirms 縁取りの太さ still reacts to the wheel.
5. The change is C++ only and purely additive (+33/−0 across 3 files). No refactoring. It follows the file's style: an anonymous namespace closed with `}  // namespace`, `/** */` doc comments in the header, and Japanese comments. The `kv::` qualification at widgets.cpp:177 is needed, because unqualified lookup would find the member function.
6. Working tree:
   - HEAD 6020166 equals origin/feat/ikumu1. The reflog shows no commit after the 2026-10-20 checkout, and the stash is empty.
   - `status` lists only the 3 intended files plus `.agents/`. The only ignored entries are the existing `native/build/` and `native/dist/`.
   - `C:\e-editor-dev\tmp\kv-wheelcheck` is gone (Test-Path False).
   - `.agents/` holds only plan.md and verification.md.
   - The diff contains no debug or test code.

Build evidence: `build.ps1 -NoDeploy` exited 0 with no warnings (`-Wall`). Deploy was blocked because the app was already running. Plan step 4 says to report that and leave the process alone, which the coder did.

## Notes (not blockers)
- `native\dist` still has the old exe. The user needs to close the app and run `build.cmd`; verification.md already says this.
- Under approved decision D2, other controls still react to the wheel: 縁取りの太さ, 背景の不透明度, 開始/長さ, and the clip and audio sliders. Because scrolling over font/size now moves the panel, continued scrolling can bring one of them under the cursor. This is existing behavior; mention it to the user in the final report.
- Not tested with a physical wheel. The harness enters Qt at `QWindowSystemInterface`.

## Findings
None.
