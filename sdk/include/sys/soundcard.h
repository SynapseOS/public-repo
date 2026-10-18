/*
    soundcard.h: Запросы воспроизведения OSS 3
    Автор: Елчинян Арен Артурович 2026
    Архитектура: x86_64, ABI: Linux (ELF64)

    Описание: Запросы воспроизведения OSS 3
    Подсистема: общий интерфейс ядра и приложений
    Первое изменение: 13.10.2026
    Последнее изменение: 18.10.2026
*/

#ifndef SYNAPSE_SYS_SOUNDCARD_H
#define SYNAPSE_SYS_SOUNDCARD_H

// Формат запросов воспроизведения OSS 3 совместим с Linux ioctl
#include <sys/ioctl.h>
#define SOUND_VERSION 0x030802
#define AFMT_QUERY 0
#define AFMT_MU_LAW 1
#define AFMT_A_LAW 2
#define AFMT_IMA_ADPCM 4
#define AFMT_U8 8
#define AFMT_S16_LE 16
#define AFMT_S16_BE 32
#define AFMT_S8 64
#define AFMT_U16_LE 128
#define AFMT_U16_BE 256
#define AFMT_S16_NE AFMT_S16_LE
#define SNDCTL_DSP_RESET _IO('P', 0)
#define SNDCTL_DSP_SYNC _IO('P', 1)
#define SNDCTL_DSP_SPEED _IOWR('P', 2, int)
#define SNDCTL_DSP_STEREO _IOWR('P', 3, int)
#define SNDCTL_DSP_GETBLKSIZE _IOWR('P', 4, int)
#define SNDCTL_DSP_SETFMT _IOWR('P', 5, int)
#define SNDCTL_DSP_SAMPLESIZE SNDCTL_DSP_SETFMT
#define SNDCTL_DSP_CHANNELS _IOWR('P', 6, int)
#define SNDCTL_DSP_POST _IO('P', 8)
#define SNDCTL_DSP_SETFRAGMENT _IOWR('P', 10, int)
#define SNDCTL_DSP_GETFMTS _IOR('P', 11, int)

typedef struct audio_buf_info {
	int fragments, fragstotal, fragsize, bytes;
} audio_buf_info;

#define SNDCTL_DSP_GETOSPACE _IOR('P', 12, audio_buf_info)
#define SNDCTL_DSP_GETISPACE _IOR('P', 13, audio_buf_info)
#define SNDCTL_DSP_NONBLOCK _IO('P', 14)
#define SNDCTL_DSP_GETCAPS _IOR('P', 15, int)
#define DSP_CAP_REVISION 0xff
#define DSP_CAP_DUPLEX 0x100
#define DSP_CAP_REALTIME 0x200
#define DSP_CAP_BATCH 0x400
#define DSP_CAP_TRIGGER 0x1000
#define DSP_CAP_MMAP 0x2000
#define SNDCTL_DSP_GETTRIGGER _IOR('P', 16, int)
#define SNDCTL_DSP_SETTRIGGER _IOW('P', 16, int)
#define PCM_ENABLE_INPUT 1
#define PCM_ENABLE_OUTPUT 2
#define SNDCTL_DSP_GETODELAY _IOR('P', 23, int)
#define SOUND_PCM_READ_RATE _IOR('P', 2, int)
#define SOUND_PCM_READ_BITS _IOR('P', 5, int)
#define SOUND_PCM_READ_CHANNELS _IOR('P', 6, int)

#endif
