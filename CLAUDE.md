# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**subconverter** is a C++20 utility for converting between various proxy subscription formats (Clash, Surge, V2Ray, SSR, Trojan, sing-box, etc.). It runs as both a command-line tool and an HTTP web service (default port 25500).

Version: v0.9.9

## Build Instructions

### Dependencies

- CMake >= 3.5
- C++20 compatible compiler
- libcurl >= 7.54.0
- yaml-cpp >= 0.6.3
- PCRE2
- RapidJSON
- toml11
- QuickJS
- LibCron

### Build Commands

```bash
# Standard build
cmake -DCMAKE_BUILD_TYPE=Release .
make -j3

# Build static library only
cmake -DBUILD_STATIC_LIBRARY=ON .
make -j3

# Enable malloc_trim for lower memory usage (Linux)
cmake -DUSING_MALLOC_TRIM=ON .
```

### Platform-Specific Build Scripts

See `scripts/` directory for CI build scripts:
- `scripts/build.alpine.release.sh` - Alpine Linux static build
- `scripts/build.macos.release.sh` - macOS build
- `scripts/build.windows.release.sh` - Windows build

## Running

```bash
# Run web server on default port 25500
./subconverter

# With custom config
./subconverter -f /path/to/pref.toml

# Generator mode
./subconverter -g --artifact=artifact_name

# Check version
curl http://localhost:25500/version
```

## Architecture

### Core Data Structure

The `Proxy` struct (defined in `src/parser/config/proxy.h`) is the central data structure representing a proxy node. It supports:
- Shadowsocks/ShadowsocksR
- VMess/VLESS
- Trojan
- Hysteria/Hysteria2
- TUIC
- AnyTLS
- WireGuard
- Snell
- HTTP/HTTPS/SOCKS5

### Request Flow

1. **HTTP Request** → `src/server/webserver_httplib.cpp`
2. **Route Handling** → `src/handler/interfaces.cpp`
   - Main endpoint: `/sub` handled by `subconverter()` function
3. **Node Parsing** → `src/parser/subparser.cpp`
   - Fetches subscription content via `webget.cpp`
   - Parses various formats into `Proxy` structs
4. **Config Generation** → `src/generator/config/subexport.cpp`
   - Converts `Proxy` structs to target format output

### Key Components

| Directory | Purpose |
|-----------|---------|
| `src/parser/` | Subscription parsing from various formats |
| `src/generator/` | Output generation for target formats |
| `src/handler/` | HTTP handlers, web requests, settings |
| `src/server/` | HTTP server implementation (cpp-httplib) |
| `src/script/` | QuickJS scripting and cron support |
| `src/utils/` | Utilities (base64, MD5, string, network) |
| `base/` | Configuration templates and base files |

### Main API Endpoints

- `GET /sub` - Main conversion endpoint, accepts `target`, `url`, `config` params
- `GET /version` - Version info
- `GET /refreshrules` - Refresh rulesets (requires token)
- `GET /getprofile` - Load and convert profile
- `GET /getruleset` - Get converted ruleset
- `GET /render` - Template rendering

### Configuration

Configuration files are loaded in order of preference:
1. `pref.toml` (TOML format)
2. `pref.yml` (YAML format)
3. `pref.ini` (INI format)

If none exist, the program copies from `pref.example.*` templates.

Key configuration sections:
- `[common]` - API mode, default URLs, proxy settings
- `[node_pref]` - Node sorting, emoji handling, Clash settings
- `[managed_config]` - Surge managed config options
- `[ruleset]` - Ruleset generation settings
- `[[custom_groups]]` - Proxy group definitions
- `[[rulesets]]` - Ruleset definitions

### Supported Target Types

| Target | Value |
|--------|-------|
| Clash | `clash` |
| ClashR | `clashr` |
| Surge | `surge&ver=2/3/4/5` |
| Surfboard | `surfboard` |
| Quantumult | `quan` |
| Quantumult X | `quanx` |
| Loon | `loon` |
| sing-box | `singbox` |
| SS | `ss` |
| SSR | `ssr` |
| V2Ray | `v2ray` |
| Trojan | `trojan` |

### Script Support

The tool supports QuickJS for:
- **Filter scripts**: Filter nodes based on custom logic
- **Sort scripts**: Custom node sorting
- **Cron tasks**: Scheduled execution

Scripts can be inline or loaded from path (`path:/path/to/script.js`).

## Key Files

- `src/main.cpp` - Entry point, server initialization
- `src/handler/interfaces.cpp` - Main API endpoint implementations
- `src/parser/subparser.cpp` - Subscription parsing logic
- `src/generator/config/subexport.cpp` - Export format generators
- `src/parser/config/proxy.h` - Core Proxy data structure
- `base/pref.example.toml` - Configuration template
