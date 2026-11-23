//go:build embedded_voice

package main

import (
	"context"

	"github.com/livekit/livekit-server/pkg/config"
	"github.com/livekit/livekit-server/pkg/routing"
	lkservice "github.com/livekit/livekit-server/pkg/service"
	"github.com/livekit/livekit-server/pkg/telemetry/prometheus"
)

// The public chat listener and the voice listeners belong to the same Windows executable.
func startVoiceService(ctx context.Context, _ string, settings string) (<-chan error, error) {
	conf, err := config.NewConfig(settings, true, nil, nil)
	if err != nil {
		return nil, err
	}
	config.InitLoggerFromConfig(&conf.Logging)
	if err := conf.ValidateKeys(); err != nil {
		return nil, err
	}
	node, err := routing.NewLocalNode(conf)
	if err != nil {
		return nil, err
	}
	if err := prometheus.Init(string(node.NodeID()), node.NodeType()); err != nil {
		return nil, err
	}
	server, err := lkservice.InitializeServer(conf, node)
	if err != nil {
		return nil, err
	}
	exited := make(chan error, 1)
	go func() {
		exited <- server.Start()
		close(exited)
	}()
	go func() {
		<-ctx.Done()
		if server.IsRunning() {
			server.Stop(true)
		}
	}()
	return exited, nil
}
