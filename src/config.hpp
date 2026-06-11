#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "routes.hpp"

namespace solcache {

// One proxy route: requests whose path matches `prefix` are forwarded to `upstream` (with the
// matched prefix preserved — the forwarded URL is `upstream + (path - prefix)`), classified
// according to `kind`, and optionally sent with extra `headers` (e.g. provider auth).
struct Route {
    std::string prefix;
    CacheKind kind = CacheKind::passthrough;
    std::string upstream;
    std::string backup;                // optional second upstream (reserved; empty if none)
    std::vector<std::string> headers;  // extra upstream headers, formatted "Key: Value"
};

struct Config {
    std::string listen_ip = "127.0.0.1";
    uint16_t port = 5014;
    std::vector<Route> routes;

    // Longest-prefix match on the request path; nullptr if nothing matches.
    const Route* match(std::string_view path) const;
};

// Parse one --route spec of the form
//   "prefix=/wallet; kind=tron_wallet; upstream=URL; [backup=URL;] [header=K: V; ...]"
// Fields are separated by ';' and split on the first '=' (so URLs may contain '='); whitespace
// around keys/values is trimmed; `header` may be repeated.  Throws std::invalid_argument on any
// malformed or missing required (prefix/kind/upstream) field.
Route parse_route(std::string_view spec);

}  // namespace solcache
