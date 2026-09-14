package main

import (
	"bytes"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

type testPKI struct {
	ca       *x509.Certificate
	caKey    *ecdsa.PrivateKey
	caPool   *x509.CertPool
	server   tls.Certificate
	allowed  tls.Certificate
	wrongURI tls.Certificate
	spoofCN  tls.Certificate
}

func makeTestPKI(t *testing.T, identity string) testPKI {
	t.Helper()
	now := time.Now()
	caKey, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	ca := &x509.Certificate{SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "test-ca"}, NotBefore: now.Add(-time.Hour), NotAfter: now.Add(time.Hour), IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign}
	caDER, err := x509.CreateCertificate(rand.Reader, ca, ca, &caKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	ca, _ = x509.ParseCertificate(caDER)
	pool := x509.NewCertPool()
	pool.AddCert(ca)
	makeCert := func(serial int64, server bool, uri, cn string) tls.Certificate {
		key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		tmpl := &x509.Certificate{SerialNumber: big.NewInt(serial), Subject: pkix.Name{CommonName: cn}, NotBefore: now.Add(-time.Hour), NotAfter: now.Add(time.Hour), KeyUsage: x509.KeyUsageDigitalSignature}
		if server {
			tmpl.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}
			tmpl.IPAddresses = []net.IP{net.ParseIP("127.0.0.1")}
		} else {
			tmpl.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
			if uri != "" {
				parsed, _ := url.Parse(uri)
				tmpl.URIs = []*url.URL{parsed}
			}
		}
		der, err := x509.CreateCertificate(rand.Reader, tmpl, ca, &key.PublicKey, caKey)
		if err != nil {
			t.Fatal(err)
		}
		keyDER, _ := x509.MarshalPKCS8PrivateKey(key)
		certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
		keyPEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
		cert, err := tls.X509KeyPair(certPEM, keyPEM)
		if err != nil {
			t.Fatal(err)
		}
		return cert
	}
	return testPKI{ca: ca, caKey: caKey, caPool: pool, server: makeCert(2, true, "", "server"), allowed: makeCert(3, false, identity, "client"), wrongURI: makeCert(4, false, "spiffe://shortlink/wrong", "client"), spoofCN: makeCert(5, false, "", identity)}
}

func TestMTLSIdentityAndSecretRedaction(t *testing.T) {
	identity := "spiffe://shortlink/usage-collector"
	pki := makeTestPKI(t, identity)
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write(envelope(validClient(1))) }))
	defer upstream.Close()
	base, _ := url.Parse(upstream.URL + "/")
	var logs bytes.Buffer
	cfg := runtimeConfig{config: config{InstanceID: testInstanceID, AllowedClientIdentity: identity, RequestBodyLimitBytes: 16 << 10, UpstreamBodyLimitBytes: 4 << 20, ResponseBodyLimitBytes: 256 << 10, MaxConcurrentUpstream: 2, RateLimitPerMinute: 60, RateLimitBurst: 10}, baseURL: base, token: "SECRET_TOKEN", identityHMACKey: []byte("0123456789abcdef0123456789abcdef"), connectTimeout: time.Second, upstreamTimeout: 3 * time.Second}
	adapter := newAdapterServer(cfg, log.New(&logs, "", 0))
	server := httptest.NewUnstartedServer(adapter.handler())
	server.Config.ErrorLog = log.New(io.Discard, "", 0)
	server.TLS = &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{pki.server}, ClientAuth: tls.RequireAndVerifyClientCert, ClientCAs: pki.caPool}
	server.StartTLS()
	defer server.Close()
	request := func(cert *tls.Certificate, path string, body io.Reader) (*http.Response, error) {
		tlsCfg := &tls.Config{MinVersion: tls.VersionTLS12, RootCAs: pki.caPool}
		if cert != nil {
			tlsCfg.Certificates = []tls.Certificate{*cert}
		}
		client := &http.Client{Transport: &http.Transport{TLSClientConfig: tlsCfg}}
		req, _ := http.NewRequest(http.MethodPost, server.URL+path, body)
		req.Header.Set("Content-Type", "application/json")
		if path == "/healthz" {
			req.Method = http.MethodGet
		}
		return client.Do(req)
	}
	resp, err := request(&pki.allowed, "/v1/clients/query", strings.NewReader(`{"schema_version":1,"client_ids":["1"]}`))
	if err != nil {
		t.Fatal(err)
	}
	responseBody, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status=%d body=%s", resp.StatusCode, responseBody)
	}
	for _, secret := range []string{"SECRET_TOKEN", "SECRET_CLIENT_NAME", "SECRET_CONFIG", "SECRET_LINK"} {
		if bytes.Contains(responseBody, []byte(secret)) || strings.Contains(logs.String(), secret) {
			t.Fatalf("secret %q leaked", secret)
		}
	}
	var decoded queryResponse
	if err := json.Unmarshal(responseBody, &decoded); err != nil || len(decoded.Items) != 1 {
		t.Fatalf("invalid response: %s", responseBody)
	}
	for name, cert := range map[string]*tls.Certificate{"wrong_uri": &pki.wrongURI, "spoof_cn": &pki.spoofCN} {
		t.Run(name, func(t *testing.T) {
			resp, err := request(cert, "/healthz", nil)
			if err != nil {
				t.Fatal(err)
			}
			defer resp.Body.Close()
			if resp.StatusCode != http.StatusForbidden {
				t.Fatalf("status=%d", resp.StatusCode)
			}
		})
	}
	if _, err := request(nil, "/healthz", nil); err == nil {
		t.Fatal("request without client certificate unexpectedly completed TLS")
	}
}

