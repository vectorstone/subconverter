package main

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"os"
	"regexp"
	"strings"
	"time"
)

const (
	defaultRequestBodyLimit  = int64(16 << 10)
	defaultUpstreamBodyLimit = int64(4 << 20)
	defaultResponseBodyLimit = int64(256 << 10)
)

type config struct {
	ListenAddr             string `json:"listen_addr"`
	SUIAPIBaseURL          string `json:"sui_api_base_url"`
	SUITokenFile           string `json:"sui_token_file"`
	InstanceID             string `json:"instance_id"`
	IdentityHMACKeyFile    string `json:"identity_hmac_key_file"`
	TLSCertFile            string `json:"tls_cert_file"`
	TLSKeyFile             string `json:"tls_key_file"`
	ClientCAFile           string `json:"client_ca_file"`
	AllowedClientIdentity  string `json:"allowed_client_identity"`
	ConnectTimeoutMS       int    `json:"connect_timeout_ms,omitempty"`
	UpstreamTimeoutMS      int    `json:"upstream_timeout_ms,omitempty"`
	RequestBodyLimitBytes  int64  `json:"request_body_limit_bytes,omitempty"`
	UpstreamBodyLimitBytes int64  `json:"upstream_response_limit_bytes,omitempty"`
	ResponseBodyLimitBytes int64  `json:"response_body_limit_bytes,omitempty"`
	MaxConcurrentUpstream  int    `json:"max_concurrent_upstream,omitempty"`
	RateLimitPerMinute     int    `json:"rate_limit_per_minute,omitempty"`
	RateLimitBurst         int    `json:"rate_limit_burst,omitempty"`
	ShutdownTimeoutSeconds int    `json:"shutdown_timeout_seconds,omitempty"`
}

type runtimeConfig struct {
	config
	baseURL         *url.URL
	token           string
	identityHMACKey []byte
	connectTimeout  time.Duration
	upstreamTimeout time.Duration
	shutdownTimeout time.Duration
}

var uuidPattern = regexp.MustCompile(`^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89aAbB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$`)

func loadConfig(path string) (runtimeConfig, error) {
	f, err := os.Open(path)
	if err != nil {
		return runtimeConfig{}, fmt.Errorf("open config: %w", err)
	}
	defer f.Close()
	configBytes, err := readLimited(f, 1<<20)
	if err != nil {
		return runtimeConfig{}, fmt.Errorf("read config: %w", err)
	}

	var cfg config
	dec := json.NewDecoder(bytes.NewReader(configBytes))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&cfg); err != nil {
		return runtimeConfig{}, fmt.Errorf("decode config: %w", err)
	}
	if err := requireJSONEOF(dec); err != nil {
		return runtimeConfig{}, fmt.Errorf("decode config: %w", err)
	}
	if err := cfg.applyDefaultsAndValidate(); err != nil {
		return runtimeConfig{}, err
	}

	baseURL, err := validateBaseURL(cfg.SUIAPIBaseURL)
	if err != nil {
		return runtimeConfig{}, err
	}
	tokenBytes, err := readSecretFile(cfg.SUITokenFile, "sui token")
	if err != nil {
		return runtimeConfig{}, fmt.Errorf("read sui token file: %w", err)
	}
	token := strings.TrimSpace(string(tokenBytes))
	if token == "" || strings.ContainsAny(token, "\r\n") || containsControlByte(token) {
		return runtimeConfig{}, errors.New("sui token file must contain one non-empty token")
	}
	keyBytes, err := readSecretFile(cfg.IdentityHMACKeyFile, "identity HMAC key")
	if err != nil {
		return runtimeConfig{}, fmt.Errorf("read identity HMAC key file: %w", err)
	}
	keyText := bytes.TrimSpace(keyBytes)
	if len(keyText) != 64 {
		return runtimeConfig{}, errors.New("identity HMAC key file must contain exactly 64 hexadecimal characters")
	}
	decodedKey := make([]byte, 32)
	if _, err := hex.Decode(decodedKey, keyText); err != nil {
		return runtimeConfig{}, errors.New("identity HMAC key file must contain exactly 64 hexadecimal characters")
	}

	return runtimeConfig{
		config:          cfg,
		baseURL:         baseURL,
		token:           token,
		identityHMACKey: decodedKey,
		connectTimeout:  time.Duration(cfg.ConnectTimeoutMS) * time.Millisecond,
		upstreamTimeout: time.Duration(cfg.UpstreamTimeoutMS) * time.Millisecond,
		shutdownTimeout: time.Duration(cfg.ShutdownTimeoutSeconds) * time.Second,
	}, nil
}

func containsControlByte(value string) bool {
	for i := 0; i < len(value); i++ {
		if value[i] < 0x20 || value[i] == 0x7f {
			return true
		}
	}
	return false
}

