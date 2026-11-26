/* SPDX-License-Identifier: MPL-2.0
 * THEORY-1: music theory for the M-VAVE FM-1. */
/* SONGS: songs you know, chord by chord. A song's sections (verse, chorus...) are walked one chord at a time: its
 * symbol (F#m7), its roman numeral (i7) and what it does in the key ("tension: pulls back home"), with the keys to
 * press lit in an easy hand shape (each chord in the position nearest the one before, the fingers numbered). PLAY
 * plays the song at its tempo, chords and melody, the keys following; ARP makes it wait at each chord until you play
 * it; K1 moves the song to another key (the numerals stay: that is the point), K2 slows it down.
 *
 * The songs come from two banks in tools/songs/t1song.py's format (that file says it all): the built-in songs
 * (songs/builtin, compiled in: songs_builtin.h) and the player's own, loaded from a computer over USB
 * (tools/songs/fm1_songs.py) into SONG_SECTORS sectors of the flash's data area. Every count and offset is checked as
 * it is read: a damaged bank shows fewer songs, never a crash.
 *
 * The loader (app_usb_cmd, SONG_REQ in the update's framing): the host asks with the payload's length and CRC-32;
 * the FM-1 erases the region, pulls the payload with read requests (plat_host_read, as a restore does), checks its
 * CRC-32 as written, then writes the 32-byte header last (the commit record: a cut cable leaves no bank, never half
 * of one), and tells the host by reading SG_OK_ADDR or SG_FAIL_ADDR | code. */
#include "songs_builtin.h"

#define SONG_BASE 0xBC000u                    /* 32 sectors, up to the panel's calibration (0xDC000: app.h) */
#define SONG_SECTORS 32u
#define SONG_HEAD 32u
#define SONG_REQ 0x70u                        /* the host's request (backup.c's 0x41..0x43, res.c's 0x44, the
                                               * mirror's 0x50..0x54) */
#define SG_OK_ADDR 0xC0000000u
#define SG_FAIL_ADDR 0xCF000000u
enum { SG_E_SIZE = 1, SG_E_FETCH = 2, SG_E_HEAD = 3, SG_E_ERASE = 7, SG_E_PROG = 8, SG_E_CHECK = 9 };
_Static_assert(SONG_BASE >= PLAT_DATA_LO && SONG_BASE + SONG_SECTORS * 4096u <= 0xDC000u && !(SONG_BASE & 0xFFFu),
               "the song region: whole sectors of the data area, below the store's objects");

#define SG_MAXSONG 200
#define SG_MAXSEC 40
#define SG_MAXCH 400
#define UPB 12u                               /* units a beat (t1song.py: 40 ticks of 480) */


/* ------------------------------------------------------------ reading --- */
typedef struct {
    const uint8_t *p, *end;
    int bad;
} rd_t;
static uint32_t rd_u8(rd_t *r)
{
    if (r->p >= r->end) {
        r->bad = 1;
        return 0;
    }
    return *r->p++;
}
static uint32_t rd_u16(rd_t *r)
{
    uint32_t v = rd_u8(r);
    return v | rd_u8(r) << 8;
}
static uint32_t rd_u32(rd_t *r)
{
    uint32_t v = rd_u16(r);
    return v | rd_u16(r) << 16;
}
static void rd_str(rd_t *r, char *dst, uint32_t max)   /* (max: the buffer, with its 0) */
{
    uint32_t n = rd_u8(r), i;
    for (i = 0; i < n; i++) {
        uint32_t c = rd_u8(r);
        if (i + 1u < max)
            dst[i] = (char)c;
    }
    dst[n < max ? n : max - 1u] = 0;
}
static void rd_skip(rd_t *r, uint32_t n)
{
    if ((uint32_t)(r->end - r->p) < n) {
        r->bad = 1;
        r->p = r->end;
    } else
        r->p += n;
}

static uint32_t sg_crc32(const uint8_t *p, uint32_t n)   /* zlib's */
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* a bank: its payload and its songs' offsets (checked once: sg_scan) */
typedef struct {
    const uint8_t *pay;
    uint32_t len, n;
} bank_t;
static bank_t sg_bank[2];                     /* 0 the built-in songs, 1 the player's (the flash) */

static void bank_open(bank_t *b, const uint8_t *pay, uint32_t len)
{
    rd_t r = {pay, pay + len, 0};
    uint32_t i, n;
    b->pay = pay, b->len = len, b->n = 0;
    if (!pay || len < 8u || pay[0] != 'T' || pay[1] != '1' || pay[2] != 'S' || pay[3] != 'B')
        return;
    rd_skip(&r, 4);
    if (rd_u16(&r) != 1u)
        return;
    n = rd_u16(&r);
    for (i = 0; i < n && !r.bad; i++)
        if (rd_u32(&r) >= len)
            return;
    if (!r.bad)
        b->n = n > SG_MAXSONG ? SG_MAXSONG : n;
}

