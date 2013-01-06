/* JACK-Transport MTC generator
 *
 * Copyright (C) 2006, 2012 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define JACK_MIDI_QUEUE_SIZE (256)

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>
#include <sys/mman.h>
#include <timecode/timecode.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

static jack_port_t *mtc_output_port = NULL;
static jack_client_t *j_client = NULL;
static jack_nframes_t jmtc_latency = 0;
static uint32_t j_samplerate = 48000;
static volatile long long int monotonic_fcnt = 0;
static int decodeahead = 2;

static jack_ringbuffer_t *rb = NULL;
static pthread_mutex_t msg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;

/* options */
static int debug = 0;
static TimecodeRate framerate = { 25, 1, 0, 80 };
static int use_jack_fps = 0;

/* a simple state machine for this client */
static volatile enum {
  Init,
  Run,
  Exit
} client_state = Init;

typedef struct my_midi_event {
  long long int monotonic_align;
  jack_nframes_t time;
  size_t size;
  jack_midi_data_t buffer[16];
} my_midi_event_t;

static my_midi_event_t event_queue[JACK_MIDI_QUEUE_SIZE];
static int queued_events_start = 0;
static int queued_events_end = 0;
int jack_graph_cb(void *arg);


static void rbprintf(const char *fmt, ...) {
  char buf[BUFSIZ];
  int n;
  va_list ap;
  va_start(ap, fmt);
  n = vsnprintf(buf, BUFSIZ, fmt, ap);
  va_end(ap);
  if (jack_ringbuffer_write_space(rb) >=n){
    jack_ringbuffer_write(rb, buf, n);
  }
  pthread_cond_signal (&data_ready);
}

/**
 * cleanup and exit
 * call this function only _after_ everything has been initialized!
 */
static void cleanup(int sig) {
  if (j_client) {
    jack_client_close (j_client);
    j_client=NULL;
  }
  if (rb) {
    jack_ringbuffer_free(rb);
  }
  fprintf(stderr, "bye.\n");
}

static int queue_mtc_quarterframe(const TimecodeTime * const t, const int mtc_tc, const long long int posinfo, const int qf) {
  unsigned char mtc_msg=0;
  switch(qf) {
    case 0: mtc_msg =  0x00 |  (t->frame&0xf); break;
    case 1: mtc_msg =  0x10 | ((t->frame&0xf0)>>4); break;
    case 2: mtc_msg =  0x20 |  (t->second&0xf); break;
    case 3: mtc_msg =  0x30 | ((t->second&0xf0)>>4); break;
    case 4: mtc_msg =  0x40 |  (t->minute&0xf); break;
    case 5: mtc_msg =  0x50 | ((t->minute&0xf0)>>4); break;
    case 6: mtc_msg =  0x60 |  ((mtc_tc|t->hour)&0xf); break;
    case 7: mtc_msg =  0x70 | (((mtc_tc|t->hour)&0xf0)>>4); break;
  }

  jack_midi_data_t *mmsg = event_queue[queued_events_start].buffer;
  mmsg[0] = (char) 0xf1;
  mmsg[1] = (char) mtc_msg;

  event_queue[queued_events_start].monotonic_align = posinfo;
  event_queue[queued_events_start].time = 0;
  event_queue[queued_events_start].size = 2;
  queued_events_start = (queued_events_start + 1)%JACK_MIDI_QUEUE_SIZE;

  return 0;
}

static void queue_mtc_quarterframes(const TimecodeTime * const t, const int mtc_tc, const int reverse, const int speed, const long long int posinfo) {
  int i;
  const float qfl = speed / 4.0;
  static TimecodeTime stime;
  static int next_quarter_frame_to_send = 0;

  if (next_quarter_frame_to_send != 0 && next_quarter_frame_to_send != 4) {
    /* this can actually never happen */
    rbprintf("quarter-frame mis-aligment: %d (should be 0 or 4)\n", next_quarter_frame_to_send);
    next_quarter_frame_to_send = 0;
  }
  if (mtc_tc != 0x20 && (t->frame%2) == 1 && next_quarter_frame_to_send == 0) {
    /* the MTC spec does note that for 24, 30 drop and 30 non-drop, the frame number computed from quarter frames is always even
     * but for 25 it might be odd or even "depending on whiuch frame number the 8 message sequence started"
     */
    rbprintf("re-align quarter-frame to even frame-number\n");
    return;
  }

  if (next_quarter_frame_to_send == 0) {
    /* MTC spans timecode over two frames.
     * remember the current timecode since the min/hour (2nd part)
     * may change.
     */
    memcpy(&stime, t, sizeof(TimecodeTime));
  }

  for (i=0;i<4;++i) {
    if (reverse)
      next_quarter_frame_to_send--;
    if (next_quarter_frame_to_send < 0)
      next_quarter_frame_to_send = 7;

    queue_mtc_quarterframe(&stime, mtc_tc, posinfo + i*qfl, next_quarter_frame_to_send);

    if (!reverse)
      next_quarter_frame_to_send++;
    if (next_quarter_frame_to_send > 7)
      next_quarter_frame_to_send = 0;
  }
}

