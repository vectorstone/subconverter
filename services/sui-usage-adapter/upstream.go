package main

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"
)

var (
	errUpstream        = errors.New("upstream error")
	errUpstreamTimeout = errors.New("upstream timeout")
	errEnvelope        = errors.New("invalid upstream envelope")
)

type upstreamClient struct {
	client     *http.Client
	endpoint   *url.URL
	token      string
	instanceID string
	hmacKey    []byte
	bodyLimit  int64
}

type queryResult struct {
	ObservedAt int64
	Items      []queryItem
	Missing    []string
	Errors     []queryError
}

type queryItem struct {
	ClientID            string  `json:"client_id"`
	IdentityFingerprint string  `json:"identity_fingerprint"`
	Metrics             metrics `json:"metrics"`
}

type queryError struct {
	ClientID string `json:"client_id"`
	Code     string `json:"code"`
}

type metrics struct {
	UploadBytes       string   `json:"upload_bytes"`
	DownloadBytes     string   `json:"download_bytes"`
	UsedBytes         string   `json:"used_bytes"`
	LimitBytes        *string  `json:"limit_bytes"`
	RemainingBytes    *string  `json:"remaining_bytes"`
	OverLimitBytes    string   `json:"over_limit_bytes"`
	ExpiresAt         *int64   `json:"expires_at"`
	Enabled           bool     `json:"enabled"`
	ActivationPending bool     `json:"activation_pending"`
	AutoReset         bool     `json:"auto_reset"`
	ResetDays         int64    `json:"reset_days"`
	NextResetAt       *int64   `json:"next_reset_at"`
	LastTrafficAt     *int64   `json:"last_traffic_at"`
	Conditions        []string `json:"conditions"`
}

type parsedClient struct {
	id, volume, expiry, down, up, createdAt, onlineAt, resetDays, nextReset int64
	enable, delayStart, autoReset                                           bool
	name                                                                    string
}

func newUpstreamClient(cfg runtimeConfig) *upstreamClient {
	dialer := &net.Dialer{Timeout: cfg.connectTimeout}
	transport := &http.Transport{
		Proxy:                  nil,
		DialContext:            dialer.DialContext,
		DisableCompression:     true,
		DisableKeepAlives:      false,
		MaxIdleConns:           cfg.MaxConcurrentUpstream,
		MaxIdleConnsPerHost:    cfg.MaxConcurrentUpstream,
		MaxConnsPerHost:        cfg.MaxConcurrentUpstream,
		MaxResponseHeaderBytes: 64 << 10,
		ResponseHeaderTimeout:  cfg.upstreamTimeout,
		TLSHandshakeTimeout:    cfg.connectTimeout,
	}
	client := &http.Client{
		Transport:     transport,
		Timeout:       cfg.upstreamTimeout,
		CheckRedirect: func(_ *http.Request, _ []*http.Request) error { return http.ErrUseLastResponse },
	}
	endpoint := *cfg.baseURL
	endpoint.Path = strings.TrimRight(endpoint.Path, "/") + "/clients"
	return &upstreamClient{client: client, endpoint: &endpoint, token: cfg.token, instanceID: cfg.InstanceID, hmacKey: cfg.identityHMACKey, bodyLimit: cfg.UpstreamBodyLimitBytes}
}

func (u *upstreamClient) query(ctx context.Context, ids []string) (queryResult, error) {
	endpoint := *u.endpoint
	q := endpoint.Query()
	q.Set("id", strings.Join(ids, ","))
	endpoint.RawQuery = q.Encode()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint.String(), nil)
	if err != nil {
		return queryResult{}, errUpstream
	}
	req.Header.Set("Token", u.token)
	req.Header.Set("Accept", "application/json")
	req.Header.Set("Accept-Encoding", "identity")
	resp, err := u.client.Do(req)
	if err != nil {
		if errors.Is(err, context.DeadlineExceeded) || errors.Is(err, context.Canceled) || isTimeout(err) {
			return queryResult{}, errUpstreamTimeout
		}
		return queryResult{}, errUpstream
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return queryResult{}, errUpstream
	}
	body, err := readLimited(resp.Body, u.bodyLimit)
	if err != nil {
		return queryResult{}, errUpstream
	}
	observedAt := time.Now().Unix()
	return u.parseResponse(body, ids, observedAt)
}

