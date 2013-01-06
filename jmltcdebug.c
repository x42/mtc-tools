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

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <math.h>
#include <sys/mman.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>

#include <ltc.h>
#include <timecode/timecode.h>
#define LTC_QUEUE_LEN (42)

#define RBSIZE (20)

typedef struct {
	int ltcid;
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

static LTCDecoder *decoder = NULL;
static LTCDecoder *decoder2 = NULL;
static jack_ringbuffer_t *rb = NULL;
static pthread_mutex_t msg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;

const char MTCTYPE[4][10] = {
	"24fps",
	"25fps",
	"29fps",
	"30fps",
};

const double expected_tme[4] = {
	24, 25, 30000.0/1001.0, 30
};

const TimecodeRate *mtctc[4] = {
	TCFPS24,
	TCFPS25,
	TCFPS2997DF,
	TCFPS30
};

/* options */
char newline = '\r'; // or '\n';

static int fps_num = 25; // LTC
static int fps_den = 1;

/************************************************
 * parse MTC message data
 */

#define SE(ARG) tc.tick=ARG; full_tc|=1<<(ARG);
#define SL(ARG) ARG = ( ARG &(~0xf)) | (data&0xf);
#define SH(ARG) ARG = ( ARG &(~0xf0)) | ((data&0xf)<<4);

static int parse_timecode( int data) {
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

static void dequeue_ltc(LTCDecoder *d, int id) {
  LTCFrameExt frame;
  while (ltc_decoder_read(d,&frame)) {
		timecode ltc;
    SMPTETimecode stime;
    ltc_frame_to_time(&stime, &frame.ltc, LTC_USE_DATE);
		memset(&ltc, 0, sizeof(timecode));
		ltc.ltcid = id;
		ltc.frame = stime.frame;
		ltc.sec   = stime.secs;
		ltc.min   = stime.mins;
		ltc.hour  = stime.hours;
		ltc.tme   = frame.off_start;

		if (jack_ringbuffer_write_space(rb) >= sizeof(timecode)) {
			jack_ringbuffer_write(rb, (void *) &ltc, sizeof(timecode));
		}
		if (pthread_mutex_trylock (&msg_thread_lock) == 0) {
			pthread_cond_signal (&data_ready);
			pthread_mutex_unlock (&msg_thread_lock);
		}
	}
}

/************************************************
 * jack-audio/midi
 */

jack_client_t *j_client = NULL;
jack_port_t   *mtc_input_port;
jack_port_t   *ltc_input_port1;
jack_port_t   *ltc_input_port2;

static uint32_t j_samplerate = 48000;
static unsigned long long qf_tme = 0;
static unsigned long long ff_tme = 0;
static volatile unsigned long long monotonic_cnt = 0;
static jack_nframes_t j_latency1 = 0;
static jack_nframes_t j_latency2 = 0;

static void process_jmidi_event(jack_midi_event_t *ev, unsigned long long mfcnt) {
	if (ev->size==2 && ev->buffer[0] == 0xf1) {
#if 0 // DEBUG quarter-frames
		printf("QF: %d [%02x %02x ] @%lld dt:%lld\n",
				tc.tick, ev->buffer[0], ev->buffer[1],
				mfcnt + ev->time, mfcnt + ev->time - qf_tme);
#endif
		if (parse_timecode(ev->buffer[1])) {
#if 0 // Warn large delta
			long ffdiff = mfcnt + ev->time - ff_tme;
			long expect = (long) rint(j_samplerate * 2.0 / expected_tme[tc.type]);
			if (have_first_full) {
				printf("->- 8qf delta-time: expected %ld - have %ld %s\n",
						expect, ffdiff,
						(abs(ffdiff - expect) > 20.0)?"!!!!!!!!!!!!!":""
				);
			}
#endif
			ff_tme = mfcnt + ev->time;
			tc.tme = ff_tme - rint(j_samplerate / expected_tme[tc.type] * 7.0 / 4.0); // 7 quarter-frames
#ifdef DEBUG_JACK_SYNC
			fprintf(stdout, "->- %02i:%02i:%02i.%02i [%s] %lld",tc.hour,tc.min,tc.sec,tc.frame,MTCTYPE[tc.type], tc.tme);
			TimecodeTime tj;
			timecode_sample_to_time(&tj, mtctc[tc.type], j_samplerate, tc.tme);
			fprintf(stdout, " == %02i:%02i:%02i.%02i.%03d\n",tj.hour,tj.minute,tj.second,tj.frame, tj.subframe);
#else
			if (jack_ringbuffer_write_space(rb) >= sizeof(timecode)) {
				jack_ringbuffer_write(rb, (void *) &tc, sizeof(timecode));
			}

			if (pthread_mutex_trylock (&msg_thread_lock) == 0) {
				pthread_cond_signal (&data_ready);
				pthread_mutex_unlock (&msg_thread_lock);
			}
#endif
		}
		qf_tme = mfcnt + ev->time;
	}
}

static int parse_ltc(LTCDecoder *d, jack_nframes_t nframes, jack_default_audio_sample_t *in, ltc_off_t posinfo) {
  jack_nframes_t i;
  unsigned char sound[8192];
  if (nframes > 8192) return 1;

  for (i = 0; i < nframes; i++) {
    const int snd=(int)rint((127.0*in[i])+128.0);
    sound[i] = (unsigned char) (snd&0xff);
  }
  ltc_decoder_write(d, sound, nframes, posinfo);
  return 0;
}

static int process(jack_nframes_t nframes, void *arg) {
	jack_default_audio_sample_t *in;
	void *jack_midi_buf = jack_port_get_buffer(mtc_input_port, nframes);
	int nevents = jack_midi_get_event_count(jack_midi_buf);
	int n;

#ifdef DEBUG_JACK_SYNC
	jack_position_t pos;
	jack_transport_query (j_client, &pos);
	//printf( "%u\n",  pos.frame);

#else

  in = jack_port_get_buffer (ltc_input_port1, nframes);
  parse_ltc(decoder, nframes, in, monotonic_cnt - j_latency1);
	dequeue_ltc(decoder, 1);

  in = jack_port_get_buffer (ltc_input_port2, nframes);
  parse_ltc(decoder2, nframes, in, monotonic_cnt - j_latency2);
	dequeue_ltc(decoder2, 2);
#endif

	for (n=0; n<nevents; n++) {
		jack_midi_event_t ev;
		jack_midi_event_get(&ev, jack_midi_buf, n);
#ifdef DEBUG_JACK_SYNC
		process_jmidi_event(&ev, pos.frame);
#else
		process_jmidi_event(&ev, monotonic_cnt);
#endif
	}
	monotonic_cnt += nframes;
	return 0;
}

int jack_latency_cb(void *arg) {
  jack_latency_range_t jlty;
  if (ltc_input_port1) {
		jack_port_get_latency_range(ltc_input_port1, JackCaptureLatency, &jlty);
		j_latency1 = jlty.max;
		printf("# LTC1 port latency: %d\n", j_latency1);
	}
  if (ltc_input_port2) {
		jack_port_get_latency_range(ltc_input_port2, JackCaptureLatency, &jlty);
		j_latency2 = jlty.max;
		printf("# LTC1 port latency: %d\n", j_latency2);
	}
  return 0;
}

void jack_shutdown(void *arg) {
	j_client=NULL;
	pthread_cond_signal (&data_ready);
	fprintf (stderr, "jack server shutdown\n");
}

void cleanup(void) {
	if (j_client) {
		jack_deactivate (j_client);
		jack_client_close (j_client);
	}
	if (rb) {
		jack_ringbuffer_free(rb);
	}
  ltc_decoder_free(decoder);
  ltc_decoder_free(decoder2);
	j_client = NULL;
}


/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name) {
	jack_status_t status;
	j_client = jack_client_open (client_name, JackNullOption, &status);
	if (j_client == NULL) {
		fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Unable to connect to JACK server\n");
		}
		return (-1);
	}
	if (status & JackServerStarted) {
		fprintf (stderr, "JACK server started\n");
	}
	if (status & JackNameNotUnique) {
		client_name = jack_get_client_name(j_client);
		fprintf (stderr, "jack-client name: `%s'\n", client_name);
	}
	jack_set_process_callback (j_client, process, 0);
  jack_set_graph_order_callback (j_client, jack_latency_cb, NULL);

#ifndef WIN32
	jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
	j_samplerate=jack_get_sample_rate (j_client);

	return (0);
}