static void queue_mtc_sysex(const TimecodeTime * const t, const int mtc_tc, const long long int posinfo) {
  jack_midi_data_t *sysex = event_queue[queued_events_start].buffer;
#if 1
  sysex[0]  = (unsigned char) 0xf0; // fixed
  sysex[1]  = (unsigned char) 0x7f; // fixed
  sysex[2]  = (unsigned char) 0x7f; // sysex channel
  sysex[3]  = (unsigned char) 0x01; // fixed
  sysex[4]  = (unsigned char) 0x01; // fixed
  sysex[5]  = (unsigned char) 0x00; // hour
  sysex[6]  = (unsigned char) 0x00; // minute
  sysex[7]  = (unsigned char) 0x00; // seconds
  sysex[8]  = (unsigned char) 0x00; // frame
  sysex[9]  = (unsigned char) 0xf7; // fixed

  sysex[5] |= (unsigned char) (mtc_tc&0x60);
  sysex[5] |= (unsigned char) (t->hour&0x1f);
  sysex[6] |= (unsigned char) (t->minute&0x7f);
  sysex[7] |= (unsigned char) (t->second&0x7f);
  sysex[8] |= (unsigned char) (t->frame&0x7f);

  event_queue[queued_events_start].size = 10;

#else

  sysex[0]   = (char) 0xf0;
  sysex[1]   = (char) 0x7f;
  sysex[2]   = (char) 0x7f;
  sysex[3]   = (char) 0x06;
  sysex[4]   = (char) 0x44;
  sysex[5]   = (char) 0x06;
  sysex[6]   = (char) 0x01;
  sysex[7]   = (char) 0x00;
  sysex[8]   = (char) 0x00;
  sysex[9]   = (char) 0x00;
  sysex[10]  = (char) 0x00;
  sysex[11]  = (char) 0x00;
  sysex[12]  = (char) 0xf7;

  sysex[7]  |= (char) 0x20; // 25fps
  sysex[7]  |= (char) (stime->hours&0x1f);
  sysex[8]  |= (char) (stime->mins&0x7f);
  sysex[9]  |= (char) (stime->secs&0x7f);
  sysex[10] |= (char) (stime->frame&0x7f);

  int checksum = (sysex[7] + sysex[8] + sysex[9] + sysex[10] + 0x3f)&0x7f ;
  sysex[11]  = (char) (127-checksum); //checksum
  event_queue[queued_events_start].size = 13;
#endif

  event_queue[queued_events_start].monotonic_align = posinfo;
  event_queue[queued_events_start].time = 0;
  queued_events_start = (queued_events_start + 1)%JACK_MIDI_QUEUE_SIZE;
}

/**
 *
 */
