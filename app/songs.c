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
    uint8_t loop;                             /* at the end: 0 stop, 1 the song again, 2 the section again */
    /* playing */
    uint8_t playing, waiting;
    int pre;                                  /* the chord coming (its index; -1 none): its shape shown a beat early */
    int passed;                               /* ARP: the chord already played (not waited on again) */
    uint16_t ready;                           /* the notes played since the chord coming was shown */
    sch_t prec;                               /* the chord coming */
    uint16_t want, got;                       /* waiting: the chord's pitch classes, those played */
    uint32_t t0, u0;                          /* at t0 the song was at unit u0 */
    int next_ch, next_nt;
    uint8_t cnt[128];                         /* notes sounding (a count: a chord and the melody may share one) */
    struct { uint8_t note; uint16_t end_sec; uint32_t end; } vo[16];
    uint8_t voice[6], nvoice;                 /* the chord's hand shape (MIDI, low to high), for the keys */
    uint8_t last[6], nlast;
    uint8_t count;                            /* the count-in: bars of clicks before PLAY (0 none) */
    uint8_t cin_n;                            /* counting in: its clicks, from cin0 to t0 */
    uint32_t cin0;
    uint8_t view, vsel, hphase, cbeat;              /* SV_*: the song, its map, the same chords elsewhere, ear training */
} sg = {.cur = -1, .key = -1, .tempo = 100, .melody = 1, .loop = 1, .pre = -1, .passed = -1, .count = 1};
enum { SV_SONG, SV_MAP, SV_FIND, SV_EAR };

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