/* a region (the 32-byte header, then the payload) -> its payload, if it checks */
static const uint8_t *region_payload(const uint8_t *reg, uint32_t room, uint32_t *len)
{
    rd_t r = {reg, reg + SONG_HEAD, 0};
    uint32_t n, crc;
    *len = 0;
    if (!reg || reg[0] != 'T' || reg[1] != '1' || reg[2] != 'S' || reg[3] != 'G')
        return 0;
    rd_skip(&r, 4);
    n = rd_u32(&r), crc = rd_u32(&r);
    if (n == 0 || n > room - SONG_HEAD || sg_crc32(reg, 28) != (reg[28] | reg[29] << 8 | reg[30] << 16 |
                                                               (uint32_t)reg[31] << 24) ||
        sg_crc32(reg + SONG_HEAD, n) != crc)
        return 0;
    *len = n;
    return reg + SONG_HEAD;
}

static void sg_scan(void)
{
    uint32_t len;
    const uint8_t *p = region_payload(SONGS_BUILTIN, sizeof SONGS_BUILTIN, &len);
    bank_open(&sg_bank[0], p, len);
    p = region_payload(plat_data(SONG_BASE), SONG_SECTORS * 4096u, &len);
    bank_open(&sg_bank[1], p, len);
}
static int sg_count(void) { return (int)(sg_bank[0].n + sg_bank[1].n); }

/* song i of both banks (the player's first): a reader at its start */
static rd_t sg_song(int i)
{
    rd_t r = {0, 0, 1};
    const bank_t *b = i < (int)sg_bank[1].n ? &sg_bank[1] : &sg_bank[0];
    uint32_t k = i < (int)sg_bank[1].n ? (uint32_t)i : (uint32_t)i - sg_bank[1].n, off;
    if (k >= b->n)
        return r;
    off = b->pay[8 + 4 * k] | b->pay[9 + 4 * k] << 8 | b->pay[10 + 4 * k] << 16 | (uint32_t)b->pay[11 + 4 * k] << 24;
    r.p = b->pay + off, r.end = b->pay + b->len, r.bad = 0;
    return r;
}

/* -------------------------------------------------------------- state --- */
typedef struct {                              /* a chord, read */
    uint16_t start, len;
    uint8_t root_st, root_semi, bass_st, bass_semi, fn, n;
    uint8_t tone[8];
    char sfx[24], num[24];
} sch_t;

static struct {
    uint8_t sel;                              /* the list's choice */
    int16_t cur;                              /* the song open */
    char title[49], artist[41];
    uint8_t nsec, sec;
    const uint8_t *secp[SG_MAXSEC];           /* each section's start */
    const uint8_t *end;                       /* the bank's end */
    /* the section open */
    char name[25], mode[24];
    nt_t tonic0;                              /* its key as written */
    uint16_t bpm, from, to, nch, nnt;
    uint8_t beats, unit;
    uint16_t choff[SG_MAXCH];                 /* each chord's offset from the section's start */
    const uint8_t *notes;
    int8_t mel_shift;                         /* the melody an octave up, or not, above the chords */
    int ch;                                   /* the chord shown */
    /* the player's choices */
    int8_t key;                               /* -1 the song's own key, else COMMON_*_ROOTS index */
    uint8_t tempo;                            /* % of the song's */
    uint8_t melody, wait;
    /* playing */
    uint8_t playing, waiting;
    uint16_t want, got;                       /* waiting: the chord's pitch classes, those played */
    uint32_t t0, u0;                          /* at t0 the song was at unit u0 */
    int next_ch, next_nt;
    uint8_t cnt[128];                         /* notes sounding (a count: a chord and the melody may share one) */
    struct { uint8_t note; uint16_t end_sec; uint32_t end; } vo[16];
    uint8_t voice[6], nvoice;                 /* the chord's hand shape (MIDI, low to high), for the keys */
    uint8_t last[6], nlast;
} sg = {.cur = -1, .key = -1, .tempo = 100, .melody = 1};

static int sg_minor(void)
{
    return sg.mode[0] == 'm' && sg.mode[1] == 'i' ? 1 :                         /* minor, minor... */
           sg.mode[0] == 'd' || (sg.mode[0] == 'l' && sg.mode[1] == 'o') ||     /* dorian, locrian */
           (sg.mode[0] == 'p' && sg.mode[8] != ' ') || sg.mode[0] == 'h' || sg.mode[0] == 'a';
}
static nt_t sg_tonic(void)
{
    if (sg.key < 0)
        return sg.tonic0;
    return ROOTS[sg_minor() ? COMMON_MINOR_ROOTS[sg.key] : COMMON_MAJOR_ROOTS[sg.key]];
}

