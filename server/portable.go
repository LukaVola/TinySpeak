package main

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"net/http/httputil"
	"net/netip"
	"net/url"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	directPort = 30472
	mediaTCP   = 30473
	mediaUDP   = 30474
)

type directRoute struct {
	public, local netip.Addr
	pin           [16]byte
	ticket        string
}

func (r directRoute) encode() string {
	public, local := r.public.As4(), r.local.As4()
	packed := append(public[:], local[:]...)
	return base64.RawURLEncoding.EncodeToString(append(packed, r.pin[:]...))
}

func decodeInvite(code string) (directRoute, string, error) {
	var route directRoute
	var encoded, server string
	switch parts := strings.Split(code, "."); {
	case len(parts) == 2 && strings.HasPrefix(parts[0], "TS1-"):
		encoded, server = parts[0][4:], parts[1]
	case len(parts) == 3 && strings.HasPrefix(parts[0], "TS2-") && validInternetTicket(parts[1]):
		encoded, route.ticket, server = parts[0][4:], parts[1], parts[2]
	default:
		return route, "", errors.New("Invalid TinySpeak host invite.")
	}
	packed, err := base64.RawURLEncoding.Strict().DecodeString(encoded)
	if err != nil || len(packed) != 24 || !validID(server, 16) {
		return route, "", errors.New("Invalid TinySpeak host invite.")
	}
	route.public = netip.AddrFrom4([4]byte(packed[:4]))
	route.local = netip.AddrFrom4([4]byte(packed[4:8]))
	copy(route.pin[:], packed[8:])
	if (!route.public.IsUnspecified() && !publicIPv4(route.public)) ||
		!route.local.IsGlobalUnicast() && !route.local.IsLoopback() && !(route.ticket != "" && route.local.IsUnspecified()) {
		return route, "", errors.New("Invalid address in the TinySpeak invite.")
	}
	return route, server, nil
}

func hostCertificate(profile string) (tls.Certificate, error) {
	file := filepath.Join(profile, "host-identity.pem")
	existing, err := os.ReadFile(file)
	if err == nil {
		return tls.X509KeyPair(existing, existing)
	}
	if !os.IsNotExist(err) {
		return tls.Certificate{}, err
	}
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, err
	}
	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return tls.Certificate{}, err
	}
	now := time.Now()
	cert := &x509.Certificate{
		SerialNumber: serial, NotBefore: now.Add(-time.Hour), NotAfter: now.AddDate(20, 0, 0),
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	raw, err := x509.CreateCertificate(rand.Reader, cert, cert, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, err
	}
	keyRaw, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return tls.Certificate{}, err
	}
	blocks := append(pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: raw}),
		pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyRaw})...)
	if err := os.WriteFile(file, blocks, 0600); err != nil {
		return tls.Certificate{}, err
	}
	return tls.X509KeyPair(blocks, blocks)
}

func directHTTP(handler http.Handler) *http.Server {
	return &http.Server{Handler: handler, ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout: 5 * time.Second, WriteTimeout: 24 * time.Second, IdleTimeout: 45 * time.Second, MaxHeaderBytes: 8192,
		ErrorLog: quietLog()}
}

func quietLog() *log.Logger { return log.New(io.Discard, "", 0) }

func loopbackListener() (net.Listener, error) {
	return net.Listen("tcp4", "127.0.0.1:0")
}

func localIPv4() netip.Addr {
	// Select the interface from the OS routing table. No packet is sent.
	conn, err := net.Dial("udp4", "1.1.1.1:53")
	if err == nil {
		defer conn.Close()
		if ip, ok := netip.AddrFromSlice(conn.LocalAddr().(*net.UDPAddr).IP.To4()); ok {
			return ip
		}
	}
	addresses, _ := net.InterfaceAddrs()
	for _, address := range addresses {
		prefix, err := netip.ParsePrefix(address.String())
		if ip := prefix.Addr(); err == nil && ip.Is4() && ip.IsGlobalUnicast() {
			return ip
		}
	}
	return netip.MustParseAddr("127.0.0.1")
}

