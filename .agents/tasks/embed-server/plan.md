# Embed the AI server address in the distributed exe

Request (2026-10-24): "APIサーバーを組み込みたいんだけど userにばれないように組み込める？" → agreed plan: git-ignored `native/release.env` → build.ps1 embeds it obfuscated → app uses it unless a `.env` is present → update the 未設定 guidance and READMEs.

## Decisions
- The address is obfuscated, not secret. The user was told this up front.
  - Network tools (DNS/SNI, Resource Monitor, Fiddler) and exe analysis reveal it.
  - The real protection is login, the token quota and approval-mode sign-up. Cloudflare Tunnel keeps the home IP hidden.
- The Bedrock key is never embedded.
- `native/release.env` (`KANTAN_AI_SERVER=`) is git-ignored via `release.env` in `.gitignore`; the template is `native/release.env.example`.
- `native/tools/common.ps1` provides three functions:
  - `Read-KantanReleaseServer` parses the file the same way the app parses `.env`.
  - `Assert-KantanReleaseServer` stops the build on a bad address (bad scheme, userinfo/query/fragment, or HTTP to a public host). It never prints the address.
  - `Write-KantanEmbeddedServer` writes `build/generated/kv_embedded_server.h`:
    - Format: `kSize`, plus a random per-write `kKey[]` and `kData[]`, where `data = utf8 ^ key ^ (i*0x9D+0x5B)`.
    - The header is rewritten only when SHA-256 of the value changes (stamp file), so unchanged builds don't recompile.
    - With no value it writes `kSize = 0`.
  - build.ps1 calls them after configure. build-installer.ps1 warns when nothing is embedded.
- CMake adds `${CMAKE_BINARY_DIR}/generated` to the include path. `aiconfig.cpp` includes the header via `__has_include`, so IDE builds without build.ps1 just have no address.
  - Decoding reads through `volatile` pointers so the compiler can't fold the plain string into the binary.
- Priority: any `.env` with a value (exe dir → repo root → data dir) → embedded → none. An empty `KANTAN_AI_SERVER=` falls through to the embedded address.
- `serverSource()` reports where the address came from: `None`, `EnvFile` or `BuiltIn`.
  - With `BuiltIn`, `serverProblem()` / `wrongServerText()` say to contact the distributor instead of naming `.env`.
  - The status tooltip shows the `.env` path only when the address came from an `.env`.

## Progress
- [x] common.ps1 / build.ps1 / build-installer.ps1 / CMakeLists / aiconfig / account / serverstatus; ps1 files keep BOM + CRLF and parse cleanly
- [x] build with test address: plain host absent from exe (ASCII and UTF-16) and from the .obj
- [x] isolated test of real aiconfig.cpp + generated header at -O2: decode/normalize, BuiltIn source, `.env` override (exe dir and data dir), empty `.env` falls back, distributor-facing texts (14/14, temporary, removed)
- [x] PowerShell function cases (quotes, comments, UTF-16, private/Tailscale, public HTTP rejected, ftp, userinfo, query, empty, missing file, no rewrite when unchanged)
- [x] no-release.env build compiles (`kSize = 0`)
- [x] `native/release.env` created from the repo `.env` value (not printed); full build + deploy; full host and registered domain absent from every file in `native/dist`
- [x] docs: README (install, AI を使う準備 + 別の AI サーバーを使う（.env）, status table, 配る steps + アドレスを組み込む, data table, folder tree), server/README, `.env.example`

## Notes
- Not verified: an actual installer run on a clean PC (no `.env`), i.e. login dialog at startup against the real server. In this repo, `native/dist` still prefers the repo-root `.env`.