static void sg_chord(int i, sch_t *c)
{
    rd_t r;
    uint32_t k, b;
    if (i < 0 || i >= sg.nch) {               /* none (a section with only a melody): a rest over all of it */
        uint32_t z;
        for (z = 0; z < sizeof *c; z++)
            ((uint8_t *)c)[z] = 0;
        c->start = sg.from, c->len = (uint16_t)(sg.to - sg.from), c->fn = FN_REST;
        return;
    }
    r.p = sg.secp[sg.sec] + sg.choff[i], r.end = sg.end, r.bad = 0;
    c->start = (uint16_t)rd_u16(&r);
    c->len = (uint16_t)rd_u16(&r);
    b = rd_u8(&r);
    c->root_st = (uint8_t)(b >> 4 & 7u), c->root_semi = (uint8_t)(b & 15u);
    b = rd_u8(&r);
    c->bass_st = (uint8_t)(b >> 4 & 7u), c->bass_semi = (uint8_t)(b & 15u);
    c->fn = (uint8_t)rd_u8(&r);
    c->n = (uint8_t)rd_u8(&r);
    for (k = 0; k < c->n; k++) {
        b = rd_u8(&r);
        if (k < 8u)
            c->tone[k] = (uint8_t)(b % 12u);
    }
    if (c->n > 8u)
        c->n = 8;
    rd_str(&r, c->sfx, sizeof c->sfx);
    rd_str(&r, c->num, sizeof c->num);
    if (c->fn >= NFN)
        c->fn = FN_COLOR;
}

/* the section's chords and notes, found (and checked) */
static void sg_section(int s)
{
    rd_t r;
    uint32_t i, sum = 0, n = 0;
    sg.sec = (uint8_t)s;
    r.p = sg.secp[s], r.end = sg.end, r.bad = 0;
    rd_str(&r, sg.name, sizeof sg.name);
    sg.tonic0.letter = (int8_t)(rd_u8(&r) % 7u);
    sg.tonic0.acc = (int8_t)rd_u8(&r);
    if (sg.tonic0.acc < -2 || sg.tonic0.acc > 2)
        sg.tonic0.acc = 0;
    rd_str(&r, sg.mode, sizeof sg.mode);
    sg.bpm = (uint16_t)rd_u16(&r);
    if (sg.bpm < 20u || sg.bpm > 300u)
        sg.bpm = 100;
    sg.beats = (uint8_t)rd_u8(&r);
    sg.unit = (uint8_t)rd_u8(&r);
    if (!sg.beats || sg.beats > 16u)
        sg.beats = 4;
    if (sg.unit != 2u && sg.unit != 4u && sg.unit != 8u && sg.unit != 16u)
        sg.unit = 4;
    sg.from = (uint16_t)rd_u16(&r);
    sg.to = (uint16_t)rd_u16(&r);
    sg.nch = (uint16_t)rd_u16(&r);
    sg.nnt = (uint16_t)rd_u16(&r);
    if (sg.nch > SG_MAXCH)
        sg.nch = SG_MAXCH;
    for (i = 0; i < sg.nch && !r.bad; i++) {
        uint32_t k;
        sg.choff[i] = (uint16_t)(r.p - sg.secp[s]);
        rd_skip(&r, 7);
        k = rd_u8(&r);
        rd_skip(&r, k);
        rd_skip(&r, rd_u8(&r));
        rd_skip(&r, rd_u8(&r));
    }
    if (r.bad)
        sg.nch = (uint16_t)(i ? i - 1u : 0u);
    sg.notes = r.p;
    if ((uint32_t)(sg.end - r.p) < sg.nnt * 5u)
        sg.nnt = (uint16_t)((uint32_t)(sg.end - r.p) / 5u);
    for (i = 0; i < sg.nnt; i++, n++)         /* the melody: up an octave if it sits low, under the chords */
        sum += (uint32_t)(60 + (int8_t)sg.notes[i * 5u + 4u] + 60);
    sg.mel_shift = (int8_t)(n && sum / n < 60u + 66u ? 12 : 0);
    if (sg.to <= sg.from)
        sg.to = (uint16_t)(sg.from + UPB * sg.beats);
    sg.ch = 0;
    sg.nlast = 0;
}

/* a song opened: its sections found */
static int sg_open(int i)
{
    rd_t r = sg_song(i);
    uint32_t s, n;
    if (r.bad)
        return 0;
    sg.end = r.end;
    rd_str(&r, sg.title, sizeof sg.title);
    rd_str(&r, sg.artist, sizeof sg.artist);
    n = rd_u8(&r);
    for (s = 0; s < n && s < SG_MAXSEC && !r.bad; s++) {
        uint32_t k, nch, nnt;
        sg.secp[s] = r.p;
        rd_skip(&r, rd_u8(&r));                /* name */
        rd_skip(&r, 2);
        rd_skip(&r, rd_u8(&r));                /* mode */
        rd_skip(&r, 8);
        nch = rd_u16(&r);
        nnt = rd_u16(&r);
        for (k = 0; k < nch && !r.bad; k++) {
            rd_skip(&r, 7);
            rd_skip(&r, rd_u8(&r));
            rd_skip(&r, rd_u8(&r));
            rd_skip(&r, rd_u8(&r));
        }
        rd_skip(&r, nnt * 5u);
    }
    if (r.bad && s)
        s--;
    if (!s)
        return 0;
    sg.nsec = (uint8_t)s;
    sg.cur = (int16_t)i;
    sg.key = -1;
    sg_section(0);
    return 1;
}

