#!/bin/sh
# Creates a SELF-SIGNED certificate for https://localhost:8443 (development only; browsers will warn).
#   scripts/dev.sh sh scripts/gen-dev-cert.sh
# For anything real, mount a certificate from a CA instead (deploy/apache/httpd.conf: SSLCertificateFile).
set -eu
cd "$(dirname "$0")/.."
mkdir -p deploy/apache/certs
openssl req -x509 -newkey rsa:2048 -nodes -days 365 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
  -keyout deploy/apache/certs/dev.key -out deploy/apache/certs/dev.crt 2>/dev/null
chmod 644 deploy/apache/certs/dev.crt deploy/apache/certs/dev.key
echo "wrote deploy/apache/certs/dev.crt and dev.key (valid 365 days, localhost only)"
