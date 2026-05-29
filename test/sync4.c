/*
 * Parkinsound Step Gate 4 - inter-channel synchronisation test.
 *
 * The whole point of the 4-channel plug-in is that its four voices are
 * locked together sample-for-sample. This test loads
 * ../parkinsound-stepgate4.lv2/stepgate4.so and checks two things:
 *
 *   A. With identical settings on all four channels the four output
 *      streams are bit-identical (perfect sync, trivially).
 *
 *   B. With different rhythmic divisions but a shared master beat, the
 *      rising edges of a slower channel land exactly on a subset of the
 *      faster channel's rising edges (phase-locked polyrhythm). A 1/8
 *      voice fires on every other 1/16 boundary, on the very same
 *      sample, demonstrating that the trigger origin is common.
 *
 * Build:
 *   gcc -O2 -Wall -o test/sync4 test/sync4.c -ldl -lm
 * Run:
 *   ./test/sync4
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
#define NPORTS 164
#define NCH    4
#define NSTEPS 16

/* Port layout mirror of stepgate4.c. */
#define P_TIME_IN     0
#define P_SYNC        1
#define P_TEMPO       2
#define P_ENABLED     3
#define P_IN_BASE     4
#define P_OUT_BASE    8
#define P_CH_BASE     12
#define CH_STRIDE     38
#define CH_DIVISION   0
#define CH_CURRENT    1
#define CH_ATTACK     2
#define CH_DECAY      3
#define CH_SUSTAIN    4
#define CH_RELEASE    5
#define CH_STEP_BASE  6

