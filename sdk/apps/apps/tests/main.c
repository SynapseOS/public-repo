// apps/tests/main.c
// Test application for SynapseOS, test basic syscalls and threading
/*
    int main(void *arg) - entry point of the program, calls test().
*/

#include <synlibc.h>

void test(void *arg) {
    (void)arg;
    syn_sleep(1000);
    syn_printf("[sus] test thread exiting\n");
    syn_proc_exit(0);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct utsname uname_test;
    uname(&uname_test);

    syn_printf("[sus] sysname: %s, node: %s, %s\n", uname_test.sysname, uname_test.nodename, uname_test.domainname);
    syn_printf("[sus] release: %s, version: %s\n", uname_test.release, uname_test.version);
    syn_printf("[sus] machine: %s\n", uname_test.machine);

    syn_thread_create((uintptr_t)test, NULL);

    return 0;
}