package main

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"math"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
	"time"
)

const testInstanceID = "11111111-1111-4111-8111-111111111111"

func validClient(id int64) map[string]any {
	return map[string]any{
		"id": id, "enable": true, "name": "SECRET_CLIENT_NAME", "config": "SECRET_CONFIG",
		"links": []string{"SECRET_LINK"}, "volume": int64(100), "expiry": int64(2000),
		"down": int64(20), "up": int64(30), "createdAt": int64(123), "onlineAt": int64(1900),
		"delayStart": false, "autoReset": false, "resetDays": 0, "nextReset": int64(0),
	}
}

func envelope(clients ...map[string]any) []byte {
	body, _ := json.Marshal(map[string]any{"success": true, "obj": map[string]any{"clients": clients}})
	return body
}

func testUpstream(body []byte) *upstreamClient {
	return &upstreamClient{instanceID: testInstanceID, hmacKey: []byte("0123456789abcdef0123456789abcdef"), bodyLimit: 4 << 20}
}

func TestParseResponseNormalizesMetricsAndFingerprint(t *testing.T) {
	c := validClient(1)
	c["volume"] = int64(50)
	c["expiry"] = int64(900)
	c["autoReset"] = true
	c["nextReset"] = int64(0)
	up := testUpstream(nil)
	result, err := up.parseResponse(envelope(c), []string{"1", "2"}, 1000)
	if err != nil {
		t.Fatal(err)
	}
	if result.ObservedAt != 1000 || len(result.Items) != 1 || len(result.Missing) != 1 || result.Missing[0] != "2" || len(result.Errors) != 0 {
		t.Fatalf("unexpected result: %+v", result)
	}
	m := result.Items[0].Metrics
	if m.UsedBytes != "50" || m.RemainingBytes == nil || *m.RemainingBytes != "0" || m.OverLimitBytes != "0" {
		t.Fatalf("unexpected byte normalization: %+v", m)
	}
	if got := strings.Join(m.Conditions, ","); got != "quota_at_limit,expired,invalid_reset_policy" {
		t.Fatalf("unexpected conditions: %s", got)
	}
	canonical, _ := json.Marshal([]string{testInstanceID, "1", "123", "SECRET_CLIENT_NAME"})
	mac := hmac.New(sha256.New, up.hmacKey)
	_, _ = mac.Write(canonical)
	wantFingerprint := hex.EncodeToString(mac.Sum(nil))
	if result.Items[0].IdentityFingerprint != wantFingerprint {
		t.Fatalf("fingerprint=%s want=%s", result.Items[0].IdentityFingerprint, wantFingerprint)
	}
	encoded, _ := json.Marshal(result)
	for _, secret := range []string{"SECRET_CLIENT_NAME", "SECRET_CONFIG", "SECRET_LINK"} {
		if strings.Contains(string(encoded), secret) {
			t.Fatalf("response leaked %q", secret)
		}
	}
}

func TestNormalizeResetDueIncludesZeroNextReset(t *testing.T) {
	c := validClient(7)
	c["autoReset"] = true
	c["resetDays"] = 30
	c["nextReset"] = int64(0)
	c["expiry"] = int64(0)
	c["volume"] = int64(0)
	result, err := testUpstream(nil).parseResponse(envelope(c), []string{"7"}, 1000)
	if err != nil {
		t.Fatal(err)
	}
	m := result.Items[0].Metrics
	if m.LimitBytes != nil || m.RemainingBytes != nil || m.ExpiresAt != nil || m.NextResetAt != nil || strings.Join(m.Conditions, ",") != "reset_due" {
		t.Fatalf("unexpected metrics: %+v", m)
	}
}

