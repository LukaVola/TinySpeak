package main

import (
	"bufio"
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/base32"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const transportALPN = "utf8:tinyspeak/1"

var transportChildren sync.WaitGroup

// A relay-only iroh ticket carries an endpoint key and a public n0 relay hint.
// Accept only this format. Invites cannot supply arbitrary relay URLs or sockets.
func validInternetTicket(ticket string) bool {
	if len(ticket) < 65 || len(ticket) > 200 || !strings.HasPrefix(ticket, "endpoint") || ticket != strings.ToLower(ticket) {
		return false
	}
	enc := base32.StdEncoding.WithPadding(base32.NoPadding)
	data, err := enc.DecodeString(strings.ToUpper(ticket[8:]))
	if err != nil || len(data) < 37 || data[0] != 0 || data[33] != 1 || data[34] != 0 || int(data[35]) != len(data)-36 {
		return false
	}
	u, err := url.Parse(string(data[36:]))
	if err != nil || u.Scheme != "https" || u.User != nil || u.Port() != "" || u.Path != "/" || u.RawQuery != "" || u.Fragment != "" {
		return false
	}
	host := strings.TrimSuffix(u.Hostname(), ".")
	label, ok := strings.CutSuffix(host, ".relay.n0.iroh.link")
	if !ok || label == "" || strings.Contains(label, ".") {
		return false
	}
	for _, c := range label {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-') {
			return false
		}
	}
	return true
}

func transportCommand(ctx context.Context, secret string, args ...string) (*exec.Cmd, error) {
	self, err := os.Executable()
	if err != nil {
		return nil, err
	}
	cmd := exec.CommandContext(ctx, filepath.Join(filepath.Dir(self), "TinySpeak Link.exe"), args...)
	hideConsole(cmd)
	cmd.Env = append(os.Environ(), "IROH_SECRET="+secret, "TOKIO_WORKER_THREADS=2", "RUST_LOG=off")
	return cmd, nil
}

func hostInternetLink(ctx context.Context, cert tls.Certificate, target string) string {
	raw, err := x509.MarshalPKCS8PrivateKey(cert.PrivateKey)
	if err != nil {
		return ""
	}
	derive := hmac.New(sha256.New, raw)
	derive.Write([]byte("TinySpeak internet endpoint v1"))
	child, cancel := context.WithCancel(ctx)
	cmd, err := transportCommand(child, hex.EncodeToString(derive.Sum(nil)), "listen-tcp", "-v", "--custom-alpn", transportALPN, "--host", target)
	if err != nil {
		cancel()
		return ""
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		cancel()
		return ""
	}
	if err := cmd.Start(); err != nil {
		cancel()
		return ""
	}
	ready := make(chan string, 1)
	transportChildren.Add(1)
	go func() {
		defer transportChildren.Done()
		defer cancel()
		defer close(ready)
		lines := bufio.NewScanner(stderr)
		reported := false
		for lines.Scan() {
			ticket, ok := strings.CutPrefix(lines.Text(), "dumbpipe connect-tcp ")
			if ok && !reported && validInternetTicket(ticket) {
				ready <- ticket
				reported = true
			}
		}
		_ = cmd.Wait()
	}()
	select {
	case ticket := <-ready:
		return ticket
	case <-ctx.Done():
		cancel()
	case <-time.After(8 * time.Second):
		cancel()
	}
	return ""
}

func guestInternetLink(ctx context.Context, ticket string) (*url.URL, error) {
	if !validInternetTicket(ticket) {
		return nil, errors.New("Invalid internet invite.")
	}
	reservation, err := loopbackListener()
	if err != nil {
		return nil, err
	}
	addr := reservation.Addr().String()
	reservation.Close()
	cmd, err := transportCommand(ctx, hex.EncodeToString([]byte(randomString(32))), "connect-tcp", "--custom-alpn", transportALPN, "--addr", addr, ticket)
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	done := make(chan struct{})
	transportChildren.Add(1)
	go func() { defer transportChildren.Done(); _ = cmd.Wait(); close(done) }()
	tick := time.NewTicker(60 * time.Millisecond)
	defer tick.Stop()
	timer := time.NewTimer(7 * time.Second)
	defer timer.Stop()
	for {
		select {
		case <-ctx.Done():
			return nil, ctx.Err()
		case <-done:
			return nil, errors.New("The internet transport stopped.")
		case <-timer.C:
			return nil, errors.New("The internet transport timed out.")
		case <-tick.C:
			c, err := net.DialTimeout("tcp4", addr, 100*time.Millisecond)
			if err == nil {
				c.Close()
				return &url.URL{Scheme: "https", Host: addr}, nil
			}
		}
	}
}

func readAdvert(ctx context.Context, target *url.URL, code string, transport *http.Transport) (string, bool) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, target.String()+"/server/"+code, nil)
	if err != nil {
		return "", false
	}
	res, err := transport.RoundTrip(req)
	if err != nil {
		return "", false
	}
	defer res.Body.Close()
	var item advert
	if res.StatusCode != http.StatusOK || json.NewDecoder(io.LimitReader(res.Body, 4096)).Decode(&item) != nil || item.Code != code || item.Name == "" {
		return "", false
	}
	return item.Name, true
}
