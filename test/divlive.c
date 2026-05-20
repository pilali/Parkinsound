/*
 * Plug-in live-division-change test.
 *
 * Reproduces what mod-ui does when the user moves the Division knob
 * without bypassing the plug-in: we keep a single instance running,
 * keep enabled=1, keep the audio rolling, and just rewrite the value
 * pointed-to by the division control port mid-stream.
 *
 * The test counts gate transitions per second BEFORE and AFTER the
 * mid-run change. If the rate after the change matches the new
 * division, the plug-in DOES react to live division changes; if the
 * rate stays the same as before, there really is a bug.
 *
 * Build:
 *   gcc -O2 -Wall -o test/divlive test/divlive.c -ldl -lm
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

static int
run_window(const LV2_Descriptor* d, LV2_Handle inst,
           float* outL, double seconds)
{
    long total_samples = (long)(SR * seconds);
    long produced = 0;
    int  transitions = 0;
    int  prev_high = (outL[0] > 0.5f);
    while (produced < total_samples) {
        d->run(inst, BLOCK);
        for (int i = 0; i < BLOCK; ++i) {
            int high = outL[i] > 0.5f;
            if (high != prev_high) ++transitions;
            prev_high = high;
        }
        produced += BLOCK;
    }
    return transitions;
}

static const char* names[6] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
static const double expected_rate[6] = { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };

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

    float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];
    for (int i = 0; i < BLOCK; ++i) { inL[i] = inR[i] = 1.0f; }

    struct { uint32_t size; uint32_t type; uint32_t unit; uint32_t pad; } seq;
    seq.type = urid_map(NULL, "http://lv2plug.in/ns/ext/atom#Sequence");
    seq.size = sizeof(seq) - 8;
    seq.unit = 0;
    seq.pad  = 0;

    float ports[NPORTS] = {0};
    ports[5] = 1.0f;    /* sync_source = Free Run */
    ports[6] = 120.0f;  /* tempo */
    for (int s = 0; s < 16; ++s) {
        ports[9  + s * 2] = 1.0f;
        ports[10 + s * 2] = 0.0f;
    }
    ports[41] = 1.0f;   /* enabled */

    /* Pair (start_div, target_div) */
    struct { int from; int to; } pairs[] = {
        {4, 0}, /* 1/16 -> 1/1   (fast -> slow) */
        {0, 5}, /* 1/1  -> 1/32  (slow -> fast) */
        {3, 1}, /* 1/8  -> 1/2 */
        {2, 4}, /* 1/4  -> 1/16 */
    };

    printf("Live division change while enabled=1, never re-instantiated:\n");
    printf("from -> to | before rate | after rate (expected after)\n");
    printf("-----------+-------------+----------------------------\n");

    for (size_t k = 0; k < sizeof(pairs)/sizeof(pairs[0]); ++k) {
        int from = pairs[k].from;
        int to   = pairs[k].to;
        ports[7] = (float)from;

        LV2_Handle inst = d->instantiate(d, SR, ".", features);
        d->connect_port(inst, 0, &seq);
        d->connect_port(inst, 1, inL);
        d->connect_port(inst, 2, inR);
        d->connect_port(inst, 3, outL);
        d->connect_port(inst, 4, outR);
        for (int p = 5; p < NPORTS; ++p) d->connect_port(inst, (uint32_t)p, &ports[p]);
        float dummy_step_out = 0.0f;
        d->connect_port(inst, 8, &dummy_step_out);
        d->activate(inst);

        /* Warm up */
        run_window(d, inst, outL, 0.5);

        double before_sec = 2.0;
        int before_tx = run_window(d, inst, outL, before_sec);

        /* LIVE CHANGE: just write to the control port memory. */
        ports[7] = (float)to;

        double after_sec = (to == 0) ? 6.0 : 3.0; /* allow at least one full step */
        int after_tx = run_window(d, inst, outL, after_sec);

        d->deactivate(inst);
        d->cleanup(inst);

        double before_rate = before_tx / before_sec;
        double after_rate  = after_tx  / after_sec;
        printf("  %4s -> %-4s |  %5.2f /s  |  %5.2f /s  (expected %.2f /s)\n",
               names[from], names[to],
               before_rate, after_rate,
               expected_rate[to]);
    }

    dlclose(h);
    return 0;
}
