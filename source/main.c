// MP3 player for Nintendo DS / DSi homebrew.
//
// Music goes in /music on the SD card. Album subfolders are fine
// (for example /music/Some Album/01 Song.mp3).
//
// Bottom screen: song list.   Top screen: title, artist, album, etc.
//
// Controls:  UP/DOWN = choose   A = play   X = pause/resume
//            B = stop   L = previous   R = next   START = quit
 
#include <nds.h>
#include <fat.h>
#include <maxmod9.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
 
// --- MP3 decoder (single header library, downloaded by the build) ---
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"
 
#define MAX_TRACKS 500
#define NAME_LEN   128
#define MAX_DEPTH  4
#define LIST_ROWS  19
#define MAX_JUNK   (256 * 1024)   // give up on a file after this many unreadable bytes
 
// ---------------------------------------------------------------
// Log file: if the app ever freezes, the last line in /dsmp3_log.txt on the
// SD card shows how far it got.
// ---------------------------------------------------------------
static bool log_started = false;
 
static void log_msg(const char *msg) {
    FILE *lf = fopen("/dsmp3_log.txt", log_started ? "a" : "w");
    if (!lf) return;
    log_started = true;
    fputs(msg, lf);
    fputc('\n', lf);
    fclose(lf);
}
 
static void log_num(const char *msg, int n) {
    char line[96];
    snprintf(line, sizeof(line), "%s %d", msg, n);
    log_msg(line);
}
 
// ---------------------------------------------------------------
// Two text screens
// ---------------------------------------------------------------
static PrintConsole topConsole;
static PrintConsole bottomConsole;
 
// ---------------------------------------------------------------
// Track list (found by searching /music and all folders inside it)
// ---------------------------------------------------------------
static char names[MAX_TRACKS][NAME_LEN];   // path relative to the music folder
static int  track_count = 0;
 
static int compare_names(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}
 
// Places we try, in order. Which one works depends on how the app was launched.
static const char *candidates[] = { "/music", "sd:/music", "fat:/music" };
#define NUM_CANDIDATES 3
static bool opened[NUM_CANDIDATES];   // did opendir() work for this path?
static int  seen[NUM_CANDIDATES];     // how many files/folders were found in it
static char music_dir[32] = "/music"; // the path that ended up working
static int  seen_total = 0;
static bool truncated = false;        // more than MAX_TRACKS songs on the card
 
static void scan_dir(const char *rel, int depth) {
    char dirpath[260];
    if (rel[0]) snprintf(dirpath, sizeof(dirpath), "%s/%s", music_dir, rel);
    else        snprintf(dirpath, sizeof(dirpath), "%s", music_dir);
 
    DIR *d = opendir(dirpath);
    if (!d) return;
 
    struct dirent *e;
    while ((e = readdir(d)) != NULL && track_count < MAX_TRACKS) {
        const char *name = e->d_name;
        if (name[0] == '.') continue;          // skips ".", ".." and hidden items
        seen_total++;
 
        if (strlen(rel) + 1 + strlen(name) >= NAME_LEN) continue;  // path too long
 
        char relpath[NAME_LEN];
        if (rel[0]) snprintf(relpath, sizeof(relpath), "%s/%s", rel, name);
        else        snprintf(relpath, sizeof(relpath), "%s", name);
 
        bool is_dir;
        if (e->d_type == DT_DIR)      is_dir = true;
        else if (e->d_type == DT_REG) is_dir = false;
        else {
            char full[260];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", dirpath, name);
            if (stat(full, &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode);
        }
 
        if (is_dir) {
            if (depth < MAX_DEPTH) scan_dir(relpath, depth + 1);
        } else {
            size_t n = strlen(name);
            if (n > 4 && strcasecmp(name + n - 4, ".mp3") == 0) {
                strcpy(names[track_count], relpath);
                track_count++;
            }
        }
    }
    closedir(d);
    if (track_count >= MAX_TRACKS) truncated = true;
}
 
static void scan_music(void) {
    for (int c = 0; c < NUM_CANDIDATES && track_count == 0; c++) {
        DIR *d = opendir(candidates[c]);
        if (!d) continue;
        closedir(d);
        opened[c] = true;
 
        strncpy(music_dir, candidates[c], sizeof(music_dir) - 1);
        music_dir[sizeof(music_dir) - 1] = '\0';
 
        seen_total = 0;
        scan_dir("", 0);
        seen[c] = seen_total;
    }
    qsort(names, track_count, NAME_LEN, compare_names);
}
 
// ---------------------------------------------------------------
// ID3 tags (title, artist, album...)
// ---------------------------------------------------------------
typedef struct {
    char title[96];
    char artist[96];
    char album_artist[96];
    char album[96];
    char date[16];
    char track[16];
} Meta;
 
static Meta meta;
 
static unsigned syncsafe(const unsigned char *b) {
    return ((unsigned)b[0] << 21) | ((unsigned)b[1] << 14) | ((unsigned)b[2] << 7) | b[3];
}
static unsigned be32(const unsigned char *b) {
    return ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) | ((unsigned)b[2] << 8) | b[3];
}
 
