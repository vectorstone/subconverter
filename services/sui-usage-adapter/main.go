package main

import (
	"context"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
)

func main() {
	configPath := flag.String("config", "config.json", "path to adapter JSON configuration")
	flag.Parse()
	logger := log.New(os.Stderr, "sui-usage-adapter: ", log.LstdFlags|log.LUTC)
	cfg, err := loadConfig(*configPath)
	if err != nil {
		logger.Fatal(err)
	}
	tlsConfig, err := loadTLSConfig(cfg)
	if err != nil {
		logger.Fatal(err)
	}
	adapter := newAdapterServer(cfg, logger)
	server := &http.Server{
		Addr: cfg.ListenAddr, Handler: adapter.handler(), TLSConfig: tlsConfig,
		ReadHeaderTimeout: cfg.upstreamTimeout, ReadTimeout: cfg.upstreamTimeout,
		WriteTimeout: cfg.upstreamTimeout, IdleTimeout: 30 * cfg.upstreamTimeout,
		MaxHeaderBytes: 16 << 10,
	}
	listenerErrors := make(chan error, 1)
	go func() { listenerErrors <- server.ListenAndServeTLS("", "") }()
	logger.Printf("listening addr=%s schema_version=1", cfg.ListenAddr)
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)
	select {
	case sig := <-stop:
		logger.Printf("shutdown signal=%s", sig)
	case err := <-listenerErrors:
		if err != nil && err != http.ErrServerClosed {
			logger.Fatal(err)
		}
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), cfg.shutdownTimeout)
	defer cancel()
	if err := server.Shutdown(ctx); err != nil {
		logger.Printf("shutdown_error=%q", err.Error())
	}
}
