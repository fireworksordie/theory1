/* SPDX-License-Identifier: MPL-2.0
 * THEORY-1: music theory for the M-VAVE FM-1. */
/* NUMERALS (a lesson topic): what I ii iii IV V vi vii° mean, page by page, each with a progression to hear in any
 * key (K1) and, where the player's songs have it, a place in one of them to open (ARP). Included by app.c after
 * songs.c, whose search (sg_find_in) finds the songs. */

typedef struct {
    int8_t semi, let, type;                   /* from the page's tonic: semitones, letters; CH_* */
    const char *num;
} nchord_t;
typedef struct {
    const char *title[NLANGS], *text[NLANGS];
    uint8_t minor, n, npat;                   /* a minor key's page; its chords; the first npat sought in the songs */
    nchord_t ch[8];
} npage_t;

#define I_ {0, 0, CH_MAJ, "I"}
#define ii_ {2, 1, CH_MIN, "ii"}
#define iii_ {4, 2, CH_MIN, "iii"}
#define IV_ {5, 3, CH_MAJ, "IV"}
#define V_ {7, 4, CH_MAJ, "V"}
#define vi_ {9, 5, CH_MIN, "vi"}
#define vii_ {11, 6, CH_DIM, "vii\x01"}
static const npage_t NUMS[NNUM] = {
    {{"Accordi come numeri", "Chords as numbers", "Acordes como números", "Akkorde als Zahlen", "Des accords en chiffres"},
     {"Un numero romano dice su quale grado della tonalità sta l'accordo. In Do maggiore il grado 1 è Do, il 5 è Sol. "
      "Gira K1 per cambiare tonalità: le lettere cambiano, i numeri no.",
      "A numeral names a chord by the step of the key it stands on. In C major, step 1 is C and step 5 is G. Turn K1 "
      "to change key: the letters change, the numbers stay.",
      "Un número romano dice sobre qué grado de la tonalidad está el acorde. En Do mayor el grado 1 es Do, el 5 es "
      "Sol. Gira K1 para cambiar de tonalidad: las letras cambian, los números no.",
      "Eine römische Zahl sagt, auf welcher Stufe der Tonart ein Akkord steht. In C-Dur ist Stufe 1 C, Stufe 5 G. "
      "Dreh K1 für eine andere Tonart: die Buchstaben ändern sich, die Zahlen nicht.",
      "Un chiffre romain dit sur quel degré de la tonalité se trouve l'accord. En Do majeur, le degré 1 est Do, le 5 "
      "est Sol. Tourne K1 pour changer de ton : les lettres changent, pas les chiffres."},
     0, 4, 0, {I_, IV_, V_, I_}},
    {{"Grandi e piccoli", "Big and small", "Mayúsculas y minúsculas", "Groß und klein", "Grands et petits"},
     {"Maiuscolo (I, IV, V): accordo maggiore, aperto e luminoso. Minuscolo (ii, iii, vi): minore, più scuro. "
      "Col cerchietto (vii\x01): diminuito, teso.",
      "Upper case (I, IV, V) is a major chord: open and bright. Lower case (ii, iii, vi) is minor: darker. With a "
      "small circle (vii\x01) it is diminished: tense.",
      "Mayúscula (I, IV, V): acorde mayor, abierto y luminoso. Minúscula (ii, iii, vi): menor, más oscuro. Con "
      "circulito (vii\x01): disminuido, tenso.",
      "Groß (I, IV, V) heißt Dur: offen und hell. Klein (ii, iii, vi) heißt Moll: dunkler. Mit Kringel (vii\x01): "
      "vermindert, gespannt.",
      "Majuscule (I, IV, V) : accord majeur, ouvert et lumineux. Minuscule (ii, iii, vi) : mineur, plus sombre. "
      "Avec un rond (vii\x01) : diminué, tendu."},
     0, 3, 0, {I_, vi_, vii_}},
    {{"I sette accordi", "The seven chords", "Los siete acordes", "Die sieben Akkorde", "Les sept accords"},
     {"Su ogni grado della scala impila una nota sì e una no: ecco la famiglia della tonalità, I ii iii IV V vi "
      "vii\x01. Quasi tutte le canzoni usano soprattutto questi.",
      "Stack every other note of the scale on each step and you get the key's family: I ii iii IV V vi vii\x01. "
      "Most songs use mostly these.",
      "Sobre cada grado de la escala apila una nota sí y otra no: es la familia de la tonalidad, I ii iii IV V vi "
      "vii\x01. Casi todas las canciones usan sobre todo estos.",
      "Schichte auf jeder Stufe jeden zweiten Ton der Tonleiter: das ist die Familie der Tonart, I ii iii IV V vi "
      "vii\x01. Fast alle Lieder nutzen vor allem diese.",
      "Sur chaque degré, empile une note sur deux de la gamme : voilà la famille du ton, I ii iii IV V vi vii\x01. "
      "La plupart des chansons utilisent surtout ceux-là."},
     0, 8, 0, {I_, ii_, iii_, IV_, V_, vi_, vii_, I_}},
    {{"I: casa", "I: home", "I: casa", "I: Zuhause", "I : la maison"},
     {"I è casa: dove la canzone riposa e di solito finisce. Tutti gli altri accordi si sentono rispetto a lui. "
      "Quando un brano sembra finito, è su I.",
      "I is home: where a song rests and usually ends. Every other chord is heard against it. When a song feels "
      "finished, it is on I.",
      "I es casa: donde la canción descansa y suele terminar. Los demás acordes se oyen respecto a él. Cuando una "
      "canción suena terminada, está en I.",
      "I ist Zuhause: hier ruht das Lied und endet meist. Alle anderen Akkorde hört man im Verhältnis zu ihm. Klingt "
      "ein Lied fertig, ist es auf I.",
      "I est la maison : la chanson s'y repose et y finit souvent. Les autres accords s'entendent par rapport à lui. "
      "Quand un morceau semble fini, il est sur I."},
     0, 3, 2, {V_, I_, I_}},
    {{"V: la spinta verso casa", "V: the pull home", "V: el tirón a casa", "V: der Zug nach Hause",
      "V : l'appel de la maison"},
     {"V è tensione: le sue note tendono a I, una a solo un semitono (Si-Do in Do). Dopo V ti aspetti I. V7 "
      "aggiunge una nota e spinge di più.",
      "V is tension: its notes lean toward I, one only a semitone away (B to C in C). After V you expect I. V7 adds "
      "a note and pulls harder.",
      "V es tensión: sus notas tienden a I, una a solo un semitono (Si-Do en Do). Tras V esperas I. V7 añade una "
      "nota y empuja más.",
      "V ist Spannung: seine Töne streben zu I, einer nur einen Halbton entfernt (H zu C in C). Nach V erwartet man "
      "I. V7 hat einen Ton mehr und zieht stärker.",
      "V, c'est la tension : ses notes penchent vers I, l'une à un demi-ton (Si-Do en Do). Après V, on attend I. "
      "V7 ajoute une note et tire plus fort."},
     0, 4, 2, {V_, I_, {7, 4, CH_DOM7, "V7"}, I_}},
    {{"IV: lo slancio", "IV: the lift", "IV: el impulso", "IV: der Aufschwung", "IV : l'envol"},
     {"IV si allontana da casa senza tensione: uno slancio, un sospiro. IV-I è l'«Amen» degli inni. IV-V prepara "
      "il ritornello.",
      "IV moves away from home without tension: a lift, a sigh. IV to I is the \"Amen\" at the end of hymns. IV to "
      "V builds up to a chorus.",
      "IV se aleja de casa sin tensión: un impulso, un suspiro. IV-I es el «Amén» de los himnos. IV-V prepara el "
      "estribillo.",
      "IV entfernt sich ohne Spannung von zu Hause: ein Aufschwung, ein Seufzer. IV-I ist das \"Amen\" der Choräle. "
      "IV-V führt zum Refrain.",
      "IV s'éloigne de la maison sans tension : un élan, un soupir. IV-I, c'est l'« Amen » des cantiques. IV-V "
      "prépare le refrain."},
     0, 4, 2, {IV_, I_, IV_, V_}},
    {{"vi: il gemello triste", "vi: the sad twin", "vi: el gemelo triste", "vi: der traurige Zwilling",
      "vi : le jumeau triste"},
     {"vi ha due note in comune con I: è casa con un'ombra. Spesso un brano va da I a vi per scurirsi. La casa di "
      "una tonalità minore è il vi della sua gemella maggiore.",
      "vi shares two notes with I: home with a shadow. Songs often go from I to vi to darken. A minor key's home is "
      "the vi of its major twin.",
      "vi comparte dos notas con I: es casa con sombra. Muchas canciones van de I a vi para oscurecerse. La casa de "
      "una tonalidad menor es el vi de su gemela mayor.",
      "vi teilt zwei Töne mit I: Zuhause mit Schatten. Oft geht ein Lied von I nach vi, um dunkler zu werden. Das "
      "Zuhause einer Molltonart ist die vi ihres Dur-Zwillings.",
      "vi partage deux notes avec I : la maison avec une ombre. On passe souvent de I à vi pour assombrir. La maison "
      "d'un ton mineur est le vi de son jumeau majeur."},
     0, 4, 2, {I_, vi_, IV_, V_}},
    {{"ii e iii", "ii and iii", "ii y iii", "ii und iii", "ii et iii"},
     {"ii è un IV più morbido: porta a V (ii-V-I, il cuore del jazz). iii è più raro: un passo tra I e vi, o una "
      "sorpresa gentile.",
      "ii is a softer IV: it leads to V (ii V I, the heart of jazz). iii is rarer: a step between I and vi, or a "
      "gentle surprise.",
      "ii es un IV más suave: lleva a V (ii-V-I, el corazón del jazz). iii es más raro: un paso entre I y vi, o una "
      "sorpresa suave.",
      "ii ist eine weichere IV: sie führt zu V (ii-V-I, das Herz des Jazz). iii ist seltener: ein Schritt zwischen "
      "I und vi oder eine sanfte Überraschung.",
      "ii est un IV plus doux : il mène à V (ii-V-I, le cœur du jazz). iii est plus rare : un pas entre I et vi, ou "
      "une douce surprise."},
     0, 5, 3, {ii_, V_, I_, iii_, vi_}},
    {{"Quattro accordi, mille canzoni", "Four chords, many songs", "Cuatro acordes, mil canciones",
      "Vier Akkorde, viele Lieder", "Quatre accords, mille chansons"},
     {"I V vi IV attraversa centinaia di successi. Partendo da vi (vi IV I V) gli stessi accordi suonano minori. "
      "In CANZONI, SAVE li trova nelle tue.",
      "I V vi IV runs through hundreds of hits. Start it from vi (vi IV I V) and the same chords sound minor. In "
      "SONGS, SAVE finds them in yours.",
      "I V vi IV recorre cientos de éxitos. Empezando por vi (vi IV I V) los mismos acordes suenan menores. En "
      "CANCIONES, SAVE los busca en las tuyas.",
      "I V vi IV steckt in Hunderten Hits. Fang bei vi an (vi IV I V), und dieselben Akkorde klingen nach Moll. In "
      "LIEDER findet SAVE sie in deinen.",
      "I V vi IV traverse des centaines de tubes. En partant de vi (vi IV I V), les mêmes accords sonnent mineurs. "
      "Dans CHANSONS, SAVE les trouve dans les tiennes."},
     0, 4, 4, {I_, V_, vi_, IV_}},
    {{"Tonalità minori", "Minor keys", "Tonalidades menores", "Molltonarten", "Tons mineurs"},
     {"In minore la casa è i (minuscolo) e si conta da lì: i iv v VI VII III. VII-i è il modo in cui rock e pop "
      "chiudono spesso in minore.",
      "In a minor key, home is i (lower case) and you count from there: i iv v VI VII III. VII to i is how rock and "
      "pop often close in minor.",
      "En menor la casa es i (minúscula) y se cuenta desde ahí: i iv v VI VII III. VII-i es como el rock y el pop "
      "suelen cerrar en menor.",
      "In Moll ist i (klein) das Zuhause, und man zählt von dort: i iv v VI VII III. VII-i ist, wie Rock und Pop in "
      "Moll oft schließen.",
      "En mineur, la maison est i (minuscule) et on compte à partir de là : i iv v VI VII III. VII-i, c'est la "
      "cadence du rock et de la pop en mineur."},
     1, 4, 4, {{0, 0, CH_MIN, "i"}, {8, 5, CH_MAJ, "VI"}, {10, 6, CH_MAJ, "VII"}, {0, 0, CH_MIN, "i"}}},
    {{"Fuori dalla tonalità", "Outside the key", "Fuera de la tonalidad", "Außerhalb der Tonart", "Hors du ton"},
     {"Un bemolle (bVII, bVI) è un accordo preso in prestito dal minore: bVII in un brano maggiore suona deciso, "
      "rock. V/V (\"cinque del cinque\") è un accordo che punta a quello dopo.",
      "A flat (bVII, bVI) is a chord borrowed from the minor key: bVII in a major song sounds bold, rock. V/V "
      "(\"five of five\") is a chord that points at the next one.",
      "Un bemol (bVII, bVI) es un acorde prestado del menor: bVII en una canción mayor suena firme, rock. V/V "
      "(\"cinco del cinco\") es un acorde que apunta al siguiente.",
      "Ein b (bVII, bVI) ist ein aus Moll geliehener Akkord: bVII in einem Dur-Lied klingt kräftig, rockig. V/V "
      "(\"Fünf von Fünf\") ist ein Akkord, der auf den nächsten zeigt.",
      "Un bémol (bVII, bVI), c'est un accord emprunté au mineur : bVII dans une chanson majeure sonne franc, rock. "
      "V/V (« cinq de cinq ») est un accord qui vise le suivant."},
     0, 5, 3, {I_, {10, 6, CH_MAJ, "bVII"}, IV_, {2, 1, CH_MAJ, "V/V"}, V_}},
    {{"Settime e cifre", "Sevenths and figures", "Séptimas y cifras", "Septimen und Ziffern", "Septièmes et chiffres"},
     {"Un 7 (V7, ii7) aggiunge una quarta nota: più colore, stesso ruolo. Cifre come 6 o 64 (I6, V64) dicono che "
      "nel basso c'è un'altra nota dell'accordo.",
      "A 7 (V7, ii7) adds a fourth note: more colour, same job. Figures like 6 or 64 (I6, V64) say another note of "
      "the chord is in the bass.",
      "Un 7 (V7, ii7) añade una cuarta nota: más color, el mismo papel. Cifras como 6 o 64 (I6, V64) dicen que en "
      "el bajo hay otra nota del acorde.",
      "Eine 7 (V7, ii7) fügt einen vierten Ton hinzu: mehr Farbe, gleiche Aufgabe. Ziffern wie 6 oder 64 (I6, V64) "
      "heißen: ein anderer Akkordton liegt im Bass.",
      "Un 7 (V7, ii7) ajoute une quatrième note : plus de couleur, même rôle. Des chiffres comme 6 ou 64 (I6, V64) "
      "disent qu'une autre note de l'accord est à la basse."},
     0, 3, 0, {{2, 1, CH_MIN7, "ii7"}, {7, 4, CH_DOM7, "V7"}, {0, 0, CH_MAJ7, "Imaj7"}}},
    {{"Allenati", "Practise", "Practica", "Üben", "Entraîne-toi"},
     {"Sentilo: in CANZONI, LFO (orecchio). Pensalo: in CANZONI premi ARP due volte, ti chiede ogni accordo per "
      "numero. Nominalo: il QUIZ ha domande sui gradi.",
      "Hear it: LFO in SONGS (ear training). Think it: press ARP twice in SONGS and it asks you for each chord by "
      "numeral. Name it: the QUIZ asks about degrees.",
      "Óyelo: en CANCIONES, LFO (oído). Piénsalo: en CANCIONES pulsa ARP dos veces y te pide cada acorde por número. "
      "Nómbralo: el QUIZ pregunta por grados.",
      "Hören: in LIEDER mit LFO (Gehör). Denken: in LIEDER zweimal ARP, dann fragt es jeden Akkord als Stufe ab. "
      "Benennen: das QUIZ fragt nach Stufen.",
      "L'entendre : dans CHANSONS, LFO (oreille). Le penser : dans CHANSONS, appuie deux fois sur ARP, il te demande "
      "chaque accord par son chiffre. Le nommer : le QUIZ pose des questions sur les degrés."},
     0, 4, 0, {I_, vi_, IV_, V_}},
};

