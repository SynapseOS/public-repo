// Synapse Recorder, терминальная панель, 21.11.2026
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t leaving;
static void leave(int sig) { (void)sig; leaving = 1; }
static int line(char *dst, size_t len, const char *prompt) {
    printf("\033[2K\r%s ",prompt); fflush(stdout);
    if (!fgets(dst,len,stdin)) return -1;
    char *end=strchr(dst,'\n');
    if (!end) { int c; while ((c=getchar())!='\n' && c!=EOF) {} return -1; }
    *end=0;
    return 0;
}

int tui(void) {
    struct termios saved, raw;
    if (!isatty(0) || tcgetattr(0,&saved)) {
        fputs("Use recorder record FILE.mkv for a non-interactive session\n",stderr); return 2;
    }
    raw=saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    struct sigaction sa={.sa_handler=leave};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL); sigaction(SIGHUP,&sa,NULL);
    char path[400], region[80]="", camera[80]="/dev/video0", message[512]="Ready";
    int fps=15, source=0, clock=1, audio=0, pipefd=-1, child_status=0, automatic=1;
    pid_t pid=0;
    const char *modes[]={"Screen","Camera","Screen + camera"};
    const char *sounds[]={"Off","System","Microphone","System + microphone"};
    const char *audio_arg[]={"","system","mic","both"};
    snprintf(path,sizeof(path),"capture-%ld.mkv",(long)time(NULL));
    fputs("\033[?1049h\033[?25l",stdout);
    if (tcsetattr(0,TCSANOW,&raw)) { fputs("\033[?25h\033[?1049l",stdout); return 1; }
    int dirty=1, ready=0;
    while (!leaving) {
        if (pid && waitpid(pid,&child_status,WNOHANG)==pid) {
            pid=0;
            char last[256];
            ssize_t n=read(pipefd,last,sizeof(last)-1);
            if (n>0) { last[n]=0; snprintf(message,sizeof(message),"%s",last); }
            close(pipefd); pipefd=-1;
            if (WIFEXITED(child_status) && WEXITSTATUS(child_status)==0) {
                snprintf(message,sizeof(message),"Saved %s",path);
                if (automatic) snprintf(path,sizeof(path),"capture-%ld.mkv",(long)time(NULL));
            } else if (!strstr(message,"recorder:")) snprintf(message,sizeof(message),"Capture failed (status %d)",child_status);
            dirty=1;
        }
        if (dirty) {
            printf("\033[H\033[2J\033[1;36mSynapse Recorder\033[0m\n\n"
                "  Source    %s\n  Region    %.70s\n  Camera    %.70s\n  Rate      %d fps\n"
                "  Audio     %s\n  Timer     %s\n  Output    %.100s\n\n  %.150s\n\n%s\n",
                modes[source],*region ? region : "Full screen",camera,fps,sounds[audio],clock?"On":"Off",path,message,
                pid ? "  [P] Pause / resume   [C] Snapshot   [S] Save and stop" :
                "  [R] Record   [M] Mode   [G] Region   [F] FPS   [A] Audio\n"
                "  [O] Output   [D] Camera device   [T] Timer   [Q] Quit");
            fflush(stdout); dirty=0;
        }
        struct pollfd fds[]={{0,POLLIN,0},{pipefd,POLLIN,0}};
        if (poll(fds,pid ? 2 : 1,200)<0) { if (errno==EINTR) continue; break; }
        if (pid && (fds[1].revents & POLLIN)) {
            char buf[230]; ssize_t n=read(pipefd,buf,sizeof(buf)-1);
            if (n>0) { buf[n]=0; if (strstr(buf,"RECORDING") || strstr(buf,"TIME ")) ready=1; snprintf(message,sizeof(message),"%s",buf); dirty=1; }
        }
        if (!(fds[0].revents&POLLIN)) { if (fds[0].revents&(POLLERR|POLLHUP)) break; continue; }
        char key;
        if (read(0,&key,1)!=1) break;
        if (key>='A' && key<='Z') key+=32;
        if (pid) {
            if (ready && key=='p') kill(pid,SIGUSR1);
            if (key=='s') kill(pid,SIGTERM);
            if (ready && key=='c') kill(pid,SIGUSR2);
            continue;
        }
        dirty=1;
        if (key=='q') break;
        if (key=='m') source=(source+1)%3;
        if (key=='f') fps=fps==15?30:fps==30?60:fps==60?5:15;
        if (key=='t') clock=!clock;
        if (key=='a') audio=(audio+1)%4;
        if (key=='o' || key=='g' || key=='d') {
            tcsetattr(0,TCSANOW,&saved); fputs("\033[?25h",stdout);
            char value[400];
            if (!line(value,key=='o'?sizeof(value):sizeof(region),key=='o'?"Output file:":key=='g'?"X,Y,W,H (empty = full):":"Device:")) {
                if (key=='o' && *value) { strcpy(path,value); automatic=0; }
                if (key=='g') strcpy(region,value);
                if (key=='d' && *value) strcpy(camera,value);
            }
            tcsetattr(0,TCSANOW,&raw); fputs("\033[?25l",stdout);
        }
        if (key!='r') continue;
        int p[2];
        if (pipe(p)) { strcpy(message,"Pipe failed"); continue; }
        pid=fork();
        if (!pid) {
            close(p[0]); dup2(p[1],1); dup2(p[1],2); close(p[1]);
            char rate[8]; snprintf(rate,sizeof(rate),"%d",fps);
            char *args[24]={"recorder","record",path,"--fps",rate,"--controls"};
            int n=6;
            if (source) { args[n++]=source==1?"--camera":"--pip"; args[n++]=camera; }
            if (*region) { args[n++]="--region"; args[n++]=region; }
            if (clock) args[n++]="--clock";
            if (audio) { args[n++]="--audio"; args[n++]=(char *)audio_arg[audio]; }
            args[n]=NULL;
            execv("/bin/recorder",args); _exit(127);
        }
        close(p[1]);
        if (pid<0) { close(p[0]); pid=0; strcpy(message,"Cannot start recording"); }
        else { ready=0; pipefd=p[0]; fcntl(pipefd,F_SETFL,O_NONBLOCK); strcpy(message,"Starting"); }
    }
    if (pid) { kill(pid,SIGTERM); while (waitpid(pid,NULL,0)<0 && errno==EINTR) {} }
    if (pipefd>=0) close(pipefd);
    tcsetattr(0,TCSANOW,&saved);
    fputs("\033[?25h\033[?1049l",stdout); fflush(stdout);
    return 0;
}
