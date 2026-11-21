// Synapse Recorder, ввод данных, 21.11.2026
#pragma once
#include "studio.h"
#include <sys/types.h>
struct screen {
    int fd, wayland, first;
    pid_t child;
    struct image im;
};
struct audio_input {
    int fd, received;
    pid_t pid;
    size_t used;
    uint8_t data[32768];
};
int screen_open(struct screen *, int, const char *, const char *, int);
int screen_next(struct screen *);
void screen_close(struct screen *);
int audio_open(struct audio_input *, const char *);
int audio_poll(struct audio_input *);
void audio_mix(struct audio_input *, struct audio_input *, int16_t *);
void audio_close(struct audio_input *);
int64_t monotime(void);
int control_open(void);
int control_key(int);
void control_close(int);
int control_send(char);
void recording_state(char);
int preview_recorded_frame(const struct image *);