func runVoice(ctx context.Context, program, nodeIP, key string, tcpPort, udpPort int) (string, <-chan error, error) {
	reservation, err := loopbackListener()
	if err != nil {
		return "", nil, err
	}
	address := reservation.Addr().String()
	port := reservation.Addr().(*net.TCPAddr).Port
	reservation.Close()
	config := fmt.Sprintf(`port: %d
bind_addresses: [127.0.0.1]
keys:
  tinyspeak: %s
rtc:
  node_ip: %s
  advertise_internal_ip: true
  enable_loopback_candidate: true
  tcp_port: %d
  udp_port: %d
logging:
  level: error
room:
  max_participants: 16
  enabled_codecs:
    - mime: audio/opus
    - mime: audio/red
`, port, key, nodeIP, tcpPort, udpPort)
	exited, err := startVoiceService(ctx, program, config)
	if err != nil {
		return "", nil, err
	}
	deadline := time.NewTimer(6 * time.Second)
	defer deadline.Stop()
	probe := time.NewTicker(40 * time.Millisecond)
	defer probe.Stop()
	for {
		select {
		case <-ctx.Done():
			return "", nil, ctx.Err()
		case <-exited:
			return "", nil, errors.New("The local voice service stopped during startup.")
		case <-deadline.C:
			return "", nil, errors.New("The local voice service did not start.")
		case <-probe.C:
			conn, err := net.DialTimeout("tcp4", address, 150*time.Millisecond)
			if err == nil {
				conn.Close()
				return address, exited, nil
			}
		}
	}
}

func directHost(ctx context.Context, profile, voiceBinary string) error {
	voiceCtx, stopVoice := context.WithCancel(ctx)
	defer stopVoice()
	certificate, err := hostCertificate(profile)
	if err != nil {
		return fmt.Errorf("Could not load the host identity: %w", err)
	}
	local, err := loopbackListener()
	if err != nil {
		return err
	}
	defer local.Close()
	inbound, err := net.Listen("tcp4", ":"+strconv.Itoa(directPort))
	standardPorts := err == nil
	tcpPort, udpPort := mediaTCP, mediaUDP
	if !standardPorts {
		// Two local hosts can coexist using their separate encrypted endpoints.
		inbound, err = loopbackListener()
		if err != nil {
			return err
		}
	}
	defer inbound.Close()
	if !standardPorts {
		tcp, err := loopbackListener()
		if err != nil {
			return err
		}
		tcpPort = tcp.Addr().(*net.TCPAddr).Port
		tcp.Close()
		udp, err := net.ListenUDP("udp4", &net.UDPAddr{})
		if err != nil {
			return err
		}
		udpPort = udp.LocalAddr().(*net.UDPAddr).Port
		udp.Close()
	}
	lanIP := localIPv4()
	gateway := gatewayMapping{public: netip.IPv4Unspecified()}
	if standardPorts {
		gateway = discoverGateway(ctx, lanIP)
	}
	wanIP := gateway.public
	nodeIP := lanIP
	if !wanIP.IsUnspecified() {
		nodeIP = wanIP
	}
	key := randomString(48)
	voiceAddress, voiceExited, err := runVoice(voiceCtx, voiceBinary, nodeIP.String(), key, tcpPort, udpPort)
	if err != nil {
		return err
	}
	defer func() {
		stopVoice()
		select {
		case <-voiceExited:
		case <-time.After(500 * time.Millisecond):
		}
	}()
	if !gateway.open(ctx, lanIP) {
		wanIP = netip.IPv4Unspecified()
	}
	mappingCtx, stopMapping := context.WithCancel(ctx)
	defer func() {
		stopMapping()
		gateway.close()
	}()
	if len(gateway.created) > 0 {
		gateway.done = make(chan struct{})
		go gateway.renew(mappingCtx, lanIP)
	}
	internalVoice := &url.URL{Scheme: "http", Host: voiceAddress}
	s := &service{
		clients: map[string]*client{}, servers: map[string]*server{}, hosted: true,
		http:  &http.Client{Timeout: 5 * time.Second},
		voice: voiceConfig{URL: "ws://" + voiceAddress, Admin: internalVoice.String(), Key: "tinyspeak", Secret: key},
	}
	ticket := ""
	turnAddress := ""
	if os.Getenv("TINYSPEAK_LAN_ONLY") != "1" {
		secret := randomString(48)
		turnServer, addr, turnErr := startInternetVoice(net.IP(lanIP.AsSlice()), udpPort, secret)
		if turnErr == nil {
			defer turnServer.Close()
			ticket = hostInternetLink(ctx, certificate, net.JoinHostPort("127.0.0.1", strconv.Itoa(inbound.Addr().(*net.TCPAddr).Port)))
			if ticket != "" {
				s.voice.turnSecret = secret
				turnAddress = addr
			}
		}
	}
	if !standardPorts && ticket == "" {
		return errors.New("The hosting port is already in use and the Internet relay is unavailable.")
	}
	go s.expire(ctx)
	localServer := directHTTP(http.HandlerFunc(s.handle))
	remote := http.NewServeMux()
	remote.HandleFunc("/api", s.handle)
	remote.HandleFunc("/server/", s.checkInvite)
	if turnAddress != "" {
		remote.Handle("/voice-path", s.internetVoiceHandler(ctx, turnAddress))
	}
	voiceProxy := httputil.NewSingleHostReverseProxy(internalVoice)
	voiceProxy.ErrorLog = quietLog()
	remote.Handle("/rtc", voiceProxy)
	remote.Handle("/rtc/", voiceProxy)
	remoteServer := directHTTP(remote)
	remoteServer.TLSConfig = &tls.Config{MinVersion: tls.VersionTLS12, Certificates: []tls.Certificate{certificate}}
	defer localServer.Close()
	defer remoteServer.Close()
	failure := make(chan error, 2)
	go func() { failure <- localServer.Serve(local) }()
	go func() { failure <- remoteServer.ServeTLS(inbound, "", "") }()
	digest := sha256.Sum256(certificate.Certificate[0])
	route := directRoute{public: wanIP, local: lanIP, pin: [16]byte(digest[:16])}
	if err := json.NewEncoder(os.Stdout).Encode(map[string]any{
		"address": "http://" + local.Addr().String(), "route": route.encode(), "internet": ticket != "" || !wanIP.IsUnspecified(), "ticket": ticket,
	}); err != nil {
		return err
	}
	select {
	case <-ctx.Done():
		return nil
	case <-voiceExited:
		return errors.New("The local voice service stopped.")
	case err := <-failure:
		return err
	}
}

