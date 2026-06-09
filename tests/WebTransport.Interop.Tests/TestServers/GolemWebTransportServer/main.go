package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	golemnet "golem-engine/golem/net"
	"golem-engine/golem/registry"
)

func main() {
	addr := flag.String("addr", "", "loopback UDP address to listen on, for example 127.0.0.1:4433")
	flag.Parse()

	if strings.TrimSpace(*addr) == "" {
		fmt.Fprintln(os.Stderr, "-addr is required")
		os.Exit(2)
	}

	host, port, err := net.SplitHostPort(*addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid -addr %q: %v\n", *addr, err)
		os.Exit(2)
	}
	if host == "" {
		host = "127.0.0.1"
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	listener := golemnet.NewListener(registry.NewRegistry(), golemnet.Config{
		Addr:              net.JoinHostPort(host, port),
		Transport:         golemnet.TransportWebTransport,
		DevSelfSignedCert: true,
	})
	listener.SetWorldSnapshotFunc(func() ([][]byte, error) {
		return [][]byte{[]byte("world-snapshot")}, nil
	})

	serveErr := make(chan error, 1)
	go func() {
		serveErr <- listener.ListenAndServe(ctx)
	}()

	readyCtx, readyCancel := context.WithTimeout(ctx, 10*time.Second)
	defer readyCancel()
	if err := listener.WaitReady(readyCtx); err != nil {
		cancel()
		fmt.Fprintf(os.Stderr, "listener readiness failed: %v\n", err)
		select {
		case serveErrValue := <-serveErr:
			if serveErrValue != nil {
				fmt.Fprintf(os.Stderr, "listener exited: %v\n", serveErrValue)
			}
		case <-time.After(time.Second):
		}
		os.Exit(1)
	}

	fmt.Printf("GOLEM_WT_URL=https://%s/wt\n", net.JoinHostPort(host, port))
	_ = os.Stdout.Sync()

	signalCh := make(chan os.Signal, 1)
	signal.Notify(signalCh, os.Interrupt, syscall.SIGTERM)

	stdinClosed := make(chan struct{})
	go func() {
		_, _ = io.Copy(io.Discard, os.Stdin)
		close(stdinClosed)
	}()

	select {
	case <-signalCh:
	case <-stdinClosed:
	case err := <-serveErr:
		if err != nil {
			fmt.Fprintf(os.Stderr, "listener exited: %v\n", err)
			os.Exit(1)
		}
		return
	}

	cancel()
	select {
	case err := <-serveErr:
		if err != nil {
			fmt.Fprintf(os.Stderr, "listener shutdown: %v\n", err)
		}
	case <-time.After(2 * time.Second):
		fmt.Fprintln(os.Stderr, "timed out waiting for listener shutdown")
		os.Exit(1)
	}
}
