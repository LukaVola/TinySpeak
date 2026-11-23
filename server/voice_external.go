//go:build !embedded_voice

package main

import (
	"context"
	"errors"
	"os/exec"
)

// Shared-relay deployments and older launchers may supply a separate LiveKit executable.
func startVoiceService(ctx context.Context, program, config string) (<-chan error, error) {
	if program == "" {
		return nil, errors.New("Open TinySpeak Host to start a desktop server.")
	}
	cmd := exec.CommandContext(ctx, program, "--config-body", config)
	hideConsole(cmd)
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	exited := make(chan error, 1)
	go func() {
		exited <- cmd.Wait()
		close(exited)
	}()
	return exited, nil
}