func (s *service) checkInvite(w http.ResponseWriter, r *http.Request) {
	code := strings.TrimPrefix(r.URL.Path, "/server/")
	if r.Method != http.MethodGet || !validID(code, 16) {
		http.NotFound(w, r)
		return
	}
	s.mu.Lock()
	srv := s.servers[code]
	var name string
	if srv != nil {
		name = srv.Name
	}
	s.mu.Unlock()
	if name == "" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(advert{Code: code, Name: name})
}

func directTransport(pin [16]byte) *http.Transport {
	return &http.Transport{
		DialContext:         (&net.Dialer{Timeout: 3 * time.Second}).DialContext,
		TLSHandshakeTimeout: 3 * time.Second, ResponseHeaderTimeout: 18 * time.Second, IdleConnTimeout: 25 * time.Second,
		TLSClientConfig: &tls.Config{
			MinVersion: tls.VersionTLS12,
			// The invite authenticates the host certificate before any credentials are sent.
			InsecureSkipVerify: true,
			VerifyConnection: func(state tls.ConnectionState) error {
				if len(state.PeerCertificates) == 0 {
					return errors.New("missing TinySpeak host identity")
				}
				digest := sha256.Sum256(state.PeerCertificates[0].Raw)
				if subtle.ConstantTimeCompare(pin[:], digest[:16]) != 1 {
					return errors.New("the TinySpeak host identity does not match the invite")
				}
				return nil
			},
		},
	}
}

func findDirectHost(ctx context.Context, route directRoute, code string, transport *http.Transport) (*url.URL, string, error) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	type result struct {
		url  *url.URL
		name string
	}
	addresses := []netip.Addr{route.public}
	if !slices.Contains(addresses, route.local) {
		addresses = append(addresses, route.local)
	}
	results := make(chan result, len(addresses))
	for _, address := range addresses {
		go func(address netip.Addr) {
			if address.IsUnspecified() {
				results <- result{}
				return
			}
			base := &url.URL{Scheme: "https", Host: net.JoinHostPort(address.String(), strconv.Itoa(directPort))}
			probe, cancel := context.WithTimeout(ctx, 4*time.Second)
			defer cancel()
			if name, ok := readAdvert(probe, base, code, transport); ok {
				results <- result{base, name}
				return
			}
			results <- result{}
		}(address)
	}
	for range addresses {
		if result := <-results; result.url != nil {
			return result.url, result.name, nil
		}
	}
	return nil, "", errors.New("Could not reach this server. Check that the owner's TinySpeak is open and ask for a fresh invite.")
}