// The DS text screen can only draw plain English letters, digits and
// punctuation. Accented letters are turned into the plain letter
// (é -> e), curly quotes into straight ones, and anything else into '?'.
static char fold(unsigned code) {
    static const char latin1[] =
        "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"    // U+00C0 - U+00DF
        "aaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00E0 - U+00FF
    if (code >= 32 && code < 127) return (char)code;
    if (code >= 0xC0 && code <= 0xFF) return latin1[code - 0xC0];
    if (code == 0xA0) return ' ';
    if (code == 0x2018 || code == 0x2019) return '\'';
    if (code == 0x201C || code == 0x201D) return '"';
    if (code == 0x2013 || code == 0x2014) return '-';
    return '?';
}
 
// Convert tag text (in any of the ID3 encodings) into ASCII.
static void to_ascii(char *dst, int dstsize, const unsigned char *src, int len, int enc) {
    int o = 0;
    int i = 0;
    bool little = true;
 
    if (enc == 1 && len >= 2) {                       // UTF-16 with byte order mark
        if (src[0] == 0xFF && src[1] == 0xFE)      { little = true;  i = 2; }
        else if (src[0] == 0xFE && src[1] == 0xFF) { little = false; i = 2; }
    } else if (enc == 2) {
        little = false;
    }
 
    if (enc == 1 || enc == 2) {
        for (; i + 1 < len && o < dstsize - 1; i += 2) {
            unsigned code = little ? (src[i] | (src[i + 1] << 8))
                                   : ((src[i] << 8) | src[i + 1]);
            if (code == 0) break;
            if (code < 32) continue;
            dst[o++] = fold(code);
        }
    } else if (enc == 3) {                            // UTF-8
        for (; i < len && o < dstsize - 1; i++) {
            unsigned char b = src[i];
            unsigned code;
            int extra;
            if (b == 0) break;
            if (b < 32) continue;
            if (b < 0x80)       { code = b;        extra = 0; }
            else if (b >= 0xF0) { code = 0xFFFD;   extra = 3; }
            else if (b >= 0xE0) { code = b & 0x0F; extra = 2; }
            else if (b >= 0xC0) { code = b & 0x1F; extra = 1; }
            else continue;                            // stray continuation byte
            for (int k = 0; k < extra && i + 1 < len && (src[i + 1] & 0xC0) == 0x80; k++) {
                i++;
                code = (code << 6) | (src[i] & 0x3F);
            }
            if (b >= 0xF0) code = 0xFFFD;
            dst[o++] = fold(code);
        }
    } else {                                          // ISO-8859-1
        for (; i < len && o < dstsize - 1; i++) {
            unsigned char b = src[i];
            if (b == 0) break;
            if (b < 32) continue;
            dst[o++] = fold(b);
        }
    }
    dst[o] = '\0';
}
 
