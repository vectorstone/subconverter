#!/usr/bin/env python3
"""Isolated usage integration tests; requires a disposable PostgreSQL database.

DATABASE_URL must point at an empty, disposable database. Builds are separate:
cmake --build build -j
go -C services/sui-usage-adapter build -o /tmp/sui-usage-adapter .
python3 tests/shortlink_usage_integration.py --adapter /tmp/sui-usage-adapter
"""

import argparse
import copy
import http.server
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(url, method="GET", body=None, headers=None, context=None):
    headers = dict(headers or {})
    if body is not None:
        body = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, method=method, data=body, headers=headers)
    try:
        response = urllib.request.urlopen(req, timeout=35, context=context)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        raw = response.read()
        try:
            payload = json.loads(raw)
        except (ValueError, UnicodeDecodeError):
            payload = raw
        return response.status, payload, dict(response.headers)


def eventually(check, timeout=45):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = check()
            if value:
                return value
        except (OSError, urllib.error.URLError):
            pass
        time.sleep(0.2)
    raise AssertionError("Timed out waiting for integration condition")


class Fixture:
    def __init__(self):
        now = int(time.time())
        self.token = secrets.token_urlsafe(24)
        self.fail = False
        self.clients = {}
        for client_id in (3, 4):
            self.clients[client_id] = {
                "id": client_id, "name": "PRIVATE-SUBID-" + str(client_id),
                "enable": client_id == 3, "up": 1024 ** 3,
                "down": 9 * 1024 ** 3, "volume": 100 * 1024 ** 3,
                "expiry": now + 86400 * 30, "createdAt": now - 86400,
                "onlineAt": now, "delayStart": False, "autoReset": False,
                "resetDays": 0, "nextReset": 0, "totalUp": 0, "totalDown": 0,
                "config": {"password": "PRIVATE-NODE-PASSWORD"},
                "links": ["PRIVATE-SUBSCRIPTION-LINK"], "inbounds": [1],
            }
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                parsed = urllib.parse.urlsplit(self.path)
                assert parsed.path == "/app/apiv2/clients"
                if fixture.fail or self.headers.get("Token") != fixture.token:
                    data = {"success": False, "obj": None, "msg": "invalid token"}
                else:
                    ids = urllib.parse.parse_qs(parsed.query).get("id", [""])[0]
                    selected = [fixture.clients[int(i)] for i in ids.split(",")
                                if i and int(i) in fixture.clients]
                    data = {"success": True, "msg": "", "obj": {"clients": copy.deepcopy(selected)}}
                raw = json.dumps(data).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


