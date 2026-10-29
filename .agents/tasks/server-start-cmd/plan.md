# server: start.cmd + auto rebuild

Request: keep server stuff in server/; a .cmd that opens the API server; rebuild automatically when updated.

## What was built
- `server/start.cmd` -> `tools/start.ps1`: pinned portable Node 24.22.1 into `server/.runtime` (SHA-256 checked, `tools/node.ps1`), `.env` created from the example (Notepad only when someone is at the console), QuickEdit off, relaunch loop on exit 75.
- `tools/supervisor.ts`: polls `src/**`, `.env`, `package*.json`, `tools/*.ts` every second (stat + sha1, settle 1.5 s); runs `src/index.ts --check` before every restart and keeps the old server if it fails; IPC ready/shutdown (waits for in-flight requests); crash -> retry 2 s..60 s; tools change -> exit 75.
- `tools/deps.ts`: stamp `node_modules/.e-editor-deps.json`; `npm ci --ignore-scripts` (`--omit=dev` unless typescript is present); while running, installs into `.runtime/next` and swaps.
- `server/create-admin.cmd` -> `tools/create-admin.ps1` (prompts; exit 2 = already registered -> offer reset).
- `src/index.ts`: `--check`, exit codes (`src/exit-codes.ts`), clear DB/port errors (`db.ts` `describeDbError`, 10 s connect timeout).
- Docs moved to `server/README.md`; root README keeps a short section. `server/.gitignore` holds node_modules/.runtime/.env.

## Verified (temp copy in a path with Japanese + space, throwaway PostgreSQL)
Node download + hash, prod install, DB retry, save .env -> start, code edit -> restart, syntax error -> old server kept, .env change, wrong DB password -> old kept, dependency change -> staged swap with 0 failed health checks, killed server -> back in 2 s, tools change -> relaunch, second instance -> port message, create-admin create/exists/reset + login, supervisor killed -> server stops, dev-mode install. tsc OK; tests 21/21 on Node 22.15.1 and 24.22.1.

## Not verified
Real Ctrl+C / window close in an interactive console, Notepad auto-open, ARM64, startup-folder autostart, a real PostgreSQL install.
