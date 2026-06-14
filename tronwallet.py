#!/usr/bin/env python3
"""Crude TRON CLI wallet — TRX only.

Exists solely to hold and move the TRX a Chainflip broadcaster wallet needs to
cover (and be reimbursed for) energy/bandwidth fees.  No tokens, no frills.

The private key is taken from the TRON_PRIVATE_KEY environment variable (64 hex
chars, optional 0x prefix) if set; otherwise, if the optional 'keyring' package
is installed and a secret store is available (gnome-keyring / KDE Wallet / macOS
Keychain), it is read from there.  Store it once with `store-key`.

Talks to the coalcache TRON route at http://10.25.0.2/tron by default; override
with TRON_NODE (give the /tron base, not /tron/wallet — tronpy appends the
wallet/ segment itself).

    pip install tronpy keyring   # keyring is optional

Examples:
    ./tronwallet.py store-key                    # prompt + save key to the keyring
    ./tronwallet.py balance                      # key from keyring (or env var)
    ./tronwallet.py send T<dest> 12.5
    ./tronwallet.py send T<dest> --all
    TRON_PRIVATE_KEY=abc123... ./tronwallet.py balance   # env var overrides keyring

Sends ask for confirmation unless -y/--yes is given.  Real funds: there is no undo.
"""

import argparse
import os
import sys
from decimal import Decimal, InvalidOperation

try:
    from tronpy import Tron
    from tronpy.keys import PrivateKey, is_base58check_address
    from tronpy.providers import HTTPProvider
    from tronpy.exceptions import AddressNotFound
except ImportError:
    sys.exit("error: tronpy not installed — run:  pip install tronpy")

try:
    import keyring  # optional: gnome-keyring / KDE Wallet / macOS Keychain via Secret Service
except ImportError:
    keyring = None

SUN = 1_000_000  # 1 TRX = 1e6 sun

# Where the key lives in the secret store.  One wallet -> one fixed slot.
KEYRING_SERVICE = "tronwallet"
KEYRING_ACCOUNT = "default"


def die(msg):
    sys.exit(f"error: {msg}")


# Our own coalcache proxy's TRON route.  tronpy issues full-node HTTP API calls
# as "wallet/<method>", so the base must be the /tron route (NOT /tron/wallet) —
# tronpy appends the "wallet/" segment itself.
DEFAULT_NODE = "http://10.25.0.2/tron"


def get_client():
    node = os.environ.get("TRON_NODE", DEFAULT_NODE)
    # tronpy joins "wallet/..." via urljoin, which drops the last path segment
    # unless the base ends in '/'.  Normalise so the "/tron" prefix is preserved
    # (-> .../tron/wallet/<method>).
    if not node.endswith("/"):
        node += "/"
    return Tron(HTTPProvider(node))


def parse_key(raw):
    """hex string (optional 0x) -> PrivateKey, or None if invalid/empty."""
    if not raw:
        return None
    raw = raw.strip()
    if raw.startswith(("0x", "0X")):
        raw = raw[2:]
    try:
        return PrivateKey(bytes.fromhex(raw))
    except Exception:
        return None


def keyring_get():
    """Fetch the stored key, or None if unavailable.  Never raises: a missing
    package, no Secret Service backend (headless server), or a locked keyring all
    just mean 'fall back to the env var'."""
    if keyring is None:
        return None
    try:
        return keyring.get_password(KEYRING_SERVICE, KEYRING_ACCOUNT)
    except Exception:
        return None


def load_key():
    env = os.environ.get("TRON_PRIVATE_KEY")
    if env:
        priv = parse_key(env)
        if priv is None:
            die("invalid TRON_PRIVATE_KEY (expected 64 hex chars)")
        return priv

    stored = keyring_get()
    if stored:
        priv = parse_key(stored)
        if priv is None:
            die("invalid key in the keyring (expected 64 hex chars); re-run 'store-key'")
        return priv

    hint = "store one with:  ./tronwallet.py store-key" if keyring is not None \
        else "install the 'keyring' package for secret-store support:  pip install keyring"
    die(f"no key found.  Set TRON_PRIVATE_KEY, or {hint}")


def trx_balance_sun(client, addr):
    """Balance in sun.  AddressNotFound (unactivated account) -> 0; any other
    failure (node unreachable, bad route, ...) dies loudly rather than masquerading
    as a zero balance."""
    try:
        return int(client.get_account_balance(addr) * SUN)
    except AddressNotFound:
        return 0
    except Exception as e:
        die(f"could not read balance from the TRON node: {e}")


def cmd_balance(client, priv, args):
    addr = priv.public_key.to_base58check_address()
    print(f"Address: {addr}")
    try:
        sun = int(client.get_account_balance(addr) * SUN)
        print(f"TRX:     {Decimal(sun) / SUN}")
    except AddressNotFound:
        print("TRX:     0  (account not activated — it has never received any TRX)")
    except Exception as e:
        die(f"could not read balance from the TRON node: {e}")


