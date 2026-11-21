// Synapse Recorder, управление сессией записи, 21.11.2026
#define _GNU_SOURCE
#include "io.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int tui(void);
int camera_preview(const char *);
int voice_record(const char *,const char *);
static volatile sig_atomic_t stop, paused, shot;
static void control(int sig) {
    if (sig == SIGUSR1) paused = !paused;
    else if (sig == SIGUSR2) shot = 1;
    else stop = 1;
}
static int number(const char *s, int lo, int hi) {
    char *end;
    errno = 0;
    long n = strtol(s,&end,10);
    return errno || !*s || *end || n < lo || n > hi ? -1 : (int)n;
}
static int region(const char *s, int *rect) {
    for (int i = 0; i < 4; i++) {
        char *end;
        errno = 0;
        long n = strtol(s,&end,10);
        if (errno || s == end || n < (i < 2 ? 0 : 1) || n > 8192 || *end != (i == 3 ? 0 : ',')) return -1;
        rect[i] = (int)n;
        if (i < 3) s = end+1;
    }
    return 0;
}
static int usage(int rc) {
    fputs("recorder [tui | gui]\n"
          "recorder record FILE.{mkv,mp4,avi} | shot FILE.{png,jpg,bmp}\n"
          "  --screen fb|wayland   --camera DEVICE   --pip DEVICE   --mode INDEX\n"
          "  --region X,Y,W,H      --fps 1..60       --seconds N    --delay N\n"
          "  --codec h264|mpeg4      --mbps 1..100         --clock           --text ASCII   --logo FILE.{png,jpg,bmp}\n"
          "  --audio system|mic|both|PCM_FILE        --pip-width PERCENT\n"
          "--monitor NAME | --window ID   --hide-cursor   recorder sources\n"
          "recorder voice FILE.wav | preview DEVICE\n"
          "--controls: recorder pause | snapshot | stop\n"
          "SIGUSR1 pause/resume, SIGUSR2 snapshot, SIGINT or SIGTERM finish\n",rc ? stderr : stdout);
    return rc;
}
static int run(int argc, char **argv) {
    int single = !strcmp(argv[1],"shot");
    const char *path = argv[2], *cam = NULL, *pip = NULL, *text = NULL, *logo = NULL, *audio = NULL, *banner = NULL;
    const char *monitor=NULL, *window=NULL;
    int cursor=1, live_preview=0;
    int h264=1, remote=0, server=-1;
    int fps=15, seconds=0, delay=0, clock=0, mbps=6, mode=-1, pip_width=25, rect[4]={0};
    int wayland = getenv("WAYLAND_DISPLAY") != NULL;
    for (int i=3; i<argc; i++) {
        const char *key = argv[i];
        if (!strcmp(key,"--live-preview")) { live_preview=1; continue; }
        if (!strcmp(key,"--controls")) { remote=1; continue; }
        if (!strcmp(key,"--hide-cursor")) { cursor=0; continue; }
        if (!strcmp(key,"--clock")) { clock=1; continue; }
        if (++i == argc) return usage(2);
        const char *v = argv[i];
        if (!strcmp(key,"--fps")) fps=number(v,1,60);
        else if (!strcmp(key,"--codec")) {
            if (!strcmp(v,"h264")) h264=1;
            else if (!strcmp(v,"mpeg4")) h264=0;
            else return usage(2);
        }
        else if (!strcmp(key,"--seconds")) seconds=number(v,1,86400);
        else if (!strcmp(key,"--delay")) delay=number(v,0,86400);
        else if (!strcmp(key,"--mbps")) mbps=number(v,1,100);
        else if (!strcmp(key,"--pip-width")) pip_width=number(v,5,100);
        else if (!strcmp(key,"--mode")) { mode=number(v,0,65535); if (mode<0) return usage(2); }
        else if (!strcmp(key,"--region")) { if (region(v,rect)) return usage(2); }
        else if (!strcmp(key,"--screen")) {
            if (!strcmp(v,"fb")) wayland=0;
            else if (!strcmp(v,"wayland")) wayland=1;
            else return usage(2);
        } else if (!strcmp(key,"--monitor")) monitor=v;
        else if (!strcmp(key,"--window")) window=v;
        else if (!strcmp(key,"--camera")) cam=v;
        else if (!strcmp(key,"--pip")) pip=v;
        else if (!strcmp(key,"--banner")) banner=v;
        else if (!strcmp(key,"--logo")) logo=v;
        else if (!strcmp(key,"--text")) text=v;
        else if (!strcmp(key,"--audio")) audio=v;
        else return usage(2);
        if (fps<0 || delay<0 || seconds<0 || mbps<0 || pip_width<0) return usage(2);
    }
    if ((cam && pip) || (single && audio) || (monitor && window) || ((monitor || window) && (!wayland || cam))) return usage(2);
    if (live_preview && (single || !cam)) return usage(2);
    FILE *events=live_preview?stderr:stdout;
    struct sigaction sa = {.sa_handler=control};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);
    sigaction(SIGUSR1,&sa,NULL); sigaction(SIGUSR2,&sa,NULL);
    signal(SIGPIPE,SIG_IGN);
    struct screen screen={.fd=-1};
    struct camera camera={.fd=-1};
    struct audio_input a={.fd=-1}, b={.fd=-1};
    struct image inset={0}, mark={0}, heading={0}, view={0};
    struct movie movie={.fd=-1};
    const char *err = NULL;
    if (remote && (server=control_open())<0) { err="Cannot claim recording controls"; goto done; }
    for (int i=0;i<delay && !stop;i++) sleep(1);
    if (stop) goto done;
    if (!cam && screen_open(&screen,wayland,monitor,window,cursor)<0) { err="Cannot capture display"; goto done; }
    if (cam || pip) {
        if (camera_open(&camera,cam ? cam : pip,mode)<0 || allocate_image(&inset,camera.w,camera.h)<0) {
            err="Cannot start UVC camera in requested mode"; goto done;
        }
    }
    if (banner && load_logo(banner,&heading)<0) { err="Invalid title image"; goto done; }
    if (logo && load_logo(logo,&mark)<0) { err="Invalid or unreadable logo image"; goto done; }
    view = cam ? inset : screen.im;
    if (rect[2]) {
        if (rect[0]+rect[2] > view.w || rect[1]+rect[3] > view.h) { err="Region outside image"; goto done; }
        view.pixels += rect[1]*view.stride + rect[0]*4;
        view.w=rect[2]; view.h=rect[3];
    }
    if (audio && (audio_open(&a,!strcmp(audio,"both") ? "system" : audio)<0 ||
        (!strcmp(audio,"both") && audio_open(&b,"mic")<0))) { err="Cannot open audio input"; goto done; }
    if (audio) {
        int64_t until=monotime()+3000000;
        while (!stop && (!a.received || (b.fd>=0 && !b.received))) {
            if (audio_poll(&a)<0 || audio_poll(&b)<0 || monotime()>=until) {
                err="No audio from requested source"; goto done;
            }
            usleep(10000);
        }
    }
    int64_t start=monotime(), last=-1, pause_at=-1, announced=-1, shot_number=0;
    while (!stop) {
        int key=control_key(server);
        if (key=='p') paused=!paused;
        if (key=='s') break;
        if (key=='c') shot=1;
        int64_t now=monotime();
        if (paused) {
            if (pause_at<0) { pause_at=now; recording_state('P'); fputs("PAUSED\n",events); fflush(events); }
            if (audio_poll(&a)<0 || audio_poll(&b)<0) { err="Audio source disconnected"; break; }
            a.used=b.used=0;
            usleep(20000); continue;
        }
        if (pause_at>=0) { start+=now-pause_at; pause_at=-1; recording_state('R'); fputs("RESUMED\n",events); fflush(events); }
        int64_t elapsed=now-start, pts=elapsed*fps/1000000;
        if (seconds && elapsed>=seconds*1000000LL && last>=0) break;
        if (audio_poll(&a)<0 || audio_poll(&b)<0) { err="Audio source disconnected"; break; }
        if (pts<=last) { usleep(5000); continue; }
        if (!cam && screen_next(&screen)<0) { err="Display stopped or changed mode"; break; }
        if ((cam || pip) && camera_read(&camera,&inset)<0) { err="Camera frame unavailable"; break; }
        if (pip) composite(&view,&inset,1,1,FFMAX(1,view.w*pip_width/100),0);
        if (logo) composite(&view,&mark,0,1,FFMIN(mark.w,FFMAX(1,view.w/3)),1);
        if (banner) composite(&view,&heading,1,0,FFMIN(heading.w,view.w),1);
        if (clock) {
            char stamp[40];
            snprintf(stamp,sizeof(stamp)," REC %02lld:%02lld:%02lld ",
                (long long)(elapsed/3600000000LL),(long long)(elapsed/60000000%60),(long long)(elapsed/1000000%60));
            caption(&view,stamp,8,8);
        }
        if (text) caption(&view,text,8,clock ? 24 : 8);
        if (single) { if (snapshot(path,&view)<0) err="Screenshot write failed"; break; }
        if (shot) {
            shot=0;
            char name[PATH_MAX];
            if (snprintf(name,sizeof(name),"%s.%04lld.png",path,(long long)++shot_number)>=(int)sizeof(name) ||
                snapshot(name,&view)<0) fprintf(stderr,"Screenshot was not saved\n");
        }
        if (!movie.header) {
            if (movie_open(&movie,path,view.w,view.h,fps,mbps,audio!=NULL,h264)<0) { err="Cannot create recording"; break; }
            recording_state('R'); fputs("RECORDING\n",events); fflush(events);
        }
        if (live_preview && preview_recorded_frame(&view)<0) { err="Preview disconnected"; break; }
        if (movie_video(&movie,&view,pts)<0) { err="Video encode or write failed"; break; }
        while (audio && movie.samples < (pts+1)*48000/fps) {
            int16_t pcm[2048];
            audio_mix(&a,&b,pcm);
            if (movie_audio(&movie,pcm)<0) { err="Audio encode or write failed"; break; }
        }
        if (err) break;
        if (announced!=elapsed/1000000) {
            announced=elapsed/1000000;
            fprintf(events,"TIME %lld %d\n",(long long)announced,movie.frames); fflush(events);
        }
        last=pts;
    }
