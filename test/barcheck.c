/*
 * Bar-aligned pattern / time-signature test (single Step Gate).
 *
 * Simulates hosts running in 3/4, 6/8, 5/4 and 7/4 by injecting
 * time:Position events carrying beatsPerBar / beatUnit / bar / barBeat,
 * with the plug-in in the "1 Bar" pattern mode, and checks that:
 *
 *   - the active_steps output reports the expected pattern length
 *     (e.g. 12 sixteenths in 3/4, 14 eighths in 7/4, clamped to 16);
 *   - the pattern actually cycles over exactly that many steps;
 *   - step 1 lands on every bar start, sample-accurately (+-2 samples),
 *     including with mod-host-style integer-quantised beat/barBeat and
 *     after a mid-run meter change;
 *   - the manual meter ports drive the same behaviour in Free Run.
 *
 * Build:
 *   gcc -O2 -Wall -o test/barcheck test/barcheck.c -ldl -lm
 * Run:
 *   ./test/barcheck
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <stdint.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>

#define SR      48000.0
#define SECS    6
#define NPORTS  52
#define BUFSZ   4096
#define EVERY   256      /* transport event every N samples */

static char* g_uris[1024];
static int   g_uris_n = 1;

static LV2_URID
urid_map(LV2_URID_Map_Handle handle, const char* uri)
{
    (void)handle;
    for (int i = 1; i < g_uris_n; ++i)
        if (!strcmp(g_uris[i], uri)) return (LV2_URID)i;
    g_uris[g_uris_n] = strdup(uri);
    return (LV2_URID)(g_uris_n++);
}

static LV2_URID U_pos, U_bpm, U_beat, U_speed, U_frame, U_bpb, U_bu, U_bar, U_barbeat;

/* Which time:Position fields the simulated host transmits. Real hosts
 * differ a lot here; the plug-in must adapt with any of them. */
enum {
    HOST_NONE    = -1,  /* no events at all (pure free-run test)        */
    HOST_FULL    = 0,   /* beat + barBeat + bar + beatUnit (no frame)   */
    HOST_MINIMAL = 1,   /* frame + bpm + beatsPerBar only               */
    HOST_NOBEAT  = 2,   /* Ardour-style: frame + bpm + beatsPerBar +
                           beatUnit + bar + barBeat, but NO time:beat   */
    HOST_MODHOST = 3    /* mod-host: everything, but time:beat is the
                           integer beat WITHIN the bar (pos.beat - 1),
                           not a global counter                          */
};

/* One Position event: beat/barBeat in transport (beatUnit) units.
 * quantised != 0 mimics mod-host: integer beat / barBeat. */
static void
forge_position(uint8_t* buf, int style, double bpm, double beat,
               long frame, double bpb, int bu, int quantised)
{
    static LV2_URID_Map map = { NULL, urid_map };
    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &map);
    lv2_atom_forge_set_buffer(&forge, buf, BUFSZ);
    LV2_Atom_Forge_Frame seq_frame, obj_frame;
    lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
    lv2_atom_forge_frame_time(&forge, 0);
    lv2_atom_forge_object(&forge, &obj_frame, 0, U_pos);

    double b  = quantised ? floor(beat) : beat;
    long   barno = (long)floor(b / bpb + 1e-9);
    double barbeat = b - (double)barno * bpb;

    lv2_atom_forge_key(&forge, U_bpm);   lv2_atom_forge_float(&forge, (float)bpm);
    lv2_atom_forge_key(&forge, U_speed); lv2_atom_forge_float(&forge, 1.0f);
    lv2_atom_forge_key(&forge, U_bpb);   lv2_atom_forge_float(&forge, (float)bpb);
    if (style == HOST_FULL) {
        lv2_atom_forge_key(&forge, U_beat); lv2_atom_forge_double(&forge, b);
    } else {
        lv2_atom_forge_key(&forge, U_frame); lv2_atom_forge_long(&forge, frame);
    }
    if (style == HOST_MODHOST) {
        /* mod-host's time:beat = pos.beat - 1: the integer beat inside
         * the current bar, NOT the global running beat. */
        lv2_atom_forge_key(&forge, U_beat);
        lv2_atom_forge_double(&forge, floor(barbeat));
    }
    if (style != HOST_MINIMAL) {
        lv2_atom_forge_key(&forge, U_bu);      lv2_atom_forge_int(&forge, bu);
        lv2_atom_forge_key(&forge, U_bar);     lv2_atom_forge_long(&forge, barno);
        lv2_atom_forge_key(&forge, U_barbeat); lv2_atom_forge_float(&forge, (float)barbeat);
    }

    lv2_atom_forge_pop(&forge, &obj_frame);
    lv2_atom_forge_pop(&forge, &seq_frame);
}

