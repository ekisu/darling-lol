"""Compile the patched CPUID-fault environment scan against host files; no CPUID faulting is enabled."""
import unittest

import test_runtime_compatibility as sources


class CpuidFaultEnvironmentChecks(unittest.TestCase):
    patched = sources.CompatibilityChecks.patched
    run_c = sources.CompatibilityChecks.run_c

    def test_environment_scan_crosses_read_chunks_and_matches_whole_names(self):
        source = self.patched('xnu', 'darling/src/libsystem_kernel/emulation/src/'
                                     'linux_premigration/signal/cpuid_fault.c')
        scan = sources.function(source, 'env_is_set')
        self.run_c(r'''
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
static const char* environ_file;
static unsigned reads;
static long fake___NR_open(long path, int flags, int mode) {
    (void) mode; assert(!strcmp((const char*) path, "/proc/self/environ"));
    return open(environ_file, flags);
}
static long fake___NR_read(int fd, long buffer, long size) { ++reads; return read(fd, (void*) buffer, size); }
static long fake___NR_close(int fd) { return close(fd); }
#define LINUX_SYSCALL(nr, ...) fake_##nr(__VA_ARGS__)
''' + scan + r'''
static int scan_with(const char* entries[], unsigned count, unsigned padding) {
    static char path[] = "/tmp/cpuid-environ-XXXXXX";
    strcpy(path, "/tmp/cpuid-environ-XXXXXX");
    int fd = mkstemp(path); assert(fd >= 0);
    for (unsigned i = 0; i < padding; ++i) assert(write(fd, "PAD=xxxxxxxxxxx", 16) == 16);
    for (unsigned i = 0; i < count; ++i) assert(write(fd, entries[i], strlen(entries[i]) + 1) > 0);
    close(fd);
    environ_file = path; reads = 0;
    int result = env_is_set("DARLING_CPUID_FAULT");
    unlink(path);
    return result;
}
int main(void) {
    prctl(PR_SET_DUMPABLE, 0); /* a failed assertion must not leave a core dump */
    const char* on[] = {"DARLING_CPUID_FAULT=1"};
    const char* off[] = {"DARLING_CPUID_FAULT=0"};
    const char* empty[] = {"DARLING_CPUID_FAULT="};
    const char* longer[] = {"DARLING_CPUID_FAULTS=1"};
    const char* prefixed[] = {"XDARLING_CPUID_FAULT=1"};
    const char* bare[] = {"DARLING_CPUID_FAULT"};
    assert(scan_with(on, 1, 0));
    assert(!scan_with(off, 1, 0) && !scan_with(empty, 1, 0) && !scan_with(longer, 1, 0));
    assert(!scan_with(prefixed, 1, 0) && !scan_with(bare, 1, 0) && !scan_with(on, 0, 4));
    /* Far past the former single 8 KiB read, and across every chunk offset of the name. */
    assert(scan_with(on, 1, 1024) && reads > 8);
    for (unsigned shift = 0; shift < 64; ++shift) {
        char pad[80]; memset(pad, 'y', shift); pad[shift] = 0;
        char first[96]; snprintf(first, sizeof first, "P=%s", pad);
        const char* shifted[] = {first, "DARLING_CPUID_FAULT=1"};
        assert(scan_with(shifted, 2, 62));
    }
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()