func (u *upstreamClient) parseResponse(body []byte, requested []string, observedAt int64) (queryResult, error) {
	if !utf8.Valid(body) {
		return queryResult{}, errEnvelope
	}
	var envelope struct {
		Success *bool           `json:"success"`
		Obj     json.RawMessage `json:"obj"`
	}
	if err := json.Unmarshal(body, &envelope); err != nil || envelope.Success == nil || !*envelope.Success || len(envelope.Obj) == 0 || string(envelope.Obj) == "null" {
		return queryResult{}, errEnvelope
	}
	var obj struct {
		Clients json.RawMessage `json:"clients"`
	}
	if err := json.Unmarshal(envelope.Obj, &obj); err != nil || len(obj.Clients) == 0 || string(obj.Clients) == "null" {
		return queryResult{}, errEnvelope
	}
	var rawClients []json.RawMessage
	if err := json.Unmarshal(obj.Clients, &rawClients); err != nil {
		return queryResult{}, errEnvelope
	}
	wanted := make(map[int64]string, len(requested))
	for _, id := range requested {
		n, _ := strconv.ParseInt(id, 10, 64)
		wanted[n] = id
	}
	seen := make(map[int64]bool, len(rawClients))
	result := queryResult{ObservedAt: observedAt, Items: make([]queryItem, 0, len(rawClients)), Missing: make([]string, 0), Errors: make([]queryError, 0)}
	for _, raw := range rawClients {
		fields := map[string]json.RawMessage{}
		if err := json.Unmarshal(raw, &fields); err != nil {
			return queryResult{}, errEnvelope
		}
		id, err := requiredInt(fields, "id")
		if err != nil || id <= 0 {
			return queryResult{}, errEnvelope
		}
		requestedID, ok := wanted[id]
		if !ok || seen[id] {
			return queryResult{}, errEnvelope
		}
		seen[id] = true
		client, err := parseClient(fields, id)
		if err != nil {
			result.Errors = append(result.Errors, queryError{ClientID: requestedID, Code: "invalid_data"})
			continue
		}
		item := queryItem{ClientID: requestedID, IdentityFingerprint: u.fingerprint(client), Metrics: normalizeMetrics(client, observedAt)}
		result.Items = append(result.Items, item)
	}
	for _, id := range requested {
		n, _ := strconv.ParseInt(id, 10, 64)
		if !seen[n] {
			result.Missing = append(result.Missing, id)
		}
	}
	sort.Slice(result.Items, func(i, j int) bool {
		a, _ := strconv.ParseInt(result.Items[i].ClientID, 10, 64)
		b, _ := strconv.ParseInt(result.Items[j].ClientID, 10, 64)
		return a < b
	})
	sort.Slice(result.Errors, func(i, j int) bool {
		a, _ := strconv.ParseInt(result.Errors[i].ClientID, 10, 64)
		b, _ := strconv.ParseInt(result.Errors[j].ClientID, 10, 64)
		return a < b
	})
	return result, nil
}

func parseClient(fields map[string]json.RawMessage, id int64) (parsedClient, error) {
	c := parsedClient{id: id}
	var err error
	for name, dst := range map[string]*int64{"volume": &c.volume, "expiry": &c.expiry, "down": &c.down, "up": &c.up, "createdAt": &c.createdAt, "onlineAt": &c.onlineAt, "resetDays": &c.resetDays, "nextReset": &c.nextReset} {
		*dst, err = requiredInt(fields, name)
		if err != nil {
			return c, err
		}
	}
	for name, dst := range map[string]*bool{"enable": &c.enable, "delayStart": &c.delayStart, "autoReset": &c.autoReset} {
		*dst, err = requiredBool(fields, name)
		if err != nil {
			return c, err
		}
	}
	c.name, err = requiredString(fields, "name")
	if err != nil {
		return c, err
	}
	if c.volume < 0 || c.expiry < 0 || c.down < 0 || c.up < 0 || c.createdAt < 0 || c.onlineAt < 0 || c.nextReset < 0 {
		return c, errors.New("negative value")
	}
	if c.resetDays < 0 && !c.delayStart && !c.autoReset {
		return c, errors.New("negative resetDays")
	}
	if c.up > math.MaxInt64-c.down {
		return c, errors.New("traffic overflow")
	}
	return c, nil
}

