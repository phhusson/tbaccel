#!/bin/sh
# Create a local, self-signed code-signing identity for TbAccel.kext.
#
# Apple Silicon refuses ad-hoc-signed kexts ("Bad code signature" from kmutil),
# and there is no Apple Developer ID on this machine, so we make our own and
# trust it. Run once (no sudo needed for the keychain part).
#
#   scripts/make-signing-cert.sh
#
# Then, so the kernel/AMFI trusts it (needs sudo):
#   sudo security add-trusted-cert -d -r trustRoot -p codeSign \
#        -k /Library/Keychains/System.keychain ~/.tbaccel/cert.pem
#
set -e

KC="$HOME/Library/Keychains/tbaccel.keychain-db"
KCPW="tbaccel"
NAME="TbAccel Local"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

security delete-keychain "$KC" 2>/dev/null || true
security create-keychain -p "$KCPW" "$KC"
security set-keychain-settings "$KC"          # no auto-lock timeout
security unlock-keychain -p "$KCPW" "$KC"

# Self-signed CA leaf that is also valid for code signing.
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout "$WORK/k.pem" -out "$WORK/c.pem" \
    -subj "/CN=$NAME/O=tbaccel/OU=TBACCEL001" \
    -addext "keyUsage=critical,digitalSignature,keyCertSign" \
    -addext "extendedKeyUsage=critical,codeSigning" \
    -addext "basicConstraints=critical,CA:TRUE" 2>/dev/null

# -legacy: macOS Security.framework cannot read the OpenSSL 3 default PKCS#12.
openssl pkcs12 -export -legacy -inkey "$WORK/k.pem" -in "$WORK/c.pem" \
    -out "$WORK/i.p12" -passout pass:"$KCPW" -name "$NAME" 2>/dev/null

security import "$WORK/i.p12" -k "$KC" -P "$KCPW" \
    -T /usr/bin/codesign -T /usr/bin/security
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KCPW" "$KC" >/dev/null

EXISTING=$(security list-keychains -d user | sed 's/[" ]//g' | tr '\n' ' ')
security list-keychains -d user -s "$KC" $EXISTING

mkdir -p "$HOME/.tbaccel"
cp "$WORK/c.pem" "$HOME/.tbaccel/cert.pem"

echo "created identity \"$NAME\" in $KC"
echo "certificate: $HOME/.tbaccel/cert.pem"
echo
echo "now trust it (needs sudo):"
echo "  sudo security add-trusted-cert -d -r trustRoot -p codeSign \\"
echo "       -k /Library/Keychains/System.keychain $HOME/.tbaccel/cert.pem"