def cmd_send(client, priv, args):
    owner = priv.public_key.to_base58check_address()
    if not is_base58check_address(args.to):
        die(f"destination is not a valid TRON address: {args.to}")
    if args.to == owner:
        die("destination is the wallet's own address")

    balance = trx_balance_sun(client, owner)
    if args.all:
        reserve = int(Decimal(str(args.reserve)) * SUN)
        amount = balance - reserve
        if amount <= 0:
            die(f"balance {Decimal(balance)/SUN} TRX <= reserve {args.reserve} TRX; nothing to send")
    else:
        try:
            v = Decimal(args.amount)
        except InvalidOperation:
            die(f"invalid amount: {args.amount!r}")
        if v <= 0:
            die("amount must be positive")
        amount = int(v * SUN)
        if amount > balance:
            die(f"insufficient TRX: have {Decimal(balance)/SUN}, want {v}")

    print(f"Send {Decimal(amount)/SUN} TRX")
    print(f"  from {owner}")
    print(f"  to   {args.to}")
    if not args.yes and input("Type 'yes' to send: ").strip().lower() != "yes":
        sys.exit("Aborted.")

    txn = client.trx.transfer(owner, args.to, amount).build().sign(priv)
    ret = txn.broadcast()
    print(f"  txid: {txn.txid}")
    try:
        receipt = ret.wait()
        print(f"  confirmed (result: {receipt.get('result', '?')})")
    except Exception as e:
        print(f"  (broadcast sent; could not confirm: {e})")


def cmd_store_key(args):
    import getpass
    if keyring is None:
        die("the 'keyring' package is not installed — run:  pip install keyring")
    entered = getpass.getpass("TRON private key (hex, input hidden): ")
    priv = parse_key(entered)
    if priv is None:
        die("that is not a valid private key (expected 64 hex chars); nothing stored")
    # Store the normalized hex (no 0x, stripped) so load_key gets a clean value.
    normalized = entered.strip()
    if normalized.startswith(("0x", "0X")):
        normalized = normalized[2:]
    backend = getattr(keyring.get_keyring(), "name", type(keyring.get_keyring()).__name__)
    try:
        keyring.set_password(KEYRING_SERVICE, KEYRING_ACCOUNT, normalized)
    except Exception as e:
        die(f"could not write to the keyring [{backend}]: {e}")

    # Some environments select a no-op backend ("null") that silently discards
    # writes, or one that can't read back.  Verify the key actually round-trips
    # before claiming success — otherwise we'd report it stored when it wasn't.
    if keyring_get() != normalized:
        die("keyring backend [{}] did NOT persist the key — it is a no-op/unusable backend.\n"
            "  Your secret store isn't wired up.  On GNOME/Debian, install the Secret Service\n"
            "  backend and make sure gnome-keyring is running and unlocked:\n"
            "      pip install --upgrade secretstorage jeepney\n"
            "  Diagnose which backend is active and why:\n"
            "      python3 -c 'import keyring; print(keyring.get_keyring())'\n"
            "      echo \"$PYTHON_KEYRING_BACKEND\"\n"
            "      cat ~/.config/python_keyring/keyringrc.cfg 2>/dev/null\n"
            "  Or skip the keyring and use the TRON_PRIVATE_KEY environment variable."
            .format(backend))
    print(f"Stored key for {priv.public_key.to_base58check_address()} in keyring [{backend}].")


def cmd_clear_key(args):
    if keyring is None:
        die("the 'keyring' package is not installed")
    try:
        keyring.delete_password(KEYRING_SERVICE, KEYRING_ACCOUNT)
    except Exception:
        die("no key was stored in the keyring (nothing to remove)")
    print("Removed the stored key from the keyring.")


def main():
    p = argparse.ArgumentParser(description="Crude TRON CLI wallet, TRX only (key via TRON_PRIVATE_KEY).")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("balance", help="show address and TRX balance")

    s = sub.add_parser("send", help="send TRX")
    s.add_argument("to", help="destination TRON address")
    s.add_argument("amount", nargs="?", help="amount of TRX to send (omit with --all)")
    s.add_argument("--all", action="store_true", help="send the entire balance (minus --reserve)")
    s.add_argument("--reserve", type=float, default=1.1,
                   help="TRX left behind on an --all sweep, to cover the fee (default 1.1)")
    s.add_argument("-y", "--yes", action="store_true", help="skip the confirmation prompt")

    sub.add_parser("store-key", help="prompt for the private key and save it to the keyring")
    sub.add_parser("clear-key", help="remove the stored private key from the keyring")

    args = p.parse_args()

    # Key-management commands don't touch the node or load an existing key.
    if args.cmd == "store-key":
        return cmd_store_key(args)
    if args.cmd == "clear-key":
        return cmd_clear_key(args)

    if args.cmd == "send":
        if not args.all and args.amount is None:
            die("specify an amount, or use --all")
        if args.all and args.amount is not None:
            die("give an amount or --all, not both")

    client = get_client()
    priv = load_key()

    {"balance": cmd_balance, "send": cmd_send}[args.cmd](client, priv, args)


if __name__ == "__main__":
    main()