func TestRequestValidationAndRateLimit(t *testing.T) {
	identity := "spiffe://shortlink/usage-collector"
	pki := makeTestPKI(t, identity)
	base, _ := url.Parse("http://127.0.0.1/")
	cfg := runtimeConfig{config: config{AllowedClientIdentity: identity, RequestBodyLimitBytes: 1024, UpstreamBodyLimitBytes: 4096, ResponseBodyLimitBytes: 4096, RateLimitPerMinute: 60, RateLimitBurst: 1, MaxConcurrentUpstream: 1}, baseURL: base, connectTimeout: time.Second, upstreamTimeout: time.Second}
	adapter := newAdapterServer(cfg, log.New(io.Discard, "", 0))
	cert := pki.allowed.Leaf
	if cert == nil {
		cert, _ = x509.ParseCertificate(pki.allowed.Certificate[0])
	}
	do := func(body string) *httptest.ResponseRecorder {
		req := httptest.NewRequest(http.MethodPost, "/v1/clients/query", strings.NewReader(body))
		req.Header.Set("Content-Type", "application/json")
		req.TLS = &tls.ConnectionState{PeerCertificates: []*x509.Certificate{cert}}
		rec := httptest.NewRecorder()
		adapter.handler().ServeHTTP(rec, req)
		return rec
	}
	for _, body := range []string{`{"schema_version":2,"client_ids":["1"]}`, `{"schema_version":1,"client_ids":["01"]}`, `{"schema_version":1,"client_ids":["1","1"]}`, `{"schema_version":1,"client_ids":["1"],"extra":true}`} {
		adapter.limiter = newCertificateLimiter(60, 1)
		if rec := do(body); rec.Code != http.StatusBadRequest {
			t.Fatalf("body=%s status=%d", body, rec.Code)
		}
	}
	adapter.limiter = newCertificateLimiter(60, 1)
	first := do(`{}`)
	if first.Code != http.StatusBadRequest {
		t.Fatal(first.Code)
	}
	second := do(`{}`)
	if second.Code != http.StatusTooManyRequests || second.Header().Get("Retry-After") == "" {
		t.Fatalf("status=%d headers=%v", second.Code, second.Header())
	}
}

func TestLoadConfigRejectsUnknownAndNonLoopbackHTTP(t *testing.T) {
	dir := t.TempDir()
	token := filepath.Join(dir, "token")
	key := filepath.Join(dir, "key")
	_ = os.WriteFile(token, []byte("token\n"), 0600)
	_ = os.WriteFile(key, []byte("3031323334353637383961626364656630313233343536373839616263646566\n"), 0600)
	base := map[string]any{"listen_addr": "127.0.0.1:8443", "sui_api_base_url": "http://127.0.0.1:2095/api/", "sui_token_file": token, "instance_id": testInstanceID, "identity_hmac_key_file": key, "tls_cert_file": "server.crt", "tls_key_file": "server.key", "client_ca_file": "ca.crt", "allowed_client_identity": "spiffe://shortlink/usage-collector"}
	write := func(name string, value any) string {
		path := filepath.Join(dir, name)
		data, _ := json.Marshal(value)
		_ = os.WriteFile(path, data, 0600)
		return path
	}
	if cfg, err := loadConfig(write("valid.json", base)); err != nil || cfg.token != "token" || cfg.RequestBodyLimitBytes != 16<<10 {
		t.Fatalf("cfg=%+v err=%v", cfg, err)
	}
	base["unknown"] = true
	if _, err := loadConfig(write("unknown.json", base)); err == nil {
		t.Fatal("unknown config field accepted")
	}
	delete(base, "unknown")
	base["sui_api_base_url"] = "http://192.0.2.1/api/"
	if _, err := loadConfig(write("remote-http.json", base)); err == nil {
		t.Fatal("remote plaintext URL accepted")
	}
}
