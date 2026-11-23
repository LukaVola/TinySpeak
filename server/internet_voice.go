package main

import (
	"context"
	"errors"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/pion/turn/v5"
)

// A TURN allocation for an Internet guest may exchange UDP packets only with
// this host's own LiveKit voice socket. No other ports or hosts are reachable.
type voiceAllocations struct {
	turn.RelayAddressGeneratorStatic
	media *net.UDPAddr
}

type voicePackets struct {
	net.PacketConn
	media *net.UDPAddr
}

func (p voicePackets) allowed(address net.Addr) bool {
	a, ok := address.(*net.UDPAddr)
	return ok && a.Port == p.media.Port && a.IP.Equal(p.media.IP)
}
func (p voicePackets) WriteTo(b []byte, to net.Addr) (int, error) {
	if !p.allowed(to) {
		return len(b), nil
	}
	return p.PacketConn.WriteTo(b, to)
}
func (p voicePackets) ReadFrom(b []byte) (int, net.Addr, error) {
	for {
		n, from, err := p.PacketConn.ReadFrom(b)
		if err != nil || p.allowed(from) {
			return n, from, err
		}
	}
}
func (g *voiceAllocations) AllocatePacketConn(conf turn.AllocateListenerConfig) (net.PacketConn, net.Addr, error) {
	conn, addr, err := g.RelayAddressGeneratorStatic.AllocatePacketConn(conf)
	if err != nil {
		return nil, nil, err
	}
	return voicePackets{conn, g.media}, addr, nil
}
func (*voiceAllocations) AllocateListener(turn.AllocateListenerConfig) (net.Listener, net.Addr, error) {
	return nil, nil, errors.New("Only TinySpeak voice datagrams are supported.")
}
func (*voiceAllocations) AllocateConn(turn.AllocateConnConfig) (net.Conn, error) {
	return nil, errors.New("Only TinySpeak voice datagrams are supported.")
}

func startInternetVoice(ip net.IP, mediaPort int, secret string) (*turn.Server, string, error) {
	l, err := loopbackListener()
	if err != nil {
		return nil, "", err
	}
	gen := &voiceAllocations{turn.RelayAddressGeneratorStatic{RelayAddress: ip, Address: ip.String()}, &net.UDPAddr{IP: ip, Port: mediaPort}}
	s, err := turn.NewServer(turn.ServerConfig{Realm: "tinyspeak", AuthHandler: turn.LongTermTURNRESTAuthHandler(secret, nil),
		ListenerConfigs: []turn.ListenerConfig{{Listener: l, RelayAddressGenerator: gen,
			PermissionHandler: func(_ net.Addr, peer net.IP) bool { return ip.Equal(peer) }}}})
	if err != nil {
		l.Close()
		return nil, "", err
	}
	return s, l.Addr().String(), nil
}

func bridgeVoice(ctx context.Context, a io.ReadWriteCloser, input io.Reader, b io.ReadWriteCloser) {
	closeBoth := func() { a.Close(); b.Close() }
	stop := context.AfterFunc(ctx, closeBoth)
	defer stop()
	done := make(chan struct{})
	go func() { _, _ = io.Copy(b, input); closeBoth(); close(done) }()
	_, _ = io.Copy(a, b)
	closeBoth()
	<-done
}

const voiceUpgrade = "tinyspeak-voice"

func voiceCredentials(secret, member string) (string, string) {
	user, password, _ := turn.GenerateLongTermTURNRESTCredentials(secret, member, 24*time.Hour)
	return user, password
}

func (s *service) internetVoiceHandler(ctx context.Context, addr string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		code := r.Header.Get("X-TinySpeak-Invite")
		s.mu.Lock()
		allowed := validID(code, 16) && s.servers[code] != nil
		s.mu.Unlock()
		if !allowed || r.Method != http.MethodGet || !strings.EqualFold(r.Header.Get("Upgrade"), voiceUpgrade) {
			http.NotFound(w, r)
			return
		}
		local, err := net.DialTimeout("tcp4", addr, time.Second)
		if err != nil {
			http.Error(w, "Voice unavailable", http.StatusServiceUnavailable)
			return
		}
		defer local.Close()
		conn, rw, err := http.NewResponseController(w).Hijack()
		if err != nil {
			return
		}
		defer conn.Close()
		_ = conn.SetDeadline(time.Time{})
		_, err = rw.WriteString("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: " + voiceUpgrade + "\r\n\r\n")
		if err != nil || rw.Flush() != nil {
			return
		}
		bridgeVoice(ctx, conn, rw.Reader, local)
	}
}

func guestVoiceBridge(ctx context.Context, target *url.URL, code string, transport *http.Transport) (net.Listener, error) {
	// WebRTC binds TURN/TCP to a network interface. On Windows, a socket
	// bound to Ethernet cannot connect to 127.0.0.1. Accept only this PC.
	ip := localIPv4()
	l, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.IP(ip.AsSlice())})
	if err != nil {
		return nil, err
	}
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			if !c.RemoteAddr().(*net.TCPAddr).IP.Equal(net.IP(ip.AsSlice())) {
				c.Close()
				continue
			}
			go func(client net.Conn) {
				defer client.Close()
				remote, err := http.NewRequestWithContext(ctx, http.MethodGet, target.String()+"/voice-path", nil)
				if err != nil {
					return
				}
				remote.Header.Set("Connection", "Upgrade")
				remote.Header.Set("Upgrade", voiceUpgrade)
				remote.Header.Set("X-TinySpeak-Invite", code)
				reply, err := transport.RoundTrip(remote)
				if err != nil {
					return
				}
				defer reply.Body.Close()
				stream, ok := reply.Body.(io.ReadWriteCloser)
				if !ok || reply.StatusCode != http.StatusSwitchingProtocols {
					return
				}
				bridgeVoice(ctx, client, client, stream)
			}(c)
		}
	}()
	return l, nil
}
