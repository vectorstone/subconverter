# Repository Guidelines

## Project Structure & Module Organization
- `src/` contains the C++20 application code. Main areas are `parser/` (subscription parsing), `generator/` (target config output), `handler/` (HTTP handlers), `server/` (web server backend), `config/` (shared config models), `script/` (cron/QuickJS integration), and `utils/` (common helpers).
- `include/` stores bundled third-party headers used at build time.
- `base/` holds runtime assets distributed with the binary: `pref.example.*`, rulesets, snippets, and profiles.
- `scripts/` contains release build scripts and tooling such as rules synchronization.
- `cmake/` provides custom CMake `Find*.cmake` modules.
- There is currently no dedicated `tests/` directory.

## Build, Test, and Development Commands
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` — recommended local build (out-of-source).
- `cmake -DCMAKE_BUILD_TYPE=Release . && make -j` — in-source build flow used by existing release scripts.
- `./subconverter` — start the local server (default port `25500`).
- `curl http://127.0.0.1:25500/version` — quick runtime smoke check.
- `python3 scripts/update_rules.py -c scripts/rules_config.conf` — refresh bundled rules from upstream repos.
- `bash scripts/build.alpine.release.sh` / `bash scripts/build.macos.release.sh` / `bash scripts/build.windows.release.sh` — platform release build automation.

## Coding Style & Naming Conventions
- Use C++20 and follow existing file-local patterns.
- Use 4-space indentation, avoid tabs, and keep braces/newlines consistent with nearby code.
- Prefer lowercase, descriptive file names (examples: `subparser.cpp`, `webserver_httplib.cpp`).
- Keep includes ordered as: standard library, system headers, then project headers.
- No repository-wide formatter is enforced; keep changes minimal, readable, and style-consistent.

## Testing Guidelines
- No formal unit-test framework is wired into CMake/CI yet.
- For functional changes, run targeted smoke tests against changed endpoints (for example `/sub`, `/version`, `/refreshrules`).
- For parser/generator updates, verify at least one real subscription input and confirm expected output format.
- If introducing automated tests, add them in a new `tests/` directory and include build/run instructions in the same PR.

## Commit & Pull Request Guidelines
- Keep commit subjects short and imperative; existing history commonly uses `fix: ...`, `Update ...`, or concise feature statements.
- Scope each commit to one logical change.
- PR descriptions should include: purpose, key behavior changes, validation steps, and linked issues/PRs (for example `#70`).
- Include sample request/response snippets when changing conversion logic or API behavior.

## Sensitive Information and Push Sanitization

This repository is published publicly. **Every push — including branch and tag pushes, force-pushes, and history rewrites — must be sanitized first.** Never commit or transmit any of the following, in file contents, file names, commit messages, tag names, author/committer identities, test fixtures, or documentation:

- Real hostnames or domains of production, staging, or personal services (including the deployment domain, API subdomains, proxy/DNS/rule-mirror hosts, and provider hostnames).
- Real IP addresses: public addresses of your own hosts, and LAN/private addresses (`10/8`, `172.16/12`, `192.168/16`, carrier-grade NAT).
- Email addresses, real names, usernames, account identifiers, and personal handles not already used as the public GitHub identity.
- Absolute local paths (`$HOME/...`, `~/...`), machine names, and backup or PKI directory locations.
- Credentials of any kind: passwords, API tokens, bootstrap tokens, Clash `secret` values, proxy passwords, UUIDs in use, HMAC keys, encryption keys, private keys, certificates, database dumps, image digests, and checksums of unpublished artifacts.
- Personal configuration snapshots (Clash/proxy profiles, subscription links, `.env` files) and operational records that tie a deployment to a real host.

Use placeholders instead: `example.com` / `example.org` / `example.invalid`, the RFC 5737 documentation ranges (`192.0.2.0/24`, `198.51.100.0/24`, `203.0.113.0/24`), `127.0.0.1` for loopback, and `<PLACEHOLDER>` for values. Keep real operations runbooks, deployment records, and host inventories in a private location rather than in this repository.

### Required pre-push check

1. Run `scripts/check-sensitive.sh` (staged changes by default; `--all` for the whole tree, `--range A..B` for a commit range). It reports generic findings (emails, private/absolute paths, private keys) plus any project-specific literal terms listed in the gitignored `.sensitive-patterns` file.
2. Fix every finding, or move the value to a placeholder. Only then commit and push.
3. If sensitive content already reached a remote, sanitizing the working tree is not enough: the history must be rewritten, force-pushing over the published branches, and any exposed credential must be rotated. Prefer a range-limited rewrite (for example `git filter-branch` over `<upstream-base>..<branch>`) so upstream-shared commit IDs survive; a full-history rewrite changes every SHA, including upstream commits, and detaches the fork from upstream. Announce the rewrite before force-pushing branches others may have cloned, and verify afterwards that no ref (including tags and remote-tracking refs) still reaches the old content.

## Fork-Specific Changes and Upstream Sync Guardrails

This repository is a fork of `https://github.com/tindy2013/subconverter`.

- Upstream `master` at the last recorded sync: `a0d4eab28cb8b6c782d4ce5c3a918de4829b4a72`. It is currently an ancestor of fork `master`, so the fork is not behind upstream.
- Derive the live relationship before syncing instead of trusting recorded numbers: `git merge-base HEAD upstream/master` and `git rev-list --left-right --count HEAD...upstream/master`.
- **History was rewritten on 2026-09-14** to strip production identifiers and leaked credentials from all branches. Every fork commit ID changed, the former local `example.yaml` snapshot was purged from history, and commits signed by upstream (including the shared base) keep their original IDs. Pre-rewrite SHAs are no longer valid; keep a private backup if you need them.
- Tree-level divergence figures are intentionally not recorded here because they change with every local commit; compute them with `git diff --stat $(git merge-base HEAD upstream/master) HEAD` when needed.