func normalizeMetrics(c parsedClient, observedAt int64) metrics {
	used := c.up + c.down
	resetDays := c.resetDays
	if resetDays < 0 {
		resetDays = 0
	}
	m := metrics{UploadBytes: strconv.FormatInt(c.up, 10), DownloadBytes: strconv.FormatInt(c.down, 10), UsedBytes: strconv.FormatInt(used, 10), OverLimitBytes: "0", Enabled: c.enable, ActivationPending: c.delayStart, AutoReset: c.autoReset, ResetDays: resetDays, Conditions: make([]string, 0, 4)}
	if c.volume > 0 {
		limit := strconv.FormatInt(c.volume, 10)
		m.LimitBytes = &limit
		remaining := c.volume - used
		if remaining < 0 {
			remaining = 0
		}
		remainingText := strconv.FormatInt(remaining, 10)
		m.RemainingBytes = &remainingText
		if used > c.volume {
			m.OverLimitBytes = strconv.FormatInt(used-c.volume, 10)
			m.Conditions = append(m.Conditions, "quota_exceeded")
		} else if used == c.volume {
			m.Conditions = append(m.Conditions, "quota_at_limit")
		}
	}
	if c.expiry > 0 {
		m.ExpiresAt = ptrInt64(c.expiry)
		if c.expiry < observedAt {
			m.Conditions = append(m.Conditions, "expired")
		}
	}
	if c.nextReset > 0 {
		m.NextResetAt = ptrInt64(c.nextReset)
	}
	if c.onlineAt > 0 {
		m.LastTrafficAt = ptrInt64(c.onlineAt)
	}
	if (c.delayStart || c.autoReset) && c.resetDays <= 0 {
		m.Conditions = append(m.Conditions, "invalid_reset_policy")
	}
	if c.autoReset && !c.delayStart && c.resetDays > 0 && c.nextReset < observedAt {
		m.Conditions = append(m.Conditions, "reset_due")
	}
	return m
}

func (u *upstreamClient) fingerprint(c parsedClient) string {
	canonical, _ := json.Marshal([]string{u.instanceID, strconv.FormatInt(c.id, 10), strconv.FormatInt(c.createdAt, 10), c.name})
	mac := hmac.New(sha256.New, u.hmacKey)
	_, _ = mac.Write(canonical)
	return hex.EncodeToString(mac.Sum(nil))
}

func requiredInt(fields map[string]json.RawMessage, name string) (int64, error) {
	raw, ok := fields[name]
	if !ok {
		return 0, errors.New("missing field")
	}
	var value any
	dec := json.NewDecoder(strings.NewReader(string(raw)))
	dec.UseNumber()
	if err := dec.Decode(&value); err != nil {
		return 0, errors.New("not integer")
	}
	n, ok := value.(json.Number)
	if !ok {
		return 0, errors.New("not integer")
	}
	v, err := strconv.ParseInt(string(n), 10, 64)
	if err != nil {
		return 0, errors.New("not integer")
	}
	return v, nil
}
func requiredBool(fields map[string]json.RawMessage, name string) (bool, error) {
	raw, ok := fields[name]
	if !ok {
		return false, errors.New("missing field")
	}
	var v *bool
	if err := json.Unmarshal(raw, &v); err != nil {
		return false, err
	}
	if v == nil {
		return false, errors.New("null field")
	}
	return *v, nil
}
func requiredString(fields map[string]json.RawMessage, name string) (string, error) {
	raw, ok := fields[name]
	if !ok {
		return "", errors.New("missing field")
	}
	var v *string
	if err := json.Unmarshal(raw, &v); err != nil {
		return "", err
	}
	if v == nil {
		return "", errors.New("null field")
	}
	return *v, nil
}
func ptrInt64(v int64) *int64 { return &v }
func readLimited(r io.Reader, limit int64) ([]byte, error) {
	data, err := io.ReadAll(io.LimitReader(r, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, errors.New("body too large")
	}
	return data, nil
}
func isTimeout(err error) bool {
	var netErr net.Error
	return errors.As(err, &netErr) && netErr.Timeout()
}
