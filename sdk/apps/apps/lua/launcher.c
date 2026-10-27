/* Keep the public command static; musl provides the full Lua C module ABI. */
#include <unistd.h>
#include <stdio.h>
extern char **environ;
int main(int argc, char **argv) {
    (void)argc;
    execve("/usr/libexec/lua", argv, environ);
    perror("lua: /usr/libexec/lua (install lua and musl-runtime)");
    return 127;
}
