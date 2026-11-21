// Minimal MP3 player for Nintendo DS / DSi homebrew.
// Put MP3s in /music on your SD card.
//
// Controls:  UP/DOWN = choose   A = play   X = pause/resume
//            B = stop           R = next   START = quit
 
#include <nds.h>
#include <fat.h>
#include <maxmod9.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
 
// --- MP3 decoder (single header library, see README) ---
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
 
#define MAX_TRACKS 200
#define NAME_LEN   128
#define LIST_ROWS  20
 
// ---------------------------------------------------------------
// Track list
// ---------------------------------------------------------------
static char names[MAX_TRACKS][NAME_LEN];
static int  track_count = 0;
 
static int compare_names(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}
 
static void scan_music(void) {
    DIR *d = opendir("/music");
    if (!d) return;
 
    struct dirent *e;
    while ((e = readdir(d)) != NULL && track_count < MAX_TRACKS) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".mp3") == 0) {
            strncpy(names[track_count], e->d_name, NAME_LEN - 1);
            names[track_count][NAME_LEN - 1] = '\0';
            track_count++;
        }
    }
    closedir(d);
 
    qsort(names, track_count, NAME_LEN, compare_names);
}
 
// ---------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------
static mp3dec_t dec;
static FILE *fp = NULL;
 
static unsigned char inbuf[16 * 1024];   // raw MP3 bytes read from the SD card
static int  inbytes = 0;                 // how many valid bytes are in inbuf
static int  inpos   = 0;                 // where the next frame starts
static bool eof     = false;
 
static short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];  // one decoded frame
static int  pcm_count = 0;               // shorts in pcm
static int  pcm_pos   = 0;               // shorts already sent to the speaker
static int  channels  = 2;
static int  hz        = 44100;
 
static bool playing  = false;
static bool paused   = false;
static bool finished = false;
static int  current  = -1;
 
// Top up inbuf from the file.
static void refill(void) {
    if (inpos > 0 && inbytes > 0) memmove(inbuf, inbuf + inpos, inbytes);
    inpos = 0;
    if (!eof && inbytes < (int)sizeof(inbuf)) {
        size_t n = fread(inbuf + inbytes, 1, sizeof(inbuf) - inbytes, fp);
        if (n == 0) eof = true;
        inbytes += (int)n;
    }
}
 
// Decode the next MP3 frame into pcm[]. Returns false at end of file.
static bool decode_next(void) {
    mp3dec_frame_info_t info;
    for (;;) {
        if (inbytes < 4096 && !eof) refill();
        if (inbytes <= 0) return false;
 
        int samples = mp3dec_decode_frame(&dec, inbuf + inpos, inbytes, pcm, &info);
        inpos   += info.frame_bytes;
        inbytes -= info.frame_bytes;
 
        if (samples > 0) {
            channels  = info.channels;
            hz        = info.hz;
            pcm_count = samples * info.channels;
            pcm_pos   = 0;
            return true;
        }
        if (info.frame_bytes == 0) {
            if (eof) return false;
            inpos++;       // no valid frame found: skip a byte and retry
            inbytes--;
        }
    }
}
 
// Maxmod calls this whenever the speaker needs more audio.
// 'length' is the number of samples (per channel) wanted.
static mm_word on_stream(mm_word length, mm_addr dest, mm_stream_formats format) {
    short *out = (short *)dest;
    int ch = channels;
    mm_word done = 0;
 
    if (!paused) {
        while (done < length) {
            if (pcm_pos >= pcm_count) {
                if (!decode_next()) { finished = true; break; }
            }
            int avail = (pcm_count - pcm_pos) / ch;
            int n = (int)(length - done);
            if (n > avail) n = avail;
            memcpy(out + done * ch, pcm + pcm_pos, n * ch * sizeof(short));
            pcm_pos += n * ch;
            done    += n;
        }
    }
    // anything we couldn't fill becomes silence
    memset(out + done * ch, 0, (length - done) * ch * sizeof(short));
    return length;
}
 
