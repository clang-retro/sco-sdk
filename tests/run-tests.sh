#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 clang-retro contributors
#
# Build the examples for SCO UnixWare and OpenServer on 32-bit x86 and run
# them in scorun (tools/scorun.c, which runs static SCO programs on Linux),
# with UnixWare's error numbers and with OpenServer's (scorun -o).
#
#   CC=.../bin/clang SYSROOT=... SCORUN=... tests/run-tests.sh [OUT]
#
# CC is clang from clang-retro/llvm and SYSROOT the sysroot made by
# build-sysroot.sh. SCORUN is the scorun program (default: built from
# tools/scorun.c with the host's cc into OUT). OUT (default ./tests-out)
# gets the programs and their output.
#
# The tests build for clang's default CPU on SCO (the i386), and for the
# i486 and the Pentium Pro too.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-clang}
: "${SYSROOT:?set SYSROOT}"
out=${1:-$PWD/tests-out}
mkdir -p "$out"
out=$(cd "$out" && pwd)
if [ -z "${SCORUN:-}" ]; then
  SCORUN=$out/scorun
  ${HOSTCC:-cc} -O2 -Wall -o "$SCORUN" "$here/tools/scorun.c"
fi
fail=0

ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

# name expected-status expected-text program args... (SCORUN_FLAGS: -o)
check() {
  name=$1 status=$2 text=$3 prog=$4
  shift 4
  set +e
  (cd "$out" && ABITEST=yes "$SCORUN" ${SCORUN_FLAGS:-} "$prog" "$@") >"$out/$name.log" 2>&1
  got=$?
  set -e
  if [ $got -ne "$status" ]; then
    bad "$name (exit status $got, not $status)"; tail -5 "$out/$name.log"
  elif grep -q FAIL "$out/$name.log"; then
    bad "$name"; grep FAIL "$out/$name.log"
  elif [ -n "$text" ] && ! grep -qF "$text" "$out/$name.log"; then
    bad "$name (no \"$text\")"; tail -5 "$out/$name.log"
  else
    ok "$name"
  fi
}

flags="--target=i386-pc-sco --sysroot=$SYSROOT -Wall -Werror"
for o in -O0 -O2 -Os; do
  n=sco$o
  if $CC $flags $o "$here/examples/abitest.c" -o "$out/abitest$n"; then
    ok "build abitest$n"
    check "abitest$n" 0 "abitest: destructor ran" "./abitest$n" one "two words"
    SCORUN_FLAGS=-o check "abitest$n-openserver" 0 "abitest: destructor ran" \
      "./abitest$n" one "two words"
  else
    bad "build abitest$n"
  fi
done
for m in i486 pentiumpro; do
  if $CC $flags -march=$m -O2 "$here/examples/abitest.c" -o "$out/abitest-$m"; then
    ok "build abitest-$m"
    check "abitest-$m" 0 "abitest: destructor ran" "./abitest-$m" one "two words"
  else
    bad "build abitest-$m"
  fi
done
if $CC $flags -O2 "$here/examples/hello.c" -o "$out/hello"; then
  ok "build hello"
  check "hello" 0 "Hello, world from ./hello! 42 123456789 3.25" "./hello"
else
  bad "build hello"
fi
# The exit status gets back to the caller.
printf 'int main(void) { return 3; }\n' >"$out/exit3.c"
if $CC $flags -O2 "$out/exit3.c" -o "$out/exit3"; then
  check "exit3" 3 "" "./exit3"
else
  bad "build exit3"
fi
# What SCO's kernels load: a static System V ELF program for the 80386 at
# 0x8048000, with no interpreter and no GNU program headers.
readelf=${READELF:-$(dirname "$CC")/llvm-readelf}
h=$("$readelf" -h -l "$out/hello")
if echo "$h" | grep -q 'OS/ABI: *UNIX - System V' &&
   echo "$h" | grep -q 'Machine: *Intel 80386' &&
   echo "$h" | grep -q 'LOAD *0x000000 0x08048000' &&
   ! echo "$h" | grep -qE 'INTERP|GNU_STACK|GNU_RELRO|GNU_EH_FRAME|NOTE'; then
  ok "hello is a plain SVR4 ELF program at 0x8048000"
else
  bad "hello is not a plain SVR4 ELF program at 0x8048000"; echo "$h"
fi
exit $fail