static struct {
    uint32_t t0, step;                        /* the progression playing: from t0, a chord a step */
    int8_t page;                              /* the page the search was done for (-1 none yet) */
} nm = {0, 0, -1};

static nt_t nm_tonic(void) { return ROOTS[ui.root[T_NUMERALS]]; }
static nt_t nm_root(const nchord_t *c) { return spell(nm_tonic(), c->semi, c->let); }
static uint16_t nm_mask(const nchord_t *c)
{
    uint16_t m = 0;
    int i, r = pc_of(nm_root(c));
    for (i = 0; i < CHORDS[c->type].n; i++)
        m |= (uint16_t)PCB((r + CHORDS[c->type].semi[i]) % 12);
    return m;
}

/* the page's progression in the player's songs (their own first): the first place it is found */
static void nm_find(int it)
{
    const npage_t *p = &NUMS[it];
    int i, k;
    sgf.n = sgf.npat = 0;
    sgf.from = 0xFFFFu;
    nm.page = (int8_t)it;
    if (p->npat < 2)
        return;
    for (k = 0; k < p->npat; k++) {           /* as the search compares: in the parent major (a minor key: from 9) */
        const nchord_t *c = &NUMS[it].ch[k];
        uint8_t tone[4];
        int j;
        for (j = 0; j < CHORDS[c->type].n && j < 4; j++)
            tone[j] = (uint8_t)((c->semi + CHORDS[c->type].semi[j]) % 12);
        sgf.pat[sgf.npat++] = sg_sig(tone, j, c->semi, p->minor ? 9 : 0);
    }
    for (i = 0; i < sg_count() && !sgf.n; i++)
        sg_find_in(i);
}