static void stop_track(void) {
    if (playing) { mmStreamClose(); playing = false; }
    if (fp)      { fclose(fp); fp = NULL; }
    paused = false;
    finished = false;
    current = -1;
}
 
static bool start_track(int index) {
    stop_track();
 
    char path[NAME_LEN + 16];
    snprintf(path, sizeof(path), "/music/%s", names[index]);
 
    fp = fopen(path, "rb");
    if (!fp) return false;
 
    mp3dec_init(&dec);
    inbytes = inpos = 0;
    eof = false;
    pcm_count = pcm_pos = 0;
 
    // decode one frame first so we know the sample rate and channel count
    if (!decode_next()) { fclose(fp); fp = NULL; return false; }
    pcm_pos = 0;   // play that first frame too
 
    mm_stream s;
    s.sampling_rate = hz;
    s.buffer_length = 4096;
    s.callback      = on_stream;
    s.format        = (channels == 1) ? MM_STREAM_16BIT_MONO : MM_STREAM_16BIT_STEREO;
    s.timer         = MM_TIMER0;
    s.manual        = true;     // we call mmStreamUpdate() ourselves
    mmStreamOpen(&s);
 
    playing = true;
    current = index;
    return true;
}
 
// ---------------------------------------------------------------
// Screen
// ---------------------------------------------------------------
static void draw(int sel) {
    iprintf("\x1b[2J");  // clear
    iprintf("DS MP3 player  (%d tracks)\n", track_count);
    iprintf("A play X pause B stop START quit\n\n");
 
    if (track_count == 0) {
        iprintf("No MP3s found.\nPut them in /music on the SD card.\n");
        return;
    }
 
    int first = sel - LIST_ROWS / 2;
    if (first < 0) first = 0;
    if (first > track_count - LIST_ROWS) first = track_count - LIST_ROWS;
    if (first < 0) first = 0;
 
    for (int i = first; i < track_count && i < first + LIST_ROWS; i++) {
        char mark = (i == sel) ? '>' : ' ';
        char play = (i == current) ? (paused ? '=' : '*') : ' ';
        iprintf("%c%c %.28s\n", mark, play, names[i]);
    }
}
 
// ---------------------------------------------------------------
// Main
// ---------------------------------------------------------------
int main(void) {
    consoleDemoInit();
 
    if (!fatInitDefault()) {
        iprintf("Could not read the SD card.\n");
        while (1) swiWaitForVBlank();
    }
 
    mm_ds_system sys;
    sys.mod_count   = 0;
    sys.samp_count  = 0;
    sys.mem_bank    = 0;
    sys.fifo_channel = FIFO_MAXMOD;
    mmInit(&sys);
 
    scan_music();
 
    int sel = 0;
    draw(sel);
 
    while (1) {
        swiWaitForVBlank();
        scanKeys();
        int down = keysDown();
        bool redraw = false;
 
        if (down & KEY_START) break;
 
        if ((down & KEY_DOWN) && sel < track_count - 1) { sel++; redraw = true; }
        if ((down & KEY_UP)   && sel > 0)               { sel--; redraw = true; }
 
        if ((down & KEY_A) && track_count > 0) { start_track(sel); redraw = true; }
        if ((down & KEY_X) && playing)         { paused = !paused; redraw = true; }
        if (down & KEY_B)                      { stop_track(); redraw = true; }
        if ((down & KEY_R) && track_count > 0 && playing) {
            int next = (current + 1) % track_count;
            start_track(next); sel = next; redraw = true;
        }
 
        if (playing) {
            mmStreamUpdate();
            if (finished) {           // song ended: go to the next one
                int next = current + 1;
                if (next < track_count && start_track(next)) sel = next;
                else stop_track();
                redraw = true;
            }
        }
 
        if (redraw) draw(sel);
    }
 
    stop_track();
    return 0;
}
 