/* ------------------------------------------------------------ spelling --- */
/* the section's mode in the player's language: the English the file names it in, matched to the lessons' scales and
 * modes ("minor": the natural minor); one not known, as written */
static int sg_same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return 0;
    return !*a && !*b;
}
static const char *sg_mode_text(void)
{
    int m;
    if (sg_same(sg.mode, "minor") || sg_same(sg.mode, "aeolian"))
        return SCALES[SC_NATMINOR].name[th_ui];
    if (sg_same(sg.mode, "ionian"))
        return SCALES[SC_MAJOR].name[th_ui];
    for (m = 0; m < SC_PENTA_MAJ; m++)
        if (sg_same(sg.mode, SCALES[m].name[1]))
            return SCALES[m].name[th_ui];
    for (m = 0; m < NMODES; m++)
        if (sg_same(sg.mode, MODES[m].name[1]))
            return MODES[m].name[th_ui];
    return sg.mode;
}

static void sg_letters(nt_t n, char *b)        /* chord symbols: letters, as on any chart (H in German) */
{
    b[0] = 0;
    if (th_names == NAMES_GERMAN && n.letter == 6 && n.acc >= -1) {
        str_cat(b, n.acc == -1 ? "B" : "H");
        str_cat(b, acc_text(n.acc > 0 ? n.acc : 0));
    } else
        note_name_s(n, b, SYS_LETTERS);
}
static void sg_symbol(const sch_t *c, char *b)
{
    nt_t t = sg_tonic();
    char x[8];
    if (!c->n) {
        t1_strcpy(b, "-");
        return;
    }
    sg_letters(spell(t, c->root_semi, c->root_st), b);
    str_cat(b, c->sfx);
    if (c->bass_semi != c->root_semi || c->bass_st != c->root_st) {
        sg_letters(spell(t, c->bass_semi, c->bass_st), x);
        str_cat(b, "/");
        str_cat(b, x);
    }
}
static uint16_t sg_mask(const sch_t *c)
{
    uint16_t m = 0;
    int k, tp = pc_of(sg_tonic());
    for (k = 0; k < c->n; k++)
        m |= (uint16_t)PCB((c->tone[k] + tp) % 12);
    return m;
}

/* ---------------------------------------------------------- hand shape --- */
/* the chord's notes for the right hand, near the last shape: up to four (a fifth dropped first), stacked from each
 * of them in turn in G3 .. G4, the one moving least from the last chord taken (the first: around D4) */
static void sg_shape(const sch_t *c)
{
    int t[8], n = 0, k, rot, lo, best = 1 << 30, tp = pc_of(sg_tonic());
    uint8_t v[6], bestv[6];
    for (k = 0; k < c->n; k++)
        if (c->n <= 4 || k != 2)
            t[n++] = (c->tone[k] + tp) % 12;
    if (n > 4)
        n = 4;
    for (rot = 0; rot < n; rot++)
        for (lo = 55; lo <= 67; lo++) {
            int i, cost = 0, prev;
            if (lo % 12 != t[rot])
                continue;
            v[0] = (uint8_t)lo;
            for (i = 1, prev = lo; i < n; i++) {
                int p = t[(rot + i) % n], x = prev + 1;
                while (x % 12 != p)
                    x++;
                v[i] = (uint8_t)x;
                prev = x;
            }
            if (v[n - 1] - v[0] > 14)
                continue;
            if (sg.nlast == n)
                for (i = 0; i < n; i++)
                    cost += v[i] > sg.last[i] ? v[i] - sg.last[i] : sg.last[i] - v[i];
            else {
                int mid = 0, ref = 62;
                for (i = 0; i < n; i++)
                    mid += v[i];
                if (sg.nlast) {
                    ref = 0;
                    for (i = 0; i < sg.nlast; i++)
                        ref += sg.last[i];
                    ref /= sg.nlast;
                }
                cost = (mid / n > ref ? mid / n - ref : ref - mid / n) * n;
            }
            cost = cost * 4 + (rot ? 1 : 0);  /* (a tie: root position) */
            if (cost < best) {
                best = cost;
                for (i = 0; i < n; i++)
                    bestv[i] = v[i];
            }
        }
    sg.nvoice = (uint8_t)(best < (1 << 30) ? n : 0);
    for (k = 0; k < sg.nvoice; k++)
        sg.voice[k] = sg.last[k] = bestv[k];
    sg.nlast = sg.nvoice;
}

/* -------------------------------------------------------------- sound --- */
static void sg_on(int note, int vel)
{
    if (note < 0 || note > 127)
        return;
    if (!sg.cnt[note]++)
        synth_note_on(note, vel);
}
static void sg_off(int note)
{
    if (note < 0 || note > 127 || !sg.cnt[note])
        return;
    if (!--sg.cnt[note])
        synth_note_off(note);
}
static void sg_hold(int note, int vel, uint32_t end)   /* a note until unit end of this section */
{
    int i;
    for (i = 0; i < 16; i++)
        if (!sg.vo[i].note) {
            sg.vo[i].note = (uint8_t)note, sg.vo[i].end = end, sg.vo[i].end_sec = sg.sec;
            sg_on(note, vel);
            return;
        }
}
static void sg_release(int all, uint32_t u)
{
    int i;
    for (i = 0; i < 16; i++)
        if (sg.vo[i].note && (all || sg.vo[i].end_sec != sg.sec || sg.vo[i].end <= u)) {
            sg_off(sg.vo[i].note);
            sg.vo[i].note = 0;
        }
}
static void sg_stop(void)
{
    sg_release(1, 0);
    sg.playing = sg.waiting = 0;
}

