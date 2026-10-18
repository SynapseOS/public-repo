/* SynapseOS OSS playback backend for SoLoud. Build with WITH_OSS and omit
 * upstream soloud_oss.cpp; PCM conversion and resampling remain in SoLoud. */
#include "soloud.h"
#include "soloud_thread.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <unistd.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

namespace SoLoud {
struct SynapseOSS {
	Soloud *soloud;
	int fd;
	unsigned frames;
	short *pcm;
	int stop;
	pthread_t thread;
	bool started;

	SynapseOSS( ) : soloud(0), fd(-1), frames(0), pcm(0), stop(0), thread( ), started(false) {}
};

static void *synapseMix(void *arg) {
	SynapseOSS *d = static_cast<SynapseOSS *>(arg);

	while (!__atomic_load_n(&d->stop, __ATOMIC_ACQUIRE)) {
		d->soloud->mixSigned16(d->pcm, d->frames);
		const char *p = reinterpret_cast<const char *>(d->pcm);
		size_t left = d->frames * 2 * sizeof(short);

		while (left && !__atomic_load_n(&d->stop, __ATOMIC_ACQUIRE)) {
			ssize_t n = write(d->fd, p, left);

			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				fprintf(stderr, "SynapseOSS: write /dev/dsp failed: result=%ld errno=%d (%s)\n",
				        (long)n, errno, strerror(errno));
				__atomic_store_n(&d->stop, 1, __ATOMIC_RELEASE);
				break;
			}

			left -= n;
			p += n;
		}
	}

	return 0;
}

static void synapseCleanup(Soloud *s) {
	SynapseOSS *d = static_cast<SynapseOSS *>(s->mBackendData);

	if (!d)
		return;
	__atomic_store_n(&d->stop, 1, __ATOMIC_RELEASE);

	if (d->started)
		pthread_join(d->thread, 0);
	if (d->fd >= 0) {
		ioctl(d->fd, SNDCTL_DSP_RESET, 0);
		close(d->fd);
	}

	delete[] d->pcm;
	delete d;
	s->mBackendData = 0;
}

static result synapseFailure(Soloud *s, const char *stage, int error, int value = 0) {
	fprintf(stderr, "SynapseOSS: %s failed: errno=%d (%s), value=%d\n", stage, error,
	        strerror(error), value);
	synapseCleanup(s);
	return UNKNOWN_ERROR;
}

result oss_init(Soloud *s, unsigned flags, unsigned rate, unsigned buffer, unsigned channels) {
	(void)channels;
	SynapseOSS *d = new (std::nothrow) SynapseOSS;

	if (!d)
		return OUT_OF_MEMORY;
	s->mBackendData = d;
	s->mBackendCleanupFunc = synapseCleanup;
	d->soloud = s;
	d->fd = open("/dev/dsp", O_WRONLY);

	if (d->fd < 0)
		return synapseFailure(s, "open /dev/dsp", errno);
	int format = AFMT_S16_LE, stereo = 2, speed = rate ? rate : 48000;

	if (ioctl(d->fd, SNDCTL_DSP_SETFMT, &format))
		return synapseFailure(s, "SETFMT ioctl", errno);
	if (format != AFMT_S16_LE)
		return synapseFailure(s, "SETFMT negotiation", 0, format);
	if (ioctl(d->fd, SNDCTL_DSP_CHANNELS, &stereo))
		return synapseFailure(s, "CHANNELS ioctl", errno);
	if (stereo != 2)
		return synapseFailure(s, "CHANNELS negotiation", 0, stereo);
	if (ioctl(d->fd, SNDCTL_DSP_SPEED, &speed))
		return synapseFailure(s, "SPEED ioctl", errno);
	if (speed <= 0)
		return synapseFailure(s, "SPEED negotiation", 0, speed);
	/* HDA accepts at most 8192 frames. Request one period, then use the
     * reported result. Negotiated 48 kHz configures SoLoud's resampler. */
	unsigned bytes = (buffer ? buffer : 2048) * 4;
	unsigned shift = 4;

	while (shift < 15 && (1u << shift) < bytes)
		++shift;
	int fragment = (1 << 16) | shift;

	if (ioctl(d->fd, SNDCTL_DSP_SETFRAGMENT, &fragment))
		return synapseFailure(s, "SETFRAGMENT ioctl", errno);
	audio_buf_info space;

	if (ioctl(d->fd, SNDCTL_DSP_GETOSPACE, &space))
		return synapseFailure(s, "GETOSPACE ioctl", errno);
	if (space.fragsize < 16 || space.fragsize > 32768 || space.fragsize % 4)
		return synapseFailure(s, "GETOSPACE fragment", 0, space.fragsize);
	d->frames = space.fragsize / 4;
	d->pcm = new (std::nothrow) short[d->frames * 2];

	if (!d->pcm) {
		synapseCleanup(s);
		return OUT_OF_MEMORY;
	}

	s->postinit_internal(speed, d->frames, flags, 2);
	int thread_error = pthread_create(&d->thread, 0, synapseMix, d);

	if (thread_error)
		return synapseFailure(s, "pthread_create mixer", thread_error);
	d->started = true;
	s->mBackendString = "SynapseOS OSS HDA";
	return SO_NO_ERROR;
}
} // namespace SoLoud
