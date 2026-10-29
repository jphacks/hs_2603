# Text-template workshop + install to Program Files

Request (2026-10-24): "自分が作ったテキスト（フォント設定、色の設定、大きさなど）設定を保存したテキストテンプレートを保存できるようにしたい / 保存したテキストテンプレートをワークショップなどで公開してみんなが使えるようにしてほしい / あとアプリのインストール先をプログラムファイルにしてほしい"

## Findings
- Saving own templates already exists (commit c9c1bf8, teammate): inspector "今の見た目をテンプレートに保存…" → `TextTemplates` (`user:` ids, AppLocalData/text-templates.json), cards under "マイテンプレート" in the library text tab (drag to timeline, right-click rename/delete), inspector style grid applies them. Only discoverability is missing → add "見た目をテンプレートに保存…" to the text right-click menu (shared helper with the inspector).
- `renderTextPresetCard` re-resolves by id (`makeText(preset.id…)`) → must draw from `preset.style` so workshop items (not in the local store) render.
- `ServerCall` only does GET/POST → add a generic method for DELETE.
- `Account::request` is private and marks login busy → the workshop gets its own small client on top of `ServerCall` + `account().server()/token()`.

## Decisions
- Workshop lives on the existing AI server (accounts, PostgreSQL, admin panel). Login required for everything; mode setting `workshop` = open (default) / approval / off, like sign-up.
- DB (003_workshop.sql): `workshop_templates` (user_id cascade, name ≤30, description ≤200, style jsonb, status published|pending|hidden, uses), `workshop_uses` (unique per user), `workshop_reports` (one per user).
- Server rebuilds the style from known keys only (enums, colors `#rrggbb`, numeric clamps = app's textFromJson/propLimits ranges, ≤100 keyframes per prop, fontFamily ≤100). No text content is ever stored (only name/description). Author shown = display name (never the email; empty → 名前なし).
- App API: GET/POST /api/workshop/templates, DELETE /api/workshop/templates/:id (owner), POST …/:id/use, POST …/:id/report. Admin: GET /api/admin/workshop, PATCH /api/admin/workshop/:id {status, clearReports}, DELETE; settings GET/PATCH gain `workshop {mode}`. Health gains `workshop` (mode) and api 3.
- App: `ai/workshop.*` client; `TextTemplates` keeps per-template meta (workshopId of a downloaded one + author, publishedId of my published one); `ui/workshopdialog.*` (browse dialog + publish dialog); library text tab button; template right-click menu gains publish/unpublish.
- Installer: per-machine. RequestExecutionLevel admin, `$PROGRAMFILES64\e-editor-cpp`, SetShellVarContext all, HKLM (64-bit view) for uninstall + .kvproj, remove an old per-user install first, finish page launches the app unelevated (via explorer.exe) so drag & drop from Explorer keeps working.

## Progress
- [x] server: 003_workshop.sql, settings (workshop mode), src/workshop.ts (sanitizeStyle), routes/workshop.ts, admin routes (list/patch/delete, settings, overview counts), health `workshop` + api 3; tests 34/34 (7 new), tsc OK
- [x] admin UI page "ワークショップ" (mode, filter, sample, approve/hide/show/clear reports/delete, overview notice, audit labels); checked in headless Edge via CDP: all functional checks OK, no script errors (only the expected 401 of the pre-login /api/me)
- [x] app: ServerCall::custom (DELETE), Account::workshop(), ai/workshop.*, TextTemplates meta + styleToJson/styleFromJson, renderTextPresetCard draws from style, ui/workshopdialog.*, library "ワークショップ" button, template menu publish/unpublish, "見た目をテンプレートに保存…" in the text right-click menu; builds with no warnings
- [x] temporary UI harness (uitest.cpp "workshop" mode, reverted afterwards) against a local server: login → publish via the dialog → browse → re-login as another user → add to my templates (look kept, no text) → use counted → report → unpublish with confirm → list empty: all OK
- [x] installer: admin, Program Files, HKLM 64-bit, all-users shortcuts, old per-user install removed first, finish page launches unelevated; makensis OK, setup.exe rebuilt (19:28)
- [x] docs: README (install, templates, workshop, data table, tech notes, file list), server/README (workshop section, admin table, API, health, folder tree)

## Notes
- Not verified: actually running the new setup.exe (needs UAC elevation; this shell is not elevated) — install/upgrade from the old per-user version, HKLM association, unelevated launch from the finish page.
- The server must be updated (`git pull` on the server PC) before the app's workshop button appears (health `workshop` field).

# Follow-up: audio in the workshop + menu bar entry (2026-10-24)

Request: "オーディオとかもワークショップに追加できるようにしたい / ワークショップを左上のタブでも開けるようにして", then "ワークショップのタブは左上のファイルとか表示とかがあるところにおいてほしい" (= the menu bar; a left-panel tab was built first and removed).

## Decisions
- Audio shares `workshop_templates` (new `kind` text|audio, `file_type`, `file_size`, `duration`); bytes in `workshop_files` (bytea, cascade). Migration 004.
- Upload: raw `application/octet-stream` to `POST /api/workshop/audio?name=&description=&fileName=&duration=`; format from magic bytes (`detectAudio`), not the extension. Limits: 1KB–20MB and ≤30 min per file, 200MB per user, 100 items per user (text + audio); downloads 300/h per user.
- Old apps: `GET /api/workshop/templates` without `kind` returns text only. New app on an old server: the app filters by kind (audio list empty) and a 404 from `POST /api/workshop/audio` says the server is old.
- Rights: the audio publish dialog requires 「自分で作った音か、公開してよいと許可されている音です」; report + admin listen/hide/delete.
- Downloaded audio: `%LOCALAPPDATA%\JPHACKS\e-editor\workshop\audio\<id>\<name>.<type>` (not the cache: projects reference it). Workshop audio cannot be republished.
- UI: menu bar 「ワークショップ(W)」 between 表示 and ヘルプ (テキストのテンプレートを探す... / オーディオを探す...), visible when `ai::workshopVisible()`. It opens one non-modal window (QWidget with Qt::Window, owned by the main window; not QDialog so Enter in the search box can't press a card button). Segmented kind switch, Esc closes, closing stops a workshop preview, reopening reloads. The text tab's 「ワークショップ」 button opens the same window. The perspective changes made for the left tab were reverted.
- Admin page: kind tag + filter, `<audio controls preload=none>` sample (210px wide so the action column fits), kind-neutral texts (overview notice, audit labels, delete confirm, not-found messages).

## Progress
- [x] server: 004 migration, routes, admin `kind` + file route; tests 39/39, tsc OK
- [x] admin UI: headless Edge check, 15 functional checks OK (plays to the end under the CSP). Table width: fits at 1366px+ without report reasons; with a reason it still scrolls sideways by 164px at 1366 / 130px at 1400 (fits at 1536+)
- [x] app: publish/browse/preview/add (harness `wsaudio`), menu + window (harness `wsmenu` 31/31); harness reverted; builds with no warnings
- [x] docs: README (できること, data table, tech notes), server/README (workshop, admin table, API, backup, folder tree)

## Notes
- Not verified: real IME typing in the window's search box; the installer was not rebuilt for this change.
- The server PC needs `git pull` (migration 004) before audio works; text templates keep working on the old server.