// Read one text frame of 'size' bytes from the file into dst.
static void read_text_frame(FILE *f, unsigned size, char *dst, int dstsize) {
    unsigned char buf[200];
    if (size < 2) return;
    unsigned want = (size > sizeof(buf)) ? (unsigned)sizeof(buf) : size;
    if (fread(buf, 1, want, f) != want) return;
    to_ascii(dst, dstsize, buf + 1, (int)want - 1, buf[0]);
}
 
// Reads the ID3v2 tag at the start of the file into 'meta'.
// Returns the file position where the actual audio starts.
static long read_id3(FILE *f) {
    unsigned char h[10];
    memset(&meta, 0, sizeof(meta));
 
    fseek(f, 0, SEEK_SET);
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
 
    int ver = h[3];
    unsigned flags = h[5];
    unsigned tagsize = syncsafe(h + 6);
    long tag_end = 10 + (long)tagsize;
    long audio_start = tag_end + ((flags & 0x10) ? 10 : 0);   // footer
 
    if (ver < 2 || ver > 4) return audio_start;
 
    long pos = 10;
    if ((flags & 0x40) && ver >= 3) {                          // extended header
        unsigned char eh[4];
        if (fread(eh, 1, 4, f) != 4) return audio_start;
        pos = 10 + ((ver == 4) ? (long)syncsafe(eh) : (long)be32(eh) + 4);
    }
 
    int idlen = (ver == 2) ? 3 : 4;
    int hlen  = (ver == 2) ? 6 : 10;
 
    while (pos + hlen <= tag_end) {
        unsigned char fh[10];
        fseek(f, pos, SEEK_SET);
        if (fread(fh, 1, hlen, f) != (size_t)hlen) break;
        if (fh[0] == 0) break;                                 // padding: no more frames
 
        unsigned fsize;
        if (ver == 2)      fsize = ((unsigned)fh[3] << 16) | ((unsigned)fh[4] << 8) | fh[5];
        else if (ver == 3) fsize = be32(fh + 4);
        else               fsize = syncsafe(fh + 4);
 
        if (fsize > tagsize) break;                            // corrupt frame size
        long next = pos + hlen + (long)fsize;
        if (next > tag_end || next <= pos) break;
 
        char id[5];
        memcpy(id, fh, idlen);
        id[idlen] = '\0';
 
        char *dst = NULL;
        int dsz = 0;
        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2"))      { dst = meta.title;        dsz = sizeof(meta.title); }
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) { dst = meta.artist;       dsz = sizeof(meta.artist); }
        else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) { dst = meta.album_artist; dsz = sizeof(meta.album_artist); }
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) { dst = meta.album;        dsz = sizeof(meta.album); }
        else if (!strcmp(id, "TDRL") || !strcmp(id, "TDRC") ||
                 !strcmp(id, "TYER") || !strcmp(id, "TYE")) { dst = meta.date;         dsz = sizeof(meta.date); }
        else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) { dst = meta.track;        dsz = sizeof(meta.track); }
 
        if (dst) read_text_frame(f, fsize, dst, dsz);
        // (the album cover is in an "APIC" frame; it is skipped for now)
 
        pos = next;
    }
    return audio_start;
}
 
// ---------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------
static mp3dec_t dec;
static FILE *mp3file = NULL;
 
static unsigned char inbuf[16 * 1024];   // raw MP3 bytes read from the SD card
static int  inbytes = 0;                 // how many valid bytes are in inbuf
static int  inpos   = 0;                 // where the next frame starts
static bool eof     = false;
 
static short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];  // one decoded frame
static int  pcm_count = 0;               // shorts in pcm
static int  pcm_pos   = 0;               // shorts already sent to the speaker
static int  channels  = 2;
static int  hz        = 44100;
static int  kbps      = 0;
 
