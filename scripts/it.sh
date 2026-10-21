#!/bin/sh
# Run selected integration tests against the release (or SAN) build.
#   scripts/dev.sh sh scripts/it.sh test_checkout.CheckoutSemantics
#   scripts/dev.sh env SF_DUMP_LOG=1 sh scripts/it.sh test_db_faults.PayFaultSweep   # show the server's log
# BUILD=san-address-undefined selects another build directory.
set -eu
cd "$(dirname "$0")/.."
cd tests/integration
SF_BIN="${SF_BIN:-/work/build/${BUILD:-rel}/stockforged}" exec python3 -W ignore -m unittest "$@"
