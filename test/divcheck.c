/*
 * Plug-in division sanity test.
 *
 * Loads ../parkinsound-stepgate.lv2/stepgate.so, instantiates the
 * plug-in, enables every step with tie=0 (so each step produces one
 * gate transition), feeds a constant DC signal, and counts how often
 * the gate crosses 0.5 per second for every division setting. The
 * expected ratio between adjacent divisions is exactly 2x, and 32x
 * between 1/1 and 1/32.
 *
 * Build:
 *   gcc -O2 -Wall -o test/divcheck test/divcheck.c -ldl -lm
 * Run:
 *   ./test/divcheck
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>

#define SR     48000.0
#define BLOCK  256
#define SECS   4
#define NPORTS 42

static char* g_uris[1024];
static int   g_uris_n = 1;

static LV2_URID
urid_map(LV2_URID_Map_Handle handle, const char* uri)
{
    (void)handle;
    for (int i = 1; i < g_uris_n; ++i) {
        if (!strcmp(g_uris[i], uri)) return (LV2_URID)i;
    }
    g_uris[g_uris_n] = strdup(uri);
    return (LV2_URID)(g_uris_n++);
}

int
main(void)
{
    void* h = dlopen("./parkinsound-stepgate.lv2/stepgate.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    const LV2_Descriptor* (*lv2_descriptor)(uint32_t) =
        (const LV2_Descriptor* (*)(uint32_t))dlsym(h, "lv2_descriptor");
    const LV2_Descriptor* d = lv2_descriptor(0);

    LV2_URID_Map map = { NULL, urid_map };
    LV2_Feature  map_feat = { LV2_URID__map, &map };
    const LV2_Feature* features[] = { &map_feat, NULL };

    /* One block of buffers, reused. */
    float audio_in_l [BLOCK], audio_in_r [BLOCK];
    float audio_out_l[BLOCK], audio_out_r[BLOCK];
    for (int i = 0; i < BLOCK; ++i) {
        audio_in_l[i] = 1.0f;  /* DC: out = gate */
        audio_in_r[i] = 1.0f;
    }

    /* Build empty atom sequence header. */
    struct { uint32_t size; uint32_t type; uint32_t unit; uint32_t pad; } seq;
    seq.size = sizeof(seq) - sizeof(uint32_t) * 2; /* atom body size */
    seq.type = urid_map(NULL, "http://lv2plug.in/ns/ext/atom#Sequence");
    seq.unit = 0;
    seq.pad  = 0;

    /* Allocate control ports. */
    float ports[NPORTS];
    for (int i = 0; i < NPORTS; ++i) ports[i] = 0.0f;

    /* Sync = 1 (Free Run), tempo = 120, division = 0..5 will be set per run. */
    ports[5] = 1.0f;   /* sync_source = Free Run */
    ports[6] = 120.0f; /* tempo */
    /* All 16 steps ON, tie = 0 (interrupted -> one transition per step). */
    for (int s = 0; s < 16; ++s) {
        ports[9  + s * 2] = 1.0f; /* step_N_on */
        ports[10 + s * 2] = 0.0f; /* step_N_tie */
    }
    ports[41] = 1.0f; /* enabled */

    printf("Division | gate transitions per second (expected)\n");
    printf("---------+----------------------------------------\n");
    static const char* names[6]   = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
    static const double expected[6] = {
        /* steps/sec at 120 BPM = (bpm/60) / div_factor; each step has one
         * 1->0 and one 0->1 transition with tie=0 -> 2 transitions/step.
         * For 1/16: (120/60)/0.25 = 8 steps/sec -> 16 transitions/sec. */
        2.0 * 0.5,   /* 1/1   : 0.5 step/s ->  1 transition/s */
        2.0 * 1.0,   /* 1/2   : 1   step/s ->  2 transition/s */
        2.0 * 2.0,   /* 1/4   : 2   step/s ->  4 transition/s */
        2.0 * 4.0,   /* 1/8   : 4   step/s ->  8 transition/s */
        2.0 * 8.0,   /* 1/16  : 8   step/s -> 16 transition/s */
        2.0 * 16.0,  /* 1/32  : 16  step/s -> 32 transition/s */
    };

    for (int div = 0; div < 6; ++div) {
        ports[7] = (float)div;

        LV2_Handle inst = d->instantiate(d, SR, ".", features);
        d->connect_port(inst, 0, &seq);
        d->connect_port(inst, 1, audio_in_l);
        d->connect_port(inst, 2, audio_in_r);
        d->connect_port(inst, 3, audio_out_l);
        d->connect_port(inst, 4, audio_out_r);
        for (int p = 5; p < NPORTS; ++p) d->connect_port(inst, (uint32_t)p, &ports[p]);
        float dummy_step_out = 0.0f;
        d->connect_port(inst, 8, &dummy_step_out);
        d->activate(inst);

        long total_samples = (long)(SR * SECS);
        long produced = 0;
        int transitions = 0;
        float prev = 0.0f;
        int prev_high = 0;
        while (produced < total_samples) {
            d->run(inst, BLOCK);
            for (int i = 0; i < BLOCK; ++i) {
                int high = audio_out_l[i] > 0.5f;
                if (high != prev_high) ++transitions;
                prev_high = high;
                prev = audio_out_l[i];
            }
            produced += BLOCK;
        }
        (void)prev;

        d->deactivate(inst);
        d->cleanup(inst);

        double per_sec = (double)transitions / (double)SECS;
        printf("  %-5s  | %6.2f /s   (expected %.2f /s)\n",
               names[div], per_sec, expected[div]);
    }

    dlclose(h);
    return 0;
}