static void generate_mtc(TimecodeTime *t, unsigned long long int mfcnt, int mode) {

  static TimecodeTime stime;
  static unsigned long long int pfcnt;
  static int pmode = -1;

  const double fptcf = timecode_frames_per_timecode_frame(&framerate, j_samplerate);
  const int64_t nfn =  timecode_to_framenumber(t, &framerate);
  int64_t ofn =  timecode_to_framenumber(&stime, &framerate);
  unsigned long long int cfcnt = mfcnt - t->subframe;

  if (pmode == mode && mode == 0 && ofn == nfn) {
    /* we already sent this frame */
    return;
  }

  if (   nfn - ofn > 3
      || mfcnt - pfcnt > 3 * fptcf
      || (nfn - ofn < 1 && mode != 2)
      ) {
#if 0 // DEBUG
    char tcs[12];
    timecode_time_to_string(tcs, t);
    printf(" !! RESET %s | pf: %lld nf: %lld\n", tcs, ofn, nfn);
#endif
    mode = 0;
    memcpy(&stime, t, sizeof(TimecodeTime));
  }

  pfcnt = mfcnt;
  pmode = mode;

  if (mode == 2 && nfn + decodeahead <= ofn) {
    return;
  }

  if (mode == 2) {
    cfcnt += fptcf * (ofn - nfn);
  }

#if 0 // DEBUG
  printf("DOIT %lld -> %lld  @ %lld\n", ofn, nfn, mfcnt);
#endif

  do {
    /*set MTC fps */
    static int fps_warn = 0;
    int mtc_tc = 0x20;
    switch ((int)floor(timecode_rate_to_double(&framerate))) {
      case 24:
	mtc_tc = 0x00;
	fps_warn = 0;
	break;
      case 25:
	mtc_tc = 0x20;
	fps_warn = 0;
	break;
      case 29:
	mtc_tc = 0x40;
	fps_warn = 0;
	break;
      case 30:
	mtc_tc = 0x60;
	fps_warn = 0;
	break;
      default:
	if (!fps_warn) {
	  fps_warn = 1;
	  rbprintf("WARNING: invalid framerate %.2f (using 25fps instead) - expect sync problems\n",
	      timecode_rate_to_double(&framerate));
	}
	break;
    }

#if 0 // DEBUG
  char tcs[12];
  timecode_time_to_string(tcs, t);
  printf("%d -> %s.%d %f %lld\n", mode, tcs, t->subframe, fptcf, cfcnt);
#endif

    if (mode != 2) {
      if (debug) rbprintf("sending sysex locate.\n");
      queued_events_end = queued_events_start; // flush queue
      queue_mtc_sysex(&stime, mtc_tc, mfcnt);
      memcpy(&stime, t, sizeof(TimecodeTime));
    } else {
      queue_mtc_quarterframes(&stime, mtc_tc, 0,
	  rint(fptcf),
	  cfcnt);

      timecode_time_increment(&stime, &framerate);
      cfcnt += fptcf;
      ofn = timecode_to_framenumber(&stime, &framerate);
    }
  } while (mode == 2 && ofn < nfn + decodeahead);
}

/**
 * jack audio process callback
 */