static void sg_chord_rd(rd_t r, sch_t *c);
static void sg_rest(sch_t *c, uint16_t from, uint16_t to)   /* no chord: a rest over from .. to */
{
    uint32_t z;
    for (z = 0; z < sizeof *c; z++)
        ((uint8_t *)c)[z] = 0;
    c->start = from, c->len = (uint16_t)(to - from), c->fn = FN_REST;
}
static void sg_chord(int i, sch_t *c)
{
    rd_t r;
    if (i < 0 || i >= sg.nch) {               /* none (a section with only a melody): a rest over all of it */
        sg_rest(c, sg.from, sg.to);
        return;
    }
    r.p = sg.secp[sg.sec] + sg.choff[i], r.end = sg.end, r.bad = 0;
    sg_chord_rd(r, c);
}
/* section s's first chord, without opening it (the chord coming at a section's end) */
static void sg_first(int s, sch_t *c)
{
    rd_t r = {sg.secp[s], sg.end, 0};
    uint32_t from, to, nch;
    rd_skip(&r, rd_u8(&r));
    rd_skip(&r, 2);
    rd_skip(&r, rd_u8(&r));
    rd_skip(&r, 4);
    from = rd_u16(&r), to = rd_u16(&r), nch = rd_u16(&r);
    rd_skip(&r, 2);
    if (r.bad || !nch)
        sg_rest(c, (uint16_t)from, (uint16_t)to);
    else
        sg_chord_rd(r, c);
}
static void sg_chord_rd(rd_t r, sch_t *c)
{
    uint32_t k, b;
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
static const char *sg_mode_name(const char *mode)
{
    int m;
    if (sg_same(mode, "minor") || sg_same(mode, "aeolian"))
        return SCALES[SC_NATMINOR].name[th_ui];
    if (sg_same(mode, "ionian"))
        return SCALES[SC_MAJOR].name[th_ui];
    for (m = 0; m < SC_PENTA_MAJ; m++)
        if (sg_same(mode, SCALES[m].name[1]))
            return SCALES[m].name[th_ui];
    for (m = 0; m < NMODES; m++)
        if (sg_same(mode, MODES[m].name[1]))
            return MODES[m].name[th_ui];
    return mode;
}
static const char *sg_mode_text(void) { return sg_mode_name(sg.mode); }
/* where a mode's tonic sits in its parent major scale (A minor: 9, in C): chords compared across songs, whatever
 * their mode, as in that major */
static int sg_mode_off(const char *m)
{
    if (m[0] == 'd')
        return 2;                             /* dorian */
    if (m[0] == 'p')
        return 4;                             /* phrygian (dominant: the same notes but one) */
    if (m[0] == 'l')
        return m[1] == 'y' ? 5 : m[1] == 'o' ? 11 : 0;
    if (m[0] == 'm' && m[1] == 'i' && m[2] == 'x')
        return 7;
    if ((m[0] == 'm' && m[1] == 'i') || m[0] == 'a' || m[0] == 'h' || (m[0] == 'm' && m[1] == 'e'))
        return 9;                             /* minor, aeolian, harmonic and melodic minor */
    return 0;
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
static int sg_counting(void) { return sg.playing && (int32_t)(plat_ms() - sg.t0) < 0; }
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
    if (sg_counting())
        play_stop();                          /* (the count-in's clicks still to come) */
    sg_release(1, 0);
    sg.playing = sg.waiting = 0;
    sg.pre = -1;
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
    uint64_t ms;
    if (sg_counting())
        return sg.u0;
    ms = plat_ms() - sg.t0;
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
    sg.passed = sg.ch, sg.pre = -1;
    while (sg.next_nt < sg.nnt && (uint32_t)(sg.notes[sg.next_nt * 5] | sg.notes[sg.next_nt * 5 + 1] << 8) < sg.u0)
        sg.next_nt++;
    sg.playing = 1;
    sg.cin_n = (uint8_t)(sg.count * sg.beats);
    if (sg.cin_n) {                           /* the count-in: a bar or two of clicks, the first of each bar high */
        uint32_t k, ub = UPB * 4u / sg.unit, rate = (uint32_t)sg.bpm * sg.tempo * UPB;
        sg.cin0 = sg.t0;
        for (k = 0; k < sg.cin_n; k++)
            play_note(sg.cin0 + (uint32_t)((uint64_t)k * ub * 6000000u / rate), k % sg.beats ? 84 : 96, 40,
                      k % sg.beats ? 70 : 110);
        sg.t0 = sg.cin0 + (uint32_t)((uint64_t)sg.cin_n * ub * 6000000u / rate);
    }
}
static int sg_count_beat(void)                /* counting in: the click it is at, 1 .. */
{
    uint32_t span = sg.t0 - sg.cin0;
    return span ? (int)((plat_ms() - sg.cin0) * (uint64_t)sg.cin_n / span) + 1 : 0;
}

/* the section after this one as the song plays: the next, or (at the end) as the loop says; -1 the end */
static int sg_next_sec(void)
{
    if (sg.loop == 2)
        return sg.sec;
    if (sg.sec + 1 < sg.nsec)
        return sg.sec + 1;
    return sg.loop ? 0 : -1;
}
static uint16_t sg_held(void)                 /* the notes held on the keys, as pitch classes */
{
    uint16_t m = 0;
    int i;
    for (i = 0; i < 128; i++)
        if (ui.held[i])
            m |= (uint16_t)PCB(i % 12);
    return m;
}

/* the player: called every pass of the main loop */
static void sg_tick(void)
{
    uint32_t u;
    if (ui.screen == SCR_SONG && sg.view == SV_SONG && !sg.playing && (uint8_t)(plat_ms() / 3000u & 1u) != sg.hphase) {
        sg.hphase = (uint8_t)(plat_ms() / 3000u & 1u);   /* the hint: the song's controls, then the others' */
        ui.redraw = 1;
    }
    if (!sg.playing)
        return;
    if (sg_counting()) {                      /* counting in: the first chord's shape shown, to get ready */
        if ((uint8_t)sg_count_beat() != sg.cbeat) {
            sg.cbeat = (uint8_t)sg_count_beat();
            ui.redraw = 1;
        }
        if (sg.pre < 0 && sg.next_ch < sg.nch) {
            sch_t n;
            sg_chord(sg.next_ch, &n);
            if (n.n) {
                sg.pre = sg.next_ch, sg.prec = n, sg.ready = 0;
                sg_shape(&n);
                ui.redraw = 1;
            }
        }
        return;
    }
    if (sg.waiting) {
        if ((sg.got & sg.want) != sg.want)
            return;
        sg.waiting = 0;                       /* played: on, from the chord's start */
        sg.passed = sg.next_ch;
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
        if (sg.wait && c.n && sg.passed != sg.next_ch &&
            ((sg.ready | sg_held()) & sg_mask(&c)) != sg_mask(&c)) {
            sg.waiting = 1;                   /* ARP: the song waits here until the chord is played (played or
                                               * held as it comes, in time: it goes on) */
            sg.pre = -1;
            sg.want = sg_mask(&c), sg.got = 0;
            sg.u0 = c.start;
            sg.ch = sg.next_ch;
            sg_shape(&c);
            sg_release(1, 0);
            ui.redraw = 1;
            return;
        }
        sg.passed = sg.next_ch;
        sg.ch = sg.next_ch++;
        sg.pre = -1;
        sg_release(1, 0);
        sg_sound(&c, (uint32_t)c.start + c.len);
        ui.redraw = 1;
    }
    if (sg.pre < 0) {                         /* a beat before the next chord: its shape, to get the hand ready */
        sch_t n;
        int at;
        if (sg.next_ch < sg.nch) {
            sg_chord(sg.next_ch, &n);
            at = n.start;
        } else {
            int s = sg_next_sec();
            if (s < 0)
                n.n = 0;
            else
                sg_first(s, &n);
            at = sg.to;                       /* (its first chord: at this section's end) */
        }
        if (n.n && u + UPB >= (uint32_t)at) {
            sg.pre = sg.next_ch;
            sg.prec = n;
            sg.ready = 0;
            sg_shape(&n);
            ui.redraw = 1;
        }
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
    if (u >= sg.to) {                         /* the section's end: the next, the song or the section again */
        int next = sg_next_sec();
        uint32_t over = u - sg.to;            /* (in time: what the loop ran past the end, carried) */
        if (next >= 0) {
            sg_section(next);
            sg.u0 = sg.from + (over < UPB ? over : 0u), sg.t0 = plat_ms();
            sg.next_ch = sg.next_nt = 0;
            sg.passed = -1;
        } else {
            sg_stop();
            sg_section(0);
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
    } else if (sg.playing && sg.pre >= 0)
        sg.ready |= (uint16_t)PCB(note % 12);
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

/* --------------------------------------------------------------- views --- */
/* a section's head, read without opening it (the map, the search) */
typedef struct {
    char name[25], mode[24];
    nt_t tonic;
    uint8_t beats, unit;
    uint16_t from, to, nch, nnt;
} shead_t;
static rd_t sg_head(rd_t r, shead_t *h)
{
    rd_str(&r, h->name, sizeof h->name);
    h->tonic.letter = (int8_t)(rd_u8(&r) % 7u);
    h->tonic.acc = (int8_t)rd_u8(&r);
    if (h->tonic.acc < -2 || h->tonic.acc > 2)
        h->tonic.acc = 0;
    rd_str(&r, h->mode, sizeof h->mode);
    rd_skip(&r, 2);
    h->beats = (uint8_t)rd_u8(&r);
    h->unit = (uint8_t)rd_u8(&r);
    if (!h->beats || h->beats > 16u)
        h->beats = 4;
    if (h->unit != 2u && h->unit != 4u && h->unit != 8u && h->unit != 16u)
        h->unit = 4;
    h->from = (uint16_t)rd_u16(&r), h->to = (uint16_t)rd_u16(&r);
    h->nch = (uint16_t)rd_u16(&r), h->nnt = (uint16_t)rd_u16(&r);
    if (h->to <= h->from)
        h->to = (uint16_t)(h->from + UPB * h->beats);
    return r;
}
static uint32_t sg_bar_of(const shead_t *h) { return UPB * 4u * h->beats / h->unit; }

/* a section's colour on the map, by what it is called: the same kind of part, the same colour */
static int sg_pre(const char *a, const char *p)
{
    for (; *p; a++, p++)
        if ((*a | 0x20) != *p)
            return 0;
    return 1;
}
static uint16_t sg_kind_col(const char *n)
{
    return sg_pre(n, "pre") ? RGB(190, 110, 255) : sg_pre(n, "chorus") || sg_pre(n, "refrain") ? RGB(255, 150, 40) :
           sg_pre(n, "verse") ? RGB(70, 170, 255) : sg_pre(n, "bridge") ? RGB(80, 220, 120) :
           sg_pre(n, "intro") || sg_pre(n, "outro") || sg_pre(n, "coda") ? RGB(130, 140, 160) :
           sg_pre(n, "solo") || sg_pre(n, "instrumental") || sg_pre(n, "interlude") || sg_pre(n, "hook") ?
           RGB(255, 225, 90) : RGB(80, 210, 200);
}

/* four rows of a list, each with its colour bar, the chosen one lit (the map's sections, the search's finds) */
static void sg_rows(int n, int sel, int32_t y0, void (*row)(int i, char *a, char *b, uint16_t *col))
{
    int i, first = clamp(sel - 1, 0, n > 4 ? n - 4 : 0);
    for (i = first; i < first + 4 && i < n; i++) {
        int32_t y = y0 + (i - first) * 42;
        char a[64], b[64];
        uint16_t col;
        row(i, a, b, &col);
        if (i == sel)
            cv_rect(4, y - 3, 232, 40, COL_SELBG);
        cv_rect(8, y, 6, 34, col);
        t1_fit(a, &FONT_B, 210);
        t1_fit(b, &FONT_XS, 210);
        t1_tx(22, y, &FONT_B, a, i == sel ? C_WHITE : C_GRAY);
        t1_tx(22, y + 20, &FONT_XS, b, i == sel ? C_HI : C_DIM);
    }
    if (n > 4)
        cv_rect(236, y0 + first * 164 / n, 3, 16, C_DIM);
}

/* the map: the song's parts as blocks, as long as they are; SEL goes to the one chosen */
static void sg_map_row(int i, char *a, char *b, uint16_t *col)
{
    shead_t h;
    rd_t r = {sg.secp[i], sg.end, 0};
    char t[24];
    uint32_t bars;
    sg_head(r, &h);
    t1_strcpy(a, h.name);
    *col = sg_kind_col(h.name);
    bars = (uint32_t)(h.to - h.from + sg_bar_of(&h) - 1u) / sg_bar_of(&h);
    int_str(t, (int)bars);
    t1_fmt(b, SG(bars == 1u ? SG_BAR1 : SG_BARS), t, 0, 0);
    str_cat(b, "   ");
    if (sg.key < 0) {                         /* (moved to another key: only the mode) */
        note_full(h.tonic, t);
        str_cat(b, t);
        str_cat(b, " ");
    }
    str_cat(b, sg_mode_name(h.mode));
}
static void draw_map(void)
{
    uint32_t total = 0, len[SG_MAXSEC], s;
    int32_t x = 8;
    char t[64];
    t1_upper(t, sg.title);
    t1_fit(t, &FONT_B, 150);
    ui_header(t, SG(SG_MAP), C_HI);
    for (s = 0; s < sg.nsec; s++) {
        shead_t h;
        rd_t r = {sg.secp[s], sg.end, 0};
        sg_head(r, &h);
        len[s] = (uint32_t)(h.to - h.from) * 48u / sg_bar_of(&h);   /* (in bars, the meter's own) */
        total += len[s] ? len[s] : 1u;
    }
    for (s = 0; s < sg.nsec && total; s++) {   /* the whole song in a strip */
        shead_t h;
        rd_t r = {sg.secp[s], sg.end, 0};
        int32_t w;
        uint32_t before = 0, k;
        for (k = 0; k <= s; k++)
            before += len[k] ? len[k] : 1u;
        w = 8 + (int32_t)(before * 224u / total) - x;
        sg_head(r, &h);
        if (s == sg.vsel)
            cv_rect(x, 28, w, 20, C_WHITE);
        if (w > 2)
            cv_rect(x + 1, 30, w - 2, 16, sg_kind_col(h.name));
        if (s == sg.sec && w > 2)             /* where the song is: a mark under it */
            cv_rect(x + 1, 50, w - 2, 3, C_WHITE);
        x += w;
    }
    sg_rows(sg.nsec, sg.vsel, 58, sg_map_row);
    hint(SG(SG_H_MAP));
}

/* the same chords elsewhere: from the chord shown, its next four (repeats counted once), searched for in every
 * part of every song, in any key or mode (compared as in their parent major, sevenths and added notes left out) */
#define SG_FMAX 40
static struct {
    uint8_t n, npat, pat[4];
    uint16_t from;                            /* the chord searched from */
    char label[64];
    struct { uint8_t song, sec; uint16_t ch, bar; } at[SG_FMAX];
} sgf;
static uint8_t sg_sig(const uint8_t *tone, int n, int root, int off)
{
    uint16_t m = 0;
    int k, q;
    for (k = 0; k < n; k++)
        m |= (uint16_t)(1u << ((tone[k] - root + 24) % 12));
    q = m & 1u << 4 ? (m & 1u << 8 && !(m & 1u << 7) ? 2 : 0) :
        m & 1u << 3 ? (m & 1u << 6 && !(m & 1u << 7) ? 3 : 1) : 4;
    return (uint8_t)((root + off) % 12 | q << 4);
}
static void sg_find_in(int i)
{
    rd_t r = sg_song(i);
    uint32_t s, n;
    rd_skip(&r, rd_u8(&r));
    rd_skip(&r, rd_u8(&r));
    n = rd_u8(&r);
    for (s = 0; s < n && s < SG_MAXSEC && !r.bad; s++) {
        shead_t h;
        uint32_t k, nw = 0, off;
        uint8_t win[4] = {0};
        uint16_t at[4] = {0}, st[4] = {0};
        r = sg_head(r, &h);
        off = (uint32_t)sg_mode_off(h.mode);
        for (k = 0; k < h.nch && !r.bad; k++) {
            uint32_t start = rd_u16(&r), root, fn, nt, j;
            uint8_t tone[8];
            uint8_t sig;
            rd_skip(&r, 2);
            root = rd_u8(&r) & 15u;
            rd_skip(&r, 1);
            fn = rd_u8(&r), nt = rd_u8(&r);
            for (j = 0; j < nt; j++) {
                uint32_t b = rd_u8(&r);
                if (j < 8u)
                    tone[j] = (uint8_t)(b % 12u);
            }
            rd_skip(&r, rd_u8(&r));
            rd_skip(&r, rd_u8(&r));
            if (fn == FN_REST || !nt) {
                nw = 0;
                continue;
            }
            sig = sg_sig(tone, nt > 8u ? 8 : (int)nt, (int)root, (int)off);
            if (nw && win[nw - 1] == sig)
                continue;
            if (nw >= sgf.npat && nw) {
                for (j = 1; j < nw; j++)
                    win[j - 1] = win[j], at[j - 1] = at[j], st[j - 1] = st[j];
                nw--;
            }
            if (nw >= 4u)
                continue;
            win[nw] = sig, at[nw] = (uint16_t)k, st[nw] = (uint16_t)start, nw++;
            if (nw == sgf.npat && sgf.n < SG_FMAX) {
                for (j = 0; j < nw && win[j] == sgf.pat[j]; j++)
                    ;
                if (j == nw && !(i == sg.cur && s == sg.sec && at[0] == sgf.from)) {
                    sgf.at[sgf.n].song = (uint8_t)i, sgf.at[sgf.n].sec = (uint8_t)s, sgf.at[sgf.n].ch = at[0];
                    sgf.at[sgf.n].bar = (uint16_t)((st[0] > h.from ? (uint32_t)(st[0] - h.from) : 0u) / sg_bar_of(&h) + 1u);
                    sgf.n++;
                }
            }
        }
        rd_skip(&r, h.nnt * 5u);
    }
}
static void sg_find(void)
{
    int i, k, off = sg_mode_off(sg.mode);
    sgf.n = sgf.npat = 0;
    sgf.label[0] = 0;
    for (k = sg.ch; k < sg.nch && sgf.npat < 4; k++) {
        sch_t c;
        uint8_t sig;
        sg_chord(k, &c);
        if (!c.n) {
            if (sgf.npat)
                break;
            continue;
        }
        sig = sg_sig(c.tone, c.n, c.root_semi, off);
        if (sgf.npat && sgf.pat[sgf.npat - 1] == sig)
            continue;
        if (!sgf.npat)
            sgf.from = (uint16_t)k;
        sgf.pat[sgf.npat++] = sig;
        if (sgf.label[0])
            str_cat(sgf.label, "  ");
        str_cat(sgf.label, c.num);
    }
    if (sgf.npat < 2)
        return;
    for (i = 0; i < sg_count(); i++)          /* other songs first, then this one's other places */
        if (i != sg.cur)
            sg_find_in(i);
    sg_find_in(sg.cur);
}
static void sg_find_row(int i, char *a, char *b, uint16_t *col)
{
    rd_t r = sg_song(sgf.at[i].song);
    char t[48], u[8];
    uint32_t s;
    rd_str(&r, a, 49);
    rd_skip(&r, rd_u8(&r));
    s = rd_u8(&r);
    t[0] = 0;
    *col = C_DIM;
    if (sgf.at[i].sec < s) {                  /* the section's name: its head, after those before it */
        uint32_t k;
        for (k = 0; k <= sgf.at[i].sec && !r.bad; k++) {
            shead_t h;
            uint32_t j, nch;
            r = sg_head(r, &h);
            if (k == sgf.at[i].sec) {
                t1_strcpy(t, h.name);
                *col = sg_kind_col(h.name);
                break;
            }
            nch = h.nch;
            for (j = 0; j < nch && !r.bad; j++) {
                rd_skip(&r, 7);
                rd_skip(&r, rd_u8(&r));
                rd_skip(&r, rd_u8(&r));
                rd_skip(&r, rd_u8(&r));
            }
            rd_skip(&r, h.nnt * 5u);
        }
    }
    int_str(u, sgf.at[i].bar);
    t1_fmt(b, SG(SG_FIND_AT), t, u, 0);
}
static void draw_find(void)
{
    char t[64];
    t1_strcpy(t, sgf.label);
    ui_header(SG(SG_FIND), "", C_HI);
    t1_fit(t, &FONT_B, 224);
    t1_tx(8, 30, &FONT_B, t, COL_ROOT);
    if (!sgf.n)
        t1_tx(8, 62, &FONT_XS, SG(SG_FIND_NONE), C_GRAY);
    else {
        char a[8];
        int_str(a, sgf.n);
        text_r(232, 30, &FONT_XS, a, C_DIM);
        sg_rows(sgf.n, sg.vsel, 58, sg_find_row);
    }
    hint(SG(SG_H_FIND));
}

/* ear training: the chord before heard, then the one that comes: which is it? four numerals to choose from */
static struct {
    int q;                                    /* the chord asked (its index in the section), -1 none */
    char opt[4][24];
    uint8_t nopt, right, done, asked, ok;     /* done: 0 asking, 1 right, 2 wrong */
    uint32_t seed;
} ear;
static uint32_t ear_rand(uint32_t n)
{
    ear.seed = ear.seed * 1103515245u + 12345u;
    return n ? (ear.seed >> 16) % n : 0;
}
static void ear_play(void)                    /* the chord before, two beats, then the one asked */
{
    sch_t c;
    uint32_t t = plat_ms(), beat = 6000000u / ((uint32_t)sg.bpm * sg.tempo), k;
    int i;
    play_stop();
    for (i = 0; i < 2; i++) {
        int b;
        if (ear.q < 0)
            return;
        if (!i) {
            int p = ear.q - 1;
            sg_chord(p, &c);
            while (p > 0 && !c.n)
                sg_chord(--p, &c);
        } else
            sg_chord(ear.q, &c);
        if (!c.n)
            continue;
        sg_shape(&c);
        for (k = 0; k < sg.nvoice; k++)
            play_note(t + i * 2u * beat, sg.voice[k], 2u * beat - 40u, 62);
        b = 40 + (c.bass_semi + pc_of(sg_tonic())) % 12;
        play_note(t + i * 2u * beat, b < 43 ? b + 12 : b, 2u * beat - 40u, 74);
    }
}
static int sg_eq(const char *a, const char *b)
{
    for (; *a && *a == *b; a++, b++)
        ;
    return *a == *b;
}
static int ear_has(const char *s)
{
    int i;
    for (i = 0; i < ear.nopt; i++)
        if (sg_eq(ear.opt[i], s))
            return 1;
    return 0;
}
static void ear_new(int from)                 /* the next question: the first chord from index from with one
                                               * before it (in a part of its own, the next part's second) */
{
    static const char *const MAJ[6] = {"I", "IV", "V", "vi", "ii", "iii"}, *const MIN[6] = {"i", "iv", "v", "VI",
                                                                                           "VII", "III"};
    sch_t c;
    int tries, k, n = 0;
    char pool[16][24];
    for (tries = 0; tries <= sg.nsec; tries++) {
        for (k = from < 1 ? 1 : from; k < sg.nch; k++) {
            sg_chord(k, &c);
            if (c.n)
                break;
        }
        if (k < sg.nch)
            break;
        sg_section(sg.sec + 1 < sg.nsec ? sg.sec + 1 : 0);
        from = 1;
    }
    ear.done = 0, ear.nopt = 0;
    sg.vsel = 0;
    if (k >= sg.nch) {
        ear.q = -1;
        return;
    }
    ear.q = k;
    sg.ch = k > 0 ? k - 1 : 0;
    t1_strcpy(ear.opt[ear.nopt++], c.num);
    for (k = 0; k < sg.nch && n < 16; k++) {  /* the wrong answers: this part's other numerals, else the key's */
        sch_t o;
        int j;
        sg_chord(k, &o);
        for (j = 0; j < n && !sg_eq(pool[j], o.num); j++)
            ;
        if (o.n && o.num[0] && j == n && !sg_eq(o.num, c.num))
            t1_strcpy(pool[n++], o.num);
    }
    while (ear.nopt < 4 && n) {
        k = (int)ear_rand((uint32_t)n);
        t1_strcpy(ear.opt[ear.nopt++], pool[k]);
        t1_strcpy(pool[k], pool[--n]);
    }
    for (k = 0; k < 6 && ear.nopt < 4; k++) {
        const char *d = sg_minor() ? MIN[k] : MAJ[k];
        if (!ear_has(d))
            t1_strcpy(ear.opt[ear.nopt++], d);
    }
    k = (int)ear_rand(ear.nopt);              /* the right one somewhere among them */
    {
        char t[24];
        t1_strcpy(t, ear.opt[0]);
        t1_strcpy(ear.opt[0], ear.opt[k]);
        t1_strcpy(ear.opt[k], t);
    }
    ear.right = (uint8_t)k;
    ear_play();
}
static void ear_answer(void)
{
    char t[48], u[32];
    sch_t c;
    if (ear.q < 0)
        return;
    if (ear.done) {                           /* answered: the next one */
        ear_new(ear.q + 1);
        return;
    }
    sg_chord(ear.q, &c);
    ear.asked++;
    ear.done = sg.vsel == ear.right ? 1 : 2;
    if (ear.done == 1)
        ear.ok++;
    sg.ch = ear.q;
    sg_symbol(&c, u);
    str_cat(u, " ");
    str_cat(u, c.num);
    t1_fmt(t, SG(ear.done == 1 ? SG_EAR_RIGHT : SG_EAR_WRONG), u, 0, 0);
    app_say(SG(SG_EAR), t);
    ear_play();
}
static void draw_ear(void)
{
    char t[64], u[32];
    sch_t c;
    int i;
    {
        char a[8], b[8];
        int_str(a, ear.ok);
        int_str(b, ear.asked);
        t1_fmt(t, SG(SG_EAR_SCORE), a, b, 0);
    }
    ui_header(SG(SG_EAR), t, C_GRAY);
    if (ear.q < 0) {
        t1_tx(8, 40, &FONT_XS, SG(SG_MELODY_ONLY), C_GRAY);
        hint(SG(SG_H_EAR));
        return;
    }
    i = ear.q - 1;
    sg_chord(i, &c);
    while (i > 0 && !c.n)
        sg_chord(--i, &c);
    sg_symbol(&c, u);
    str_cat(u, " ");
    str_cat(u, c.num);
    t1_fmt(t, SG(SG_EAR_Q), u, 0, 0);
    t1_fit(t, &FONT_B, 224);
    t1_tx(8, 30, &FONT_B, t, C_WHITE);
    for (i = 0; i < ear.nopt; i++) {          /* the four, two by two */
        int32_t x = 8 + (i & 1) * 116, y = 54 + (i >> 1) * 44;
        uint16_t bg = ear.done && i == ear.right ? RGB(30, 110, 60) :
                      ear.done == 2 && i == sg.vsel ? RGB(120, 35, 35) : COL_PANEL;
        if (i == sg.vsel && !ear.done)
            cv_rect(x - 2, y - 2, 112, 42, C_HI);
        cv_rect(x, y, 108, 38, bg);
        t1_strcpy(t, ear.opt[i]);
        t1_fit(t, &FONT_B, 100);
        if (t1_tw(&FONT_M, t) <= 100)
            t1_tx(x + (108 - t1_tw(&FONT_M, t)) / 2, y + 8, &FONT_M, t, C_WHITE);
        else
            t1_tx(x + (108 - t1_tw(&FONT_B, t)) / 2, y + 11, &FONT_B, t, C_WHITE);
    }
    if (ear.done) {                           /* answered: the chord, what it does, on the keys */
        sg_chord(ear.q, &c);
        sg_symbol(&c, u);
        str_cat(u, " ");
        str_cat(u, c.num);
        t1_fmt(t, SG(ear.done == 1 ? SG_EAR_RIGHT : SG_EAR_WRONG), u, 0, 0);
        t1_fit(t, &FONT_B, 224);
        t1_tx(8, 146, &FONT_B, t, ear.done == 1 ? COL_OK : COL_BAD);
        t1_tx(8, 166, &FONT_XS, SG(SG_FN0 + c.fn), C_HI);
        mk.scale = sg_mask(&c);
        mk.root = (uint16_t)PCB((c.root_semi + pc_of(sg_tonic())) % 12);
    }
    hint(SG(SG_H_EAR));
}

/* a view opened from the song, or closed (HOME, or its button again) */
static void sg_view(int v)
{
    if (sg.playing)
        sg_stop();
    play_stop();
    if (v == sg.view)
        v = SV_SONG;
    sg.view = (uint8_t)v;
    sg.vsel = 0;
    if (v == SV_MAP)
        sg.vsel = sg.sec;
    else if (v == SV_FIND)
        sg_find();
    else if (v == SV_EAR) {
        ear.asked = ear.ok = 0;
        ear.seed = plat_ms() | 1u;
        ear_new(sg.ch + 1);
    }
    if (v == SV_SONG) {
        sch_t c;
        sg.nlast = 0;
        sg_chord(sg.ch, &c);
        sg_shape(&c);
    }
    ui.redraw = 1;
}
static int sg_back(void)                      /* HOME in a view: back to the song */
{
    if (sg.view == SV_SONG)
        return 0;
    sg_view(sg.view);
    return 1;
}
static int sg_no_kb(void) { return sg.view == SV_MAP || sg.view == SV_FIND; }
static uint32_t sg_live(void)
{
    if (sg.view == SV_MAP)
        return 1u << B_SEL | 1u << B_ENV;
    if (sg.view == SV_FIND)
        return (sgf.n ? 1u << B_SEL : 0u) | 1u << B_SAVE;
    if (sg.view == SV_EAR)
        return 1u << B_SEL | 1u << B_PLAY | 1u << B_LFO;
    return 1u << B_SEL | 1u << B_PLAY | 1u << B_ARP | 1u << B_FX | 1u << B_EDIT | 1u << B_SEQ | 1u << B_ENV |
           1u << B_LFO | 1u << B_SAVE;
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
    if (sg.view == SV_MAP) {
        draw_map();
        return;
    }
    if (sg.view == SV_FIND) {
        draw_find();
        return;
    }
    if (sg.view == SV_EAR) {
        draw_ear();
        return;
    }
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
        if (sg.loop)
            str_cat(t, sg.loop == 2 ? "LOOP 1 " : "LOOP ");
        text_r(232, 136, &FONT_XS, t, COL_SCALE);
    }
    /* waiting: whose turn */
    if (sg.playing && sg.waiting) {
        sg_symbol(&c, u);
        t1_fmt(t, SG(SG_WAIT_PLAY), u, 0, 0);
        t1_tx(8, 158, &FONT_B, t, COL_HELD);
    } else if (sg.playing && sg.pre >= 0) {   /* the chord coming: its name, its shape on the keys */
        sg_symbol(&sg.prec, u);
        t1_fmt(t, SG(SG_READY), u, 0, 0);
        t1_tx(8, 158, &FONT_B, t, COL_ALT);
    }
    if (sg_counting()) {                      /* counting in: the beat */
        int_str(t, (sg_count_beat() - 1) % sg.beats + 1);
        text_r(232, 152, &FONT_M, t, C_WHITE);
    }
    hint(SG(sg.playing ? SG_H_PLAY : sg.hphase ? SG_H_SONG2 : SG_H_SONG));
    /* the keys: the hand shape, the chord's other notes */
    if (sg.playing && sg.pre >= 0 && !sg.waiting) {
        c = sg.prec;                          /* the keys: the chord coming, in the hint's colour */
        mk.hint = sg_mask(&c);
        mk.scale = mk.root = 0;
    } else {
        mk.scale = c.n ? sg_mask(&c) : 0;
        mk.root = c.n ? (uint16_t)PCB((c.root_semi + pc_of(tn)) % 12) : 0;
    }
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
    if (sg.view != SV_SONG) {                 /* the views: SELECT chooses */
        int n = sg.view == SV_MAP ? sg.nsec : sg.view == SV_FIND ? sgf.n : ear.done ? 0 : ear.nopt;
        if (e == EN_SELECT && n) {
            sg.vsel = (uint8_t)t1_wrap(sg.vsel + (d < 0 ? -1 : 1), n);
            if (sg.view == SV_EAR)
                app_say(SG(SG_EAR), ear.opt[sg.vsel]);
        }
        return;
    }
    if (e == EN_K3) {
        sg.count = (uint8_t)clamp(sg.count + (d > 0 ? 1 : -1), 0, 2);
        if (sg.count == 2) {
            char t[24];
            t1_fmt(t, SG(SG_BARS), "2", 0, 0);
            app_say(SG(SG_SAY_COUNT), t);
        } else
            app_say(SG(SG_SAY_COUNT), sg.count ? SG(SG_BAR1) : S(S_OFF));
        return;
    }
    if (e == EN_SELECT)
        sg_step(d < 0 ? -1 : 1);
    else if (e == EN_ALGO && !sg.playing) {
        sg_section(t1_wrap(sg.sec + (d < 0 ? -1 : 1), sg.nsec));
    } else if (e == EN_K1 || e == EN_PRESET) {
        sg_key_turn(d);
    } else if (e == EN_K2) {
        char t[8];
        sg.tempo = (uint8_t)clamp(sg.tempo + (d > 0 ? 5 : -5), 25, 100);
        if (sg.playing && !sg_counting()) {   /* the song goes on from where it is, at the new pace */
            sg.u0 = sg.waiting ? sg.u0 : sg_unit();
            sg.t0 = plat_ms();
        }
        int_str(t, sg.tempo);
        str_cat(t, "%");
        app_say(SG(SG_SAY_TEMPO), t);
    }
}
/* SEL: the next chord, or the view's choice */
static void sg_select(void)
{
    if (sg.view == SV_MAP) {
        sg_section(sg.vsel);
        sg_view(SV_MAP);                      /* (the view open again: closed) */
    } else if (sg.view == SV_FIND) {
        int i = sg.vsel;
        if (i >= sgf.n || !sg_open(sgf.at[i].song))
            return;
        sg.sel = sgf.at[i].song;
        sg_section(sgf.at[i].sec < sg.nsec ? sgf.at[i].sec : 0);
        sg.ch = sgf.at[i].ch < sg.nch ? sgf.at[i].ch : 0;
        sg_view(SV_FIND);
    } else if (sg.view == SV_EAR)
        ear_answer();
    else
        sg_step(1);
}
static void sg_button(int b)
{
    if (b == B_ENV || b == B_LFO || b == B_SAVE) {
        sg_view(b == B_ENV ? SV_MAP : b == B_LFO ? SV_EAR : SV_FIND);
        return;
    }
    if (sg.view == SV_EAR) {
        if (b == B_PLAY)
            ear_play();
        return;
    }
    if (sg.view != SV_SONG)
        return;
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
    } else if (b == B_SEQ) {
        sg.loop = (uint8_t)((sg.loop + 1u) % 3u);
        app_say(SG(SG_SAY_LOOP), sg.loop == 1 ? SG(SG_LOOP_SONG) : sg.loop == 2 ? SG(SG_LOOP_PART) : S(S_OFF));
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