func readSecretFile(path, description string) ([]byte, error) {
	info, err := os.Stat(path)
	if err != nil {
		return nil, fmt.Errorf("stat %s file: %w", description, err)
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("%s file must be a regular file", description)
	}
	if info.Mode().Perm()&0077 != 0 {
		return nil, fmt.Errorf("%s file must not be accessible by group or others", description)
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("read %s file: %w", description, err)
	}
	defer f.Close()
	data, err := readLimited(f, 1<<20)
	if err != nil {
		return nil, fmt.Errorf("read %s file: %w", description, err)
	}
	return data, nil
}

func (c *config) applyDefaultsAndValidate() error {
	required := map[string]string{
		"listen_addr": c.ListenAddr, "sui_api_base_url": c.SUIAPIBaseURL,
		"sui_token_file": c.SUITokenFile, "instance_id": c.InstanceID,
		"identity_hmac_key_file": c.IdentityHMACKeyFile, "tls_cert_file": c.TLSCertFile,
		"tls_key_file": c.TLSKeyFile, "client_ca_file": c.ClientCAFile,
		"allowed_client_identity": c.AllowedClientIdentity,
	}
	for name, value := range required {
		if strings.TrimSpace(value) == "" {
			return fmt.Errorf("config field %q is required", name)
		}
	}
	if _, _, err := net.SplitHostPort(c.ListenAddr); err != nil {
		return fmt.Errorf("invalid listen_addr: %w", err)
	}
	if !uuidPattern.MatchString(c.InstanceID) {
		return errors.New("instance_id must be a UUID")
	}
	identity, err := url.Parse(c.AllowedClientIdentity)
	if err != nil || identity.Scheme == "" || identity.Host == "" || identity.String() != c.AllowedClientIdentity {
		return errors.New("allowed_client_identity must be an absolute canonical URI SAN")
	}
	setDefaultInt(&c.ConnectTimeoutMS, 1000)
	setDefaultInt(&c.UpstreamTimeoutMS, 3000)
	setDefaultInt64(&c.RequestBodyLimitBytes, defaultRequestBodyLimit)
	setDefaultInt64(&c.UpstreamBodyLimitBytes, defaultUpstreamBodyLimit)
	setDefaultInt64(&c.ResponseBodyLimitBytes, defaultResponseBodyLimit)
	setDefaultInt(&c.MaxConcurrentUpstream, 2)
	setDefaultInt(&c.RateLimitPerMinute, 60)
	setDefaultInt(&c.RateLimitBurst, 10)
	setDefaultInt(&c.ShutdownTimeoutSeconds, 10)
	if c.ConnectTimeoutMS < 1 || c.ConnectTimeoutMS > 30000 || c.UpstreamTimeoutMS < c.ConnectTimeoutMS || c.UpstreamTimeoutMS > 60000 {
		return errors.New("timeouts must be positive, total >= connect, connect <= 30000ms, total <= 60000ms")
	}
	if c.RequestBodyLimitBytes < 1024 || c.RequestBodyLimitBytes > defaultRequestBodyLimit || c.UpstreamBodyLimitBytes < 1024 || c.UpstreamBodyLimitBytes > defaultUpstreamBodyLimit || c.ResponseBodyLimitBytes < 1024 || c.ResponseBodyLimitBytes > defaultResponseBodyLimit {
		return errors.New("body limits must be between 1024 bytes and their v1 maximum")
	}
	if c.MaxConcurrentUpstream < 1 || c.MaxConcurrentUpstream > 32 || c.RateLimitPerMinute < 1 || c.RateLimitPerMinute > 6000 || c.RateLimitBurst < 1 || c.RateLimitBurst > c.RateLimitPerMinute || c.ShutdownTimeoutSeconds < 1 || c.ShutdownTimeoutSeconds > 60 {
		return errors.New("invalid concurrency, rate limit, burst, or shutdown timeout")
	}
	return nil
}

func validateBaseURL(raw string) (*url.URL, error) {
	u, err := url.Parse(raw)
	if err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
		return nil, errors.New("sui_api_base_url must be an absolute HTTP(S) URL")
	}
	if u.User != nil || u.RawQuery != "" || u.Fragment != "" {
		return nil, errors.New("sui_api_base_url must not contain credentials, query, or fragment")
	}
	host := u.Hostname()
	if net.ParseIP(host) == nil || !net.ParseIP(host).IsLoopback() {
		return nil, errors.New("sui_api_base_url must use a literal loopback IP")
	}
	u.Path = strings.TrimRight(u.Path, "/") + "/"
	u.RawPath = ""
	return u, nil
}

func requireJSONEOF(dec *json.Decoder) error {
	var extra any
	if err := dec.Decode(&extra); err != io.EOF {
		if err == nil {
			return errors.New("multiple JSON values")
		}
		return err
	}
	return nil
}

func setDefaultInt(v *int, value int) {
	if *v == 0 {
		*v = value
	}
}
func setDefaultInt64(v *int64, value int64) {
	if *v == 0 {
		*v = value
	}
}