def run(args):
    dsn = os.environ["DATABASE_URL"]
    # Refuse non-local databases and existing application records.
    parsed_dsn = urllib.parse.urlsplit(dsn)
    if parsed_dsn.scheme not in ("postgres", "postgresql") or parsed_dsn.hostname not in ("127.0.0.1", "localhost"):
        raise SystemExit("DATABASE_URL must explicitly use localhost/127.0.0.1")
    result = subprocess.run(["psql", dsn, "-Atc", "SELECT to_regclass('public.shortlink_usage_bindings')"],
                            check=True, text=True, capture_output=True)
    if result.stdout.strip():
        raise SystemExit("Use a fresh disposable database (usage tables already exist)")
    directory = Path(tempfile.mkdtemp(prefix="subconverter-usage-test-"))
    print("Integration artifacts:", directory, flush=True)
    fixture = Fixture()
    processes = []
    logs = []
    try:
        pki = directory / "pki"
        subprocess.run(["bash", str(ROOT / "scripts/create_usage_pki.sh"), str(pki), "127.0.0.1"],
                       check=True, stdout=subprocess.DEVNULL)
        token_file = directory / "token"
        token_file.write_text(fixture.token)
        token_file.chmod(0o600)
        identity = directory / "identity.key"
        identity.write_text(secrets.token_hex(32))
        identity.chmod(0o600)
        adapter_port, app_port = free_port(), args.port or free_port()
        adapter_config = {
            "listen_addr": "127.0.0.1:" + str(adapter_port),
            "sui_api_base_url": "http://127.0.0.1:" + str(fixture.server.server_port) + "/app/apiv2",
            "sui_token_file": str(token_file), "identity_hmac_key_file": str(identity),
            "instance_id": str(uuid.uuid4()), "tls_cert_file": str(pki / "server.crt"),
            "tls_key_file": str(pki / "server.key"), "client_ca_file": str(pki / "ca.crt"),
            "allowed_client_identity": "spiffe://subconverter/usage-client",
        }
        config_file = directory / "adapter.json"
        config_file.write_text(json.dumps(adapter_config))
        log = open(directory / "adapter.log", "wb")
        logs.append(log)
        processes.append(subprocess.Popen([str(Path(args.adapter).resolve()), "-config", str(config_file)],
                                          stdout=log, stderr=log))
        context = ssl.create_default_context(cafile=str(pki / "ca.crt"))
        context.load_cert_chain(str(pki / "client.crt"), str(pki / "client.key"))
        adapter_url = "https://127.0.0.1:" + str(adapter_port)
        eventually(lambda: request(adapter_url + "/healthz", context=context)[0] == 200)
        status, normalized, _ = request(adapter_url + "/v1/clients/query", "POST",
                                         {"schema_version": 1, "client_ids": ["3", "4", "99"]}, context=context)
        assert status == 200 and normalized["missing_client_ids"] == ["99"]
        assert len(normalized["items"]) == 2
        assert "PRIVATE-" not in json.dumps(normalized)
        assert normalized["items"][1]["metrics"]["enabled"] is False
        no_cert = ssl.create_default_context(cafile=str(pki / "ca.crt"))
        try:
            request(adapter_url + "/healthz", context=no_cert)
        except (OSError, urllib.error.URLError):
            pass
        else:
            raise AssertionError("mTLS accepted a caller without a client certificate")
        providers = directory / "providers.json"
        providers.write_text(json.dumps({"schema_version": 1, "providers": [{
            "provider_id": "fixture", "label": "Fixture", "endpoint": adapter_url,
            "ca_file": str(pki / "ca.crt"), "client_cert_file": str(pki / "client.crt"),
            "client_key_file": str(pki / "client.key"),
        }]}))
        # Copy runtime assets so tests do not alter tracked base/cache or preferences.
        runtime = directory / "base"
        shutil.copytree(ROOT / "base", runtime, ignore=shutil.ignore_patterns("cache"))
        pref = runtime / "pref.toml"
        pref.write_text(pref.read_text().replace('listen = "0.0.0.0"', 'listen = "127.0.0.1"'))
        admin_token = secrets.token_urlsafe(32)
        app_url = "http://127.0.0.1:" + str(app_port)
        env = dict(os.environ, API_MODE="true", API_TOKEN=admin_token,
                   SHORTLINK_ENABLED="true", SHORTLINK_ENCRYPTION_KEY=secrets.token_hex(32),
                   SHORTLINK_TRUST_ACCESS_HEADER="true", SHORTLINK_USAGE_ENABLED="true",
                   SHORTLINK_USAGE_PROVIDERS_FILE=str(providers), PORT=str(app_port),
                   SHORTLINK_PORTAL_ORIGIN=app_url,
                   PUBLIC_BASE_URL=app_url, SHORTLINK_ADMIN_SUBJECTS="usage-admin@example.invalid")
        env.pop("SHORTLINK_DEV_USER", None)
        if args.serve:
            env["SHORTLINK_DEV_USER"] = "usage-a@example.invalid"
            env["SHORTLINK_ADMIN_SUBJECTS"] += ",usage-a@example.invalid"
        log = open(directory / "subconverter.log", "wb")
        logs.append(log)
        processes.append(subprocess.Popen([str(Path(args.binary).resolve()), "-f", str(runtime / "pref.toml")],
                                          cwd=runtime, env=env, stdout=log, stderr=log))
        eventually(lambda: request(app_url + "/version")[0] == 200)
        admin = {"Authorization": "Bearer " + admin_token}

        def api(path, method="GET", body=None, headers=None):
            return request(app_url + path, method, body, headers)

        keys = {}
        for owner in ("usage-a@example.invalid", "usage-b@example.invalid"):
            status, data, _ = api("/api/keys", "POST", {"owner": owner, "name": "usage-fixture"}, admin)
            assert status == 201, (status, data)
            keys[owner] = {"X-API-Key": data["api_key"]}
        a, b = keys.values()
        status, preserved, _ = api("/api/short-links", "POST", {
            "name": "immutable-usage-regression", "target": "clash", "expires_in": 3600,
            "links": ["ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#immutable"]}, a)
        assert status == 201
        preserved_status, preserved_body, preserved_headers = request(preserved["short_url"])
        assert preserved_status == 200
        if not args.serve:
            smoke = subprocess.run(["bash", str(ROOT / "tests/shortlink_api_smoke.sh")],
                                   env=dict(env, BASE_URL=app_url, API_KEY=a["X-API-Key"],
                                            ASSERT_LITE_OUTPUT="1"),
                                   capture_output=True, text=True, timeout=120)
            assert smoke.returncode == 0, (smoke.stdout + smoke.stderr).replace(a["X-API-Key"], "[TEST-KEY]")
            print(smoke.stdout.strip(), flush=True)
            query = urllib.parse.urlencode({"target": "clash", "expand": "false",
                "config": "config/default_clash_lite.ini",
                "url": "ss://YWVzLTEyOC1nY206Zml4dHVyZQ==@198.51.100.10:443#usage-regression"})
            status, converted, _ = api("/sub?" + query)
            assert status == 200 and b"usage-regression" in converted
            assert api("/refreshrules")[0] in (401, 403)
        if not args.serve:
            assert api("/api/usage")[0] == 401
        assert api("/api/usage?client_id=4", headers=a)[0] == 400
        assert api("/api/admin/usage-bindings", headers=b)[0] == 403
        assert api("/api/usage", headers=a)[1]["state"] == "unbound"

        bindings = []
        for owner, client_id in (("usage-a@example.invalid", "3"), ("usage-b@example.invalid", "4")):
            body = {"owner_subject": owner, "provider_id": "fixture", "client_id": client_id,
                    "label": "Usage fixture " + client_id}
            status, preview, _ = api("/api/admin/usage-bindings/preview", "POST", body, admin)
            assert status == 200, (status, preview)
            body.update({k: preview[k] for k in ("expected_instance_id", "expected_identity_fingerprint")})
            status, binding, _ = api("/api/admin/usage-bindings", "POST", body, admin)
            assert status == 201, (status, binding)
            bindings.append(binding)
            status, _, _ = api("/api/admin/usage-bindings", "POST", body, admin)
            assert status == 409, status
        snapshot = eventually(lambda: (lambda data: data if data.get("items") and
                              data["items"][0]["availability"] == "fresh" else None)(api("/api/usage", headers=a)[1]))
        assert len(snapshot["items"]) == 1
        assert snapshot["items"][0]["metrics"]["used_bytes"] == str(10 * 1024 ** 3)
        b_snapshot = eventually(lambda: (lambda data: data if data.get("items") and
                                data["items"][0]["metrics"] else None)(api("/api/usage", headers=b)[1]))
        assert len(b_snapshot["items"]) == 1 and b_snapshot["items"][0]["metrics"]["enabled"] is False
        assert "PRIVATE-" not in json.dumps(snapshot)
        assert not {"client_id", "provider_id", "owner_subject", "identity_fingerprint"}.intersection(snapshot["items"][0])
        assert api("/api/usage", headers=admin)[1]["items"] == []
        binding = bindings[0]
        path = "/api/admin/usage-bindings/" + binding["id"]
        assert api(path, "POST", {"label": "Renamed"}, admin)[0] == 428
        status, renamed, _ = api(path, "POST", {"label": "Renamed"}, dict(admin, **{"If-Match": '"' + binding["revision"] + '"'}))
        assert status == 200, (status, renamed)
        assert api(path, "DELETE", headers=dict(admin, **{"If-Match": '"' + binding["revision"] + '"'}))[0] == 412
        assert api(path, "DELETE", headers=dict(admin, Origin="https://evil.example", **{"If-Match": '"' + renamed["revision"] + '"'}))[0] == 403
        assert api("/api/admin/usage-bindings/preview", "POST", {}, {"Cf-Access-Authenticated-User-Email": "usage-admin@example.invalid"})[0] == 403
        print("PASS: mTLS, adapter schema, disabled client, owner isolation, duplicate binding, revisions, CSRF", flush=True)

        if args.serve:
            print("Fixture UI:", app_url, flush=True)
            while True:
                time.sleep(1)

        # Failures and identity changes must be reflected without rewriting bindings.
        fixture.fail = True
        eventually(lambda: api("/api/usage", headers=b)[1]["items"][0]["availability"] == "stale", timeout=75)
        subprocess.run(["psql", dsn, "-v", "ON_ERROR_STOP=1", "-c",
            "UPDATE shortlink_usage_cache SET observed_at=NOW()-INTERVAL '901 seconds' "
            "WHERE binding_id=" + bindings[1]["id"]], check=True, capture_output=True)
        expired_cache = api("/api/usage", headers=b)[1]["items"][0]
        assert expired_cache["availability"] == "unavailable" and expired_cache["metrics"] is None
        fixture.fail = False
        fixture.clients[4]["name"] = "PRIVATE-REPLACED-IDENTITY"
        eventually(lambda: api("/api/usage", headers=b)[1]["items"][0]["availability"] == "binding_invalid", timeout=100)
        assert api("/api/usage", headers=b)[1]["items"][0]["metrics"] is None
        # A display-only rename must never clear permanent identity invalidation.
        invalid_binding = bindings[1]
        status, invalid_renamed, _ = api("/api/admin/usage-bindings/" + invalid_binding["id"],
            "POST", {"label": "Still invalid"},
            dict(admin, **{"If-Match": '"' + invalid_binding["revision"] + '"'}))
        assert status == 200 and invalid_renamed["availability"] == "binding_invalid"
        assert api("/api/usage", headers=b)[1]["items"][0]["availability"] == "binding_invalid"
        assert api(path, "DELETE", headers=dict(admin, **{"If-Match": '"' + renamed["revision"] + '"'}))[0] == 204
        assert api("/api/usage", headers=a)[1]["items"] == []
        status, after_body, after_headers = request(preserved["short_url"])
        assert status == 200 and after_body == preserved_body
        assert after_headers["Content-Type"] == preserved_headers["Content-Type"]
        assert request(preserved["short_url"], "HEAD")[0] == 200
        print("PASS: outage stale cache, identity invalidation, revoke removes usage", flush=True)
        print("shortlink-usage-integration-ok", flush=True)
    finally:
        for process in reversed(processes):
            process.terminate()
            try:
                process.wait(timeout=12)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        fixture.server.shutdown()
        for log in logs:
            log.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default=str(ROOT / "build/subconverter"))
    parser.add_argument("--adapter", required=True)
    parser.add_argument("--serve", action="store_true", help="Keep a demo with synthetic accounts running for UI QA")
    parser.add_argument("--port", type=int)
    run(parser.parse_args())
