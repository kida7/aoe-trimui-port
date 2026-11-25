/* midisynth: a tiny General MIDI software synthesizer for ALSA.
 * Creates an ALSA sequencer client with one writable port ("AoE MIDI Synth") so that Wine's winealsa MIDI driver
 * (and any ALSA MIDI app) can play into it, renders with TinySoundFont (SoundFont 2) and plays through the ALSA PCM.
 *
 * usage: midisynth SOUNDFONT.sf2          env: MIDISYNTH_PCM (default "default"), MIDISYNTH_GAIN_DB (default -6),
 *                                               MIDISYNTH_RATE (default 48000), MIDISYNTH_VERBOSE=1
 * build: aarch64-linux-gnu-gcc -O2 -o midisynth midisynth.c -lasound -lm      (tsf.h: MIT, github.com/schellingb/TinySoundFont)
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TSF_IMPLEMENTATION
#include "tsf.h"

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }

#define FRAMES 512

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: midisynth SOUNDFONT.sf2\n"); return 2; }
    int verbose = getenv("MIDISYNTH_VERBOSE") != NULL;
    int rate = getenv("MIDISYNTH_RATE") ? atoi(getenv("MIDISYNTH_RATE")) : 48000;
    float gain_db = getenv("MIDISYNTH_GAIN_DB") ? (float)atof(getenv("MIDISYNTH_GAIN_DB")) : -6.0f;
    const char *pcm_name = getenv("MIDISYNTH_PCM") ? getenv("MIDISYNTH_PCM") : "default";
    setvbuf(stderr, NULL, _IONBF, 0);
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig);

    tsf *f = tsf_load_filename(argv[1]);
    if (!f) { fprintf(stderr, "midisynth: cannot load soundfont %s\n", argv[1]); return 1; }
    tsf_set_output(f, TSF_STEREO_INTERLEAVED, rate, gain_db);
    for (int c = 0; c < 16; c++) tsf_channel_set_presetnumber(f, c, 0, c == 9);
    fprintf(stderr, "midisynth: soundfont %s loaded, %d presets, rate %d, gain %.1f dB\n", argv[1], tsf_get_presetcount(f), rate, gain_db);

    snd_seq_t *seq;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) { fprintf(stderr, "midisynth: snd_seq_open failed\n"); return 1; }
    snd_seq_set_client_name(seq, "AoE MIDI Synth");
    int port = snd_seq_create_simple_port(seq, "Synth",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_MIDI_GM | SND_SEQ_PORT_TYPE_SYNTHESIZER | SND_SEQ_PORT_TYPE_SOFTWARE | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0) { fprintf(stderr, "midisynth: cannot create sequencer port\n"); return 1; }
    fprintf(stderr, "midisynth: sequencer client %d port %d ready\n", snd_seq_client_id(seq), port);

    snd_pcm_t *pcm;
    if (snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, 0) < 0) { fprintf(stderr, "midisynth: cannot open PCM %s\n", pcm_name); return 1; }
    if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1, 60000) < 0) { fprintf(stderr, "midisynth: snd_pcm_set_params failed\n"); return 1; }

    short buf[FRAMES * 2];
    long events = 0, notes = 0;
    double last_log = 0;
    while (running) {
        snd_seq_event_t *ev;
        while (snd_seq_event_input(seq, &ev) >= 0 && ev) {
            events++;
            int ch = ev->data.note.channel & 15;
            switch (ev->type) {
            case SND_SEQ_EVENT_NOTEON:
                if (ev->data.note.velocity) { tsf_channel_note_on(f, ch, ev->data.note.note, ev->data.note.velocity / 127.0f); notes++; }
                else tsf_channel_note_off(f, ch, ev->data.note.note);
                break;
            case SND_SEQ_EVENT_NOTEOFF: tsf_channel_note_off(f, ch, ev->data.note.note); break;
            case SND_SEQ_EVENT_PGMCHANGE: tsf_channel_set_presetnumber(f, ev->data.control.channel & 15, ev->data.control.value & 127, (ev->data.control.channel & 15) == 9); break;
            case SND_SEQ_EVENT_CONTROLLER: {
                int c = ev->data.control.channel & 15, p = ev->data.control.param, v = ev->data.control.value;
                if (p == 120) tsf_channel_sounds_off_all(f, c);
                else if (p == 123) tsf_channel_note_off_all(f, c);
                else tsf_channel_midi_control(f, c, p, v);
                break; }
            case SND_SEQ_EVENT_PITCHBEND: tsf_channel_set_pitchwheel(f, ev->data.control.channel & 15, ev->data.control.value + 8192); break;
            case SND_SEQ_EVENT_RESET: tsf_reset(f); for (int c = 0; c < 16; c++) tsf_channel_set_presetnumber(f, c, 0, c == 9); break;
            default: break;
            }
            if (verbose && ev->type != SND_SEQ_EVENT_CLOCK) fprintf(stderr, "midisynth: event type %d ch %d\n", ev->type, ch);
        }
        tsf_render_short(f, buf, FRAMES, 0);
        snd_pcm_sframes_t n = snd_pcm_writei(pcm, buf, FRAMES);
        if (n < 0) n = snd_pcm_recover(pcm, (int)n, 1);
        if (n < 0) { fprintf(stderr, "midisynth: pcm write error: %s\n", snd_strerror((int)n)); usleep(20000); }
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        double now = t.tv_sec + t.tv_nsec / 1e9;
        if (now - last_log > 30) { last_log = now; fprintf(stderr, "midisynth: %ld events, %ld notes, %d voices\n", events, notes, tsf_active_voice_count(f)); }
    }
    snd_pcm_drain(pcm); snd_pcm_close(pcm); snd_seq_close(seq); tsf_close(f);
    return 0;
}
