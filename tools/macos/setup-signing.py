#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# SPDX-FileCopyrightText: 2026 Tommaso Bendinelli
"""Create once and reuse a local Thoughts identity in the user's login keychain."""
import os
from pathlib import Path
import subprocess
import tempfile

os.umask(0o077)
state = Path.home() / "Library/Application Support/Thoughts/signing"
state.mkdir(parents=True, exist_ok=True)
cert = state / "certificate.pem"
keychain = Path.home() / "Library/Keychains/login.keychain-db"


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


if not cert.exists():
    with tempfile.TemporaryDirectory(prefix="thoughts-signing-") as tmp:
        tmp = Path(tmp)
        config = tmp / "openssl.cnf"
        config.write_text("""[req]
distinguished_name = dn
x509_extensions = signing
prompt = no
[dn]
CN = Thoughts Local Code Signing
[signing]
basicConstraints = critical,CA:TRUE
keyUsage = critical,digitalSignature,keyCertSign
extendedKeyUsage = critical,codeSigning
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
""")
        run("/usr/bin/openssl", "req", "-new", "-x509", "-newkey", "rsa:3072",
            "-nodes", "-sha256", "-days", "3650", "-config", config,
            "-keyout", tmp / "key.pem", "-out", tmp / "cert.pem")
        identity = tmp / "identity.pem"
        identity.write_bytes((tmp / "key.pem").read_bytes() + (tmp / "cert.pem").read_bytes())
        # Only codesign receives automatic private-key access; do not use -A.
        run("/usr/bin/security", "import", identity, "-k", keychain,
            "-f", "pemseq", "-x", "-T", "/usr/bin/codesign")
        cert.write_bytes((tmp / "cert.pem").read_bytes())

# This may require the user's macOS authorization. Reruns reuse the same key.
# Trust is limited to code signing in the user domain, never TLS or system-wide.
run("/usr/bin/security", "add-trusted-cert", "-r", "trustRoot", "-p", "codeSign",
    "-k", keychain, cert)
fingerprint = subprocess.check_output([
    "/usr/bin/openssl", "x509", "-in", str(cert), "-noout", "-fingerprint", "-sha1"
], text=True).strip().split("=", 1)[1].replace(":", "")
identities = subprocess.check_output([
    "/usr/bin/security", "find-identity", "-v", "-p", "codesigning", str(keychain)
], text=True)
if fingerprint not in identities:
    raise SystemExit("The existing certificate is not a usable identity. Repair its keychain access; do not regenerate it.")
(state / "identity.txt").write_text(fingerprint + "\n")
print(f"Persistent Thoughts signing identity ready: {fingerprint}")
