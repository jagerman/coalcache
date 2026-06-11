#include "config.hpp"

#include <stdexcept>

namespace solcache {

const Route* Config::match(std::string_view path) const {
    const Route* best = nullptr;
    for (const auto& r : routes) {
        const auto& p = r.prefix;
        if (path.size() < p.size() || path.substr(0, p.size()) != p)
            continue;
        // Match only on a path boundary: exact match, the next char is '/', or the prefix itself
        // ends in '/' (e.g. a "/" catch-all).
        if (path.size() > p.size() && path[p.size()] != '/' && p.back() != '/')
            continue;
        if (!best || p.size() > best->prefix.size())
            best = &r;
    }
    return best;
}

namespace {

    std::string_view trim(std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.remove_suffix(1);
        return s;
    }

}  // namespace

Route parse_route(std::string_view spec) {
    Route r;
    bool have_prefix = false, have_kind = false, have_upstream = false;

    for (size_t pos = 0; pos <= spec.size();) {
        auto semi = spec.find(';', pos);
        auto end = semi == std::string_view::npos ? spec.size() : semi;
        auto field = trim(spec.substr(pos, end - pos));
        pos = end + 1;
        if (field.empty())
            continue;

        auto eq = field.find('=');
        if (eq == std::string_view::npos)
            throw std::invalid_argument{"route field missing '=': " + std::string{field}};
        auto key = trim(field.substr(0, eq));
        auto val = trim(field.substr(eq + 1));

        if (key == "prefix") {
            r.prefix = std::string{val};
            have_prefix = true;
        } else if (key == "kind") {
            r.kind = cache_kind_from_string(val);
            have_kind = true;
        } else if (key == "upstream") {
            r.upstream = std::string{val};
            have_upstream = true;
        } else if (key == "backup") {
            r.backup = std::string{val};
        } else if (key == "header") {
            r.headers.emplace_back(val);
        } else {
            throw std::invalid_argument{"unknown route field: " + std::string{key}};
        }
    }

    if (!have_prefix || !have_kind || !have_upstream)
        throw std::invalid_argument{
                "route needs prefix, kind and upstream: " + std::string{spec}};
    if (r.prefix.empty() || r.prefix.front() != '/')
        throw std::invalid_argument{"route prefix must start with '/': " + r.prefix};
    // A trailing slash on the upstream would double up with the preserved path; trim it.
    while (!r.upstream.empty() && r.upstream.back() == '/')
        r.upstream.pop_back();

    return r;
}

}  // namespace solcache