/* a chord heard: its shape, and its bass an octave and more below */
static void sg_sound(const sch_t *c, uint32_t end)
{
    int k, b;
    sg_shape(c);
    if (!c->n)
        return;
    for (k = 0; k < sg.nvoice; k++)
        sg_hold(sg.voice[k], 62, end);
    b = 40 + (c->bass_semi + pc_of(sg_tonic())) % 12;
    sg_hold(b < 43 ? b + 12 : b, 74, end);
}

/* the unit the song is at now */
static uint32_t sg_unit(void)
{
    uint64_t ms = plat_ms() - sg.t0;
    return sg.u0 + (uint32_t)(ms * sg.bpm * sg.tempo * UPB / 6000000u);
}

/* PLAY: from the chord shown (the section's start, with its lead-in, from its first) */
static void sg_play(void)
{
    sch_t c;
    sg_stop();
    play_stop();
    sg_chord(sg.ch, &c);
    sg.u0 = sg.ch ? c.start : sg.from;
    sg.t0 = plat_ms();
    sg.next_ch = sg.ch;
    sg.next_nt = 0;
    while (sg.next_nt < sg.nnt && (uint32_t)(sg.notes[sg.next_nt * 5] | sg.notes[sg.next_nt * 5 + 1] << 8) < sg.u0)
        sg.next_nt++;
    sg.playing = 1;
}

/* the player: called every pass of the main loop */
static void sg_tick(void)
{
    uint32_t u;
    if (!sg.playing)
        return;
    if (sg.waiting) {
        if ((sg.got & sg.want) != sg.want)
            return;
        sg.waiting = 0;                       /* played: on, from the chord's start */
        sg.t0 = plat_ms();
        ui.redraw = 1;
    }
    u = sg_unit();
    sg_release(0, u);
    while (sg.next_ch < sg.nch) {
        sch_t c;
        sg_chord(sg.next_ch, &c);
        if (c.start > u)
            break;
        if (sg.wait && c.n && !sg.waiting && sg.u0 != c.start) {
            sg.waiting = 1;                   /* ARP: the song waits here until the chord is played */
            sg.want = sg_mask(&c), sg.got = 0;
            sg.u0 = c.start;
            sg.ch = sg.next_ch;
            sg_shape(&c);
            sg_release(1, 0);
            ui.redraw = 1;
            return;
        }
        sg.ch = sg.next_ch++;
        sg_release(1, 0);
        sg_sound(&c, (uint32_t)c.start + c.len);
        ui.redraw = 1;
    }
    while (sg.next_nt < sg.nnt) {
        const uint8_t *p = sg.notes + sg.next_nt * 5;
        uint32_t st = p[0] | p[1] << 8, ln = p[2] | p[3] << 8;
        if (st > u)
            break;
        if (sg.melody)
            sg_hold(60 + pc_of(sg_tonic()) + (int8_t)p[4] + sg.mel_shift - (pc_of(sg_tonic()) >= 7 ? 12 : 0), 100,
                    st + (ln > 1u ? ln - 1u : 1u));
        sg.next_nt++;
    }
    if (u >= sg.to) {                         /* the section's end: the next, or the song's */
        if (sg.sec + 1 < sg.nsec) {
            sg_section(sg.sec + 1);
            sg.u0 = sg.from, sg.t0 = plat_ms();
            sg.next_ch = sg.next_nt = 0;
        } else {
            sg_stop();
            sg.ch = 0;
        }
        ui.redraw = 1;
    }
}

/* a note from the keys: the one waited for */
static void sg_note(int note)
{
    if (sg.playing && sg.waiting) {
        sg.got |= (uint16_t)PCB(note % 12);
        ui.redraw = 1;
    }
}

/* SEL, SELECT: a chord heard (stopped) */
static void sg_step(int d)
{
    sch_t c;
    if (sg.playing)
        return;
    if (sg.ch + d >= sg.nch) {                /* past the last: the next section */
        sg_section(sg.sec + 1 < sg.nsec ? sg.sec + 1 : 0);   /* (after the last: the song again) */
    } else if (sg.ch + d < 0) {
        if (sg.sec > 0) {
            sg_section(sg.sec - 1);
            sg.ch = sg.nch ? sg.nch - 1 : 0;
        }
    } else
        sg.ch += d;
    if (!sg.nch)
        return;
    sg_chord(sg.ch, &c);
    sg_release(1, 0);
    sg_sound(&c, 0);
    {                                         /* stopped: the chord rings for two beats */
        int i;
        uint32_t t = plat_ms() + 60000u / sg.bpm * 2u;
        for (i = 0; i < 16; i++)
            if (sg.vo[i].note) {
                play_ev(t, sg.vo[i].note, 0, 0);
                pev_sounding[sg.vo[i].note] = 1;
                sg.cnt[sg.vo[i].note] = 0;
                sg.vo[i].note = 0;
            }
    }
}

