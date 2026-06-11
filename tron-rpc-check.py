#!/usr/bin/env python3
"""tron-rpc-check.py — validate a TRON RPC provider against everything the
Chainflip engine (chainflip-engine 2.2.3) actually needs.

It exercises the exact calls the engine makes and, crucially, checks that they
return *real* data — not just HTTP 200 with an empty/zero body. The headline
gates are:

  * /wallet/getblockbalance returns a non-empty balance trace for a block that
    demonstrably has transactions  ->  proves the node has the historical
    balance feature (java-tron `storage.balance.history.lookup = true`).
  * eth_getLogs returns USDT Transfer events  ->  proves the JSON-RPC log path
    used for USDT deposit witnessing works.

Give it a single provider base URL; it derives and probes *both* the native
wallet API (…/wallet/*) and the Ethereum-style JSON-RPC facade (…/jsonrpc), so
you can see at a glance which surfaces — and which capabilities (esp. historical
balance) — a given provider actually offers:

  ./tron-rpc-check.py https://prov.example/<token>
  ./tron-rpc-check.py https://api.trongrid.io
  ./tron-rpc-check.py https://prov.example/<token>/jsonrpc   # trailing /wallet
                                                             # or /jsonrpc is fine

A trailing /wallet or /jsonrpc on the URL is stripped before deriving both.
Exit status is non-zero only if the base is unreachable on *both* surfaces.
"""

import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from urllib.parse import parse_qsl, urlsplit, urlunsplit

# ---------------------------------------------------------------------------
# Network defaults
# ---------------------------------------------------------------------------
NETWORKS = {
    # chain_id (eth_chainId hex), USDT contract in TRON hex (41-prefixed)
    "mainnet": {"chain_id": "0x2b6653dc", "usdt_tron": "41a614f803b6fd780986a42c78ec9c7f77e6ded13c"},
    "nile":    {"chain_id": "0xcd8690dc", "usdt_tron": None},  # supply --usdt for nile
}
# keccak256("Transfer(address,address,uint256)")
TRANSFER_TOPIC = "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef"

# How far behind the tip to probe (mirrors the engine's ~safety-margin lookback)
LOOKBACK = 20
# When hunting for a block with activity, scan at most this many blocks back
SCAN_DEPTH = 60

# ---------------------------------------------------------------------------
# Tiny result tracker
# ---------------------------------------------------------------------------
_USE_COLOR = sys.stdout.isatty()


def _c(code, s):
    return f"\033[{code}m{s}\033[0m" if _USE_COLOR else s


# Path words that are structural, not secrets — never redacted.
_KNOWN_SEGMENTS = {
    "wallet", "walletsolidity", "jsonrpc", "tron", "eth", "rpc", "http", "https",
    "v1", "v2", "mainnet", "nile", "testnet", "shasta", "api", "core", "ext", "lb",
}


def _mask(s):
    return "…" if len(s) <= 6 else f"{s[:2]}…{s[-2:]}"


def _looks_secret(seg):
    return len(seg) >= 12 and seg.lower() not in _KNOWN_SEGMENTS and bool(
        re.fullmatch(r"[A-Za-z0-9_-]+", seg)
    )


def redact(url):
    """Mask token-like path segments and all query-string values so URLs can be
    pasted without leaking API keys. Host is left intact for readability."""
    try:
        p = urlsplit(url)
    except ValueError:
        return url
    path = "/".join(_mask(s) if _looks_secret(s) else s for s in p.path.split("/"))
    if p.query:
        query = "&".join(
            f"{k}={_mask(v)}" if v else k for k, v in parse_qsl(p.query, keep_blank_values=True)
        )
    else:
        query = ""
    return urlunsplit((p.scheme, p.netloc, path, query, p.fragment))


class Results:
    def __init__(self):
        self.failed = 0
        self.passed = 0
        self.warned = 0
        self.skipped = 0

    def ok(self, name, detail=""):
        self.passed += 1
        print(f"  {_c('32', 'PASS')}  {name}" + (f"  — {detail}" if detail else ""))

    def fail(self, name, detail=""):
        self.failed += 1
        print(f"  {_c('31', 'FAIL')}  {name}" + (f"  — {detail}" if detail else ""))

    def warn(self, name, detail=""):
        self.warned += 1
        print(f"  {_c('33', 'WARN')}  {name}" + (f"  — {detail}" if detail else ""))

    def skip(self, name, detail=""):
        self.skipped += 1
        print(f"  {_c('90', 'SKIP')}  {name}" + (f"  — {detail}" if detail else ""))

    def note(self, text):
        # neutral line; not counted toward pass/fail
        print(f"  {_c('90', '····')}  {text}")


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
# Simple client-side throttle so we don't trip free-tier per-second limits and
# mistake a rate-limit for a missing capability.
_THROTTLE = {"min_interval": 0.0, "last": 0.0}


