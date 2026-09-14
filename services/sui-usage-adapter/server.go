package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math"
	"mime"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"
)

type adapterServer struct {
	cfg      runtimeConfig
	upstream *upstreamClient
	sem      chan struct{}
	limiter  *certificateLimiter
	logger   *log.Logger
	now      func() time.Time
}

type queryRequest struct {
	SchemaVersion int      `json:"schema_version"`
	ClientIDs     []string `json:"client_ids"`
}

type queryResponse struct {
	SchemaVersion    int          `json:"schema_version"`
	InstanceID       string       `json:"instance_id"`
	ObservedAt       int64        `json:"observed_at"`
	Items            []queryItem  `json:"items"`
	MissingClientIDs []string     `json:"missing_client_ids"`
	Errors           []queryError `json:"errors"`
}

type errorResponse struct {
	Error     string `json:"error"`
	RequestID string `json:"request_id"`
}

func newAdapterServer(cfg runtimeConfig, logger *log.Logger) *adapterServer {
	if logger == nil {
		logger = log.New(os.Stderr, "", log.LstdFlags|log.LUTC)
	}
	return &adapterServer{cfg: cfg, upstream: newUpstreamClient(cfg), sem: make(chan struct{}, cfg.MaxConcurrentUpstream), limiter: newCertificateLimiter(cfg.RateLimitPerMinute, cfg.RateLimitBurst), logger: logger, now: time.Now}
}

func (s *adapterServer) handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.health)
	mux.HandleFunc("POST /v1/clients/query", s.clientsQuery)
	mux.HandleFunc("/", s.notFound)
	return s.authenticateAndLimit(mux)
}

func (s *adapterServer) authenticateAndLimit(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requestID := newRequestID()
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("X-Request-ID", requestID)
		if r.TLS == nil || len(r.TLS.PeerCertificates) == 0 || !hasExactURI(r.TLS.PeerCertificates[0], s.cfg.AllowedClientIdentity) {
			s.writeError(w, http.StatusForbidden, "forbidden", requestID)
			return
		}
		certKey := sha256.Sum256(r.TLS.PeerCertificates[0].Raw)
		allowed, retry := s.limiter.allow(hex.EncodeToString(certKey[:]), s.now())
		if !allowed {
			w.Header().Set("Retry-After", strconv.Itoa(retry))
			s.writeError(w, http.StatusTooManyRequests, "rate_limited", requestID)
			return
		}
		start := s.now()
		recorder := &statusRecorder{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(recorder, r.WithContext(context.WithValue(r.Context(), requestIDKey{}, requestID)))
		s.logger.Printf("request_id=%s method=%s path=%s status=%d duration_ms=%d", requestID, r.Method, r.URL.Path, recorder.status, s.now().Sub(start).Milliseconds())
	})
}

func (s *adapterServer) health(w http.ResponseWriter, _ *http.Request) {
	s.writeJSON(w, http.StatusOK, struct {
		Status        string `json:"status"`
		SchemaVersion int    `json:"schema_version"`
	}{Status: "ok", SchemaVersion: 1}, requestIDFromContext(nil))
}

func (s *adapterServer) notFound(w http.ResponseWriter, r *http.Request) {
	requestID := requestIDFromContext(r)
	if r.URL.Path == "/healthz" || r.URL.Path == "/v1/clients/query" {
		s.writeError(w, http.StatusMethodNotAllowed, "method_not_allowed", requestID)
		return
	}
	s.writeError(w, http.StatusNotFound, "not_found", requestID)
}

func (s *adapterServer) clientsQuery(w http.ResponseWriter, r *http.Request) {
	requestID := requestIDFromContext(r)
	mediaType, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || mediaType != "application/json" {
		s.writeError(w, http.StatusBadRequest, "invalid_request", requestID)
		return
	}
	var req queryRequest
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, s.cfg.RequestBodyLimitBytes))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&req); err != nil || requireJSONEOF(dec) != nil || validateQueryRequest(req) != nil {
		s.writeError(w, http.StatusBadRequest, "invalid_request", requestID)
		return
	}
	select {
	case s.sem <- struct{}{}:
		defer func() { <-s.sem }()
	default:
		w.Header().Set("Retry-After", "1")
		s.writeError(w, http.StatusTooManyRequests, "overloaded", requestID)
		return
	}
	result, err := s.upstream.query(r.Context(), req.ClientIDs)
	if err != nil {
		status := http.StatusBadGateway
		code := "upstream_error"
		if errors.Is(err, errUpstreamTimeout) {
			status = http.StatusGatewayTimeout
			code = "upstream_timeout"
		}
		s.writeError(w, status, code, requestID)
		return
	}
	response := queryResponse{SchemaVersion: 1, InstanceID: s.cfg.InstanceID, ObservedAt: result.ObservedAt, Items: result.Items, MissingClientIDs: result.Missing, Errors: result.Errors}
	s.writeJSON(w, http.StatusOK, response, requestID)
}