int process (jack_nframes_t nframes, void *arg) {
  void *out;
  jack_transport_state_t state;
  jack_position_t pos;
  TimecodeTime t;
  jack_nframes_t sample_pos;

  state = jack_transport_query (j_client, &pos);
  sample_pos = pos.frame;

  if (use_jack_fps && pos.valid & JackAudioVideoRatio) {
    static float audio_frames_per_video_frame = 0;
    if (pos.audio_frames_per_video_frame != audio_frames_per_video_frame) {
      audio_frames_per_video_frame = pos.audio_frames_per_video_frame;
      rbprintf("new APV: %.2f\n", pos.audio_frames_per_video_frame);
      switch ((int)floor(j_samplerate/audio_frames_per_video_frame)) {
	case 24:
	  framerate.num=24; framerate.den=1; framerate.drop=0;
	  break;
	case 25:
	  framerate.num=25; framerate.den=1; framerate.drop=0;
	  break;
	case 29:
	  framerate.num=30000; framerate.den=1001; framerate.drop=1;
	  break;
	case 30:
	  framerate.num=30; framerate.den=1; framerate.drop=0;
	  break;
	default:
	  rbprintf("invalid framerate.\n");
	  break;
      }
      // TODO use timecode_strftimecode()
      rbprintf("FPS changed to %.2f%s\n", timecode_rate_to_double(&framerate), framerate.drop?"df":"");
      framerate.subframes = timecode_frames_per_timecode_frame(&framerate, j_samplerate);
      decodeahead = 2 + ceil((double)jmtc_latency / timecode_frames_per_timecode_frame(&framerate, j_samplerate));
    }
  }

  if (pos.valid & JackVideoFrameOffset) {
    if (pos.video_offset >= sample_pos) {
      sample_pos -= pos.video_offset;
    } else {
      sample_pos = 0;
    }
  }

  timecode_sample_to_time(&t, &framerate, pos.frame_rate, sample_pos);

  switch (state) {
    case JackTransportStopped:
      //send sysex-MTC message - if changed
      generate_mtc(&t, monotonic_fcnt, 0);
      break;
    case JackTransportStarting:
#if 0 // jack2 only
    case JackTransportNetStarting:
#endif
      //send sysex-MTC message
      generate_mtc(&t, monotonic_fcnt, 1);
      break;
    case JackTransportRolling:
      // enqueue quarter-frame MTC messages
      generate_mtc(&t, monotonic_fcnt, 2);
      break;
    default: /* old JackTransportLooping */
      break;
  }

  out = jack_port_get_buffer(mtc_output_port, nframes);

#if 0 // workaround jack2 latency cb order - fixed in jack2 e577581de (2012-10-30)
  jack_graph_cb(out);
#endif

  jack_midi_clear_buffer(out);
  while (queued_events_end != queued_events_start) {
    const long long int mt = event_queue[queued_events_end].monotonic_align - jmtc_latency;
    if (mt >= monotonic_fcnt + nframes) {
      // fprintf(stderr, "DEBUG: MTC timestamp is for next jack cycle.\n"); // XXX
      break;
    }
    if (mt < monotonic_fcnt) {
      if (debug) rbprintf("WARNING: MTC was for previous jack cycle (port latency too large?)\n");
      //fprintf(stderr, "TME: %lld < %lld)\n", mt, monotonic_fcnt); // XXX
    } else {

#if 0 // DEBUG quarter frame timing
      static long long int prev = 0;
      if (mt-prev != rint((double)j_samplerate / timecode_rate_to_double(&framerate) / 4)) {
	fprintf(stderr, " QT time %lld != %u\n", mt-prev, (int) rint((double)j_samplerate / timecode_rate_to_double(&framerate) / 4));
      }
      prev = mt;
#endif

      event_queue[queued_events_end].time = mt - monotonic_fcnt;
#if 0 // DEBUG dump Events & Timing
      printf("QF:%02x abs: %"PRId64" rel:%4u @%"PRId64" jt:%"PRId64"\n",
	  event_queue[queued_events_end].buffer[1], mt,
	  event_queue[queued_events_end].time, monotonic_fcnt,
	  (int64_t) (sample_pos + event_queue[queued_events_end].time));
#endif
      jack_midi_event_write(out,
	  event_queue[queued_events_end].time,
	  event_queue[queued_events_end].buffer,
	  event_queue[queued_events_end].size
	  );
    }
    queued_events_end = (queued_events_end + 1)%JACK_MIDI_QUEUE_SIZE;
  }

  monotonic_fcnt += nframes;

  return 0;
}

#if 0 // unused - custom latency callback
#ifndef MAX
#define MAX(a,b) ( ((a) < (b)) ? (b) : (a))
#endif

static int max_latency(jack_port_t *port, jack_latency_callback_mode_t mode) {
  int max_lat = 0;
  jack_latency_range_t jlty;
  const char ** ports = jack_port_get_connections(port);
  const char ** it = ports;
  // printf("query latency for %s\n", jack_port_name(port));
  for (it = ports; it && *it ; ++it) {
    //printf("  conn %s\n", *it);
    jack_port_t * jp = jack_port_by_name(j_client, *it);
    jack_port_get_latency_range(jp, mode, &jlty);
    max_lat = MAX(max_lat, jlty.max);
  }
  if (ports) jack_free(ports);
  return max_lat;
}

void jack_latency_cb(jack_latency_callback_mode_t mode, void *arg) {
  jack_latency_range_t range;
  if (mtc_output_port && mode == JackPlaybackLatency) {
    jmtc_latency = max_latency(mtc_output_port, JackCaptureLatency);
    if (debug && !arg)
      rbprintf("MTC port set latency: %d\n", jmtc_latency);
    range.min = range.max = jmtc_latency;
    jack_port_set_latency_range(mtc_output_port, JackPlaybackLatency, &range);
  }
  decodeahead = 2 + ceil((double)jmtc_latency / timecode_frames_per_timecode_frame(&framerate, j_samplerate));
}
#endif

