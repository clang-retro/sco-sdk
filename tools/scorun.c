/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (c) 2026 clang-retro contributors
 *
 * scorun: run a static 32-bit x86 SCO UnixWare or OpenServer program on
 * Linux, for testing.
 *
 *   scorun [-v] [-o] PROGRAM [ARGS...]
 *
 * Linux loads the program itself (a static SVR4 i386 ELF program has no
 * interpreter and nothing Linux refuses), and scorun follows it with
 * ptrace. SCO programs make system calls with lcall $7,$0, the number in
 * eax and the arguments on the stack above a return address. Linux has no
 * call gate there, so the lcall faults (SIGSEGV), and scorun puts the
 * Linux call that does the same work in the registers, an int $0x80 over
 * the lcall for one step, and then the lcall back. The kernel runs the
 * call in the program, so memory, files and brk are the program's own.
 * The result goes back the SVR4 way: in eax, with the carry flag set and
 * an SVR4 error number on failure, and the old struct stat converted.
 * Then the program goes on after the lcall.
 *
 * -o gives OpenServer's error numbers for the few that differ from
 * UnixWare's (ENOTEMPTY and ELOOP); -v prints each call. Only what the C
 * library in the sysroot uses is translated; any other call fails with
 * ENOSYS. Needs an x86-64 or i386 Linux host that runs 32-bit programs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__)
#define R_NR(r) ((r).orig_rax)
#define R_AX(r) ((r).rax)
#define R_BX(r) ((r).rbx)
#define R_CX(r) ((r).rcx)
#define R_DX(r) ((r).rdx)
#define R_SI(r) ((r).rsi)
#define R_DI(r) ((r).rdi)
#define R_BP(r) ((r).rbp)
#define R_SP(r) ((r).rsp)
#define R_FL(r) ((r).eflags)
#define R_IP(r) ((r).rip)
#elif defined(__i386__)
#define R_NR(r) ((r).orig_eax)
#define R_AX(r) ((r).eax)
#define R_BX(r) ((r).ebx)
#define R_CX(r) ((r).ecx)
#define R_DX(r) ((r).edx)
#define R_SI(r) ((r).esi)
#define R_DI(r) ((r).edi)
#define R_BP(r) ((r).ebp)
#define R_SP(r) ((r).esp)
#define R_FL(r) ((r).eflags)
#define R_IP(r) ((r).eip)
#else
#error "scorun needs an x86 Linux host"
#endif

/* Linux i386 system call numbers. */
enum {
    L_read = 3, L_write = 4, L_open = 5, L_close = 6, L_link = 9,
    L_unlink = 10, L_chdir = 12, L_time = 13, L_lseek = 19, L_getpid = 20,
    L_access = 33, L_kill = 37, L_mkdir = 39, L_rmdir = 40, L_brk = 45,
    L_ioctl = 54, L_stat64 = 195, L_fstat64 = 197, L_exit_group = 252,
};

/* The SVR4 numbers, which UnixWare and OpenServer share. */
enum {
    S_exit = 1, S_read = 3, S_write = 4, S_open = 5, S_close = 6,
    S_link = 9, S_unlink = 10, S_chdir = 12, S_time = 13, S_brk = 17,
    S_stat = 18, S_lseek = 19, S_getpid = 20, S_fstat = 28, S_access = 33,
    S_kill = 37, S_ioctl = 54, S_rmdir = 79, S_mkdir = 80,
};

enum { X_none, X_brk, X_stat, X_ioctl };

static int   verbose, openserver;
static pid_t child;

static int rmem(uint32_t addr, void *buf, size_t n)
{
    struct iovec l = { buf, n }, r = { (void *)(uintptr_t)addr, n };
    return process_vm_readv(child, &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
}

/* Writes go through ptrace, which also writes read-only pages (the lcall
 * in the program's text). */
static int wmem(uint32_t addr, const void *buf, size_t n)
{
    uint32_t start = addr & ~(uint32_t)(sizeof(long) - 1);
    for (uint32_t a = start; a < addr + n; a += sizeof(long)) {
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, child, (void *)(uintptr_t)a, 0);
        if (errno)
            return -1;
        for (unsigned i = 0; i < sizeof(long); i++)
            if (a + i >= addr && a + i < addr + n)
                ((unsigned char *)&w)[i] = ((const unsigned char *)buf)[a + i - addr];
        if (ptrace(PTRACE_POKEDATA, child, (void *)(uintptr_t)a, (void *)w) < 0)
            return -1;
    }
    return 0;
}

/* Linux error numbers to SVR4 ones (UnixWare's), or OpenServer's. 1 to
 * 34 are the same. */
