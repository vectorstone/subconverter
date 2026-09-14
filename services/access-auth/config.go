package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"os"
	"strconv"
	"strings"
)

const defaultListenAddr = "127.0.0.1:15053"

type config struct {
	ListenAddr string `json:"listen_addr"`
	Issuer     string `json:"issuer"`
	Audience   string `json:"audience"`
}

func loadConfig(path string) (config, error) {
	f, err := os.Open(path)
	if err != nil {
		return config{}, fmt.Errorf("open config: %w", err)
	}
	defer f.Close()

	var cfg config
	decoder := json.NewDecoder(io.LimitReader(f, 64*1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&cfg); err != nil {
		return config{}, fmt.Errorf("decode config: %w", err)
	}
	if err := ensureJSONEOF(decoder); err != nil {
		return config{}, err
	}
	if cfg.ListenAddr == "" {
		cfg.ListenAddr = defaultListenAddr
	}
	if err := cfg.validate(); err != nil {
		return config{}, err
	}
	return cfg, nil
}

func ensureJSONEOF(decoder *json.Decoder) error {
	var extra any
	if err := decoder.Decode(&extra); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("config contains multiple JSON values")
		}
		return fmt.Errorf("decode config trailer: %w", err)
	}
	return nil
}

func (cfg config) validate() error {
	host, portText, err := net.SplitHostPort(cfg.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen_addr must be a loopback host and port: %w", err)
	}
	ip := net.ParseIP(host)
	if ip == nil || !ip.IsLoopback() {
		return errors.New("listen_addr must use a numeric loopback address")
	}
	port, err := strconv.Atoi(portText)
	if err != nil || port < 1 || port > 65535 {
		return errors.New("listen_addr must use a port from 1 through 65535")
	}

	issuerURL, err := url.Parse(cfg.Issuer)
	if err != nil {
		return fmt.Errorf("issuer is invalid: %w", err)
	}
	if issuerURL.Scheme != "https" || issuerURL.Host == "" || issuerURL.User != nil {
		return errors.New("issuer must be an HTTPS URL without user information")
	}
	if issuerURL.RawQuery != "" || issuerURL.Fragment != "" {
		return errors.New("issuer must not contain a query or fragment")
	}
	if issuerURL.Path != "" && issuerURL.Path != "/" {
		return errors.New("issuer must not contain a path")
	}
	if strings.TrimSpace(cfg.Issuer) != cfg.Issuer {
		return errors.New("issuer must not contain surrounding whitespace")
	}
	if strings.TrimSpace(cfg.Audience) == "" || strings.TrimSpace(cfg.Audience) != cfg.Audience {
		return errors.New("audience must be nonempty and must not contain surrounding whitespace")
	}
	return nil
}

func (cfg config) jwksURL() string {
	return strings.TrimRight(cfg.Issuer, "/") + "/cdn-cgi/access/certs"
}
