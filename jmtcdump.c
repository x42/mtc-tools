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

typedef struct {
	int frame;
	int sec;
	int min;
	int hour;

	int day; // overflow
	int type;
	int tick;
} smpte;

/* global Vars */
smpte tc;
int full_tc = 0;

const char MTCTYPE[4][10] = {
	"24fps",
	"25fps",
	"29fps",
	"30fps",
};

#define SE(ARG) tc.tick=ARG; full_tc|=1<<(ARG);
#define SL(ARG) ARG = ( ARG &(~0xf)) | (data&0xf);
#define SH(ARG) ARG = ( ARG &(~0xf0)) | ((data&0xf)<<4);

/* parse MTC message data */
void parse_timecode( int data) {
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
			printf("->- %02i:%02i:%02i.%02i[%s]\n",tc.hour,tc.min,tc.sec,tc.frame,MTCTYPE[tc.type]);
		default: 
			;
	}
}

/************************************************
 * jack-midi 
 */

#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/midiport.h>

jack_client_t *jack_midi_client = NULL;
jack_port_t   *jack_midi_port; 

static unsigned long long tme = 0;
static unsigned long long monotonic_cnt = 0;

void process_jmidi_event(jack_midi_event_t *ev, unsigned long long mfcnt) {
	if (ev->size==2 && ev->buffer[0] == 0xf1) {
		parse_timecode(ev->buffer[1]);
		printf("QF: %d [%02x %02x ] @%lld dt:%lld\n", tc.tick, ev->buffer[0], ev->buffer[1], mfcnt + ev->time, mfcnt + ev->time - tme);
		tme = mfcnt + ev->time;
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
	fprintf (stderr, "jack server shutdown\n");
}

void jm_midi_close(void) {
	if (jack_midi_client) {
		jack_deactivate (jack_midi_client);
		jack_client_close (jack_midi_client);
  }
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

	// init smpte
	tc.type=tc.min=tc.frame=tc.sec=tc.hour=0;

  if (jack_activate(jack_midi_client)) {
    fprintf(stderr, "can't activate jack-midi-client\n");
    jm_midi_close(); 
  }
	return 0;
}

static int run = 1;
void wearedone(int sig) { run=0; }

int main (int argc, char ** argv) {
	if (jm_midi_open()) {
		return 1;
	}
	signal(SIGINT, wearedone);
	while (run) {
		sleep (1);
	}
	jm_midi_close();
	printf("bye\n");
	return 0;
}