static uint32_t sco_errno(int e)
{
    switch (e) {
    case 35: return 45;                     /* EDEADLK */
    case 36: return 78;                     /* ENAMETOOLONG */
    case 37: return 46;                     /* ENOLCK */
    case 38: return 89;                     /* ENOSYS */
    case 39: return openserver ? 145 : 93;  /* ENOTEMPTY */
    case 40: return openserver ? 150 : 90;  /* ELOOP */
    case 75: return 79;                     /* EOVERFLOW */
    default: return e <= 34 ? (uint32_t)e : 22; /* else EINVAL */
    }
}

/* SVR4 open flags to Linux ones. */
static uint32_t open_flags(uint32_t f)
{
    uint32_t l = f & 3;               /* O_RDONLY, O_WRONLY, O_RDWR */
    if (f & 0x0084) l |= 04000;       /* O_NDELAY, O_NONBLOCK */
    if (f & 0x0008) l |= 02000;       /* O_APPEND */
    if (f & 0x0010) l |= 04010000;    /* O_SYNC */
    if (f & 0x0100) l |= 0100;        /* O_CREAT */
    if (f & 0x0200) l |= 01000;       /* O_TRUNC */
    if (f & 0x0400) l |= 0200;        /* O_EXCL */
    if (f & 0x0800) l |= 0400;        /* O_NOCTTY */
    return l;
}

/* SVR4 signal numbers to Linux ones, for kill. */
static uint32_t linux_sig(uint32_t s)
{
    static const unsigned char map[30] = {
        0, 1, 2, 3, 4, 5, 6, 0 /* EMT */, 8, 9, 7 /* BUS */, 11, 31 /* SYS */,
        13, 14, 15, 10 /* USR1 */, 12 /* USR2 */, 17 /* CHLD */, 30 /* PWR */,
        28 /* WINCH */, 23 /* URG */, 29 /* POLL */, 19 /* STOP */,
        20 /* TSTP */, 18 /* CONT */, 21 /* TTIN */, 22 /* TTOU */,
        26 /* VTALRM */, 27 /* PROF */,
    };
    return s < 30 ? map[s] : 0;
}

/* Linux struct stat64 (i386) to the old SVR4 struct stat (32 bytes). */
static void stat_to_sco(const unsigned char *l, unsigned char *b)
{
#define G32(o) (*(const uint32_t *)(l + (o)))
#define G64(o) (*(const uint64_t *)(l + (o)))
#define P16(o, v) (*(uint16_t *)(b + (o)) = (uint16_t)(v))
#define P32(o, v) (*(uint32_t *)(b + (o)) = (uint32_t)(v))
    memset(b, 0, 32);
    P16(0, G64(0));   /* st_dev */
    P16(2, G64(88));  /* st_ino */
    P16(4, G32(16));  /* st_mode */
    P16(6, G32(20));  /* st_nlink */
    P16(8, G32(24));  /* st_uid */
    P16(10, G32(28)); /* st_gid */
    P16(12, G64(32)); /* st_rdev */
    P32(16, G64(44)); /* st_size */
    P32(20, G32(64)); /* st_atime */
    P32(24, G32(72)); /* st_mtime */
    P32(28, G32(80)); /* st_ctime */
}

static const unsigned char lcall7[7] = { 0x9a, 0, 0, 0, 0, 7, 0 };

