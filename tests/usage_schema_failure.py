#!/usr/bin/env python3
"""Prove a failed usage migration does not poison the shared PostgreSQL connection."""

import argparse
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[1]


def request(url, method="GET", body=None, headers=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, json.loads(response.read() or b"{}")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read() or b"{}")


def wait_ready(base_url):
    for _ in range(100):
        try:
            with urllib.request.urlopen(base_url + "/version", timeout=1) as response:
                if response.status == 200:
                    return
        except (OSError, urllib.error.URLError):
            time.sleep(0.1)
    raise RuntimeError("subconverter did not start")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--database-url", default=os.environ.get("DATABASE_URL"))
    parser.add_argument("--binary", default=str(ROOT / "build-usage" / "subconverter"))
    parser.add_argument("--port", type=int, default=25593)
    args = parser.parse_args()
    if not args.database_url:
        raise SystemExit("DATABASE_URL or --database-url is required")

    existing = subprocess.run(
        ["psql", args.database_url, "-Atc", "SELECT to_regclass('public.shortlink_usage_bindings')"],
        check=True, capture_output=True, text=True).stdout.strip()
    if existing:
        raise SystemExit("use a disposable database without usage tables")

    subprocess.run(
        ["psql", args.database_url, "-v", "ON_ERROR_STOP=1", "-c",
         "CREATE TABLE shortlink_usage_bindings(id TEXT PRIMARY KEY)"], check=True,
        stdout=subprocess.DEVNULL)

    with tempfile.TemporaryDirectory(prefix="usage-schema-failure-") as directory:
        directory = Path(directory)
        providers = directory / "providers.json"
        providers.write_text(json.dumps({"schema_version": 1, "providers": [{
            "provider_id": "fixture", "label": "Fixture",
            "endpoint": "https://127.0.0.1:1", "ca_file": "/nonexistent/ca.crt",
            "client_cert_file": "/nonexistent/client.crt",
            "client_key_file": "/nonexistent/client.key"
        }]}))
        token = secrets.token_urlsafe(32)
        base_url = f"http://127.0.0.1:{args.port}"
        environment = dict(os.environ, API_MODE="true", API_TOKEN=token,
                           SHORTLINK_ENABLED="true", DATABASE_URL=args.database_url,
                           SHORTLINK_ENCRYPTION_KEY=secrets.token_hex(32),
                           SHORTLINK_USAGE_ENABLED="true",
                           SHORTLINK_USAGE_PROVIDERS_FILE=str(providers),
                           PUBLIC_BASE_URL=base_url, PORT=str(args.port))
        log_path = directory / "subconverter.log"
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                [str(Path(args.binary).resolve()), "-f", str(ROOT / "base" / "pref.example.toml")],
                env=environment, stdout=log, stderr=log)
            try:
                wait_ready(base_url)
                admin = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}
                status, error = request(base_url + "/api/usage", headers=admin)
                assert status == 503 and error.get("error") == "usage_unavailable", (status, error)

                status, key = request(base_url + "/api/keys", "POST",
                                      {"owner": "schema-failure-user", "name": "failure-smoke"}, admin)
                assert status == 201 and key.get("api_key"), (status, key)
                smoke_env = dict(os.environ, BASE_URL=base_url, API_KEY=key["api_key"])
                subprocess.run([str(ROOT / "tests" / "shortlink_api_smoke.sh")],
                               cwd=ROOT, env=smoke_env, check=True)
            finally:
                process.terminate()
                process.wait(timeout=12)

        log_text = log_path.read_text(errors="replace")
        assert "PostgreSQL usage schema initialization failed" in log_text
        assert "current transaction is aborted" not in log_text
    print("usage-schema-failure-isolation-ok")


if __name__ == "__main__":
    main()