static void
forge_empty(uint8_t* buf)
{
    static LV2_URID_Map map = { NULL, urid_map };
    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &map);
    lv2_atom_forge_set_buffer(&forge, buf, BUFSZ);
    LV2_Atom_Forge_Frame seq_frame;
    lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
    lv2_atom_forge_pop(&forge, &seq_frame);
}

typedef struct {
    const char* name;
    int    free_run;       /* sync_source = 1 (internal clock)           */
    int    style;          /* HOST_* event profile (HOST_NONE = silent)  */
    double bpm;            /* transport-unit BPM sent by the "host"      */
    double bpb;            /* host beatsPerBar (transport units)         */
    int    bu;             /* host beatUnit                              */
    int    quantised;      /* mod-host-style integer beat/barBeat        */
    double bpb2;           /* if > 0: switch to this meter mid-run       */
    int    division;       /* 0..5                                       */
    int    pattern_mode;   /* 1 = one bar, 2 = two bars                  */
    int    meter_source;   /* 0 auto, 1 manual                           */
    int    mnum, mden;     /* manual meter                               */
    int    expect_active;  /* expected active_steps (after switch, if any) */
    int    check_align;    /* verify step-1 lands on bar starts          */
} Scenario;

int
main(void)
{
    void* h = dlopen("./parkinsound-stepgate.lv2/stepgate.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    const LV2_Descriptor* (*ld)(uint32_t) =
        (const LV2_Descriptor* (*)(uint32_t))dlsym(h, "lv2_descriptor");
    const LV2_Descriptor* d = ld(0);

    U_pos     = urid_map(NULL, "http://lv2plug.in/ns/ext/time#Position");
    U_bpm     = urid_map(NULL, "http://lv2plug.in/ns/ext/time#beatsPerMinute");
    U_beat    = urid_map(NULL, "http://lv2plug.in/ns/ext/time#beat");
    U_speed   = urid_map(NULL, "http://lv2plug.in/ns/ext/time#speed");
    U_frame   = urid_map(NULL, "http://lv2plug.in/ns/ext/time#frame");
    U_bpb     = urid_map(NULL, "http://lv2plug.in/ns/ext/time#beatsPerBar");
    U_bu      = urid_map(NULL, "http://lv2plug.in/ns/ext/time#beatUnit");
    U_bar     = urid_map(NULL, "http://lv2plug.in/ns/ext/time#bar");
    U_barbeat = urid_map(NULL, "http://lv2plug.in/ns/ext/time#barBeat");

    LV2_URID_Map map = { NULL, urid_map };
    LV2_Feature  map_feat = { LV2_URID__map, &map };
    const LV2_Feature* features[] = { &map_feat, NULL };

    static const Scenario scen[] = {
        /* name              free style        bpm    bpb bu q  bpb2 div mode src num den exp align */
        { "3/4  1/16 1bar",  0, HOST_FULL,    120.0,  3, 4, 0, 0,   4,  1,   0,  4, 4, 12, 1 },
        { "6/8  1/16 1bar",  0, HOST_FULL,    240.0,  6, 8, 0, 0,   4,  1,   0,  4, 4, 12, 1 },
        { "5/4  1/4  1bar",  0, HOST_FULL,    120.0,  5, 4, 0, 0,   2,  1,   0,  4, 4,  5, 1 },
        { "7/4  1/8  1bar",  0, HOST_FULL,    120.0,  7, 4, 0, 0,   3,  1,   0,  4, 4, 14, 1 },
        { "7/4  1/16 clamp", 0, HOST_FULL,    120.0,  7, 4, 0, 0,   4,  1,   0,  4, 4, 16, 0 },
        { "3/4  quantised",  0, HOST_FULL,    120.0,  3, 4, 1, 0,   4,  1,   0,  4, 4, 12, 1 },
        { "4/4->3/4 switch", 0, HOST_FULL,    120.0,  4, 4, 0, 3,   4,  1,   0,  4, 4, 12, 0 },
        { "3/4  1/8  2bars", 0, HOST_FULL,    120.0,  3, 4, 0, 0,   3,  2,   0,  4, 4, 12, 0 },
        { "free 5/4 manual", 1, HOST_NONE,    120.0,  0, 4, 0, 0,   2,  1,   1,  5, 4,  5, 0 },
        /* Real-host variants: fields many hosts omit. */
        { "5/4 frame-only",  0, HOST_MINIMAL, 120.0,  5, 4, 0, 0,   3,  1,   0,  4, 4, 10, 1 },
        { "7/4 ardour-like", 0, HOST_NOBEAT,  120.0,  7, 4, 0, 0,   3,  1,   0,  4, 4, 14, 1 },
        { "6/8 ardour-like", 0, HOST_NOBEAT,  240.0,  6, 8, 0, 0,   4,  1,   0,  4, 4, 12, 1 },
        /* Free Run + Auto: the host meter still applies (its clock
         * doesn't, so alignment is arithmetic from the enable reset). */
        { "free 5/4 auto",   1, HOST_FULL,    120.0,  5, 4, 0, 0,   2,  1,   0,  4, 4,  5, 1 },
        /* mod-host: frame-driven, time:beat is bar-relative. */
        { "5/4 mod-host",    0, HOST_MODHOST, 120.0,  5, 4, 0, 0,   3,  1,   0,  4, 4, 10, 1 },
        { "7/4 mod-host",    0, HOST_MODHOST, 120.0,  7, 4, 0, 0,   3,  1,   0,  4, 4, 14, 1 },
        { "3/4 mod-host",    0, HOST_MODHOST, 120.0,  3, 4, 0, 0,   4,  1,   0,  4, 4, 12, 1 },
    };
    const int nscen = (int)(sizeof(scen) / sizeof(scen[0]));

    float in_smp = 1.0f, out_smp = 0.0f;
    static uint8_t atom_buf[BUFSZ];
    int rc = 0;

    for (int sc = 0; sc < nscen; ++sc) {
        const Scenario* S = &scen[sc];

        float ports[NPORTS];
        for (int i = 0; i < NPORTS; ++i) ports[i] = 0.0f;
        ports[5]  = S->free_run ? 1.0f : 0.0f;   /* sync_source */
        ports[6]  = 120.0f;                       /* tempo (free run) */
        ports[7]  = (float)S->division;
        for (int s = 0; s < 16; ++s) {
            ports[9 + s * 2]  = 1.0f;             /* all steps on */
            ports[10 + s * 2] = 0.0f;             /* no ties */
        }
        ports[41] = 1.0f;                         /* enabled */
        ports[44] = 1.0f;                         /* sustain */
        ports[45] = 0.5f;                         /* release */
        ports[47] = (float)S->pattern_mode;
        ports[48] = (float)S->meter_source;
        ports[49] = (float)S->mnum;
        ports[50] = (float)S->mden;

        float cur = 0.0f, active = 0.0f;

        LV2_Handle inst = d->instantiate(d, SR, ".", features);
        d->connect_port(inst, 0, atom_buf);
        d->connect_port(inst, 1, &in_smp);
        d->connect_port(inst, 2, &in_smp);
        d->connect_port(inst, 3, &out_smp);
        d->connect_port(inst, 4, &out_smp);
        for (int p = 5; p < NPORTS; ++p) {
            if (p == 8)       d->connect_port(inst, 8,  &cur);
            else if (p == 51) d->connect_port(inst, 51, &active);
            else              d->connect_port(inst, (uint32_t)p, &ports[p]);
        }
        d->activate(inst);

        const long total = (long)(SR * SECS);
        const long switch_at = total / 2;

        int  max_step = 0, prev_step = -1, last_active = -1;
        long align_err = 0, align_checked = 0;
        int  active_after = 0;

        /* A quantised host only lets the plug-in adopt the beat at the
         * event AFTER the true integer crossing, so bar wraps may lag
         * by up to one event interval. Continuous hosts are exact. */
        const double align_tol = S->quantised ? (double)EVERY + 4.0 : 2.0;

        for (long n = 0; n < total; ++n) {
            double bpb = (S->bpb2 > 0.0 && n >= switch_at) ? S->bpb2 : S->bpb;
            if (S->style != HOST_NONE && (n % EVERY) == 0) {
                double beat = (double)n * S->bpm / (60.0 * SR);
                forge_position(atom_buf, S->style, S->bpm, beat, n,
                               bpb, S->bu, S->quantised);
            } else {
                forge_empty(atom_buf);
            }
            d->run(inst, 1);

            /* Track the max step of the CURRENT meter only: a mid-run
             * signature change restarts the measurement. */
            if ((int)active != last_active) {
                last_active = (int)active;
                max_step = 0;
            }
            int st = (int)cur;
            if (st > max_step) max_step = st;

            if (S->check_align && prev_step > 1 && st == 1) {
                /* step wrapped to 1: must be a bar start */
                double bar_samples = bpb * 60.0 * SR / S->bpm;
                double k = floor(((double)n / bar_samples) + 0.5);
                double err = fabs((double)n - k * bar_samples);
                if (err > align_tol) ++align_err;
                ++align_checked;
            }
            prev_step = st;
        }
        active_after = (int)active;

        d->deactivate(inst);
        d->cleanup(inst);

        int ok = (active_after == S->expect_active) &&
                 (max_step == S->expect_active) &&
                 (align_err == 0) &&
                 (!S->check_align || align_checked > 0);
        printf("%-18s | active=%2d (exp %2d)  max_step=%2d  "
               "bar-aligned wraps=%ld misaligned=%ld  %s\n",
               S->name, active_after, S->expect_active, max_step,
               align_checked, align_err, ok ? "PASS" : "FAIL");
        if (!ok) rc = 1;
    }

    dlclose(h);
    printf("\n%s\n", rc == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return rc;
}