static bool playing  = false;
static bool paused   = false;
static bool finished = false;
static int  current  = -1;
 
// Top up inbuf from the file.
static void refill(void) {
    if (inpos > 0 && inbytes > 0) memmove(inbuf, inbuf + inpos, inbytes);
    inpos = 0;
    if (!eof && inbytes < (int)sizeof(inbuf)) {
        size_t n = fread(inbuf + inbytes, 1, sizeof(inbuf) - inbytes, mp3file);
        if (n == 0) eof = true;
        inbytes += (int)n;
    }
}
 
// Decode the next MP3 frame into pcm[]. Returns false at end of file
// (or when the rest of the file is unreadable).
static bool decode_next(void) {
    mp3dec_frame_info_t info;
    int junk = 0;
    for (;;) {
        if (inbytes < 4096 && !eof) refill();
        if (inbytes <= 0) return false;
 
        int samples = mp3dec_decode_frame(&dec, inbuf + inpos, inbytes, pcm, &info);
        inpos   += info.frame_bytes;
        inbytes -= info.frame_bytes;
 
        if (samples > 0) {
            if (channels == 0) {              // first frame decides the stream format
                channels = info.channels;
                hz       = info.hz;
                kbps     = info.bitrate_kbps;
            }
            // if a later frame has a different channel count, convert it
            // so it always matches the open stream
            if (info.channels == 1 && channels == 2) {
                for (int k = samples - 1; k >= 0; k--) {
                    pcm[2 * k]     = pcm[k];
                    pcm[2 * k + 1] = pcm[k];
                }
            } else if (info.channels == 2 && channels == 1) {
                for (int k = 0; k < samples; k++)
                    pcm[k] = (short)(((int)pcm[2 * k] + (int)pcm[2 * k + 1]) / 2);
            }
            pcm_count = samples * channels;
            pcm_pos   = 0;
            return true;
        }
 
        junk += info.frame_bytes;
        if (info.frame_bytes == 0) {
            if (eof) return false;
            inpos++;       // no valid frame found: skip a byte and retry
            inbytes--;
            junk++;
        }
        if (junk > MAX_JUNK) return false;    // not an MP3 after all
    }
}
 
// Maxmod calls this whenever the speaker needs more audio.
// 'length' is the number of samples (per channel) wanted.
static mm_word on_stream(mm_word length, mm_addr dest, mm_stream_formats format) {
    short *out = (short *)dest;
    int ch = channels ? channels : 2;
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
 
static char status_msg[64];          // shown on the top screen when something goes wrong
static bool log_first_update = false;
 
static void stop_track(void) {
    if (playing)  { mmStreamClose(); playing = false; }
    if (mp3file)  { fclose(mp3file); mp3file = NULL; }
    paused = false;
    finished = false;
    current = -1;
}
 
static bool start_track(int index) {
    stop_track();
    status_msg[0] = '\0';
    log_msg(names[index]);
 
    char path[NAME_LEN + 48];
    snprintf(path, sizeof(path), "%s/%s", music_dir, names[index]);
 
    mp3file = fopen(path, "rb");
    if (!mp3file) {
        snprintf(status_msg, sizeof(status_msg), "Can't open that file");
        log_msg("  fopen failed");
        return false;
    }
    log_msg("  file opened");
 
    // read the tag, then jump to where the audio begins
    long audio_start = read_id3(mp3file);
    fseek(mp3file, audio_start, SEEK_SET);
    log_msg("  tag read");
 
    mp3dec_init(&dec);
    inbytes = inpos = 0;
    eof = false;
    pcm_count = pcm_pos = 0;
    channels = 0;                       // decode_next fills this in
 
    // decode one frame first so we know the sample rate and channel count
    if (!decode_next()) {
        fclose(mp3file);
        mp3file = NULL;
        snprintf(status_msg, sizeof(status_msg), "Not a playable MP3");
        log_msg("  first frame failed");
        return false;
    }
    pcm_pos = 0;   // play that first frame too
    log_num("  first frame decoded, hz =", hz);
 
    if (hz < 8000 || hz > 48000) hz = 44100;
 
    mm_stream s;
    s.sampling_rate = hz;
    s.buffer_length = 8192;     // bigger buffer = fewer stutters
    s.callback      = on_stream;
    s.format        = (channels == 1) ? MM_STREAM_16BIT_MONO : MM_STREAM_16BIT_STEREO;
    s.timer         = MM_TIMER0;
    s.manual        = true;     // we call mmStreamUpdate() ourselves
    mmStreamOpen(&s);
    log_msg("  stream opened");
 
    playing = true;
    current = index;
    log_first_update = true;
    return true;
}
 
// Try to play 'index'. If that file is broken, move on to the next few.
static bool play_from(int index, int step) {
    for (int tries = 0; tries < 8; tries++) {
        if (start_track(index)) return true;
        index += step;
        if (index >= track_count) index = 0;
        if (index < 0) index = track_count - 1;
    }
    return false;
}
 
// ---------------------------------------------------------------
// Screens
// ---------------------------------------------------------------
static const char *dash(const char *s) {
    return (s && s[0]) ? s : "-";
}
 
// Replace anything the DS text screen can't draw with '?'
static void sanitize(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 32 || (unsigned char)*s > 126) *s = '?';
}
 