/* ------------------------------------------------------------- screens --- */
static const char *sg_name(int i)
{
    static char t[52];
    rd_t r = sg_song(i);
    rd_str(&r, t, 49);
    if (r.bad)
        return "?";
    t1_fit(t, &FONT_B, 220);
    return t;
}
static const char *sg_sub(int i, char *b)
{
    rd_t r = sg_song(i);
    rd_skip(&r, rd_u8(&r));
    rd_str(&r, b, 41);
    if (r.bad)
        b[0] = 0;
    t1_fit(b, &FONT_XS, 216);
    if (i >= (int)sg_bank[1].n && !b[0])
        t1_strcpy(b, SG(SG_BUILTIN));
    return b;
}
static void draw_songs(void)
{
    char t[24];
    int_str(t, sg_count());
    t1_fmt(t, SG(SG_LOADED), t, 0, 0);
    ui_header(SG(SG_MENU), t, C_GRAY);
    big_list(sg_count(), sg.sel, sg_name, sg_sub, C_HI);
    hint(S(S_H_TOPICS));
}

static const uint16_t FN_COL[NFN] = {RGB(255, 150, 40), RGB(80, 220, 120), RGB(240, 80, 70), RGB(150, 200, 255),
                                     RGB(190, 110, 255), RGB(255, 225, 90), RGB(120, 120, 130)};

static void draw_song(void)
{
    char t[64], u[64];
    sch_t c;
    int i;
    int32_t x;
    nt_t tn = sg_tonic();
    t1_strcpy(t, sg.name);
    if (sg.nsec > 1) {
        str_cat(t, " ");
        int_str(u, sg.sec + 1);
        str_cat(t, u);
        str_cat(t, "/");
        int_str(u, sg.nsec);
        str_cat(t, u);
    }
    t1_upper(u, sg.title);                    /* the title: what room the section's name leaves */
    t1_fit(u, &FONT_B, 216 - (t1_tw(&FONT_B, t) <= 120 ? t1_tw(&FONT_B, t) : t1_tw(&FONT_XS, t)));
    ui_header(u, t, C_HI);
    /* the key, the tempo, the meter */
    note_full(tn, t);
    str_cat(t, " ");
    str_cat(t, sg_mode_text());
    str_cat(t, "   ");
    int_str(u, (int)(sg.bpm * sg.tempo / 100u));
    str_cat(t, u);
    str_cat(t, " BPM   ");
    int_str(u, sg.beats);
    str_cat(t, u);
    str_cat(t, "/");
    int_str(u, sg.unit);
    str_cat(t, u);
    t1_tx(8, 30, &FONT_XS, t, sg.key >= 0 || sg.tempo != 100u ? COL_ROOT : C_GRAY);
    /* the chord: its symbol big, its numeral, what it does */
    sg_chord(sg.ch, &c);
    sg_symbol(&c, t);
    t1_tx(8, 48, t1_tw(&FONT_M, t) <= 150 ? &FONT_M : &FONT_B, t, C_WHITE);
    text_r(232, 48, t1_tw(&FONT_M, c.num) <= 80 ? &FONT_M : &FONT_B, c.num, FN_COL[c.fn]);
    cv_rect(8, 74, 3, 13, FN_COL[c.fn]);
    t1_tx(16, 74, &FONT_XS, SG(sg.nch ? SG_FN0 + c.fn : SG_MELODY_ONLY), C_HI);
    /* the next chords */
    x = t1_tx(8, 96, &FONT_XS, SG(SG_NEXT), C_DIM) + 8;
    for (i = 1; i <= 3 && sg.ch + i < sg.nch; i++) {
        sch_t n;
        sg_chord(sg.ch + i, &n);
        sg_symbol(&n, t);
        str_cat(t, " ");
        str_cat(t, n.num);
        if (x + t1_tw(&FONT_XS, t) > 232)
            break;
        x = t1_tx(x, 96, &FONT_XS, t, i == 1 ? C_WHITE : C_GRAY) + 12;
    }
    /* the section, chord by chord, and where the song is */
    {
        int32_t w = 224, y = 118;
        uint32_t span = (uint32_t)(sg.to - sg.from), now = sg.playing ? (sg.waiting ? sg.u0 : sg_unit()) : 0;
        cv_rect(8, y, w, 14, COL_PANEL);
        for (i = 0; i < sg.nch && span; i++) {
            sch_t k;
            int32_t a, b;
            sg_chord(i, &k);
            a = (int32_t)((uint32_t)(k.start - sg.from) * (uint32_t)w / span);
            b = (int32_t)((uint32_t)(k.start + k.len - sg.from) * (uint32_t)w / span);
            if (b > w)
                b = w;
            if (b - a > 1)
                cv_rect(8 + a + 1, y + 1, b - a - 1, 12, i == sg.ch ? FN_COL[k.fn] : RGB(50, 60, 80));
        }
        if (sg.playing && span && now >= sg.from && now < sg.to)
            cv_rect(8 + (int32_t)((now - sg.from) * (uint32_t)w / span), y - 2, 2, 18, C_WHITE);
        {
            uint32_t bar = UPB * 4u * sg.beats / sg.unit, at = sg.playing ? now : c.start;
            char a[8], b[8];
            int_str(a, (int)((at > sg.from ? at - sg.from : 0) / bar + 1u));
            int_str(b, (int)(span / bar));
            t1_fmt(t, SG(SG_BAR), a, b, 0);
            t1_tx(8, 136, &FONT_XS, t, C_DIM);
        }
        t1_strcpy(t, "");
        if (sg.wait)
            str_cat(t, "ARP ");
        if (!sg.melody)
            str_cat(t, "-MEL ");
        text_r(232, 136, &FONT_XS, t, COL_SCALE);
    }
    /* waiting: whose turn */
    if (sg.playing && sg.waiting) {
        sg_symbol(&c, u);
        t1_fmt(t, SG(SG_WAIT_PLAY), u, 0, 0);
        t1_tx(8, 158, &FONT_B, t, COL_HELD);
    }
    hint(SG(sg.playing ? SG_H_PLAY : SG_H_SONG));
    /* the keys: the hand shape, the chord's other notes */
    mk.scale = c.n ? sg_mask(&c) : 0;
    mk.root = c.n ? (uint16_t)PCB((c.root_semi + pc_of(tn)) % 12) : 0;
    if (sg.waiting)
        mk.ok = sg.got & sg.want;
    if (!sg.nvoice || sg.nlast != sg.nvoice)
        sg_shape(&c);
    mk.nkeys = 0;
    for (i = 0; i < sg.nvoice && c.n; i++) {
        int k = sg.voice[i] - 53 - 12 * ui.octave;
        if (k >= 0 && k < NKEYS)
            mk.key[mk.nkeys++] = (uint8_t)k;
    }
}

