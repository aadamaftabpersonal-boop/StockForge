#!/bin/sh
# Runs stockforged under Valgrind memcheck so the integration tests can drive it.
#   scripts/dev.sh env SF_BIN=/work/scripts/valgrind-sf.sh SF_START_TIMEOUT=120 SF_STOP_TIMEOUT=180 \
#       sh scripts/it.sh test_auth.Auth
# Valgrind exits 99 if it finds an error or a DEFINITELY lost block, which fails the test run
# (the harness asserts the server's exit status). Expect ~5-20x slowdown.
# SF_REAL_BIN overrides the program (scripts/valgrind-selftest.sh uses it).
echo '{"level":"info","event":"valgrind","msg":"server is running under valgrind memcheck"}' >&2
exec valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite \
     --num-callers=25 "${SF_REAL_BIN:-/work/build/rel/stockforged}" "$@"