func validateQueryRequest(req queryRequest) error {
	if req.SchemaVersion != 1 || len(req.ClientIDs) < 1 || len(req.ClientIDs) > 100 {
		return errors.New("invalid schema or count")
	}
	seen := make(map[int64]struct{}, len(req.ClientIDs))
	for _, text := range req.ClientIDs {
		id, err := strconv.ParseInt(text, 10, 64)
		if err != nil || id <= 0 || strconv.FormatInt(id, 10) != text {
			return errors.New("invalid client id")
		}
		if _, exists := seen[id]; exists {
			return errors.New("duplicate client id")
		}
		seen[id] = struct{}{}
	}
	return nil
}

func (s *adapterServer) writeJSON(w http.ResponseWriter, status int, value any, requestID string) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(true)
	if err := enc.Encode(value); err != nil || int64(buf.Len()) > s.cfg.ResponseBodyLimitBytes {
		if status >= 400 {
			http.Error(w, "", http.StatusInternalServerError)
			return
		}
		s.writeError(w, http.StatusBadGateway, "response_too_large", requestID)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_, _ = w.Write(buf.Bytes())
}

func (s *adapterServer) writeError(w http.ResponseWriter, status int, code, requestID string) {
	s.writeJSON(w, status, errorResponse{Error: code, RequestID: requestID}, requestID)
}

func hasExactURI(cert *x509.Certificate, expected string) bool {
	for _, uri := range cert.URIs {
		if uri.String() == expected {
			return true
		}
	}
	return false
}

func newRequestID() string {
	var raw [16]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return fmt.Sprintf("fallback-%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(raw[:])
}

type requestIDKey struct{}

func requestIDFromContext(r *http.Request) string {
	if r == nil {
		return ""
	}
	value, _ := r.Context().Value(requestIDKey{}).(string)
	return value
}

type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (w *statusRecorder) WriteHeader(status int) {
	w.status = status
	w.ResponseWriter.WriteHeader(status)
}

type rateBucket struct {
	tokens  float64
	updated time.Time
}
type certificateLimiter struct {
	mu      sync.Mutex
	rate    float64
	burst   float64
	buckets map[string]rateBucket
}

func newCertificateLimiter(perMinute, burst int) *certificateLimiter {
	return &certificateLimiter{rate: float64(perMinute) / 60, burst: float64(burst), buckets: make(map[string]rateBucket)}
}
func (l *certificateLimiter) allow(key string, now time.Time) (bool, int) {
	l.mu.Lock()
	defer l.mu.Unlock()
	b, ok := l.buckets[key]
	if !ok {
		if len(l.buckets) >= 1024 {
			var oldestKey string
			var oldestTime time.Time
			for candidate, bucket := range l.buckets {
				if oldestKey == "" || bucket.updated.Before(oldestTime) {
					oldestKey, oldestTime = candidate, bucket.updated
				}
			}
			delete(l.buckets, oldestKey)
		}
		b = rateBucket{tokens: l.burst, updated: now}
	}
	elapsed := now.Sub(b.updated).Seconds()
	if elapsed > 0 {
		b.tokens = math.Min(l.burst, b.tokens+elapsed*l.rate)
		b.updated = now
	}
	if b.tokens >= 1 {
		b.tokens--
		l.buckets[key] = b
		return true, 0
	}
	l.buckets[key] = b
	retry := int(math.Ceil((1 - b.tokens) / l.rate))
	if retry < 1 {
		retry = 1
	}
	return false, retry
}

func loadTLSConfig(cfg runtimeConfig) (*tls.Config, error) {
	certPEM, err := os.ReadFile(cfg.TLSCertFile)
	if err != nil {
		return nil, fmt.Errorf("read server certificate: %w", err)
	}
	keyPEM, err := readSecretFile(cfg.TLSKeyFile, "TLS private key")
	if err != nil {
		return nil, err
	}
	certificate, err := tls.X509KeyPair(certPEM, keyPEM)
	if err != nil {
		return nil, fmt.Errorf("load server certificate: %w", err)
	}
	caPEM, err := os.ReadFile(cfg.ClientCAFile)
	if err != nil {
		return nil, fmt.Errorf("read client CA: %w", err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(caPEM) {
		return nil, errors.New("client CA file contains no certificates")
	}
	return &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{certificate}, ClientAuth: tls.RequireAndVerifyClientCert, ClientCAs: pool}, nil
}
