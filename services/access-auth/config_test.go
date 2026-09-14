package main

import "testing"

func TestConfigValidation(t *testing.T) {
	valid := config{
		ListenAddr: "127.0.0.1:15053",
		Issuer:     "https://example.cloudflareaccess.com",
		Audience:   "access-audience",
	}
	if err := valid.validate(); err != nil {
		t.Fatalf("valid config rejected: %v", err)
	}

	tests := []struct {
		name   string
		mutate func(*config)
	}{
		{name: "non-loopback", mutate: func(c *config) { c.ListenAddr = "0.0.0.0:15053" }},
		{name: "hostname loopback", mutate: func(c *config) { c.ListenAddr = "localhost:15053" }},
		{name: "nonnumeric port", mutate: func(c *config) { c.ListenAddr = "127.0.0.1:http" }},
		{name: "zero port", mutate: func(c *config) { c.ListenAddr = "127.0.0.1:0" }},
		{name: "HTTP issuer", mutate: func(c *config) { c.Issuer = "http://example.cloudflareaccess.com" }},
		{name: "issuer path", mutate: func(c *config) { c.Issuer = "https://example.cloudflareaccess.com/path" }},
		{name: "issuer query", mutate: func(c *config) { c.Issuer = "https://example.cloudflareaccess.com?x=1" }},
		{name: "empty audience", mutate: func(c *config) { c.Audience = "" }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			cfg := valid
			test.mutate(&cfg)
			if err := cfg.validate(); err == nil {
				t.Fatal("invalid config accepted")
			}
		})
	}
}

func TestJWKSURLIsFixedUnderIssuer(t *testing.T) {
	cfg := config{Issuer: "https://example.cloudflareaccess.com/"}
	if got, want := cfg.jwksURL(), "https://example.cloudflareaccess.com/cdn-cgi/access/certs"; got != want {
		t.Fatalf("jwksURL() = %q, want %q", got, want)
	}
}
