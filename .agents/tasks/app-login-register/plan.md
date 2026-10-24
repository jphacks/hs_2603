# App: server address, startup login, sign-up, connection status

Request (2026-10-23): "APIのURL登録するところなくない？ / アプリ自体にAPIkeyいらないから消した / APIserverにつながっているか右下に確認できるように / 最初にユーザーログイン機能 / 登録機能も"
Follow-up: the user gave the real server's domain (kept only in the ignored `.env`), then "URLはアプリ内で書きこむのではなくて envファイルかなんかで記述できるようにしといてね / アプリ内でserver指定してるのおかしいでしょ", then "そもそも最初のAPIサーバーが見えてるのおかしい / 見えないようにして"

Project lives in `JPHACKS\e-editor` (master, includes the AI + server work). The key commit is gone from GitHub.

## Decisions
- App never talks to Bedrock directly: direct mode removed (no API key / region / model in the app). Model, max tokens and effort are set by the server.
- Server address comes from `.env` only: `KANTAN_AI_SERVER`. No address field in the app, no built-in default (both rejected by the user).
  - Search order: exe folder -> repo root (dev only, found via `.env.example` / `.git` up to 4 levels) -> `%LOCALAPPDATA%\JPHACKS\kantan-video-editor`. The first `.env` that has the key wins (an unrelated `.env` without it does not hide the setting).
  - UTF-8 (with/without BOM) and UTF-16 LE (PowerShell 5 `>`), CRLF, `#` comments, quotes, ` # comment` after the value.
  - Normalize: no scheme -> `http://` for private hosts, `https://` otherwise; host lower-cased; trailing `/` dropped. HTTP to a public host is refused (status "アドレスを確かめてください", login refused).
  - Re-read on app activation, on "確かめる" in the dialog and on status-bar click. Changing the address drops the in-memory token for the other server; `session.dat` is kept (switching back reuses it).
- No `KANTAN_AI_SERVER`: AI button, menu item, Ctrl+Shift+A and the shortcut row are hidden; no startup dialog; status "未設定"; clicking it shows the `.env` path, "フォルダーを開く", "読み直す".
- Startup (server set): saved session -> restore quietly (expired -> dialog, must change password -> password page); otherwise the modal `AccountDialog` (login / 新規登録 tabs, "ログインしないで使う"). Login stays optional so the editor works offline.
- One `AccountDialog` for login, sign-up, forced and voluntary password change; read-only server line + link dot + "確かめる". Pages are plain widgets shown/hidden (QStackedLayout::heightForWidth counted hidden pages and left a gap); layout `SetFixedSize` so the dialog fits the visible page.
- Status bar (right): "AI サーバー: 未設定 / 確認中… / つながりません / アドレスを確かめてください / データベースにつながりません / ログインしていません / パスワードを変えてください / 接続中（名前）". `/api/health` every 30 s, on activation, after failures. `Account::linkChecked` fires after every check (so "確かめる" never sticks on "確かめています…", and a changed sign-up mode shows even when the link state is unchanged).
- Server sign-up: `POST /api/auth/register`; settings `off | approval | open` (default `approval`) + non-admin role; pending users can't log in until approved; admin page "新規登録"; health returns `service`, `api`, `registration`.
- The address is never shown in the app (2nd follow-up): the dialog's server box and "確かめる" are gone (title "ログイン"); only a reason line `AccountLinkNote` when the link has a problem. No address in the AI panel gate / intro, the status tooltip, toasts, or `serverProblem` texts; `hideServer()` blanks it out of Qt error strings. The status tooltip names the `.env` file only for address problems.
- `.env.example` has `KANTAN_AI_SERVER=` (empty) instead of the real domain, and an empty value counts as "not set" (the search goes on to the next `.env`). The domain is not in any tracked file.
- Health 5xx (Cloudflare 502/530 when the home PC is down) = "つながりません", not "アドレスを確かめてください".

## Progress
- [x] server: migration 002, settings.ts, register endpoint, pending in sessions/login, admin settings + approve API, health fields, tests 25/25, tsc OK
- [x] admin UI: 新規登録 page, pending tag / approve in users, overview notice, audit labels; checked in headless Edge (approve, reject with confirm, mode change saved, no script errors)
- [x] app: `.env`-based aiconfig, account (reloadServer, linkChecked), AccountDialog (read-only server line), AiPanel gate, ServerStatus, AI availability, startup flow; builds with no warnings
- [x] UI e2e (temporary harness, removed afterwards): register / restore / temppw / open / offline / badaddr / noenv / real all 0 failures. Real server reachable over HTTPS (old version: registration "off" until updated)
- [x] docs: README (app `.env`, login / sign-up, status bar), server/README.md (sign-up, model, API, Cloudflare notes), `.env.example` (app setting only), server/.env.example hint
- [x] user's `e-editor\.env` (ignored by git): `KANTAN_AI_SERVER=<real server>`
- [x] cleanup: temporary harness removed, clean rebuild + smoke launch

## Notes
- Real server still runs the old code: `git pull` on the server PC (start.cmd picks it up). Until then the app shows "新規登録: 受け付けていません".
- Cloudflare answers plain HTTP too: turn on "Always Use HTTPS". Behind the tunnel: `HOST=127.0.0.1`, `TRUST_PROXY=true`, `COOKIE_SECURE=true`.
- `TRUST_PROXY=true` trusts every hop, so a client can spoof `X-Forwarded-For` to dodge the per-IP limits (per-email login limit still holds). Trusting only loopback would fix it (not done).