static void nm_draw(const char *r)
{
    int it = ui.item[T_NUMERALS], i, now = -1;
    const npage_t *p = &NUMS[it];
    nt_t tn = nm_tonic();
    char t[64], u[32];
    int32_t w;
    if (nm.page != it)
        nm_find(it);
    t1_strcpy(t, r);                          /* (the topic's name is long: only the page number beside it) */
    {
        const char *n = t;
        while (*n && *n != '/')
            n++;
        while (n > t && n[-1] != ' ')
            n--;
        ui_header(TOPIC_NAME[th_ui][T_NUMERALS], n, C_GRAY);
    }
    t1_tx(8, 29, fit_font(TR(p->title), 224), TR(p->title), C_WHITE);
    para(8, 52, 226, TR(p->text), C_HI, 122);
    if (nm.step && (int32_t)(plat_ms() - nm.t0) >= 0) {   /* the chord playing */
        uint32_t k = (plat_ms() - nm.t0) / nm.step;
        if (k < p->n)
            now = (int)k;
    }
    w = 224 / p->n;
    for (i = 0; i < p->n; i++) {              /* the progression: numerals, and their letters in this key */
        const nchord_t *c = &p->ch[i];
        int32_t x = 8 + i * w;
        const felucca_font_t *f = t1_tw(&FONT_B, c->num) <= w - 4 ? &FONT_B : &FONT_XS;
        if (i == now)
            cv_rect(x, 126, w - 4, 38, COL_SELBG);
        t1_tx(x + (w - 4 - t1_tw(f, c->num)) / 2, 128, f, c->num, i == now ? C_WHITE : COL_ROOT);
        chord_symbol(c->type, nm_root(c), u);
        t1_fit(u, &FONT_XS, w - 4);
        t1_tx(x + (w - 4 - t1_tw(&FONT_XS, u)) / 2, 148, &FONT_XS, u, i == now ? C_WHITE : C_GRAY);
    }
    if (sgf.n) {                              /* in a song of the player's */
        char a[64], b[64];
        uint16_t col;
        sg_find_row(0, a, b, &col);
        t1_strcpy(t, a);
        str_cat(t, " - ");
        str_cat(t, b);
        t1_fit(t, &FONT_XS, 224 - t1_tw(&FONT_XS, "ARP ") - 4);
        t1_tx(t1_tx(8, 169, &FONT_XS, "ARP ", COL_SCALE) + 2, 169, &FONT_XS, t, C_HI);
    }
    if (now >= 0) {
        mk.scale = nm_mask(&p->ch[now]);
        mk.root = (uint16_t)PCB(pc_of(nm_root(&p->ch[now])));
    } else {
        scale_t s = SCALES[p->minor ? SC_NATMINOR : SC_MAJOR];
        for (i = 0; i < s.n; i++)
            mk.scale |= (uint16_t)PCB((pc_of(tn) + s.semi[i]) % 12);
        mk.root = (uint16_t)PCB(pc_of(tn));
    }
    hint(S(S_H_NUMERALS));
}

/* PLAY: the progression, a chord every two beats, the root low under it */
static void nm_play(void)
{
    const npage_t *p = &NUMS[ui.item[T_NUMERALS]];
    uint32_t t = plat_ms() + 30u, st = step_ms() * 2u + 300u;
    int i, k;
    nm.t0 = t, nm.step = st;
    for (i = 0; i < p->n; i++, t += st) {
        const nchord_t *c = &p->ch[i];
        int b = base_midi(nm_root(c));
        for (k = 0; k < CHORDS[c->type].n; k++)
            play_note(t, b + CHORDS[c->type].semi[k], st - 60u, 80);
        play_note(t, b - 12, st - 60u, 90);
    }
}

/* ARP: the song it was found in, open there */
static void nm_open(void)
{
    if (!sgf.n || !sg_open(sgf.at[0].song))
        return;
    sg.sel = sgf.at[0].song;
    sg_section(sgf.at[0].sec < sg.nsec ? sgf.at[0].sec : 0);
    sg.ch = sgf.at[0].ch < sg.nch ? sgf.at[0].ch : 0;
    sg.nlast = 0;
    ui_go(SCR_SONG);
    {
        sch_t c;
        sg_chord(sg.ch, &c);
        sg_shape(&c);
    }
}