int jack_graph_cb(void *arg) {
  jack_latency_range_t jlty;
  if (mtc_output_port) {
    jack_port_get_latency_range(mtc_output_port, JackPlaybackLatency, &jlty);
    if (debug && !arg)
      rbprintf("MTC port latency: %d\n", jmtc_latency);
  }
  decodeahead = 2 + ceil((double)jmtc_latency / timecode_frames_per_timecode_frame(&framerate, j_samplerate));
  return 0;
}

void jack_shutdown (void *arg) {
  fprintf(stderr,"recv. shutdown request from jackd.\n");
  client_state=Exit;
  pthread_cond_signal (&data_ready);
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

  //jack_set_latency_callback (j_client, jack_latency_cb, NULL);
  jack_set_graph_order_callback (j_client, jack_graph_cb, NULL);

#ifndef WIN32
  jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif
  j_samplerate=jack_get_sample_rate (j_client);

  return (0);
}

static int jack_portsetup(void) {
  if ((mtc_output_port = jack_port_register(j_client, "mtc_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)) == 0) {
    fprintf (stderr, "cannot register mtc ouput port !\n");
    return (-1);
  }
  return (0);
}

static void port_connect(char *mtc_port) {
  if (mtc_port && jack_connect(j_client, jack_port_name(mtc_output_port), mtc_port)) {
    fprintf(stderr, "cannot connect port %s to %s\n", jack_port_name(mtc_output_port), mtc_port);
  }
}

void catchsig (int sig) {
#ifndef _WIN32
  signal(SIGHUP, catchsig);
#endif
  fprintf(stderr,"caught signal - shutting down.\n");
  client_state=Exit;
  pthread_cond_signal (&data_ready);
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"help", no_argument, 0, 'h'},
  {"jackvideo", no_argument, 0, 'F'},
  {"fps", required_argument, 0, 'f'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jmtcgen - JACK app to generate MTC from JACK transport.\n\n");
  printf ("Usage: jmtcgen [ OPTIONS ] [JACK-port]*\n\n");
  printf ("Options:\n\
  -f, --fps <num>[/den]      set MTC framerate (default 25/1)\n\
  -F, --jackvideo            use jack-transport's FPS setting if available\n\
  -h, --help                 display this help and exit\n\
  -V, --version              print version information and exit\n\
\n");
  printf ("\n\
This tool generates Midi Time Code from JACK transport and sends it\n\
on a JACK-midi port.\n\
\n\
Note that MTC only supports 4 framerates: 24, 25, 30df and 30 fps.\n\
30df == 30000/1001 fps\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website and manual: <https://github.com/x42/mtc-tools>\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
			   "d"	/* debug */
			   "F"	/* jack_video */
			   "f:"	/* fps */
			   "h"	/* help */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
    {
      switch (c)
	{

	case 'd':
	  debug = 1;
	  break;

	case 'F':
	  use_jack_fps = 1;
	  break;

	case 'f':
	{
	  framerate.num = atoi(optarg);
	  char *tmp = strchr(optarg, '/');
	  if (tmp) framerate.den=atoi(++tmp);
	}
	break;

	case 'V':
	  printf ("jmtcgen version %s\n\n", VERSION);
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

int main (int argc, char **argv) {

  decode_switches (argc, argv);

  // -=-=-= INITIALIZE =-=-=-

  if (init_jack("jmtcgen"))
    goto out;
  if (jack_portsetup())
    goto out;

  rb = jack_ringbuffer_create(4096 * sizeof(char));

  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

  framerate.subframes = timecode_frames_per_timecode_frame(&framerate, j_samplerate);
  // -=-=-= RUN =-=-=-

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  while (optind < argc)
    port_connect(argv[optind++]);

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);
#endif

  // -=-=-= JACK DOES ALL THE WORK =-=-=-

  pthread_mutex_lock (&msg_thread_lock);
  while (client_state != Exit) {
    while(jack_ringbuffer_read_space (rb) > 0) {
      char x;
      jack_ringbuffer_read(rb, (char*) &x, sizeof(char));
      fputc(x, stdout);
    }
    fflush(stdout);
    pthread_cond_wait (&data_ready, &msg_thread_lock);
  }
  pthread_mutex_unlock (&msg_thread_lock);

  // -=-=-= CLEANUP =-=-=-

out:
  cleanup(0);
  return(0);
}
/* vi:set ts=8 sts=2 sw=2: */
