/* JACK MIDI MTC Parser
 *
 * (C) 2006, 2012  Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <ctype.h>
#include <math.h>
#include <sys/mman.h>

#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>

#define RBSIZE 10

typedef struct {
	int frame;
	int sec;
	int min;
	int hour;

	int type;
	int tick;
	unsigned long long int tme;
} timecode;

/* global Vars */
static timecode tc;
static int full_tc = 0;
static int have_first_full = 0;

static jack_ringbuffer_t *rb = NULL;
static pthread_mutex_t msg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

const char MTCTYPE[4][10] = {
	"24fps",
	"25fps",
	"29fps",
	"30fps",
};

const double expected_tme[4] = {
	24, 25, 30000.0/1001.0, 30
};

#define SE(ARG) tc.tick=ARG; full_tc|=1<<(ARG);
#define SL(ARG) ARG = ( ARG &(~0xf)) | (data&0xf);
#define SH(ARG) ARG = ( ARG &(~0xf0)) | ((data&0xf)<<4);

/* parse MTC message data */
int parse_timecode( int data) {
	int rv = 0;
	switch (data>>4) {
		case 0x0: // #0000 frame LSN
			SE(1); SL(tc.frame); break;
		case 0x1: // #0001 frame MSN
			SE(2); SH(tc.frame); break;
		case 0x2: // #0010 sec LSN
			SE(3); SL(tc.sec); break;
		case 0x3: // #0011 sec MSN
			SE(4); SH(tc.sec); break;
		case 0x4: // #0100 min LSN
			SE(5); SL(tc.min); break;
		case 0x5: // #0101 min MSN
			SE(6); SH(tc.min); break;
		case 0x6: // #0110 hour LSN
			SE(7); SL(tc.hour); break;
		case 0x7: // #0111 hour MSN and type
			SE(0);tc.hour= (tc.hour&(~0xf0)) | ((data&1)<<4);
			tc.type = (data>>1)&3;
			if (full_tc!=0xff) break;
#if 0
			printf("->- %02i:%02i:%02i.%02i[%s]\n",tc.hour,tc.min,tc.sec,tc.frame,MTCTYPE[tc.type]);
#endif
			full_tc = 0; rv = 1; have_first_full = 1;
		default:
			;
	}
	return rv;
}

/************************************************
 * jack-midi
 */

jack_client_t *jack_midi_client = NULL;
jack_port_t   *jack_midi_port;

static int samplerate = 48000;
static unsigned long long qf_tme = 0;
static unsigned long long ff_tme = 0;
static volatile unsigned long long monotonic_cnt = 0;

void process_jmidi_event(jack_midi_event_t *ev, unsigned long long mfcnt) {
	if (ev->size==2 && ev->buffer[0] == 0xf1) {
#if 0 // DEBUG quarter-frames
		printf("QF: %d [%02x %02x ] @%lld dt:%lld\n",
				tc.tick, ev->buffer[0], ev->buffer[1],
				mfcnt + ev->time, mfcnt + ev->time - qf_tme);
#endif
		if (parse_timecode(ev->buffer[1])) {
#if 0 // Warn large delta
			long ffdiff = mfcnt + ev->time - ff_tme;
			long expect = (long) rint(samplerate * 2.0 / expected_tme[tc.type]);
			if (have_first_full) {
				printf("->- 8qf delta-time: expected %ld - have %ld %s\n",
						expect, ffdiff,
						(abs(ffdiff - expect) > 20.0)?"!!!!!!!!!!!!!":""
				);
			}
#endif
			ff_tme = mfcnt + ev->time;

			tc.tme = ff_tme;

			if (jack_ringbuffer_write_space(rb) >= sizeof(timecode)) {
				jack_ringbuffer_write(rb, (void *) &tc, sizeof(timecode));
			}

			if (pthread_mutex_trylock (&msg_thread_lock) == 0) {
				pthread_cond_signal (&data_ready);
				pthread_mutex_unlock (&msg_thread_lock);
			}
		}
		qf_tme = mfcnt + ev->time;
	}
}

static int jack_midi_process(jack_nframes_t nframes, void *arg) {
  void *jack_buf = jack_port_get_buffer(jack_midi_port, nframes);
  int nevents = jack_midi_get_event_count(jack_buf);
  int n;
  for (n=0; n<nevents; n++) {
		jack_midi_event_t ev;
    jack_midi_event_get(&ev, jack_buf, n);
		process_jmidi_event(&ev, monotonic_cnt);
  }
	monotonic_cnt += nframes;
  return 0;
}

void jack_midi_shutdown(void *arg)
{
	jack_midi_client=NULL;
  pthread_cond_signal (&data_ready);
	fprintf (stderr, "jack server shutdown\n");
}

void jm_midi_close(void) {
	if (jack_midi_client) {
		jack_deactivate (jack_midi_client);
		jack_client_close (jack_midi_client);
  }
  if (rb) jack_ringbuffer_free(rb);
  jack_midi_client = NULL;
}

int jm_midi_open() {
	if (jack_midi_client) {
		fprintf (stderr, "xjadeo is already connected to jack-midi.\n");
		return -1;
  }

	jack_midi_client = jack_client_open ("jmtcdump", JackNullOption, NULL);

	if (!jack_midi_client) {
		fprintf(stderr, "could not connect to jack server.\n");
    return -1 ;
  }

  jack_on_shutdown (jack_midi_client, jack_midi_shutdown, 0);
  jack_set_process_callback(jack_midi_client, jack_midi_process, NULL);
  jack_midi_port = jack_port_register(jack_midi_client, "MTC in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput , 0);

  if (jack_midi_port == NULL) {
    fprintf(stderr, "can't register jack-midi-port\n");
    jm_midi_close();
    return -1;
  }

	samplerate=jack_get_sample_rate (jack_midi_client);
	memset(&tc, 0, sizeof(timecode));

	// TODO port-connect

  if (jack_activate(jack_midi_client)) {
    fprintf(stderr, "can't activate jack-midi-client\n");
    jm_midi_close();
  }
	return 0;
}

static int run = 1;

void wearedone(int sig) {
  fprintf(stderr,"caught signal - shutting down.\n");
	run=0;
  pthread_cond_signal (&data_ready);
}

int main (int argc, char ** argv) {
	// TODO parse args, auto-connect
	char newline = '\r'; // or '\n';

	if (jm_midi_open()) {
		return 1;
	}

  rb = jack_ringbuffer_create(RBSIZE * sizeof(timecode));
  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

	signal(SIGINT, wearedone);

  pthread_mutex_lock (&msg_thread_lock);
	while (run && jack_midi_client) {
		int avail_tc = jack_ringbuffer_read_space (rb) / sizeof(timecode);
		if (avail_tc > 0) {
			timecode t;
			jack_ringbuffer_read(rb, (char*) &t, sizeof(timecode));
			fprintf(stdout, "->- %02i:%02i:%02i.%02i [%s] %lld%c",t.hour,t.min,t.sec,t.frame,MTCTYPE[t.type], t.tme, newline);
			fflush(stdout);
		}
    pthread_cond_wait (&data_ready, &msg_thread_lock);
	}
  pthread_mutex_unlock (&msg_thread_lock);

	jm_midi_close();
	printf("bye\n");
	return 0;
}
