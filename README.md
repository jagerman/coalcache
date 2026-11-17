# coalcache

A caching, request-coalescing reverse proxy for blockchain RPC providers.

coalcache sits between RPC clients (such as a fleet of validators or chain-watching services) and
one or more upstream RPC providers.  Identical concurrent requests are collapsed into a single
upstream request, and responses whose content cannot change (historical blocks, mined
transactions, logs) are cached, so that many clients asking the same questions cost roughly one
client's worth of upstream requests.

It currently understands:

- **Solana JSON-RPC**: `getSignatureStatuses` lookups from many clients are batched into combined
  upstream requests, and all other requests are cached for a short, fixed time.
- **EVM-style JSON-RPC** (any Ethereum-compatible chain, or TRON's `/jsonrpc` facade): immutable
  lookups are cached for a long time, the chain head briefly, and current-state reads, fee queries
  and writes are never cached.
- **TRON native HTTP API** (`/wallet/...`): historical balance and transaction lookups are cached;
  everything else is passed through.
- **Passthrough**: forward to the upstream without caching.

## Building

### Dependencies

You need a C++20 compiler, CMake 3.18 or newer, and pkg-config, plus development packages for:

- libcurl
- nlohmann-json
- libsodium
- zlib
- the dependencies of [libquic](https://github.com/session-foundation/libquic): ngtcp2 (with its
  GnuTLS crypto library), GnuTLS, nettle and libevent

On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config git \
    libcurl4-openssl-dev nlohmann-json3-dev libsodium-dev zlib1g-dev \
    libngtcp2-dev libngtcp2-crypto-gnutls-dev libgnutls28-dev nettle-dev libevent-dev
```

If libquic can't find a suitable version of one of its dependencies, it downloads and builds that
dependency statically.  Pass `-DBUILD_STATIC_DEPS=ON` to build all of them that way.

uWebSockets, CLI11 and libquic are included as git submodules.

### Compiling

```sh
git clone --recursive https://github.com/jagerman/coalcache.git coalcache
cd coalcache
# or, in an existing checkout without the submodules:
git submodule update --init --recursive

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

This produces the binary `build/coalcache`.  There is no install target, so copy it wherever you
want it, or run it directly from the build directory.

If linking fails with undefined references to `spdlog::details::log_msg` or other `fmt::` symbols,
your system's spdlog and fmt packages were built against different fmt versions (this can happen
during distribution library transitions).  Reconfigure with
`-DOXEN_LOGGING_FORCE_SUBMODULES=ON` to use the bundled oxen-logging, fmt and spdlog instead.

## Configuring

All configuration is done through command-line options, which can also be put in a config file
(see below).  The only required setting is at least one `--route`.

### Routes

A route maps an incoming path prefix to an upstream URL and decides how requests on it are handled.
A route is written as `;`-separated `key=value` fields:

```
prefix=/tron/wallet; kind=tron_wallet; upstream=https://provider.example/KEY/wallet; header=X-Api-Key: abc123
```

| Field      | Required | Meaning                                                                 |
|------------|----------|-------------------------------------------------------------------------|
| `prefix`   | yes      | Incoming path prefix to match; must start with `/`.                     |
| `kind`     | yes      | `solana`, `evm_jsonrpc`, `tron_wallet` or `passthrough`.                |
| `upstream` | yes      | Upstream URL to forward to.                                             |
| `header`   | no       | Extra `Key: Value` header to send upstream (e.g. provider API keys); may be repeated. |
| `backup`   | no       | Reserved for a secondary upstream; currently unused.                    |

Each incoming request is matched to the route with the longest matching prefix (matching only at
`/` boundaries, so `/tron` matches `/tron/x` but not `/tronx`).  For `evm_jsonrpc`,
`tron_wallet` and `passthrough` routes, the prefix is replaced by the upstream URL: with the route
above, a request for `/tron/wallet/getblockbalance` is forwarded to
`https://provider.example/KEY/wallet/getblockbalance`.  `solana` routes send every request to the
upstream URL exactly as given.