int main(int argc, char **argv)
{
    int a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        if (strcmp(argv[a], "-v") == 0)
            verbose = 1;
        else if (strcmp(argv[a], "-o") == 0)
            openserver = 1;
        else
            break;
    }
    if (a >= argc) {
        fprintf(stderr, "usage: scorun [-v] [-o] PROGRAM [ARGS...]\n");
        return 2;
    }

    child = fork();
    if (child < 0) {
        perror("fork");
        return 2;
    }
    if (child == 0) {
        /* No address randomization: the heap then starts right after the
         * bss, where an SVR4 kernel puts it, and brk() can grow it. */
        personality(ADDR_NO_RANDOMIZE);
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        raise(SIGSTOP);
        execv(argv[a], argv + a);
        perror(argv[a]);
        _exit(127);
    }

    int st;
    waitpid(child, &st, 0);
    ptrace(PTRACE_SETOPTIONS, child, 0, (void *)(long)PTRACE_O_EXITKILL);

    int sig = 0;
    for (;;) {
        if (ptrace(PTRACE_CONT, child, 0, (void *)(long)sig) < 0)
            break;
        sig = 0;
        if (waitpid(child, &st, 0) < 0)
            break;
        if (WIFEXITED(st))
            return WEXITSTATUS(st);
        if (WIFSIGNALED(st)) {
            fprintf(stderr, "scorun: program killed by signal %d\n", WTERMSIG(st));
            return 128 + WTERMSIG(st);
        }
        if (!WIFSTOPPED(st))
            continue;
        if (WSTOPSIG(st) != SIGSEGV) {
            /* The SIGTRAP after execve is the debugger's, not the program's. */
            sig = WSTOPSIG(st) == SIGTRAP ? 0 : WSTOPSIG(st);
            continue;
        }

        struct user_regs_struct r, saved;
        ptrace(PTRACE_GETREGS, child, 0, &r);
        saved = r;
        uint32_t      ip = (uint32_t)R_IP(r), sp = (uint32_t)R_SP(r);
        unsigned char insn[7];
        if (rmem(ip, insn, 7) < 0 || memcmp(insn, lcall7, 7) != 0) {
            sig = SIGSEGV; /* a real fault */
            continue;
        }

        uint32_t snr = (uint32_t)R_AX(r), arg[4] = { 0 };
        rmem(sp + 4, arg, sizeof arg);
        long     nr = -1;
        int      extra = X_none;
        uint32_t la[3] = { arg[0], arg[1], arg[2] };
        uint32_t scratch = (sp - 512) & ~15u; /* below the program's stack */
        switch (snr) {
        case S_exit: nr = L_exit_group; break;
        case S_read: nr = L_read; break;
        case S_write: nr = L_write; break;
        case S_open: nr = L_open; la[1] = open_flags(arg[1]); break;
        case S_close: nr = L_close; break;
        case S_link: nr = L_link; break;
        case S_unlink: nr = L_unlink; break;
        case S_chdir: nr = L_chdir; break;
        case S_time: nr = L_time; la[0] = 0; break;
        case S_brk: nr = L_brk; extra = X_brk; break;
        case S_stat: nr = L_stat64; la[1] = scratch; extra = X_stat; break;
        case S_fstat: nr = L_fstat64; la[1] = scratch; extra = X_stat; break;
        case S_lseek: nr = L_lseek; break;
        case S_getpid: nr = L_getpid; break;
        case S_access: nr = L_access; break;
        case S_kill: nr = L_kill; la[1] = linux_sig(arg[1]); break;
        case S_ioctl:
            /* TCGETA, for isatty: Linux's TCGETS has the same number and a
             * bigger structure, so it goes into the scratch space. */
            if (arg[1] == 0x5401) {
                nr = L_ioctl; la[2] = scratch; extra = X_ioctl;
            }
            break;
        case S_rmdir: nr = L_rmdir; break;
        case S_mkdir: nr = L_mkdir; break;
        default: break;
        }

        long res;
        if (nr < 0) {
            res = -38; /* ENOSYS */
        } else {
            /* int $0x80 over the lcall, for one step. */
            static const unsigned char int80[2] = { 0xcd, 0x80 };
            R_AX(r) = nr;
            R_BX(r) = la[0];
            R_CX(r) = la[1];
            R_DX(r) = la[2];
            R_NR(r) = -1;
            ptrace(PTRACE_SETREGS, child, 0, &r);
            wmem(ip, int80, 2);
            ptrace(PTRACE_SINGLESTEP, child, 0, 0);
            if (waitpid(child, &st, 0) < 0)
                break;
            if (WIFEXITED(st))
                return WEXITSTATUS(st);
            if (WIFSIGNALED(st))
                return 128 + WTERMSIG(st);
            wmem(ip, lcall7, 7);
            ptrace(PTRACE_GETREGS, child, 0, &r);
            res = (int32_t)(uint32_t)R_AX(r);
        }

        if (res >= 0) {
            if (extra == X_brk) {
                /* Linux returns the new break; SVR4's brk, 0 or failure. */
                res = (uint32_t)res >= arg[0] ? 0 : -12; /* ENOMEM */
            } else if (extra == X_stat) {
                unsigned char l[96], b[32];
                rmem(scratch, l, 96);
                stat_to_sco(l, b);
                wmem(arg[1], b, 32);
                res = 0;
            } else if (extra == X_ioctl) {
                res = 0;
            }
        }
        if (verbose)
            fprintf(stderr, "scorun: call %u (%#x, %#x, %#x) = %ld\n", snr, arg[0],
                    arg[1], arg[2], res);

        r = saved;
        R_IP(r) = ip + 7;
        if (res < 0 && res > -4096) {
            R_AX(r) = sco_errno((int)-res);
            R_FL(r) |= 1;
        } else {
            R_AX(r) = (uint32_t)res;
            R_FL(r) &= ~1ul;
        }
        ptrace(PTRACE_SETREGS, child, 0, &r);
    }
    return 2;
}
