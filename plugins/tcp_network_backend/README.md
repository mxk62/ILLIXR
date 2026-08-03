# TCP_network_backend
## Summary
`TCP_network_backend` manages the transmission and reception of data published on networked topics (created by `get_network_writer` in Switchboard) through a TCP backend.
The users need to specify the IP address and port number of the server and client through setting some environment variables.
```
ILLIXR_TCP_SERVER_IP: IP address of the server.
ILLIXR_TCP_SERVER_PORT: Port number of the server.
ILLIXR_IS_CLIENT: Set to `1` if this process is acting as a client; otherwise, `0`.
```
The client will initiate connection to the server by specifying the IP and port number, and the server will accept the connection request.

Optionally, the client can set its IP address and port number.
```
ILLIXR_TCP_CLIENT_IP: IP address of the client.
ILLIXR_TCP_CLIENT_PORT: Port number of the client.
```

## Multipath TCP (MPTCP)

Set `ILLIXR_TCP_USE_MPTCP=1` on both endpoints to open the connection as [Multipath TCP](https://www.mptcp.dev/) instead of plain TCP:
```
ILLIXR_TCP_USE_MPTCP: Set to `1` to use MPTCP; otherwise plain TCP is used (default).
```

This only changes the protocol passed to `socket()` — the rest of the plugin (config, framing, topic handshake) is unchanged, so it's a drop-in toggle rather than a separate plugin.

Requirements and behavior:
- Linux only, kernel >= 5.6 (>= 5.19 recommended), with `net.mptcp.enabled=1`.
- MPTCP negotiates capability with the peer during the TCP handshake, so it's safe to enable on just one side: if the peer doesn't support MPTCP, the connection transparently falls back to plain single-path TCP.
- Actually using more than one network path requires OS-level provisioning on both hosts (e.g. `ip mptcp endpoint add <ip> dev <iface> subflow` / `signal`) — this plugin only requests an MPTCP-capable socket, it does not configure endpoints or interfaces itself. Without that provisioning, an MPTCP connection still works but behaves like single-path TCP.
- If `ILLIXR_TCP_USE_MPTCP=1` is set but the local kernel doesn't support MPTCP (or the build isn't Linux), the plugin fails fast with a clear error rather than silently falling back.

### Verifying subflow usage

When `ILLIXR_TCP_USE_MPTCP=1` on Linux, the plugin polls per-subflow statistics every 2 seconds via `getsockopt(SOL_MPTCP, MPTCP_FULL_INFO)` (kernel >= 5.19) and logs each subflow's local/remote address plus cumulative and delta byte counts at `debug` level. Set `ILLIXR_LOG_LEVEL=debug` to see it — if more than one subflow shows nonzero deltas, traffic is actually being split across paths rather than just having negotiated MPTCP capability.
