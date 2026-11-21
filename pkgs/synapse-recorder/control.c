// Synapse Recorder, локальное управление активной записью, 21.11.2026
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
static int lockfd=-1;
static struct sockaddr_un addr;
static int location(void) {
    char dir[64];
    snprintf(dir,sizeof(dir),"/tmp/synapse-recorder-%lu",(unsigned long)getuid());
    if (mkdir(dir,0700) && errno!=EEXIST) return -1;
    struct stat st;
    if (lstat(dir,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&077)) {
        errno=EACCES; return -1;
    }
    addr.sun_family=AF_UNIX;
    snprintf(addr.sun_path,sizeof(addr.sun_path),"%s/control",dir);
    return 0;
}
int control_open(void) {
    if (location()<0) return -1;
    char lock[128]; snprintf(lock,sizeof(lock),"%s.lock",addr.sun_path);
    lockfd=open(lock,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
    if (lockfd<0 || flock(lockfd,LOCK_EX|LOCK_NB)) goto fail;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if (fd<0) goto fail;
    unlink(addr.sun_path);
    if (bind(fd,(struct sockaddr *)&addr,sizeof(addr)) || listen(fd,4)) { close(fd); goto fail; }
    return fd;
fail:
    if (lockfd>=0) close(lockfd);
    lockfd=-1; return -1;
}
int control_key(int server) {
    if (server<0) return 0;
    int fd=accept(server,NULL,NULL);
    if (fd<0) return 0;
    // Команда отправляется сразу после connect, ожидание ограничено
    struct pollfd p={fd,POLLIN,0};
    char key=0;
    ssize_t r=poll(&p,1,100)>0 ? read(fd,&key,1) : 0;
    close(fd);
    return r==1 ? key : 0;
}
void control_close(int server) {
    if (server>=0) { unlink(addr.sun_path); close(server); close(lockfd); lockfd=-1; }
}
int control_send(char key) {
    if (location()<0) return 1;
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if (fd<0) return 1;
    int ok=connect(fd,(struct sockaddr *)&addr,sizeof(addr))==0 && write(fd,&key,1)==1;
    if (!ok) perror("recorder control");
    close(fd); return !ok;
}

// Панель отличает живую запись от файла, оставшегося после сбоя, по блокировке
static char state_path[128];
static int state_fd=-1;
void recording_state(char state) {
    if (!state) {
        if (state_fd>=0) { unlink(state_path); close(state_fd); state_fd=-1; }
        return;
    }
    if (state_fd<0) {
        if (location()<0) return;
        snprintf(state_path,sizeof(state_path),"/tmp/synapse-recorder-%lu/state-%ld",
                 (unsigned long)getuid(),(long)getpid());
        int fd=open(state_path,O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC,0600);
        if (fd<0) return;
        if (flock(fd,LOCK_EX|LOCK_NB)) { close(fd); return; }
        state_fd=fd;
    }
    pwrite(state_fd,&state,1,0);
}
