package main

import (
	"context"
	"crypto/tls"
	"log"
	"net"
	"net/http"
	"net/mail"
	"strings"
	"time"

	"github.com/coreos/go-oidc/v3/oidc"
)

const maxAssertionBytes = 16 * 1024

type tokenVerifier interface {
	Verify(context.Context, string) (*oidc.IDToken, error)
}

type accessClaims struct {
	Email     string `json:"email"`
	NotBefore *int64 `json:"nbf"`
	Type      string `json:"type"`
}

type verifyHandler struct {
	verifier tokenVerifier
	now      func() time.Time
	logger   *log.Logger
}

func newOIDCVerifier(cfg config) tokenVerifier {
	client := outboundHTTPClient()
	ctx := oidc.ClientContext(context.Background(), client)
	keySet := oidc.NewRemoteKeySet(ctx, cfg.jwksURL())
	return oidc.NewVerifier(cfg.Issuer, keySet, &oidc.Config{ClientID: cfg.Audience})
}

func outboundHTTPClient() *http.Client {
	dialer := &net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}
	transport := &http.Transport{
		Proxy:                 nil,
		DialContext:           dialer.DialContext,
		ForceAttemptHTTP2:     true,
		TLSClientConfig:       &tls.Config{MinVersion: tls.VersionTLS12},
		TLSHandshakeTimeout:   5 * time.Second,
		ResponseHeaderTimeout: 5 * time.Second,
		IdleConnTimeout:       30 * time.Second,
	}
	return &http.Client{
		Transport: transport,
		Timeout:   5 * time.Second,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
}

func (h verifyHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path == "/healthz" {
		if r.Method != http.MethodGet {
			w.WriteHeader(http.StatusMethodNotAllowed)
			return
		}
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.URL.Path != "/verify" {
		http.NotFound(w, r)
		return
	}
	if r.Method != http.MethodGet {
		w.WriteHeader(http.StatusMethodNotAllowed)
		return
	}

	// Explicit API credentials are authenticated by the C++ backend. This branch
	// deliberately emits no user identity for Nginx to forward.
	if hasExplicitCredential(r.Header) {
		w.WriteHeader(http.StatusNoContent)
		return
	}

	assertion := r.Header.Get("Cf-Access-Jwt-Assertion")
	if assertion == "" || len(assertion) > maxAssertionBytes {
		h.deny(w, "missing_or_oversize_assertion")
		return
	}

	idToken, err := h.verifier.Verify(r.Context(), assertion)
	if err != nil {
		h.deny(w, "invalid_token")
		return
	}
	var claims accessClaims
	if err := idToken.Claims(&claims); err != nil {
		h.deny(w, "invalid_claims")
		return
	}
	if claims.NotBefore == nil || h.now().Unix() < *claims.NotBefore {
		h.deny(w, "invalid_not_before")
		return
	}
	if claims.Type != "app" {
		h.deny(w, "invalid_token_type")
		return
	}
	if !safeEmail(claims.Email) {
		h.deny(w, "invalid_email")
		return
	}

	w.Header().Set("X-Verified-Email", claims.Email)
	w.WriteHeader(http.StatusNoContent)
}

func hasExplicitCredential(header http.Header) bool {
	if strings.TrimSpace(header.Get("X-API-Key")) != "" {
		return true
	}
	authorization := strings.TrimSpace(header.Get("Authorization"))
	scheme, value, found := strings.Cut(authorization, " ")
	return found && strings.EqualFold(scheme, "Bearer") && strings.TrimSpace(value) != ""
}

func safeEmail(email string) bool {
	if email == "" || len(email) > 254 || strings.TrimSpace(email) != email {
		return false
	}
	for i := 0; i < len(email); i++ {
		if email[i] < 0x21 || email[i] > 0x7e {
			return false
		}
	}
	address, err := mail.ParseAddress(email)
	return err == nil && address.Address == email
}

func (h verifyHandler) deny(w http.ResponseWriter, category string) {
	if h.logger != nil {
		h.logger.Printf("access verification denied category=%s", category)
	}
	w.WriteHeader(http.StatusUnauthorized)
}