func TestDisabledDelayAndInvalidResetPolicyRemainSeparate(t *testing.T) {
	c := validClient(9)
	c["enable"] = false
	c["delayStart"] = true
	c["autoReset"] = true
	c["resetDays"] = -1
	c["nextReset"] = int64(0)
	c["volume"] = int64(40)
	result, err := testUpstream(nil).parseResponse(envelope(c), []string{"9"}, 1000)
	if err != nil {
		t.Fatal(err)
	}
	m := result.Items[0].Metrics
	if m.Enabled || !m.ActivationPending || !m.AutoReset || m.ResetDays != 0 {
		t.Fatalf("unexpected state fields: %+v", m)
	}
	if got := strings.Join(m.Conditions, ","); got != "quota_exceeded,invalid_reset_policy" {
		t.Fatalf("conditions=%s", got)
	}
}

func TestInvalidClientDataIsScopedButBrokenEnvelopeFails(t *testing.T) {
	tests := []struct {
		name   string
		mutate func(map[string]any)
	}{
		{"missing", func(c map[string]any) { delete(c, "up") }},
		{"string_number", func(c map[string]any) { c["up"] = "30" }},
		{"negative", func(c map[string]any) { c["onlineAt"] = -1 }},
		{"overflow", func(c map[string]any) { c["up"] = int64(math.MaxInt64); c["down"] = int64(1) }},
		{"enable_null", func(c map[string]any) { c["enable"] = nil }},
		{"delay_start_null", func(c map[string]any) { c["delayStart"] = nil }},
		{"auto_reset_null", func(c map[string]any) { c["autoReset"] = nil }},
		{"name_null", func(c map[string]any) { c["name"] = nil }},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			bad := validClient(1)
			tt.mutate(bad)
			result, err := testUpstream(nil).parseResponse(envelope(bad, validClient(2)), []string{"1", "2"}, 1000)
			if err != nil {
				t.Fatal(err)
			}
			if len(result.Errors) != 1 || result.Errors[0] != (queryError{ClientID: "1", Code: "invalid_data"}) || len(result.Items) != 1 {
				t.Fatalf("unexpected result: %+v", result)
			}
		})
	}
	for name, body := range map[string][]byte{
		"success_false": []byte(`{"success":false,"msg":"SECRET_TOKEN","obj":null}`),
		"html":          []byte(`<html>login</html>`),
		"extra_id":      envelope(validClient(3)),
		"duplicate_id":  envelope(validClient(1), validClient(1)),
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := testUpstream(nil).parseResponse(body, []string{"1"}, 1000); err == nil {
				t.Fatal("expected envelope error")
			}
		})
	}
}

func TestUpstreamRequestUsesFixedGETAndDoesNotFollowRedirect(t *testing.T) {
	var gotToken, gotMethod, gotIDs string
	mux := http.NewServeMux()
	mux.HandleFunc("/api/clients", func(w http.ResponseWriter, r *http.Request) {
		gotToken, gotMethod, gotIDs = r.Header.Get("Token"), r.Method, r.URL.Query().Get("id")
		_, _ = w.Write(envelope(validClient(1), validClient(2)))
	})
	mux.HandleFunc("/redirect/clients", func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, "/api/clients", http.StatusFound) })
	server := httptest.NewServer(mux)
	defer server.Close()
	base, _ := url.Parse(server.URL + "/api/")
	cfg := runtimeConfig{config: config{InstanceID: testInstanceID, UpstreamBodyLimitBytes: 4 << 20, MaxConcurrentUpstream: 2}, baseURL: base, token: "SECRET_TOKEN", identityHMACKey: []byte("0123456789abcdef0123456789abcdef"), connectTimeout: time.Second, upstreamTimeout: 3 * time.Second}
	up := newUpstreamClient(cfg)
	if _, err := up.query(context.Background(), []string{"1", "2"}); err != nil {
		t.Fatal(err)
	}
	if gotMethod != http.MethodGet || gotIDs != "1,2" || gotToken != "SECRET_TOKEN" {
		t.Fatalf("method=%s ids=%s token=%s", gotMethod, gotIDs, gotToken)
	}
	redirectBase, _ := url.Parse(server.URL + "/redirect/")
	cfg.baseURL = redirectBase
	if _, err := newUpstreamClient(cfg).query(context.Background(), []string{"1"}); err == nil {
		t.Fatal("redirect unexpectedly followed")
	}
}
