package main

import (
	"context"
	"crypto/rand"
	"crypto/rsa"
	"encoding/base64"
	"encoding/json"
	"io"
	"log"
	"math/big"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coreos/go-oidc/v3/oidc"
	"github.com/golang-jwt/jwt/v5"
)

const testAudience = "6f0f28fca3d640979c27b8b72c6df760"

type jwtFixture struct {
	issuer   string
	key      *rsa.PrivateKey
	wrongKey *rsa.PrivateKey
	verifier tokenVerifier
	close    func()
}

func newJWTFixture(t *testing.T) jwtFixture {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	wrongKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}

	mux := http.NewServeMux()
	server := httptest.NewTLSServer(mux)
	mux.HandleFunc("/cdn-cgi/access/certs", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"keys": []any{rsaJWK(&key.PublicKey, "test-key")},
		})
	})
	ctx := oidc.ClientContext(context.Background(), server.Client())
	keySet := oidc.NewRemoteKeySet(ctx, server.URL+"/cdn-cgi/access/certs")
	verifier := oidc.NewVerifier(server.URL, keySet, &oidc.Config{ClientID: testAudience})
	return jwtFixture{
		issuer:   server.URL,
		key:      key,
		wrongKey: wrongKey,
		verifier: verifier,
		close:    server.Close,
	}
}

func rsaJWK(key *rsa.PublicKey, kid string) map[string]string {
	exponent := big.NewInt(int64(key.E)).Bytes()
	return map[string]string{
		"kty": "RSA",
		"use": "sig",
		"alg": "RS256",
		"kid": kid,
		"n":   base64.RawURLEncoding.EncodeToString(key.N.Bytes()),
		"e":   base64.RawURLEncoding.EncodeToString(exponent),
	}
}

func (f jwtFixture) sign(t *testing.T, key *rsa.PrivateKey, claims jwt.MapClaims) string {
	t.Helper()
	token := jwt.NewWithClaims(jwt.SigningMethodRS256, claims)
	token.Header["kid"] = "test-key"
	signed, err := token.SignedString(key)
	if err != nil {
		t.Fatal(err)
	}
	return signed
}

func baseClaims(issuer string, now time.Time) jwt.MapClaims {
	return jwt.MapClaims{
		"iss":   issuer,
		"aud":   []string{testAudience},
		"exp":   now.Add(5 * time.Minute).Unix(),
		"nbf":   now.Add(-time.Minute).Unix(),
		"type":  "app",
		"email": "signed@example.com",
	}
}

func TestVerifyJWTClaims(t *testing.T) {
	fixture := newJWTFixture(t)
	defer fixture.close()
	now := time.Now().UTC().Truncate(time.Second)

	tests := []struct {
		name   string
		mutate func(jwt.MapClaims)
		key    *rsa.PrivateKey
	}{
		{name: "wrong issuer", mutate: func(c jwt.MapClaims) { c["iss"] = "https://other.cloudflareaccess.com" }},
		{name: "wrong audience", mutate: func(c jwt.MapClaims) { c["aud"] = []string{"wrong-audience"} }},
		{name: "expired", mutate: func(c jwt.MapClaims) { c["exp"] = now.Add(-time.Minute).Unix() }},
		{name: "expiry missing", mutate: func(c jwt.MapClaims) { delete(c, "exp") }},
		{name: "nbf in future", mutate: func(c jwt.MapClaims) { c["nbf"] = now.Add(time.Minute).Unix() }},
		{name: "nbf missing", mutate: func(c jwt.MapClaims) { delete(c, "nbf") }},
		{name: "wrong signature", key: fixture.wrongKey},
		{name: "email missing", mutate: func(c jwt.MapClaims) { delete(c, "email") }},
		{name: "unsafe email", mutate: func(c jwt.MapClaims) { c["email"] = "signed@example.com\r\nX-Forged: yes" }},
		{name: "token type missing", mutate: func(c jwt.MapClaims) { delete(c, "type") }},
		{name: "wrong token type", mutate: func(c jwt.MapClaims) { c["type"] = "org" }},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			claims := baseClaims(fixture.issuer, now)
			if test.mutate != nil {
				test.mutate(claims)
			}
			key := test.key
			if key == nil {
				key = fixture.key
			}
			assertion := fixture.sign(t, key, claims)
			response := requestVerify(t, fixture.verifier, now, http.Header{
				"Cf-Access-Jwt-Assertion": []string{assertion},
			})
			if response.Code != http.StatusUnauthorized {
				t.Fatalf("status = %d, want 401", response.Code)
			}
			if got := response.Header().Get("X-Verified-Email"); got != "" {
				t.Fatalf("X-Verified-Email = %q, want empty", got)
			}
		})
	}
}