def _post(url, payload, headers, timeout, retries=3):
    """POST json, return (http_status, parsed_json_or_None, error_str_or_None).

    Honours a global min-interval throttle and retries on HTTP 429 with backoff.
    """
    data = json.dumps(payload).encode()
    hdrs = {"Content-Type": "application/json", "User-Agent": "tron-rpc-check/1"}
    hdrs.update(headers)
    req = urllib.request.Request(url, data=data, headers=hdrs, method="POST")
    attempt = 0
    while True:
        if _THROTTLE["min_interval"]:
            wait = _THROTTLE["last"] + _THROTTLE["min_interval"] - time.monotonic()
            if wait > 0:
                time.sleep(wait)
        _THROTTLE["last"] = time.monotonic()
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                raw = r.read()
                try:
                    return r.status, (json.loads(raw) if raw else None), None
                except json.JSONDecodeError:
                    return r.status, None, f"non-JSON response: {raw[:200]!r}"
        except urllib.error.HTTPError as e:
            if e.code == 429 and attempt < retries:
                attempt += 1
                time.sleep(2 ** attempt)  # 2s, 4s, 8s
                continue
            body = e.read()[:300]
            return e.code, None, f"HTTP {e.code}: {body!r}"
        except urllib.error.URLError as e:
            return 0, None, f"connection error: {e.reason}"
        except Exception as e:  # noqa: BLE001 — surface anything else cleanly
            return 0, None, f"{type(e).__name__}: {e}"


class Wallet:
    def __init__(self, base, headers, timeout):
        self.base = base.rstrip("/")
        self.headers = headers
        self.timeout = timeout

    def call(self, method, body):
        return _post(f"{self.base}/{method}", body, self.headers, self.timeout)


class JsonRpc:
    def __init__(self, urls, headers, timeout):
        # urls is an ordered list of candidate endpoints to try (e.g. the
        # …/jsonrpc form first, then the bare base — some providers, e.g. dRPC,
        # serve JSON-RPC at the root). The first that doesn't 404 is used for
        # all subsequent calls.
        self.urls = urls
        self.url = urls[0]
        self.headers = headers
        self.timeout = timeout
        self._resolved = False

    def _rpc(self, url, method, params):
        status, parsed, err = _post(
            url,
            {"jsonrpc": "2.0", "id": 1, "method": method, "params": params},
            self.headers,
            self.timeout,
        )
        if err:
            return status, None, err
        if not isinstance(parsed, dict):
            return status, None, f"unexpected response: {parsed!r}"
        if "error" in parsed and parsed["error"] is not None:
            return status, None, f"rpc error: {json.dumps(parsed['error'])}"
        return status, parsed.get("result"), None

    def call(self, method, params):
        if self._resolved:
            return self._rpc(self.url, method, params)
        # First call: pick the first candidate URL that isn't a plain 404.
        last = None
        for u in self.urls:
            status, result, err = self._rpc(u, method, params)
            last = (status, result, err)
            if err and status == 404:
                continue  # wrong path — try the next candidate
            self.url, self._resolved = u, True
            return status, result, err
        # Every candidate 404'd: lock to the first and report its result.
        self.url, self._resolved = self.urls[0], True
        return last


def _tron_err(parsed):
    """Return an error string if a wallet response is actually a TRON error."""
    if isinstance(parsed, dict):
        for k in ("Error", "error"):
            if k in parsed:
                return str(parsed[k])
        # {"result": {"code": "...", "message": "..."}} style failures
        if parsed.get("result") is False or (
            isinstance(parsed.get("result"), dict) and parsed["result"].get("code")
        ):
            return json.dumps(parsed.get("result"))
    return None