/* ------------------------------------------------------------- controls --- */
static void sg_enter_list(void)
{
    sg_scan();
    if (sg.sel >= sg_count())
        sg.sel = 0;
}
static void sg_open_sel(void)
{
    if (!sg_count() || !sg_open(sg.sel))
        return;
    sg.nlast = 0;
    ui_go(SCR_SONG);
    {
        sch_t c;
        sg_chord(0, &c);
        sg_shape(&c);
    }
}
static void sg_key_turn(int d)
{
    int n = sg_minor() ? (int)sizeof COMMON_MINOR_ROOTS : (int)sizeof COMMON_MAJOR_ROOTS, i;
    if (sg.key < 0) {                         /* from the song's own key: the list's root of that pitch */
        const uint8_t *l = sg_minor() ? COMMON_MINOR_ROOTS : COMMON_MAJOR_ROOTS;
        for (i = 0, sg.key = 0; i < n; i++)
            if (pc_of(ROOTS[l[i]]) == pc_of(sg.tonic0))
                sg.key = (int8_t)i;
    }
    sg.key = (int8_t)t1_wrap(sg.key + d, n);
    if (pc_of(sg_tonic()) == pc_of(sg.tonic0))
        sg.key = -1;
    sg.nlast = 0;
    {
        char t[16];
        note_full(sg_tonic(), t);
        app_say(SG(SG_SAY_KEY), t);
    }
}
static void sg_encoder(int e, int d)
{
    if (e == EN_SELECT)
        sg_step(d < 0 ? -1 : 1);
    else if (e == EN_ALGO && !sg.playing) {
        sg_section(t1_wrap(sg.sec + (d < 0 ? -1 : 1), sg.nsec));
    } else if (e == EN_K1 || e == EN_PRESET) {
        sg_key_turn(d);
    } else if (e == EN_K2) {
        char t[8];
        sg.tempo = (uint8_t)clamp(sg.tempo + (d > 0 ? 5 : -5), 25, 100);
        if (sg.playing) {                     /* the song goes on from where it is, at the new pace */
            sg.u0 = sg.waiting ? sg.u0 : sg_unit();
            sg.t0 = plat_ms();
        }
        int_str(t, sg.tempo);
        str_cat(t, "%");
        app_say(SG(SG_SAY_TEMPO), t);
    }
}
static void sg_button(int b)
{
    if (b == B_PLAY) {
        if (sg.playing)
            sg_stop();
        else
            sg_play();
    } else if (b == B_ARP) {
        sg.wait = (uint8_t)!sg.wait;
        if (!sg.wait && sg.waiting) {
            sg.waiting = 0;
            sg.t0 = plat_ms();
        }
        app_say(SG(SG_SAY_WAIT), S(sg.wait ? S_ON : S_OFF));
    } else if (b == B_FX) {
        sg.melody = (uint8_t)!sg.melody;
        app_say(SG(SG_SAY_MELODY), S(sg.melody ? S_ON : S_OFF));
    } else if (b == B_EDIT)
        names_next();
}

