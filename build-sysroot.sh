#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (c) 2026 clang-retro contributors
#
# Build the SCO sysroot for static programs on 32-bit x86 (UnixWare 2 and
# later, OpenServer 5 and later): picolibc with its SCO layer and the
# compiler-rt builtins, for the i386.
#
#   BIN=... LLVM_SRC=... PICOLIBC_SRC=... ./build-sysroot.sh OUT
#
#   BIN           directory with clang, llvm-ar and the other LLVM tools
#                 (clang-retro/llvm, with the X86 back end and the
#                 i386-pc-sco target)
#   LLVM_SRC      clang-retro/llvm checkout (compiler-rt)
#   PICOLIBC_SRC  clang-retro/picolibc checkout
#   WORK          build directory (default: ./build-sco)
#   MARCH         CPU the libraries are built for (default: i386)
#
# OUT gets the layout clang's SCO driver links against (it is the default
# sysroot when it is ../i386-pc-sco from clang's bin directory; otherwise
# pass it with --sysroot):
#
#   usr/include/  picolibc's headers
#   usr/lib/      crt1.o libc.a libm.a libclang_rt.builtins.a
#
# Everything in it can be redistributed. Nothing from SCO is used.
set -e
out=${1:?usage: build-sysroot.sh OUT}
: "${BIN:?set BIN}" "${LLVM_SRC:?set LLVM_SRC}" "${PICOLIBC_SRC:?set PICOLIBC_SRC}"
WORK=${WORK:-$PWD/build-sco}
mkdir -p "$out/usr/include" "$out/usr/lib" "$WORK"
out=$(cd "$out" && pwd) WORK=$(cd "$WORK" && pwd)
PATH=$BIN:$PATH
export PATH
triple=i386-pc-sco flags=-march=${MARCH:-i386}

"$BIN/clang" --target=$triple -print-targets | grep -qw x86 ||
  { echo "$BIN/clang has no x86 back end" >&2; exit 1; }
"$BIN/clang" --target=$triple -dM -E -x c /dev/null | grep -q __SCO__ ||
  { echo "$BIN/clang has no i386-pc-sco target" >&2; exit 1; }

# picolibc, with its malloc on top of the SCO layer's sbrk.
pb=$WORK/picolibc pi=$WORK/picolibc-install
rm -rf "$pb" "$pi"
meson setup "$pb" "$PICOLIBC_SRC" \
  --cross-file "$PICOLIBC_SRC/scripts/cross-clang-$triple.txt" \
  -Dc_args="$flags" \
  -Dos-sco=true -Dmultilib=false -Dpicocrt=false \
  -Dsemihost=false -Dtests=false -Dposix-console=true -Dsingle-thread=true \
  -Dthread-local-storage=false -Dspecsdir=none \
  --prefix="$pi" >"$WORK/picolibc.log" 2>&1 ||
  { tail -20 "$WORK/picolibc.log"; exit 1; }
ninja -C "$pb" install >>"$WORK/picolibc.log" 2>&1 ||
  { grep -E 'error|FAILED' "$WORK/picolibc.log" | head -20; exit 1; }
cp -R "$pi/include/." "$out/usr/include/"
for f in crt1.o libc.a libm.a; do
  cp "$pi/lib/$f" "$out/usr/lib/"
done

# compiler-rt builtins: 64-bit division and the like. They are built with
# the sysroot, so the driver finds picolibc's headers, not the host's.
bb=$WORK/builtins
rm -rf "$bb"
cmake -G Ninja -S "$LLVM_SRC/compiler-rt/lib/builtins" -B "$bb" \
  -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$BIN/clang" -DCMAKE_ASM_COMPILER="$BIN/clang" \
  -DCMAKE_CXX_COMPILER="$BIN/clang++" \
  -DCMAKE_C_COMPILER_TARGET=$triple \
  -DCMAKE_CXX_COMPILER_TARGET=$triple \
  -DCMAKE_ASM_COMPILER_TARGET=$triple \
  -DCMAKE_C_FLAGS="$flags --sysroot=$out" -DCMAKE_ASM_FLAGS="$flags --sysroot=$out" \
  -DCMAKE_AR="$BIN/llvm-ar" -DCMAKE_RANLIB="$BIN/llvm-ranlib" \
  -DCMAKE_NM="$BIN/llvm-nm" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON -DCOMPILER_RT_BAREMETAL_BUILD=ON \
  -DCOMPILER_RT_OS_DIR=sco -DLLVM_CMAKE_DIR="$BIN/../lib/cmake/llvm" \
  >"$WORK/builtins.log" 2>&1 ||
  { tail -20 "$WORK/builtins.log"; exit 1; }
ninja -C "$bb" >>"$WORK/builtins.log" 2>&1 ||
  { grep -E 'error|FAILED' "$WORK/builtins.log" | head -20; exit 1; }
lib=$(ls "$bb"/lib/sco/libclang_rt.builtins-*.a)
# compiler-rt's x86 assembly for these conversions uses SSE2, which an
# i386, i486 or Pentium has not got. Put the C versions in their place.
for f in floatdidf floatdisf floatdixf floatundidf floatundisf floatundixf; do
  "$BIN/clang" --target=$triple $flags --sysroot="$out" -O2 -c \
    "$LLVM_SRC/compiler-rt/lib/builtins/$f.c" -o "$bb/$f.o"
  "$BIN/llvm-ar" d "$lib" $("$BIN/llvm-ar" t "$lib" | grep "^$f\.S\.")
  "$BIN/llvm-ar" r "$lib" "$bb/$f.o"
done
! "$BIN/llvm-objdump" -d "$lib" | grep -qw 'xmm[0-9]*' ||
  { echo "SSE instructions left in $lib" >&2; exit 1; }
cp "$lib" "$out/usr/lib/libclang_rt.builtins.a"
echo "sysroot written to $out"
