package main

import (
	"encoding/base32"
	"net"
	"net/netip"
	"strings"
	"testing"
)

func exampleTicket(relay string) string {
	raw := make([]byte, 33)
	raw = append(raw, 1, 0, byte(len(relay)))
	raw = append(raw, relay...)
	return "endpoint" + strings.ToLower(base32.StdEncoding.WithPadding(base32.NoPadding).EncodeToString(raw))
}

func TestInternetInviteCanConnectWithoutAnyDirectAddress(t *testing.T) {
	route := directRoute{public: netip.IPv4Unspecified(), local: netip.IPv4Unspecified()}
	tag := "ABCD0123EFGH4567"
	ticket := exampleTicket("https://euc1-1.relay.n0.iroh.link./")
	got, room, err := decodeInvite("TS2-" + route.encode() + "." + ticket + "." + tag)
	if err != nil || got.ticket != ticket || room != tag {
		t.Fatal("Could not decode the relay-only invite", err)
	}
	if _, _, err = decodeInvite("TS1-" + route.encode() + "." + tag); err == nil {
		t.Fatal("Legacy invite must carry a local address")
	}
	for _, relay := range []string{"https://unrelated.example/", "https://test.relay.n0.iroh.link:444/", "http://test.relay.n0.iroh.link/"} {
		if validInternetTicket(exampleTicket(relay)) {
			t.Fatal("Unexpected relay accepted")
		}
	}
}

func TestVoiceRelayIsRestrictedToThisHostsMediaSocket(t *testing.T) {
	media := &net.UDPAddr{IP: net.IPv4(192, 0, 2, 1), Port: 30474}
	filter := voicePackets{media: media}
	if !filter.allowed(media) || filter.allowed(&net.UDPAddr{IP: media.IP, Port: 53}) ||
		filter.allowed(&net.UDPAddr{IP: net.IPv4(192, 0, 2, 2), Port: media.Port}) {
		t.Fatal("Unexpected voice forwarding policy")
	}
}