# ---------------------------------------------------------------------------
# Wallet-surface checks
# ---------------------------------------------------------------------------
def check_wallet(w, net, usdt_tron, R):
    print(_c("1", f"\n=== Native wallet API ({redact(w.base)}) ==="))
    cap = {"reachable": False, "historical": None}

    # --- getnowblock: current tip (also our reachability probe) ---
    status, parsed, err = w.call("getnowblock", {})
    if err or _tron_err(parsed):
        R.note(f"wallet API not available here — {err or _tron_err(parsed)}")
        return cap
    try:
        tip = int(parsed["block_header"]["raw_data"]["number"])
    except (KeyError, TypeError, ValueError):
        R.fail("getnowblock", f"no block number in response: {json.dumps(parsed)[:200]}")
        return cap
    cap["reachable"] = True
    R.ok("getnowblock", f"tip = {tip}")

    # --- find a recent settled block that actually has transactions ---
    target_n, target_hash, target_txid = None, None, None
    start = tip - LOOKBACK
    for n in range(start, max(start - SCAN_DEPTH, 1), -1):
        status, blk, err = w.call("getblockbynum", {"num": n})
        if err or _tron_err(blk):
            continue
        txs = (blk or {}).get("transactions") or []
        if txs and blk.get("blockID"):
            target_n, target_hash = n, blk["blockID"]
            target_txid = txs[0].get("txID")
            break
    if target_n is None:
        R.fail("getblockbynum", f"no block with transactions found in {start}..{start-SCAN_DEPTH}")
        return cap
    R.ok("getblockbynum", f"block {target_n} ({len((blk.get('transactions') or []))} txs), id {target_hash[:16]}…")

    # --- getblockbalance: THE historical/archive gate ---
    status, bal, err = w.call(
        "getblockbalance", {"number": target_n, "hash": target_hash, "visible": False}
    )
    terr = _tron_err(bal)
    if err or terr:
        cap["historical"] = False
        R.fail(
            "getblockbalance (HISTORICAL)",
            f"{err or terr} — node likely lacks the historical balance feature "
            "(set java-tron storage.balance.history.lookup = true, or pick a provider that supports it)",
        )
    else:
        trace = (bal or {}).get("transaction_balance_trace")
        bid = (bal or {}).get("block_identifier") or {}
        if not trace:
            cap["historical"] = False
            R.fail(
                "getblockbalance (HISTORICAL)",
                f"block {target_n} has transactions but the balance trace is EMPTY — "
                "historical balance is NOT enabled on this node",
            )
        elif bid.get("hash") and bid["hash"].lower() != target_hash.lower():
            cap["historical"] = False
            R.fail("getblockbalance (HISTORICAL)", f"block hash mismatch: asked {target_hash}, got {bid['hash']}")
        else:
            cap["historical"] = True
            R.ok("getblockbalance (HISTORICAL)", f"{len(trace)} balance-changing tx(s) in block {target_n}")

    # --- gettransactionbyid / gettransactioninfobyid on a real txid ---
    if target_txid:
        status, tx, err = w.call("gettransactionbyid", {"value": target_txid, "visible": False})
        if err or _tron_err(tx) or not tx:
            R.fail("gettransactionbyid", err or _tron_err(tx) or "empty response")
        else:
            R.ok("gettransactionbyid", f"txID {target_txid[:16]}…")

        status, info, err = w.call("gettransactioninfobyid", {"value": target_txid})
        if err or _tron_err(info) or not info or not info.get("id"):
            R.fail("gettransactioninfobyid", err or _tron_err(info) or "empty response")
        else:
            R.ok("gettransactioninfobyid", f"block {info.get('blockNumber')}")
    else:
        R.skip("gettransaction{,info}byid", "no txID available from sample block")

    # --- triggerconstantcontract: contract read (USDT decimals()) ---
    if usdt_tron:
        body = {
            "owner_address": usdt_tron,
            "contract_address": usdt_tron,
            "function_selector": "decimals()",
            "parameter": "",
            "visible": False,
        }
        status, res, err = w.call("triggerconstantcontract", body)
        cr = (res or {}).get("constant_result") if isinstance(res, dict) else None
        if err or _tron_err(res) or not cr:
            R.fail("triggerconstantcontract", err or _tron_err(res) or "no constant_result")
        else:
            try:
                decimals = int(cr[0][-2:], 16)
                R.ok("triggerconstantcontract", f"USDT decimals() = {decimals}")
            except (ValueError, IndexError):
                R.warn("triggerconstantcontract", f"unexpected constant_result: {cr}")

        # --- estimateenergy: also verifies vm.estimateEnergy = true ---
        status, res, err = w.call("estimateenergy", body)
        terr = _tron_err(res)
        if not err and isinstance(res, dict) and "energy_required" in res:
            R.ok("estimateenergy", f"energy_required = {res['energy_required']} (vm.estimateEnergy enabled)")
        else:
            msg = (err or terr or json.dumps(res))[:200]
            if "estimateenergy" in msg.lower() or "estimate energy" in msg.lower() or "not enable" in msg.lower():
                R.fail("estimateenergy", f"appears DISABLED — set java-tron vm.estimateEnergy = true ({msg})")
            else:
                R.warn("estimateenergy", f"could not confirm: {msg}")

        # --- triggersmartcontract: builds (does NOT broadcast) an unsigned tx ---
        status, res, err = w.call("triggersmartcontract", {**body, "fee_limit": 1000000})
        tx = (res or {}).get("transaction") if isinstance(res, dict) else None
        if not err and tx:
            R.ok("triggersmartcontract", "builds unsigned tx (not broadcast)")
        else:
            R.warn(
                "triggersmartcontract",
                f"could not confirm: {(err or _tron_err(res) or json.dumps(res))[:160]}",
            )
    else:
        R.skip("triggerconstantcontract / estimateenergy", "no USDT address for this network (pass --usdt)")

    # --- broadcasttransaction is the only call we cannot exercise read-only ---
    R.skip("broadcasttransaction", "submits a signed tx — would spend TRX / mutate chain")
    return cap


