package main

import (
	"context"
	"errors"
	"net/netip"
	"os"
	"time"

	"github.com/huin/goupnp/dcps/internetgateway1"
	"github.com/huin/goupnp/dcps/internetgateway2"
	"github.com/huin/goupnp/soap"
)

type mapper interface {
	GetExternalIPAddressCtx(context.Context) (string, error)
	GetSpecificPortMappingEntryCtx(context.Context, string, uint16, string) (uint16, string, bool, string, uint32, error)
	AddPortMappingCtx(context.Context, string, uint16, string, uint16, string, bool, string, uint32) error
	DeletePortMappingCtx(context.Context, string, uint16, string) error
}

type mapping struct {
	port     uint16
	protocol string
}

var directMappings = [...]mapping{{directPort, "TCP"}, {mediaUDP, "UDP"}, {mediaTCP, "TCP"}}

const leaseSeconds = 300

type gatewayMapping struct {
	public  netip.Addr
	router  mapper
	label   string
	created []mapping
	done    chan struct{}
}

func publicIPv4(ip netip.Addr) bool {
	return ip.Is4() && ip.IsGlobalUnicast() && !ip.IsPrivate() &&
		!netip.MustParsePrefix("100.64.0.0/10").Contains(ip) &&
		!netip.MustParsePrefix("0.0.0.0/8").Contains(ip) &&
		!netip.MustParsePrefix("240.0.0.0/4").Contains(ip)
}

func discoverGateway(ctx context.Context, local netip.Addr) gatewayMapping {
	result := gatewayMapping{public: netip.IPv4Unspecified()}
	if os.Getenv("TINYSPEAK_LAN_ONLY") == "1" || local.IsLoopback() {
		return result
	}
	if publicIPv4(local) {
		result.public = local
		return result
	}
	discovery, cancel := context.WithTimeout(ctx, 4200*time.Millisecond)
	defer cancel()
	results := make(chan mapper, 3)
	go func() {
		list, _, _ := internetgateway2.NewWANIPConnection2ClientsCtx(discovery)
		results <- checkGateways(discovery, list)
	}()
	go func() {
		list, _, _ := internetgateway1.NewWANIPConnection1ClientsCtx(discovery)
		results <- checkGateways(discovery, list)
	}()
	go func() {
		list, _, _ := internetgateway1.NewWANPPPConnection1ClientsCtx(discovery)
		results <- checkGateways(discovery, list)
	}()
	for range 3 {
		select {
		case <-discovery.Done():
			return result
		case router := <-results:
			if router == nil {
				continue
			}
			raw, _ := router.GetExternalIPAddressCtx(discovery)
			ip, err := netip.ParseAddr(raw)
			if err == nil && publicIPv4(ip) {
				return gatewayMapping{public: ip, router: router, label: "TinySpeak " + randomString(12)}
			}
		}
	}
	return result
}

func checkGateways[T mapper](ctx context.Context, routers []T) mapper {
	for _, router := range routers {
		raw, _ := router.GetExternalIPAddressCtx(ctx)
		if ip, err := netip.ParseAddr(raw); err == nil && publicIPv4(ip) {
			return router
		}
	}
	return nil
}

func (g *gatewayMapping) open(ctx context.Context, local netip.Addr) bool {
	if g.router == nil {
		return !g.public.IsUnspecified()
	}
	setup, cancel := context.WithTimeout(ctx, 2700*time.Millisecond)
	defer cancel()
	for _, rule := range directMappings {
		_, _, _, _, _, err := g.router.GetSpecificPortMappingEntryCtx(setup, "", rule.port, rule.protocol)
		var fault *soap.SOAPFaultError
		if !errors.As(err, &fault) || fault.Detail.UPnPError.Errorcode != 714 {
			continue
		}
		if g.router.AddPortMappingCtx(setup, "", rule.port, rule.protocol, rule.port, local.String(), true, g.label, leaseSeconds) == nil {
			g.created = append(g.created, rule)
		}
	}
	signaling, media := false, false
	for _, rule := range g.created {
		signaling = signaling || rule.port == directPort
		media = media || rule.port != directPort
	}
	if !signaling || !media {
		g.close()
		g.created = nil
		return false
	}
	return true
}

func (g *gatewayMapping) renew(ctx context.Context, local netip.Addr) {
	if len(g.created) == 0 {
		return
	}
	tick := time.NewTicker(2 * time.Minute)
	defer tick.Stop()
	defer close(g.done)
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
			update, cancel := context.WithTimeout(ctx, 2500*time.Millisecond)
			for _, rule := range g.created {
				_, _, _, label, _, err := g.router.GetSpecificPortMappingEntryCtx(update, "", rule.port, rule.protocol)
				if err == nil && label == g.label {
					_ = g.router.AddPortMappingCtx(update, "", rule.port, rule.protocol, rule.port, local.String(), true, g.label, leaseSeconds)
				}
			}
			cancel()
		}
	}
}

func (g *gatewayMapping) close() {
	if g.done != nil {
		<-g.done
	}
	if len(g.created) == 0 {
		return
	}
	cleanup, cancel := context.WithTimeout(context.Background(), 1800*time.Millisecond)
	defer cancel()
	for _, rule := range g.created {
		_, _, _, label, _, err := g.router.GetSpecificPortMappingEntryCtx(cleanup, "", rule.port, rule.protocol)
		if err == nil && label == g.label {
			_ = g.router.DeletePortMappingCtx(cleanup, "", rule.port, rule.protocol)
		}
	}
}