### Changes that belong to this fork

When synchronizing upstream, preserve these behaviors and files unless a deliberate migration plan says otherwise:

1. **Protocol parsing and export** (`src/parser/subparser.cpp`, `src/parser/subparser.h`, `src/parser/config/proxy.h`, `src/generator/config/subexport.cpp`): SS dialer/underlying-proxy parameters, VLESS + Reality parsing/export (including Surge/QuanX/Sing-box/mixed targets), TUIC parsing/export for Clash and Sing-box, IPv6 host normalization, and Clash dialer group generation with `DIRECT`.
2. **Clash defaults, DNS, and routing data** (`base/base/all_base.tpl`, `base/base/clash_dns_base.yml`, `base/config/default_clash_chainproxy.ini`, `base/rules/meituan.list`, `base/rules/custom_proxy.list`, `base/rules/custom_direct.list`, `base/pref.*`, `src/generator/config/subexport.cpp`): built-in Clash fallback when `config` is omitted and `default_external_config` is empty; `ChainProxyEntry`/`♻️ 自动选择` group semantics; bundled DNS template; locally maintained direct/proxy domain exceptions; refreshed routing-rule defaults.
3. **Fetch, concurrency, and log-safety behavior** (`src/handler/interfaces.cpp`, `src/handler/webget.cpp`, `src/handler/settings.cpp`, `src/handler/settings.h`, `src/utils/string.*`, `base/pref.example.*`): resilient subscription fetching, retry/backoff/timeouts, concurrent request controls, and redacted subscription URLs in logs and errors.
4. **PostgreSQL short-link service** (`src/handler/shortlink_api.*`, `src/storage/postgres_store.*`, `src/security/secretbox.*`, `db/migrations/001_initial.sql`, `base/web/*`, `src/main.cpp`, `src/server/webserver_httplib.cpp`, `CMakeLists.txt`): multi-user Web UI/API, encrypted snapshot storage, chained short links, refresh/revoke/admin operations, quotas, libpq/OpenSSL linkage, `/s/*` plus `/api/*` routes, and the loop-detection exception required for short-link fetches.
5. **Deployment and release integration** (`docker-compose.shortlink.yml`, `docker_compose.yml`, `deploy/*`, `scripts/Dockerfile`, `.github/workflows/docker.yml`, `README*.md`, `README-docker.md`, `docs/short-link-postgresql-plan.md`, `tests/shortlink_api_smoke.sh`): fork image/secret names, Docker context, pinned compose/deployment configuration, Nginx and environment examples, and the short-link operational documentation.
6. **Local project artifacts** (`AGENTS.md`, `CLAUDE.md`, `.sisyphus/*`, plus the custom base assets above) are intentional fork-local state. Do not delete or replace them as “generated” files without checking their purpose. Personal profile snapshots such as the former `example.yaml` must stay out of the repository: they carry live credentials and were purged from history on 2026-09-14.

### Upstream integration status

`upstream/master` is currently an ancestor of fork `master`, so no upstream-only commits are pending. Re-verify with `git rev-list --left-right --count HEAD...upstream/master` before starting a sync. Work previously listed as awaiting integration (AnyTLS parser support, build and dependency workflow updates, CMake minimum-version cleanup) is already present in this history.

### Safe synchronization procedure

Use a merge-based update for the shared `master` branch so the fork commits remain identifiable. Do not reset the branch to upstream, use `git checkout --theirs .`, or force-push over the fork history.

1. Confirm a clean worktree, record `HEAD`, and fetch both remotes. If no `upstream` remote exists, add `https://github.com/tindy2013/subconverter.git` first.
2. Create a recovery branch/tag (and, for a high-risk update, a `git bundle`) pointing at the current `HEAD` before touching the branch.
3. Run a read-only preflight with `git merge-tree --write-tree HEAD upstream/master`, then start `git merge --no-ff --no-commit upstream/master`.
4. Expect and review conflicts in `.gitignore`, `src/generator/config/subexport.cpp`, `src/parser/config/proxy.h`, `src/parser/subparser.cpp`, and `src/parser/subparser.h`. The parser/generator resolutions must retain VLESS, TUIC, Reality, dialer, and the fork’s output behavior while adding upstream AnyTLS support.
5. Preserve the fork’s `libpq`/OpenSSL CMake wiring, short-link sources/assets/deployment files, local Clash defaults/rule lists, and fork Docker image/secret settings. Incorporate upstream build-script and GitHub Action upgrades only after checking their compatibility with those local additions.
6. After resolution run `git diff --check`, inspect the staged diff, build with the recommended CMake command, and smoke-test `/version`, `/sub`, `/refreshrules`; run `tests/shortlink_api_smoke.sh` when PostgreSQL/short-link settings are available.
7. If resolution or validation is unsafe, use `git merge --abort`; the recovery branch/tag must remain untouched. Only commit and push after the fork-specific checklist and behavior tests pass.

For a private, unpublished topic branch, rebasing onto `upstream/master` is possible, but it rewrites the fork commit IDs and still requires the same conflict checklist and tests.