static void show_field(const char *label, const char *value) {
    iprintf("%s\n", label);
    iprintf(" %.30s\n", dash(value));
    if (value && strlen(value) > 30) iprintf(" %.30s\n", value + 30);
}
 
// File name without folder and without ".mp3"
static void base_name(const char *path, char *out, int outsize) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    snprintf(out, outsize, "%s", base);
    size_t n = strlen(out);
    if (n > 4 && strcasecmp(out + n - 4, ".mp3") == 0) out[n - 4] = '\0';
    sanitize(out);
}
 
// Top screen: what is playing
static void draw_top(void) {
    consoleSelect(&topConsole);
    iprintf("\x1b[2J");
 
    if (current < 0) {
        iprintf("\n\n\n\n\n\n\n     Nothing playing\n");
        if (status_msg[0]) iprintf("\n %.30s\n", status_msg);
    } else {
        char fallback[NAME_LEN];
        base_name(names[current], fallback, sizeof(fallback));
 
        iprintf(paused ? "== PAUSED ==\n\n" : "NOW PLAYING\n\n");
        show_field("Title", meta.title[0] ? meta.title : fallback);
        show_field("Artist", meta.artist);
        show_field("Album", meta.album);
        show_field("Album artist", meta.album_artist);
        iprintf("Date: %.10s  Track: %.5s\n", dash(meta.date), dash(meta.track));
        iprintf("%d Hz %s %d kbps\n", hz, channels == 1 ? "mono" : "stereo", kbps);
    }
    consoleSelect(&bottomConsole);
}
 
// Bottom screen: the list, shown as "Album/Song"
static void list_label(int i, char *out, int outsize) {
    char base[NAME_LEN];
    base_name(names[i], base, sizeof(base));
 
    const char *last = strrchr(names[i], '/');
    if (!last) {
        snprintf(out, outsize, "%.27s", base);
    } else {
        // the folder just above the file
        const char *start = last;
        while (start > names[i] && *(start - 1) != '/') start--;
        int len = (int)(last - start);
        if (len > NAME_LEN - 1) len = NAME_LEN - 1;
 
        char folder[NAME_LEN];
        memcpy(folder, start, len);
        folder[len] = '\0';
        sanitize(folder);
        snprintf(out, outsize, "%.9s/%.17s", folder, base);
    }
    sanitize(out);
}
 