static int jack_portsetup(void) {
	if ((mtc_input_port = jack_port_register(j_client, "mtc_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0)) == 0) {
		fprintf (stderr, "cannot register mtc input port !\n");
		return (-1);
	}
	if ((ltc_input_port1 = jack_port_register(j_client, "ltc_in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
		fprintf (stderr, "cannot register ltc input port !\n");
		return (-1);
	}
	if ((ltc_input_port2 = jack_port_register(j_client, "ltc_in2", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
		fprintf (stderr, "cannot register ltc input port !\n");
		return (-1);
	}
  decoder = ltc_decoder_create(j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN);
  decoder2 = ltc_decoder_create(j_samplerate * fps_den / fps_num, LTC_QUEUE_LEN);
	return (0);
}

static void mtc_port_connect(char *mtc_port) {
	if (mtc_port && jack_connect(j_client, mtc_port, jack_port_name(mtc_input_port))) {
		fprintf(stderr, "cannot connect port %s to %s\n", mtc_port, jack_port_name(mtc_input_port));
	}
}

static void ltc_port_connect(char *ltc_port, jack_port_t *p) {
	if (ltc_port && jack_connect(j_client, ltc_port, jack_port_name(p))) {
		fprintf(stderr, "cannot connect port %s to %s\n", ltc_port, jack_port_name(p));
	}
}


/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"newline", no_argument, 0, 'n'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jmtcdump - JACK MIDI Timecode dump.\n\n");
  printf ("Usage: jmtcdump [ OPTIONS ] [JACK-port]\n\n");
  printf ("Options:\n\
  -h, --help                 display this help and exit\n\
  -n, --newline              print a newline after each Timecode\n\
  -V, --version              print version information and exit\n\
\n");
  printf ("\n\
This tool subscribes to a JACK Midi Port and prints received Midi\n\
time code to stdout.\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website and manual: <https://github.com/x42/mtc-tools>\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
	int c;

	while ((c = getopt_long (argc, argv,
			   "h"	/* help */
			   "n"	/* newline */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF) {
		switch (c) {
			case 'n':
				newline = '\n';
				break;
			case 'V':
				printf ("jmtcdump version %s\n\n", VERSION);
				printf ("Copyright (C) GPL 2012 Robin Gareus <robin@gareus.org>\n");
				exit (0);

			case 'h':
				usage (0);

			default:
			  usage (EXIT_FAILURE);
		}
	}
	return optind;
}

static int run = 1;

void wearedone(int sig) {
	fprintf(stderr,"caught signal - shutting down.\n");
	run=0;
	pthread_cond_signal (&data_ready);
}

int main (int argc, char ** argv) {
	decode_switches (argc, argv);

	if (init_jack("jmtcdump"))
		goto out;
	if (jack_portsetup())
		goto out;

	memset(&tc, 0, sizeof(timecode));
	tc.ltcid = -1;
	rb = jack_ringbuffer_create(RBSIZE * sizeof(timecode));

	if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
		fprintf(stderr, "Warning: Can not lock memory.\n");
	}

	// -=-=-= RUN =-=-=-

	if (jack_activate (j_client)) {
		fprintf (stderr, "cannot activate client.\n");
		goto out;
	}

	while (optind < argc) {
		mtc_port_connect(argv[optind++]);
		if (optind < argc)
			ltc_port_connect(argv[optind++], ltc_input_port1);
		if (optind < argc)
			ltc_port_connect(argv[optind++], ltc_input_port2);
	}

#ifndef _WIN32
	signal(SIGINT, wearedone);
#endif

	pthread_mutex_lock (&msg_thread_lock);
	while (run && j_client) {
		int avail_tc = jack_ringbuffer_read_space (rb) / sizeof(timecode);
		if (avail_tc > 0) {
			timecode t;
			jack_ringbuffer_read(rb, (char*) &t, sizeof(timecode));
			if (t.ltcid<0)
				fprintf(stdout, "MTC%d %02i:%02i:%02i.%02i [%s] %lld%c",
						abs(t.ltcid),
						t.hour,t.min,t.sec,t.frame,MTCTYPE[t.type], t.tme, newline);
			else
				fprintf(stdout, "%sLTC%d %02i:%02i:%02i.%02i ------- %lld%c",
						(newline=='\r' ? "\t\t\t\t":""),
						abs(t.ltcid),
						t.hour,t.min,t.sec,t.frame, t.tme, newline);
			fflush(stdout);
		}
		pthread_cond_wait (&data_ready, &msg_thread_lock);
	}
	pthread_mutex_unlock (&msg_thread_lock);

out:
	cleanup();
	return 0;
}