func TestVerifyUsesOnlySignedEmail(t *testing.T) {
	fixture := newJWTFixture(t)
	defer fixture.close()
	now := time.Now().UTC().Truncate(time.Second)
	assertion := fixture.sign(t, fixture.key, baseClaims(fixture.issuer, now))

	response := requestVerify(t, fixture.verifier, now, http.Header{
		"Cf-Access-Jwt-Assertion":            []string{assertion},
		"X-Verified-Email":                   []string{"forged@example.com"},
		"Cf-Access-Authenticated-User-Email": []string{"also-forged@example.com"},
	})
	if response.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204", response.Code)
	}
	if got := response.Header().Get("X-Verified-Email"); got != "signed@example.com" {
		t.Fatalf("X-Verified-Email = %q, want signed claim", got)
	}
}

func TestExplicitCredentialsBypassWithoutIdentity(t *testing.T) {
	tests := []struct {
		name   string
		header http.Header
	}{
		{name: "bearer", header: http.Header{"Authorization": []string{"Bearer backend-token"}, "X-Verified-Email": []string{"forged@example.com"}}},
		{name: "api key", header: http.Header{"X-Api-Key": []string{"backend-key"}, "X-Verified-Email": []string{"forged@example.com"}}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response := requestVerify(t, rejectingVerifier{}, time.Now(), test.header)
			if response.Code != http.StatusNoContent {
				t.Fatalf("status = %d, want 204", response.Code)
			}
			if got := response.Header().Get("X-Verified-Email"); got != "" {
				t.Fatalf("X-Verified-Email = %q, want empty", got)
			}
		})
	}
}

func TestArbitraryAuthorizationDoesNotBypass(t *testing.T) {
	response := requestVerify(t, rejectingVerifier{}, time.Now(), http.Header{
		"Authorization": []string{"Basic Zm9vOmJhcg=="},
	})
	if response.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", response.Code)
	}
}

func TestAssertionSizeLimit(t *testing.T) {
	response := requestVerify(t, rejectingVerifier{}, time.Now(), http.Header{
		"Cf-Access-Jwt-Assertion": []string{strings.Repeat("x", maxAssertionBytes+1)},
	})
	if response.Code != http.StatusUnauthorized {
		t.Fatalf("status = %d, want 401", response.Code)
	}
}

func TestHealthz(t *testing.T) {
	handler := verifyHandler{verifier: rejectingVerifier{}, now: time.Now}
	request := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204", response.Code)
	}
}

func TestDeniedLogDoesNotContainSensitiveValues(t *testing.T) {
	var output strings.Builder
	handler := verifyHandler{
		verifier: rejectingVerifier{},
		now:      time.Now,
		logger:   log.New(&output, "", 0),
	}
	request := httptest.NewRequest(http.MethodGet, "/verify", nil)
	request.Header.Set("Cf-Access-Jwt-Assertion", "secret.jwt.value")
	request.Header.Set("X-Verified-Email", "private@example.com")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if strings.Contains(output.String(), "secret.jwt.value") || strings.Contains(output.String(), "private@example.com") {
		t.Fatalf("log contains sensitive value: %q", output.String())
	}
}

type rejectingVerifier struct{}

func (rejectingVerifier) Verify(context.Context, string) (*oidc.IDToken, error) {
	return nil, io.ErrUnexpectedEOF
}

func requestVerify(t *testing.T, verifier tokenVerifier, now time.Time, header http.Header) *httptest.ResponseRecorder {
	t.Helper()
	handler := verifyHandler{verifier: verifier, now: func() time.Time { return now }}
	request := httptest.NewRequest(http.MethodGet, "/verify", nil)
	request.Header = header.Clone()
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	return response
}