done:
    if (!err && !single && !movie.frames) err="Recording stopped before the first frame";
    if (movie_close(&movie)<0 && !err) err="Cannot finalize output";
    if (movie.owned && !movie.frames) unlink(path);
    recording_state(0);
    control_close(server);
    screen_close(&screen); camera_close(&camera);
    audio_close(&a); audio_close(&b);
    av_free(inset.pixels); av_free(mark.pixels); av_free(heading.pixels);
    if (err) { fprintf(stderr,"recorder: %s\n",err); return 1; }
    return 0;
}
int main(int argc, char **argv) {
    av_log_set_level(AV_LOG_ERROR);
    if (argc==1 || (argc==2 && !strcmp(argv[1],"gui"))) {
        if (argc==2 || (getenv("WAYLAND_DISPLAY") && access("/bin/recorder-desktop",X_OK)==0)) {
            execl("/bin/recorder-desktop","recorder-desktop",(char *)NULL); perror("recorder-desktop"); return 1;
        }
        return tui();
    }
    if (argc==3 && !strcmp(argv[1],"preview")) return camera_preview(argv[2]);
    if ((argc==3 || argc==4) && !strcmp(argv[1],"voice")) return voice_record(argv[2],argc==4 ? argv[3] : "mic");
    if (argc==2 && !strcmp(argv[1],"sources")) { execl("/usr/libexec/recorder-grab","recorder-grab","--list",(char *)NULL); return 1; }
    if (argc==2 && !strcmp(argv[1],"pause")) return control_send('p');
    if (argc==2 && !strcmp(argv[1],"stop")) return control_send('s');
    if (argc==2 && !strcmp(argv[1],"snapshot")) return control_send('c');
    if (argc==2 && !strcmp(argv[1],"tui")) return tui();
    if (argc>=3 && (!strcmp(argv[1],"record") || !strcmp(argv[1],"shot"))) return run(argc,argv);
    return usage(strcmp(argv[1],"--help") != 0);
}
