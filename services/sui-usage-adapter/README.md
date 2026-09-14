# s-ui usage adapter

This service exposes the read-only v1 usage contract from
`docs/short-link-usage-design.md`. It accepts mTLS-authenticated requests from
one configured URI SAN and queries only the configured s-ui
`GET clients?id=...` endpoint.

## Build and test

```sh
go build -o sui-usage-adapter .
go test -race ./...
```

Run it with:

```sh
./sui-usage-adapter -config /etc/sui-usage-adapter/config.json
```

The configuration is strict JSON. Unknown fields and unsafe values stop
startup. See `config.example.json`. The optional limit fields and their v1
defaults are:

| Field | Default |
| --- | ---: |
| `connect_timeout_ms` | 1000 |
| `upstream_timeout_ms` | 3000 |
| `request_body_limit_bytes` | 16384 |
| `upstream_response_limit_bytes` | 4194304 |
| `response_body_limit_bytes` | 262144 |
| `max_concurrent_upstream` | 2 |
| `rate_limit_per_minute` | 60 |
| `rate_limit_burst` | 10 |
| `shutdown_timeout_seconds` | 10 |

Upstream URLs must use a literal loopback IP. HTTPS additionally uses Go's
normal certificate and IP SAN verification. Redirects, environment proxies,
automatic retries, and compressed upstream responses are disabled.

The s-ui token file contains one token. The identity HMAC key file contains
exactly 64 hexadecimal characters encoding a random 32-byte key. Both files
must be regular files inaccessible to group and other users. For example, a
key can be generated directly into a protected credential file with
`openssl rand -hex 32` under an appropriate restrictive umask.

Both `GET /healthz` and `POST /v1/clients/query` require a client certificate
signed by `client_ca_file`. At least one URI SAN on the leaf certificate must
exactly equal `allowed_client_identity`; a matching common name is ignored.

The query body is:

```json
{"schema_version":1,"client_ids":["1","2"]}
```

It accepts 1 to 100 distinct, canonical positive decimal IDs. A requested ID
appears exactly once in `items`, `missing_client_ids`, or `errors`. The only v1
per-client error is `invalid_data`. Upstream transport, authentication,
envelope, and size failures fail the whole request and never expose the s-ui
message or response body.