# ---------------------------------------------------------------------------
# JSON-RPC-surface checks
# ---------------------------------------------------------------------------
def _usdt_evm(usdt_tron):
    """41-prefixed TRON hex -> 0x EVM address."""
    if not usdt_tron:
        return None
    h = usdt_tron.lower()
    if h.startswith("41") and len(h) == 42:
        return "0x" + h[2:]
    if h.startswith("0x") and len(h) == 42:
        return h
    return None


def check_jsonrpc(j, net, usdt_evm, R):
    cap = {"reachable": False, "logs": None}

    # --- eth_blockNumber: reachability probe (also resolves which URL works) ---
    status, res, err = j.call("eth_blockNumber", [])
    print(_c("1", f"\n=== Ethereum-style JSON-RPC ({redact(j.url)}) ==="))
    if j.url != j.urls[0]:
        R.note("fell back to bare base URL (no /jsonrpc suffix)")
    if err or not res:
        R.note(f"JSON-RPC not available here — {err or 'empty result'}")
        return cap
    tip = int(res, 16)
    if tip <= 0:
        R.fail("eth_blockNumber", f"non-positive height {tip}")
        return cap
    cap["reachable"] = True
    R.ok("eth_blockNumber", f"tip = {tip}")

    # --- eth_chainId ---
    status, res, err = j.call("eth_chainId", [])
    if err or not res:
        R.fail("eth_chainId", err or "empty result")
    elif net.get("chain_id") and int(res, 16) != int(net["chain_id"], 16):
        R.warn("eth_chainId", f"got {res}, expected {net['chain_id']} — wrong network?")
    else:
        R.ok("eth_chainId", f"{res}")

    # --- eth_getBlockByNumber (settled block, with tx hashes) ---
    target_n = tip - LOOKBACK
    status, blk, err = j.call("eth_getBlockByNumber", [hex(target_n), False])
    if err or not blk or not blk.get("hash"):
        R.fail("eth_getBlockByNumber", err or f"no block at {target_n}")
        return cap
    block_hash = blk["hash"]
    sample_tx = (blk.get("transactions") or [None])[0]
    R.ok("eth_getBlockByNumber", f"block {target_n}, {len(blk.get('transactions') or [])} txs")

    # --- eth_getBlockByHash ---
    status, blk2, err = j.call("eth_getBlockByHash", [block_hash, False])
    if err or not blk2 or not blk2.get("hash"):
        R.fail("eth_getBlockByHash", err or "no block")
    else:
        R.ok("eth_getBlockByHash", f"hash {block_hash[:18]}…")

    # --- eth_getLogs: USDT Transfer events (USDT deposit witnessing gate) ---
    if usdt_evm:
        found = None
        scanned = 0
        for n in range(target_n, max(target_n - SCAN_DEPTH, 1), -1):
            s, b, e = j.call("eth_getBlockByNumber", [hex(n), False])
            if e or not b or not b.get("hash"):
                continue
            scanned += 1
            s, logs, e = j.call(
                "eth_getLogs",
                [{"blockHash": b["hash"], "address": usdt_evm, "topics": [TRANSFER_TOPIC]}],
            )
            if e:
                R.fail("eth_getLogs (USDT)", f"error at block {n}: {e}")
                found = "error"
                break
            if logs:
                found = (n, len(logs))
                break
        if found == "error":
            cap["logs"] = False
        elif found:
            cap["logs"] = True
            R.ok("eth_getLogs (USDT)", f"{found[1]} Transfer event(s) in block {found[0]}")
        else:
            R.warn(
                "eth_getLogs (USDT)",
                f"no USDT transfers in last {scanned} blocks (no errors) — verify USDT address {usdt_evm}",
            )
    else:
        R.skip("eth_getLogs (USDT)", "no USDT address for this network (pass --usdt)")

    # --- eth_getTransactionByHash / eth_getTransactionReceipt ---
    if sample_tx:
        status, tx, err = j.call("eth_getTransactionByHash", [sample_tx])
        if err or not tx:
            R.fail("eth_getTransactionByHash", err or "null result")
        else:
            R.ok("eth_getTransactionByHash", f"{sample_tx[:18]}…")

        status, rcpt, err = j.call("eth_getTransactionReceipt", [sample_tx])
        if err or not rcpt or not rcpt.get("blockHash"):
            R.fail("eth_getTransactionReceipt", err or "null/empty receipt")
        else:
            R.ok("eth_getTransactionReceipt", f"status {rcpt.get('status')}")
    else:
        R.skip("eth_getTransaction{ByHash,Receipt}", "sample block had no transactions")
    return cap


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Validate a TRON RPC provider against Chainflip engine 2.2.3 requirements.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("base", metavar="BASE_URL",
                    help="provider base URL; …/wallet and …/jsonrpc are derived from it")
    ap.add_argument("--network", choices=sorted(NETWORKS), default="mainnet")
    ap.add_argument("--usdt", metavar="HEX", help="override USDT contract (TRON 41… or 0x… hex)")
    ap.add_argument("--header", action="append", default=[], metavar="K:V",
                    help="extra request header (repeatable), e.g. 'TRON-PRO-API-KEY: abc'")
    ap.add_argument("--timeout", type=float, default=15.0, help="per-request timeout seconds")
    ap.add_argument("--max-rps", type=float, default=4.0,
                    help="client-side request rate cap to avoid free-tier 429s (0 disables)")
    args = ap.parse_args()

    if args.max_rps > 0:
        _THROTTLE["min_interval"] = 1.0 / args.max_rps

    headers = {}
    for h in args.header:
        if ":" not in h:
            ap.error(f"bad --header {h!r}, expected 'Key: Value'")
        k, v = h.split(":", 1)
        headers[k.strip()] = v.strip()

    # Derive both surface URLs from the single base, tolerating a trailing
    # /wallet, /walletsolidity or /jsonrpc that the user may have pasted.
    root = args.base.rstrip("/")
    for suffix in ("/walletsolidity", "/wallet", "/jsonrpc"):
        if root.lower().endswith(suffix):
            root = root[: -len(suffix)]
            break
    root = root.rstrip("/")
    wallet_url = root + "/wallet"
    # Try …/jsonrpc first, then the bare base (some providers, e.g. dRPC, serve
    # JSON-RPC at the root and 404 on /jsonrpc).
    jsonrpc_urls = [root + "/jsonrpc", root]

    net = NETWORKS[args.network]
    usdt_tron = args.usdt or net["usdt_tron"]
    if usdt_tron and usdt_tron.startswith("0x") and len(usdt_tron) == 42:
        usdt_tron = "41" + usdt_tron[2:]  # accept EVM form for the wallet side too
    usdt_evm = _usdt_evm(usdt_tron)

    print(f"base={redact(root)}  network={args.network}  usdt={usdt_tron or '(none)'}")

    R = Results()
    wcap = check_wallet(Wallet(wallet_url, headers, args.timeout), net, usdt_tron, R)
    jcap = check_jsonrpc(JsonRpc(jsonrpc_urls, headers, args.timeout), net, usdt_evm, R)

    def yn(v):
        return "yes" if v is True else "NO" if v is False else "?"

    print(_c("1", "\n=== Capability summary ==="))
    if wcap["reachable"]:
        hist = wcap["historical"]
        line = f"  wallet API   : SUPPORTED   (historical getblockbalance: {yn(hist)})"
        print(_c("32", line) if hist else _c("33", line))
    else:
        print(f"  wallet API   : not available")
    if jcap["reachable"]:
        logs = jcap["logs"]
        line = f"  JSON-RPC     : SUPPORTED   (USDT eth_getLogs: {yn(logs)})"
        print(_c("32", line) if logs is not False else _c("33", line))
    else:
        print(f"  JSON-RPC     : not available")
    print(f"  ({R.passed} passed, {R.failed} failed, {R.warned} warned, {R.skipped} skipped)")

    if not wcap["reachable"] and not jcap["reachable"]:
        print(_c("31", "\nNeither surface reachable at this base URL."))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
