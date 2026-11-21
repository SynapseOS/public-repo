// Декодирование одного файла и вывод PCM через OSS
#include <vlc/vlc.h>
#include <sys/soundcard.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

struct Playback {
    libvlc_instance_t *instance = nullptr;
    libvlc_media_player_t *player = nullptr;
    int device = -1;
    std::atomic<int> error{0};
    std::atomic<unsigned long long> frames{0};
    std::atomic<bool> audible{false};
    std::atomic<int> volume{75};

    Playback() {
        const char *options[] = {"--quiet", "--no-video", "--no-media-library", "--no-plugins-cache", "--no-audio-time-stretch"};
        instance = libvlc_new(sizeof(options) / sizeof(options[0]), options);
    }

    static void playAudio(void *context, const void *samples, unsigned count, int64_t) {
        auto &out = *static_cast<Playback *>(context);
        const auto *src = static_cast<const int16_t *>(samples);
        const int gain = out.volume.load();
        int16_t chunk[1024];
        while (count && !out.error.load()) {
            unsigned n = count > 512 ? 512 : count;
            bool nonzero = false;
            for (unsigned i = 0; i < n * 2; ++i) {
                chunk[i] = static_cast<int16_t>(src[i] * gain / 100);
                nonzero |= chunk[i] != 0;
            }
            if (nonzero) out.audible = true;
            if (out.device >= 0) {
                size_t pos = 0;
                while (pos < n * 4) {
                    ssize_t written = write(out.device, reinterpret_cast<char *>(chunk) + pos, n * 4 - pos);
                    if (written < 0 && errno == EINTR) continue;
                    if (written <= 0) {
                        out.error = written == 0 ? EIO : errno;
                        return;
                    }
                    pos += static_cast<size_t>(written);
                }
            }
            out.frames += n;
            src += n * 2;
            count -= n;
        }
    }

    static void drainAudio(void *context) {
        auto &out = *static_cast<Playback *>(context);
        if (out.device >= 0 && ioctl(out.device, SNDCTL_DSP_SYNC, 0) < 0) out.error = errno;
    }

    static void flushAudio(void *context, int64_t) {
        auto &out = *static_cast<Playback *>(context);
        if (out.device >= 0 && ioctl(out.device, SNDCTL_DSP_RESET, 0) < 0) out.error = errno;
    }

    bool start(const char *file, const char *output) {
        frames = 0;
        audible = false;
        error = 0;
        if (!instance) { error = ENODEV; return false; }
        if (output) {
            device = open(output, O_WRONLY | O_CLOEXEC);
            int fmt = AFMT_S16_LE, channels = 2, rate = 48000;
            if (device < 0 || ioctl(device, SNDCTL_DSP_SETFMT, &fmt) < 0 ||
                ioctl(device, SNDCTL_DSP_CHANNELS, &channels) < 0 || ioctl(device, SNDCTL_DSP_SPEED, &rate) < 0) {
                error = errno; return false;
            }
            if (fmt != AFMT_S16_LE || channels != 2 || rate != 48000) {
                error = ENOTSUP; return false;
            }
        }
        auto *media = libvlc_media_new_path(instance, file);
        if (media) {
            player = libvlc_media_player_new_from_media(media);
            libvlc_media_release(media);
        }
        if (player) {
            libvlc_audio_set_callbacks(player, playAudio, nullptr, nullptr, flushAudio, drainAudio, this);
            libvlc_audio_set_format(player, "S16N", 48000, 2);
            if (libvlc_media_player_play(player) == 0) return true;
        }
        error = EIO;
        return false;
    }
};

// Отдельный процесс владеет потоками декодера и звуковым дескриптором до конца трека
int main(int argc, char **argv) {
    bool check = argc == 3 && !strcmp(argv[1], "--check-audio");
    if (!check && (argc != 4 || strcmp(argv[1], "--play"))) return 2;
    if (!check && strcmp(argv[2], "/dev/dsp") && strcmp(argv[2], "/dev/audio-hdmi") &&
        strcmp(argv[2], "/dev/audio-jack")) return 2;
    Playback song;
    if (!song.start(argv[argc - 1], check ? nullptr : argv[2])) {
        fprintf(stderr, "%s\n", strerror(song.error));
        return 1;
    }
    int64_t previous = -1;
    for (;;) {
        const auto state = libvlc_media_player_get_state(song.player);
        if (state == libvlc_Error || song.error) {
            fprintf(stderr, "%s\n", song.error ? strerror(song.error) : "Ошибка декодирования VLC");
            return 1;
        }
        if (state == libvlc_Ended) {
            Playback::drainAudio(&song);
            if (check) printf("PCM 48000 Hz stereo: %llu frames, signal=%u\n", song.frames.load(),
                              unsigned(song.audible.load()));
            return song.frames && !song.error ? 0 : 1;
        }
        if (check) { usleep(20000); continue; }
        int64_t seconds = libvlc_media_player_get_time(song.player) / 1000;
        if (seconds != previous) {
            printf("%lld\n", static_cast<long long>(seconds));
            fflush(stdout);
            previous = seconds;
        }
        struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
        int ready = poll(&fd, 1, 100);
        if (ready < 0 && errno != EINTR) return 1;
        if (ready > 0) {
            unsigned char command;
            if (read(0, &command, 1) != 1 || command == 202) return 0;
            if (command <= 100) song.volume = command;
            else if (command == 200 || command == 201) libvlc_media_player_set_pause(song.player, command == 201);
        }
    }
}
