# biphony_network_backend
## Summary
`biphony_network_backend` manages the transmission and reception of data published on
networked topics (created by `get_network_writer` in Switchboard) through
[Biphony](https://github.com/NSF-Breaking-Low-Agora/biphony_quiche), a QUIC-based
multipath transport. It is a drop-in structural alternative to `tcp_network_backend`:
both implement the same `network::network_backend` phonebook service, so exactly one of
the two may be loaded in a given profile (they can't coexist — `phonebook::register_impl`
asserts against duplicate registration of the same service interface).

```
ILLIXR_IS_CLIENT: Set to `1` if this process is acting as the client; otherwise `0`.
ILLIXR_BIPHONY_SERVER_ADDRESS: Bare "ip:port" of the server, e.g. `127.0.0.1:4433`.
  Used by both roles: the server binds to it directly, and the client wraps it
  into a connect URL (`https://<address>/`) internally, since Biphony's client
  API takes a URL rather than a bare socket address. Set the same value on
  both the client and server process of a pair.
```

### Client-role environment variables
```
ILLIXR_BIPHONY_NO_VERIFY: Set to `1` to skip TLS certificate verification (needed
  when the server is using a self-signed certificate).
ILLIXR_BIPHONY_CLIENT_ADDRESSES: Comma-separated list of extra local "ip:port"
  addresses to bind, one per additional multipath QUIC path (path 0 is
  implicit/default; each entry adds one more path). Equivalent to passing
  `--address`/`-A` once per address to the Biphony example clients
  (biphony-api-echo-client, biphony-api-xr-emulator-client). For the common
  two-path (eMBB + URLLC) setup used with a non-"single_path" steering scheme,
  set this to a single extra address and set ILLIXR_BIPHONY_INITIAL_MAX_PATH_ID=1.
```

### Server-role environment variables
```
ILLIXR_BIPHONY_CERT_PATH: Path to the TLS certificate file.
ILLIXR_BIPHONY_KEY_PATH: Path to the TLS private key file.
```

### Optional, both roles
```
ILLIXR_BIPHONY_STEERING_SCHEME: Multipath steering scheme (default: Biphony's own
  default, "single_path").
ILLIXR_BIPHONY_INITIAL_MAX_PATH_ID: Initial max path id for multipath (default: 0,
  i.e. single-path).
ILLIXR_BIPHONY_IDLE_TIMEOUT_MS: QUIC idle timeout in milliseconds (default: 60000).
  Biphony's own hardcoded default is 5000ms, which is too short for ILLIXR: the
  VIO/encoder pipeline can go several seconds between the topic_create handshake
  and the first real data send while it warms up (dataset loading, GStreamer
  encoder init, etc.), long enough for quiche to tear the connection down as idle
  before any real traffic ever flows.
```

## Priority mapping
`topic_config::priority` is mapped onto Biphony/QUIC's HTTP/3 extensible-priority
"urgency" (RFC 9218), where a LOWER numeric value means MORE urgent — the opposite of
ILLIXR's convention, where `HIGHEST` is the numerically largest enum value. The mapping
inverts the scale: `biphony_priority = topic_config::HIGHEST - topic_config_priority`.

## Build prerequisites
Building this plugin fetches and compiles the Biphony QUIC library via Cargo, as part
of the normal CMake build. This requires a Rust toolchain (`cargo` on `PATH`; see
https://rustup.rs) in addition to the `cmake` ILLIXR itself already requires (Biphony's
`quiche` dependency builds BoringSSL via CMake at cargo-build time). The first build can
take several minutes while BoringSSL compiles.
