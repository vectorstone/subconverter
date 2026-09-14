# Cloudflare Access origin verifier

`access-auth` is a loopback-only HTTP service for Nginx `auth_request`. It verifies
Cloudflare Access application JWTs before exposing the signed `email` claim as
`X-Verified-Email`.

Signature verification uses `github.com/coreos/go-oidc/v3/oidc` with a
`RemoteKeySet`. The verifier does not perform OIDC discovery and does not use a
JWT-provided `jku`: it always loads keys from:

```text
<issuer>/cdn-cgi/access/certs
```

The OIDC verifier checks the signature, issuer, audience, and expiry. The service
also requires `nbf` to be present and current, requires the signed `type` claim to
equal `app`, and accepts only a nonempty ASCII email address that is safe to put in
an HTTP response header. Assertions larger than 16 KiB are rejected.

## Configuration

Pass a JSON file with `-config`. The default path is
`/etc/subconverter/access-auth.json`.

```json
{
  "listen_addr": "127.0.0.1:15053",
  "issuer": "https://example.cloudflareaccess.com",
  "audience": "replace-with-the-access-application-aud"
}
```

- `listen_addr` is optional and defaults to `127.0.0.1:15053`. A numeric loopback
  address is mandatory; wildcard and non-loopback listeners are rejected.
- `issuer` must be an HTTPS origin with no path, query, fragment, or user info.
  A Cloudflare Access tenant issuer such as
  `https://example.cloudflareaccess.com` is the normal production value.
- `audience` is the Access application AUD tag and must be nonempty.

The JWKS client disables environment proxies and redirects, requires TLS 1.2 or
newer, and has a five-second request timeout.

## HTTP contract

`GET /verify` returns:

- `204` plus `X-Verified-Email: <signed email>` for a valid Access application
  JWT supplied in `Cf-Access-Jwt-Assertion`.
- `204` without `X-Verified-Email` when a nonempty `X-API-Key` or an explicit
  nonempty `Authorization: Bearer ...` is present. The verifier does not validate
  these credentials; the subconverter backend must receive and authenticate them.
- `401` for a missing or invalid JWT when the explicit credential branch does not
  apply.

An arbitrary Authorization scheme, an empty Bearer value, and incoming email
headers do not grant access. Nginx must clear all client-provided identity headers,
use only the `X-Verified-Email` response from the auth subrequest for the Access
branch, and keep the identity header empty for the explicit credential branch.

`GET /healthz` returns `204` without fetching JWKS.

Logs contain only stable failure categories. JWTs, request headers, verifier error
details, and email values are never logged.

## Build and test

The service is an independent Go module and requires Go 1.24 or newer.

```sh
cd services/access-auth
go test ./...
go build -o access-auth .
./access-auth -config ./access-auth.json
```

Local smoke checks after startup:

```sh
curl -i http://127.0.0.1:15053/healthz
curl -i http://127.0.0.1:15053/verify
curl -i -H 'X-API-Key: backend-validates-this' http://127.0.0.1:15053/verify
```

The expected statuses are `204`, `401`, and `204`. The final response must not
contain `X-Verified-Email`. A production smoke test must additionally send a real
Access JWT and confirm that wrong audience, expired, future-`nbf`, and incorrectly
signed tokens return `401`.
