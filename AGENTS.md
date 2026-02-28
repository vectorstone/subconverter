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