Requests that match no route get a `404`.  Only `POST` requests are handled.

When an `evm_jsonrpc` or `tron_wallet` route receives a method that has no caching policy, it passes
the request through and logs a warning (once per method) so that you can decide whether it should
be cached.

### Options

| Option                            | Default     | Description                                               |
|-----------------------------------|-------------|-----------------------------------------------------------|
| `-r`, `--route SPEC`              | (none)      | Add a route (repeatable; at least one required).          |
| `-l`, `--listen-ip IP`            | `127.0.0.1` | IPv4 address to listen on.                                |
| `-p`, `--port PORT`               | `5014`      | Port to listen on.                                        |
| `-t`, `--upstream-timeout MS`     | `3000`      | How long to wait for an upstream response before failing the request (all routes).  Set this below your clients' own RPC timeout, so a hung upstream produces a prompt error instead of a dropped request. |
| `-s`, `--sigstat-coalesce N`      | `256`       | (solana) Maximum `getSignatureStatuses` lookups per combined upstream request; 0 disables coalescing. |
| `-S`, `--sigstat-wait MS`         | `3000`      | (solana) Maximum time to wait for more `getSignatureStatuses` lookups before sending. |
| `-c`, `--cache-time MS`           | `15000`     | (solana) How long to cache results.                       |
| `-C`, `--negative-cache-time MS`  | `5000`      | (solana) How long to cache not-found signature statuses; 0 or less to never cache them. |
| `-L`, `--log-level LEVEL`         | `info`      | One of `trace`, `debug`, `info`, `warn`, `error`, `critical`, `off`. |
| `--config FILE`                   |             | Read options from a config file.                          |

Cache lifetimes for the `evm_jsonrpc` and `tron_wallet` kinds are built in (see `src/routes.cpp`)
and are not configurable.

### Config file

Any option can be given in a config file using its long name.  Repeat `route` once per route:

```toml
# coalcache.conf
listen-ip = "10.0.0.2"
port = 80
upstream-timeout = 3000
log-level = "info"

route = "prefix=/sol; kind=solana; upstream=https://solana.provider.example/KEY"
route = "prefix=/tron/wallet; kind=tron_wallet; upstream=https://tron.provider.example/wallet; header=TRON-PRO-API-KEY: abc123"
route = "prefix=/tron/jsonrpc; kind=evm_jsonrpc; upstream=https://tron.provider.example/jsonrpc; header=TRON-PRO-API-KEY: abc123"
route = "prefix=/eth; kind=evm_jsonrpc; upstream=https://eth.provider.example/KEY"
```

Options given on the command line override the same option in the config file.

## Running

```sh
./build/coalcache --config coalcache.conf
```

or entirely from the command line:

```sh
./build/coalcache -l 0.0.0.0 -p 5014 \
    -r 'prefix=/eth; kind=evm_jsonrpc; upstream=https://eth.provider.example/KEY'
```

Then point your clients at coalcache instead of the provider, e.g. `http://10.0.0.2/eth` instead of
`https://eth.provider.example/KEY`.  Because the provider URL and authentication headers live in
coalcache's configuration, clients don't need the provider's API key.

Logs go to stderr.  coalcache stays in the foreground, so run it under a service manager such as
systemd for production use.  If it sits behind another reverse proxy, the client address it logs is
taken from `X-Real-IP` or `X-Forwarded-For` when present.

coalcache does not do TLS or authentication on its listening socket: bind it to a private address,
or put it behind a reverse proxy that handles those.

## Helper scripts

- `tron-rpc-check.py URL`: checks whether a TRON RPC provider supports everything the Chainflip
  engine needs, including historical balance lookups (`/wallet/getblockbalance`) and `eth_getLogs`.
  Useful for choosing an upstream for TRON routes.
- `tronwallet.py`: a minimal TRX-only command-line wallet that talks to a coalcache TRON route.  See
  the script's docstring for usage.

## License

GPLv3; see [LICENSE](LICENSE).
