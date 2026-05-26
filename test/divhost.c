/*
 * Verify host_sync step rate for every division, WITHOUT injecting
 * any time:Position events. With no events, host_bpm stays 0 and the
 * plug-in falls back to tempo_ctrl - so this is the cleanest test of
 * the host_sync math (host_beat integrated by beat_inc, seq_pos =
 * host_beat / step_in_beats).
 *
 * Build:  gcc -O2 -Wall -o /tmp/divhost test/divhost.c -ldl -lm
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
#define NPORTS 47

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

    float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];
    for (int i = 0; i < BLOCK; ++i) { inL[i] = inR[i] = 1.0f; }

    struct { uint32_t size; uint32_t type; uint32_t unit; uint32_t pad; } seq;
    seq.type = urid_map(NULL, "http://lv2plug.in/ns/ext/atom#Sequence");
    seq.size = sizeof(seq) - 8;
    seq.unit = 0;
    seq.pad  = 0;

    float ports[NPORTS] = {0};
    ports[5] = 0.0f;    /* sync_source = Host Sync */
    ports[6] = 120.0f;  /* tempo (fallback when host_bpm == 0) */
    /* Every step ON with tie=0 -> 2 gate transitions per step. */
    for (int s = 0; s < 16; ++s) {
        ports[9  + s * 2] = 1.0f;
        ports[10 + s * 2] = 0.0f;
    }
    ports[41] = 1.0f;   /* enabled */
    ports[42] = 1.0f;   /* depth */
    ports[43] = 0.0f;   /* attack */
    ports[44] = 0.0f;   /* decay */
    ports[45] = 1.0f;   /* sustain */
    ports[46] = 0.5f;   /* release -> approximates the legacy 50% gate */

    static const char* names[6]     = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
    static const double expected[6] = { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };

    printf("host_sync mode, no time:Position events, tempo=120 BPM:\n");
    printf("division | measured rate | expected\n");
    printf("---------+---------------+---------\n");

    for (int div = 0; div < 6; ++div) {
        ports[7] = (float)div;

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

        long total = (long)(SR * SECS);
        long produced = 0;
        int  transitions = 0;
        int  prev_high = 0;
        while (produced < total) {
            d->run(inst, BLOCK);
            for (int i = 0; i < BLOCK; ++i) {
                int high = outL[i] > 0.5f;
                if (high != prev_high) ++transitions;
                prev_high = high;
            }
            produced += BLOCK;
        }

        d->deactivate(inst);
        d->cleanup(inst);

        double per_sec = (double)transitions / (double)SECS;
        printf("  %-5s  |  %6.2f /s    | %5.2f /s\n",
               names[div], per_sec, expected[div]);
    }

    dlclose(h);
    return 0;
}