static int ch_port(int ch, int off) { return P_CH_BASE + ch * CH_STRIDE + off; }
static int ch_step_on(int ch, int m)  { return ch_port(ch, CH_STEP_BASE + m * 2); }
static int ch_step_tie(int ch, int m) { return ch_port(ch, CH_STEP_BASE + m * 2 + 1); }

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
    void* h = dlopen("./parkinsound-stepgate4.lv2/stepgate4.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    const LV2_Descriptor* (*lv2_descriptor)(uint32_t) =
        (const LV2_Descriptor* (*)(uint32_t))dlsym(h, "lv2_descriptor");
    const LV2_Descriptor* d = lv2_descriptor(0);

    LV2_URID_Map map = { NULL, urid_map };
    LV2_Feature  map_feat = { LV2_URID__map, &map };
    const LV2_Feature* features[] = { &map_feat, NULL };

    float in [NCH][BLOCK];
    float out[NCH][BLOCK];
    for (int c = 0; c < NCH; ++c)
        for (int i = 0; i < BLOCK; ++i) in[c][i] = 1.0f; /* DC: out = gate */

    struct { uint32_t size; uint32_t type; uint32_t unit; uint32_t pad; } seq;
    seq.size = sizeof(seq) - sizeof(uint32_t) * 2;
    seq.type = urid_map(NULL, "http://lv2plug.in/ns/ext/atom#Sequence");
    seq.unit = 0;
    seq.pad  = 0;

    float ports[NPORTS];
    for (int i = 0; i < NPORTS; ++i) ports[i] = 0.0f;
    float dummy_current[NCH];

    int rc = 0;

    /* ============================ Test A ============================ */
    /* All four channels identical -> outputs must be bit-identical. */
    {
        for (int i = 0; i < NPORTS; ++i) ports[i] = 0.0f;
        ports[P_SYNC]    = 1.0f;   /* free run */
        ports[P_TEMPO]   = 120.0f;
        ports[P_ENABLED] = 1.0f;
        for (int c = 0; c < NCH; ++c) {
            ports[ch_port(c, CH_DIVISION)] = 4.0f; /* 1/16 */
            ports[ch_port(c, CH_ATTACK)]   = 0.0f;
            ports[ch_port(c, CH_DECAY)]    = 0.0f;
            ports[ch_port(c, CH_SUSTAIN)]  = 1.0f;
            ports[ch_port(c, CH_RELEASE)]  = 0.5f;
            for (int m = 0; m < NSTEPS; ++m) {
                ports[ch_step_on(c, m)]  = 1.0f;
                ports[ch_step_tie(c, m)] = 0.0f;
            }
        }

        LV2_Handle inst = d->instantiate(d, SR, ".", features);
        d->connect_port(inst, P_TIME_IN, &seq);
        for (int c = 0; c < NCH; ++c) {
            d->connect_port(inst, P_IN_BASE  + c, in[c]);
            d->connect_port(inst, P_OUT_BASE + c, out[c]);
            d->connect_port(inst, (uint32_t)ch_port(c, CH_CURRENT), &dummy_current[c]);
        }
        /* Connect every remaining control port. */
        for (int p = 0; p < NPORTS; ++p) {
            int is_audio = (p >= P_IN_BASE && p < P_IN_BASE + 2 * NCH);
            int is_cur = 0;
            for (int c = 0; c < NCH; ++c) if (p == ch_port(c, CH_CURRENT)) is_cur = 1;
            if (p == P_TIME_IN || is_audio || is_cur) continue;
            d->connect_port(inst, (uint32_t)p, &ports[p]);
        }
        d->activate(inst);

        long total = (long)(SR * SECS), produced = 0;
        long mismatches = 0;
        while (produced < total) {
            d->run(inst, BLOCK);
            for (int i = 0; i < BLOCK; ++i) {
                for (int c = 1; c < NCH; ++c) {
                    if (out[c][i] != out[0][i]) ++mismatches;
                }
            }
            produced += BLOCK;
        }
        d->deactivate(inst);
        d->cleanup(inst);

        printf("Test A (identical channels bit-exact): %s (%ld mismatches)\n",
               mismatches == 0 ? "PASS" : "FAIL", mismatches);
        if (mismatches) rc = 1;
    }

    /* ============================ Test B ============================ */
    /* ch0 = 1/8, ch1 = 1/16. Stepping one sample at a time, every step
     * boundary on the slower channel (ch0) must coincide, on the exact
     * same sample, with a step boundary on the faster channel (ch1).
     *
     * We read the integer current_step outputs directly so the result
     * is independent of the anti-click gate smoothing. */
    {
        for (int i = 0; i < NPORTS; ++i) ports[i] = 0.0f;
        ports[P_SYNC]    = 1.0f;
        ports[P_TEMPO]   = 120.0f;
        ports[P_ENABLED] = 1.0f;
        int divs[NCH] = { 3, 4, 2, 1 }; /* 1/8, 1/16, 1/4, 1/2 */
        for (int c = 0; c < NCH; ++c) {
            ports[ch_port(c, CH_DIVISION)] = (float)divs[c];
            ports[ch_port(c, CH_SUSTAIN)]  = 1.0f;
            ports[ch_port(c, CH_RELEASE)]  = 0.5f;
            for (int m = 0; m < NSTEPS; ++m) {
                ports[ch_step_on(c, m)]  = 1.0f;
                ports[ch_step_tie(c, m)] = 0.0f;
            }
        }

        float cur[NCH];

        LV2_Handle inst = d->instantiate(d, SR, ".", features);
        d->connect_port(inst, P_TIME_IN, &seq);
        for (int c = 0; c < NCH; ++c) {
            d->connect_port(inst, P_IN_BASE  + c, in[c]);
            d->connect_port(inst, P_OUT_BASE + c, out[c]);
            d->connect_port(inst, (uint32_t)ch_port(c, CH_CURRENT), &cur[c]);
        }
        for (int p = 0; p < NPORTS; ++p) {
            int is_audio = (p >= P_IN_BASE && p < P_IN_BASE + 2 * NCH);
            int is_cur = 0;
            for (int c = 0; c < NCH; ++c) if (p == ch_port(c, CH_CURRENT)) is_cur = 1;
            if (p == P_TIME_IN || is_audio || is_cur) continue;
            d->connect_port(inst, (uint32_t)p, &ports[p]);
        }
        d->activate(inst);

        long total = (long)(SR * SECS);
        int prev_step[NCH];
        for (int c = 0; c < NCH; ++c) prev_step[c] = -1;
        long bound0 = 0, bound1 = 0, aligned = 0, unaligned = 0;

        for (long n = 0; n < total; ++n) {
            d->run(inst, 1);                       /* one sample per call */
            int boundary[NCH];
            for (int c = 0; c < NCH; ++c) {
                int s = (int)cur[c];
                boundary[c] = (prev_step[c] != -1 && s != prev_step[c]);
                prev_step[c] = s;
            }
            if (boundary[1]) ++bound1;
            if (boundary[0]) {
                ++bound0;
                if (boundary[1]) ++aligned; else ++unaligned;
            }
        }
        d->deactivate(inst);
        d->cleanup(inst);

        printf("Test B (1/8 boundaries align with 1/16 boundaries): %s\n",
               (unaligned == 0 && bound0 > 0) ? "PASS" : "FAIL");
        printf("        ch0(1/8) boundaries=%ld, ch1(1/16) boundaries=%ld, aligned=%ld, unaligned=%ld\n",
               bound0, bound1, aligned, unaligned);
        if (unaligned != 0 || bound0 == 0) rc = 1;
    }

    dlclose(h);
    printf("\n%s\n", rc == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return rc;
}
