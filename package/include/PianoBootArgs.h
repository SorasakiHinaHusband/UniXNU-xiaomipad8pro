/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PIANO_BOOT_ARGS_H
#define PIANO_BOOT_ARGS_H

#include <errno.h>
#include <stddef.h>
#include <sys/sysctl.h>

/* k27 0066's merged line, including the terminating NUL. */
#define PIANO_BOOT_ARGS_MAX 4096

/* The userland stager recognizes readers built with this header. */
static const char pianoBootArgsOID[] __attribute__((used)) = "kern.piano_bootargs\0PianoBootArgs ABI 1";

/* Read the whole line (k27 0076), or the legacy clip when the OID is absent. */
static inline void
pianoBootArgs(char args[PIANO_BOOT_ARGS_MAX])
{
    size_t size = PIANO_BOOT_ARGS_MAX;
    int status = sysctlbyname(pianoBootArgsOID, args, &size, NULL, 0);
    if (status != 0 && errno == ENOENT) {
        size = PIANO_BOOT_ARGS_MAX;
        status = sysctlbyname("kern.bootargs", args, &size, NULL, 0);
    }
    if (status == 0 && size > 0 && size <= PIANO_BOOT_ARGS_MAX)
        args[size - 1] = '\0';
    else
        args[0] = '\0';
}

#endif /* PIANO_BOOT_ARGS_H */