func findHost(ctx context.Context, route directRoute, code string, transport *http.Transport) (*url.URL, string, bool, error) {
	if route.ticket != "" {
		if !route.local.IsUnspecified() {
			lan := &url.URL{Scheme: "https", Host: net.JoinHostPort(route.local.String(), strconv.Itoa(directPort))}
			probe, cancel := context.WithTimeout(ctx, 650*time.Millisecond)
			name, ok := readAdvert(probe, lan, code, transport)
			cancel()
			if ok {
				return lan, name, false, nil
			}
		}
		linkCtx, stopLink := context.WithCancel(ctx)
		if relay, err := guestInternetLink(linkCtx, route.ticket); err == nil {
			check, done := context.WithTimeout(linkCtx, 14*time.Second)
			name, ok := readAdvert(check, relay, code, transport)
			done()
			if ok {
				context.AfterFunc(ctx, stopLink)
				return relay, name, true, nil
			}
		}
		stopLink()
	}
	target, name, err := findDirectHost(ctx, route, code, transport)
	return target, name, false, err
}

func directGuest(ctx context.Context, invite string) error {
	route, code, err := decodeInvite(invite)
	if err != nil {
		return err
	}
	transport := directTransport(route.pin)
	if route.ticket != "" {
		transport.TLSHandshakeTimeout = 12 * time.Second
	}
	defer transport.CloseIdleConnections()
	target, _, relayed, err := findHost(ctx, route, code, transport)
	if err != nil {
		return err
	}
	listener, err := loopbackListener()
	if err != nil {
		return err
	}
	defer listener.Close()
	turnPort := 0
	turnHost := ""
	if relayed {
		bridge, err := guestVoiceBridge(ctx, target, code, transport)
		if err != nil {
			return err
		}
		defer bridge.Close()
		turnPort = bridge.Addr().(*net.TCPAddr).Port
		turnHost = bridge.Addr().(*net.TCPAddr).IP.String()
	}
	proxy := httputil.NewSingleHostReverseProxy(target)
	proxy.Transport = transport
	proxy.ErrorLog = quietLog()
	direct := proxy.Director
	proxy.Director = func(r *http.Request) {
		direct(r)
		r.Host = target.Host
		r.Header.Set("X-TinySpeak-Invite", code)
	}
	mux := http.NewServeMux()
	mux.Handle("/api", proxy)
	mux.Handle("/rtc", proxy)
	mux.Handle("/rtc/", proxy)
	server := directHTTP(mux)
	defer server.Close()
	failure := make(chan error, 1)
	go func() { failure <- server.Serve(listener) }()
	if err := json.NewEncoder(os.Stdout).Encode(map[string]any{"address": "http://" + listener.Addr().String(), "turnPort": turnPort, "turnHost": turnHost}); err != nil {
		return err
	}
	select {
	case <-ctx.Done():
		return nil
	case err := <-failure:
		return err
	}
}

func probeInvites(ctx context.Context, invites []string) error {
	jobs := make(chan string)
	results := make(chan advert, len(invites))
	var workers sync.WaitGroup
	for range min(2, len(invites)) {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for invite := range jobs {
				route, code, err := decodeInvite(invite)
				if err != nil {
					continue
				}
				transport := directTransport(route.pin)
				attempt, cancel := context.WithCancel(ctx)
				if route.ticket != "" {
					transport.TLSHandshakeTimeout = 12 * time.Second
				}
				_, name, _, err := findHost(attempt, route, code, transport)
				cancel()
				transport.CloseIdleConnections()
				if err == nil {
					results <- advert{Code: code, Name: name}
				}
			}
		}()
	}
	for _, invite := range invites {
		select {
		case jobs <- invite:
		case <-ctx.Done():
			close(jobs)
			workers.Wait()
			return nil
		}
	}
	close(jobs)
	workers.Wait()
	close(results)
	list := make([]advert, 0, len(results))
	for result := range results {
		list = append(list, result)
	}
	return json.NewEncoder(os.Stdout).Encode(struct {
		Catalog []advert `json:"catalog"`
	}{Catalog: list})
}

func portableMain(args []string) error {
	ctx, cancel := context.WithCancel(context.Background())
	defer func() { cancel(); transportChildren.Wait() }()
	go func() {
		_, _ = io.Copy(io.Discard, os.Stdin)
		cancel()
	}()
	switch {
	case len(args) == 2 && args[0] == "host":
		return directHost(ctx, args[1], "")
	case len(args) == 3 && args[0] == "host":
		return directHost(ctx, args[1], args[2])
	case len(args) == 2 && args[0] == "join":
		return directGuest(ctx, args[1])
	case len(args) > 1 && len(args) <= 513 && args[0] == "probe":
		return probeInvites(ctx, args[1:])
	default:
		return errors.New("Unknown TinySpeak helper command.")
	}
}
