# sco-sdk

A sysroot, examples and tests for SCO UnixWare and OpenServer on 32-bit
x86, built with clang from
[clang-retro/llvm](https://github.com/clang-retro/llvm) (its `i386-pc-sco`
target).

| System | Releases | State |
|---|---|---|
| SCO UnixWare | 2.x, 7.x (and Caldera OpenUNIX 8) | static programs work, tested in scorun |
| SCO OpenServer | 5.0.x, 6.0 | static programs work, tested in scorun |

The C library is [picolibc](https://github.com/clang-retro/picolibc) with
its SCO layer (`libos/sco`), which makes the system calls directly, so
static programs need nothing from SCO. Nothing from SCO is in this
repository or in the sysroot. None of it has run on a real UnixWare or
OpenServer system yet.

## In short

    BIN=.../bin LLVM_SRC=llvm PICOLIBC_SRC=picolibc ./build-sysroot.sh <bin>/../i386-pc-sco
    clang --target=i386-pc-sco hello.c -o hello
    cc -o scorun tools/scorun.c && ./scorun ./hello

`BIN` is the directory with clang and the LLVM tools, `LLVM_SRC` a
checkout of clang-retro/llvm (for compiler-rt) and `PICOLIBC_SRC` one of
clang-retro/picolibc. `<bin>/../i386-pc-sco` is where clang looks for the
sysroot by default; anywhere else, pass it with `--sysroot`.

## What a program is

clang's SCO driver makes a static SVR4 ELF executable for the 80386,
linked with ld.lld at 0x8048000 (where SCO's linkers put programs), with
no interpreter and none of the GNU program headers the old kernels don't
know. Both systems load it:

- The code is for the i386 with x87 floating point, clang's default for
  `i386-pc-sco`. `-march=i486`, `-march=pentium` and so on work too, but
  leave SSE alone: UnixWare before 7.1.3 and OpenServer before 5.0.7 don't
  save SSE registers.
- System calls go through the `lcall $7,$0` gate, as with SCO's own libc.
  The layer uses only the calls UnixWare (SVR4.2) and OpenServer (SVR3.2
  with iBCS2) number the same: `read`, `write`, `open`, `close`, `lseek`,
  the old `stat` and `fstat`, `brk`, `time`, `link`, `unlink`, `mkdir`,
  `rmdir`, `chdir`, `access`, `ioctl`, `getpid`, `kill` and `exit`.
- So some things are simpler than on the systems themselves: `rename` is
  `link` and then `unlink` (not atomic, and not for directories), `lstat`
  is `stat`, the clock has whole seconds, and inode numbers, user and
  group IDs and device numbers are 16 bits (the old `struct stat`).
- Error numbers are read both ways: OpenServer numbers `ENOTEMPTY` and
  `ELOOP` differently from UnixWare (145 and 150 against 93 and 90).

Linking against SCO's own shared libraries (`libc.so.1`) isn't done yet.

## Running programs on Linux

`tools/scorun.c` builds with any Linux `cc` into a tool that runs a static
SCO program on an x86 Linux host that runs 32-bit programs. Linux loads
the program itself; scorun follows it with ptrace, and at each `lcall`
(which faults on Linux) makes the Linux call that does the same work in
the program and gives the result back the SCO way.

    scorun [-v] [-o] PROGRAM [ARGS...]

`-o` gives OpenServer's error numbers in place of UnixWare's, and `-v`
prints each call. Only the calls the sysroot's C library makes are
translated.

## Tests

    CC=$BIN/clang SYSROOT=sysroot tests/run-tests.sh tests-out

builds `examples/abitest.c` (calls, struct return, 64-bit, 128-bit and
floating point arithmetic, varargs, setjmp, static constructors, malloc,
files, directories, errors, the command line and time) at `-O0`, `-O2`
and `-Os` and for three CPUs, and `examples/hello.c`, and runs them in
scorun with both sets of error numbers. It also checks the program
headers. The CI does the same with clang from clang-retro/llvm's
`llvm-prebuilt` release.

## License

Copyright (C) clang-retro contributors.

The clang-retro SCO SDK is free software under the GNU Affero General
Public License, version 3 only (`AGPL-3.0-only`); the full text is in
[LICENSE](LICENSE). If you distribute a program built with it, or let
people use a modified version over a network, you must offer the
program's complete source under the same license.

Each contributor keeps the copyright in their own work and contributes
it under the same license; see [CONTRIBUTING.md](CONTRIBUTING.md).

picolibc and compiler-rt, which the build script builds, have their own
licenses (BSD-style, and Apache 2.0 with LLVM exceptions). UnixWare and
OpenServer are Xinuos's: nothing of either is in this repository.