/* ------------------------------------------------------------ the loader --- */
static void ui_draw(void);

/* the bank's payload (len bytes, CRC-32 crc) from fetch into the region; 0 or SG_E_* */
static int sg_load(uint32_t len, uint32_t crc, int (*fetch)(uint32_t off, uint8_t *dst, uint32_t n))
{
    static uint8_t buf[512];
    uint8_t h[SONG_HEAD];
    uint32_t off, s, nsec = (SONG_HEAD + len + 4095u) / 4096u, k;
    const uint8_t *p;
    if (!len || SONG_HEAD + len > SONG_SECTORS * 4096u)
        return SG_E_SIZE;
    for (s = 0; s < nsec; s++)
        if (plat_data_erase(SONG_BASE + s * 4096u))
            return SG_E_ERASE;
    for (off = 0; off < len; off += 512u) {
        uint32_t n = len - off > 512u ? 512u : len - off, at = SONG_BASE + SONG_HEAD + off, done = 0;
        if (fetch(off, buf, n))
            return SG_E_FETCH;
        while (done < n) {                    /* split at the NOR's 256-byte pages */
            k = 256u - ((at + done) & 255u);
            if (k > n - done)
                k = n - done;
            if (plat_data_write(at + done, buf + done, k))
                return SG_E_PROG;
            done += k;
        }
    }
    p = plat_data(SONG_BASE + SONG_HEAD);
    if (!p || sg_crc32(p, len) != crc)
        return SG_E_CHECK;
    for (k = 0; k < SONG_HEAD; k++)
        h[k] = 0;
    h[0] = 'T', h[1] = '1', h[2] = 'S', h[3] = 'G';
    for (k = 0; k < 4u; k++) {
        h[4 + k] = (uint8_t)(len >> 8 * k);
        h[8 + k] = (uint8_t)(crc >> 8 * k);
    }
    s = sg_crc32(h, 28);
    for (k = 0; k < 4u; k++)
        h[28 + k] = (uint8_t)(s >> 8 * k);
    if (plat_data_write(SONG_BASE, h, SONG_HEAD))
        return SG_E_PROG;
    sg_scan();
    return sg_bank[1].pay ? 0 : SG_E_CHECK;
}

static int sg_fetch_host(uint32_t off, uint8_t *dst, uint32_t n) { return plat_host_read(off, dst, n); }

/* a song bank from the computer (tools/songs/fm1_songs.py) */
void app_usb_cmd(const uint8_t *d, uint32_t n)
{
    uint32_t len, crc;
    uint8_t ack[8];
    int rc;
    char t[24];
    if (n != 15u || d[2] != SONG_REQ)
        return;
    len = d[6] | d[7] << 8 | d[8] << 16 | (uint32_t)d[9] << 24;
    crc = d[10] | d[11] << 8 | d[12] << 16 | (uint32_t)d[13] << 24;
    sg_stop();
    play_stop();
    synth_all_off();
    if (ui.screen == SCR_SONG)                /* (the song open is read from the flash about to change) */
        ui_go(SCR_SONGS);
    sg.cur = -1;
    app_say(SG(SG_LOADING), "");
    ui_draw();
    plat_host_patience(2000u, 120000u);
    rc = sg_load(len, crc, sg_fetch_host);
    plat_host_read(rc ? SG_FAIL_ADDR | (uint32_t)rc : SG_OK_ADDR, ack, sizeof ack);
    sg_enter_list();
    int_str(t, (int)sg_bank[1].n);
    {
        char m[32];
        t1_fmt(m, SG(SG_LOADED), t, 0, 0);
        app_say(SG(SG_MENU), rc ? SG(SG_NOT_LOADED) : m);
    }
    if (ui.screen != SCR_SONGS)
        ui_go(SCR_SONGS);
    ui.redraw = 1;
}

#ifdef PLAT_HOST
/* the simulator: a bank file loaded through the same path ("songload FILE") */
static uint8_t *sg_file;
static uint32_t sg_file_len;
static int sg_fetch_file(uint32_t off, uint8_t *dst, uint32_t n)
{
    uint32_t i;
    if (off + n > sg_file_len)
        return -1;
    for (i = 0; i < n; i++)
        dst[i] = sg_file[off + i];
    return 0;
}
static int sg_host_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    static uint8_t reg[SONG_SECTORS * 4096u];
    uint32_t n, len, crc;
    int rc;
    if (!f)
        return -1;
    n = (uint32_t)fread(reg, 1, sizeof reg, f);
    fclose(f);
    if (n < SONG_HEAD)
        return -1;
    len = reg[4] | reg[5] << 8 | reg[6] << 16 | (uint32_t)reg[7] << 24;
    crc = reg[8] | reg[9] << 8 | reg[10] << 16 | (uint32_t)reg[11] << 24;
    sg_file = reg + SONG_HEAD, sg_file_len = n - SONG_HEAD;
    rc = sg_load(len, crc, sg_fetch_file);
    sg_enter_list();
    return rc;
}
#endif