static void draw_bottom(int sel) {
    consoleSelect(&bottomConsole);
    iprintf("\x1b[2J");
    iprintf("DS MP3 player (%d%s tracks)\n", track_count, truncated ? "+" : "");
    iprintf("A play X pause B stop L/R skip\n\n");
 
    if (track_count == 0) {
        iprintf("No MP3s found.\n");
        iprintf("Put them in /music on the SD card.\n\n");
        for (int c = 0; c < NUM_CANDIDATES; c++) {
            if (opened[c]) iprintf("%s: opened, %d items\n", candidates[c], seen[c]);
            else           iprintf("%s: not found\n", candidates[c]);
        }
        char cwd[64];
        if (getcwd(cwd, sizeof(cwd))) iprintf("\nRunning from:\n%.60s\n", cwd);
        return;
    }
 
    int first = sel - LIST_ROWS / 2;
    if (first > track_count - LIST_ROWS) first = track_count - LIST_ROWS;
    if (first < 0) first = 0;
 
    for (int i = first; i < track_count && i < first + LIST_ROWS; i++) {
        char label[64];
        list_label(i, label, sizeof(label));
        char mark = (i == sel) ? '>' : ' ';
        char play = (i == current) ? (paused ? '=' : '*') : ' ';
        iprintf("%c%c%.28s\n", mark, play, label);
    }
}
 
// ---------------------------------------------------------------
// Main
// ---------------------------------------------------------------
int main(void) {
    // set up the two text screens
    lcdMainOnTop();
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&topConsole,    3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true,  true);
    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
    consoleSelect(&bottomConsole);
 
    if (!fatInitDefault()) {
        iprintf("Could not read the SD card.\n");
        while (pmMainLoop()) swiWaitForVBlank();
        return 0;
    }
    log_msg("app started, SD card ok");
 
    mm_ds_system sys;
    sys.mod_count   = 0;
    sys.samp_count  = 0;
    sys.mem_bank    = 0;
    mmInit(&sys);
    log_msg("audio system started");
 
    iprintf("Scanning music...\n");
    scan_music();
    log_num("scan finished, tracks:", track_count);
 
    int sel = 0;
    draw_top();
    draw_bottom(sel);
 
    while (pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        int down = keysDown();
        bool redraw_bottom = false;
        bool redraw_top = false;
 
        if (down & KEY_START) break;
 
        if ((down & KEY_DOWN) && sel < track_count - 1) { sel++; redraw_bottom = true; }
        if ((down & KEY_UP)   && sel > 0)               { sel--; redraw_bottom = true; }
 
        if ((down & KEY_A) && track_count > 0) {
            start_track(sel);
            redraw_bottom = redraw_top = true;
        }
        if ((down & KEY_X) && playing) {
            paused = !paused;
            redraw_bottom = redraw_top = true;
        }
        if (down & KEY_B) {
            stop_track();
            status_msg[0] = '\0';
            redraw_bottom = redraw_top = true;
        }
        if ((down & (KEY_R | KEY_L)) && track_count > 0 && playing) {
            int step = (down & KEY_R) ? 1 : -1;
            int next = current + step;
            if (next >= track_count) next = 0;
            if (next < 0) next = track_count - 1;
            play_from(next, step);
            if (current >= 0) sel = current;
            redraw_bottom = redraw_top = true;
        }
 
        if (playing) {
            mmStreamUpdate();
            if (log_first_update) {
                log_msg("  first stream update ok");
                log_first_update = false;
            }
            if (finished) {           // song ended: go to the next one
                int next = current + 1;
                if (next < track_count && play_from(next, 1)) sel = current;
                else stop_track();
                redraw_bottom = redraw_top = true;
            }
        }
 
        if (redraw_top)    draw_top();
        if (redraw_bottom) draw_bottom(sel);
    }
 
    stop_track();
    return 0;
}
 
