#!/usr/bin/env bash
#
# orcha-plugin-sign.sh - Ed25519 keys and signatures for Orcha plugin uploads.
#
#   keygen <private-key.pem>                  create a signing key and print its public key
#   pubkey <private-key.pem>                  print the public key again
#   sign   <private-key.pem> <library> [manifest.json]
#                                             write <library>.sig next to the library
#
# Put the printed public key in orcha.yaml (plugins.upload.public_keys) on the
# servers that should trust plugins signed with this key. Keep the private key
# secret: anyone holding it (and an admin login) can run code on those servers.
#
# The signature covers the library's file name and SHA-256 of the library and
# of manifest.json (empty when there is none): renaming the library or
# swapping the manifest invalidates it, so it decides which plugin the upload
# becomes. Upload the library under the name it was signed with. It must match
# core/PluginPackage.cpp signing_message().
#
# Needs OpenSSL 3 or newer (LibreSSL cannot sign raw Ed25519). On macOS:
#   brew install openssl && OPENSSL=$(brew --prefix openssl)/bin/openssl ./orcha-plugin-sign.sh ...
#
set -euo pipefail

OPENSSL="${OPENSSL:-openssl}"

die() { echo "orcha-plugin-sign: $*" >&2; exit 1; }
usage() { sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

"$OPENSSL" version 2>/dev/null | grep -Eq '^OpenSSL ([3-9]|[1-9][0-9])\.' \
  || die "needs OpenSSL 3+ (found: $("$OPENSSL" version 2>/dev/null || echo none)); set OPENSSL=/path/to/openssl"

sha256_file() { "$OPENSSL" dgst -sha256 -r "$1" | cut -d' ' -f1; }
sha256_empty() { printf '' | "$OPENSSL" dgst -sha256 -r | cut -d' ' -f1; }
public_key() { "$OPENSSL" pkey -in "$1" -pubout -outform DER | tail -c 32 | "$OPENSSL" base64 -A; }

cmd="${1:-}"
case "$cmd" in
  keygen)
    [ $# -eq 2 ] || usage
    [ -e "$2" ] && die "$2 already exists"
    (umask 077 && "$OPENSSL" genpkey -algorithm ed25519 -out "$2")
    echo "Private key: $2 (keep it secret)"
    echo "Public key (add to plugins.upload.public_keys):"
    public_key "$2"; echo
    ;;
  pubkey)
    [ $# -eq 2 ] || usage
    public_key "$2"; echo
    ;;
  sign)
    [ $# -eq 3 ] || [ $# -eq 4 ] || usage
    key="$2"; library="$3"; manifest="${4:-}"
    [ -f "$library" ] || die "no such library: $library"
    lib_sha="$(sha256_file "$library")"
    if [ -n "$manifest" ]; then
      [ -f "$manifest" ] || die "no such manifest: $manifest"
      man_sha="$(sha256_file "$manifest")"
    else
      man_sha="$(sha256_empty)"
    fi
    message="$(mktemp)"; trap 'rm -f "$message"' EXIT
    printf 'orcha-plugin-signature-v2\n%s\n%s\n%s\n' "$(basename "$library")" "$lib_sha" "$man_sha" > "$message"
    "$OPENSSL" pkeyutl -sign -inkey "$key" -rawin -in "$message" | "$OPENSSL" base64 -A > "$library.sig"
    echo >> "$library.sig"
    echo "Wrote $library.sig (key $(public_key "$key" | "$OPENSSL" base64 -d -A | "$OPENSSL" dgst -sha256 -r | cut -c1-16))"
    ;;
  *) usage ;;
esac
