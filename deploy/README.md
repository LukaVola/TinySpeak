# Deploy the TinySpeak relay

This configuration runs the TinySpeak relay, LiveKit and Caddy on one publicly reachable Linux host with Docker and Compose.

1. Point two DNS names at the host's public IP address, one for the relay and one for LiveKit.
2. Allow incoming TCP on ports 80, 443 and 7881, and UDP on ports 7882 and 3478.
3. Copy `.env.example` to `.env`. Set both DNS names and generate a random secret of at least 32 characters. Keep the LiveKit key and secret on the server.
4. Run `docker compose config --quiet` and then `docker compose up -d --build` from this directory.
5. Check `https://<KANAL_DOMAIN>/health`. For the optional shared-relay mode, launch the Windows client with its `KANAL_DEMO_ADDRESS` environment variable set to `https://<KANAL_DOMAIN>`.

Caddy obtains HTTPS certificates for both names. LiveKit serves media over public UDP with TURN/UDP available. Clients on networks that block UDP can use the exposed RTC TCP port. For networks limited to HTTPS-like traffic, see the [LiveKit self-hosting documentation](https://docs.livekit.io/transport/self-hosting/deployment/) for configuring TURN/TLS on a separate DNS name.

The TinySpeak relay holds live sessions in memory. Caddy stores its certificates and configuration in persistent volumes. Users control local storage of servers and chat logs. The `KANAL_` environment variable names remain compatible with earlier releases.
