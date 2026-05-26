/*
 * Verify host_sync step rate when time:Position events ARE injected.
 *
 * Two scenarios:
 *   A) Fractional beat:  beat = frame * bpm / (60 * SR)   (continuous)
 *   B) Quantised beat:   beat = floor(frame * bpm / (60 * SR))  (integer only)
 *
 * If the math is right, both should produce the same step rate as the
 * no-events test in divhost.c. If scenario (B) collapses fast divisions
 * onto the 1/4 rate, it confirms the integer-quantisation hypothesis.
 *
 * Build: gcc -O2 -Wall -o /tmp/divevents test/divevents.c -ldl -lm
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

#define SR     48000.0
#define BLOCK  256
#define SECS   4
#define NPORTS 42
#define BUFSZ  4096

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

static LV2_URID U_seq, U_pos, U_bpm, U_beat, U_speed, U_frame;

static void
write_position(LV2_Atom_Forge* forge, double bpm, double beat, double speed, int64_t frame)
{
    LV2_Atom_Forge_Frame obj_frame;
    lv2_atom_forge_frame_time(forge, 0);
    lv2_atom_forge_object(forge, &obj_frame, 0, U_pos);

    lv2_atom_forge_key(forge, U_bpm);
    lv2_atom_forge_float(forge, (float)bpm);

    lv2_atom_forge_key(forge, U_beat);
    lv2_atom_forge_double(forge, beat);

    lv2_atom_forge_key(forge, U_speed);
    lv2_atom_forge_float(forge, (float)speed);

    lv2_atom_forge_key(forge, U_frame);
    lv2_atom_forge_long(forge, frame);

    lv2_atom_forge_pop(forge, &obj_frame);
}

static int
run_block(const LV2_Descriptor* d, LV2_Handle inst,
          uint8_t* atom_buf, double bpm, double beat, double speed, int64_t frame,
          float* outL)
{
    LV2_Atom_Forge forge;
    LV2_URID_Map map = { NULL, urid_map };
    lv2_atom_forge_init(&forge, &map);
    lv2_atom_forge_set_buffer(&forge, atom_buf, BUFSZ);

    LV2_Atom_Forge_Frame seq_frame;
    lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
    write_position(&forge, bpm, beat, speed, frame);
    lv2_atom_forge_pop(&forge, &seq_frame);

    d->run(inst, BLOCK);

    int trans = 0;
    static int prev_high = 0;
    for (int i = 0; i < BLOCK; ++i) {
        int high = outL[i] > 0.5f;
        if (high != prev_high) ++trans;
        prev_high = high;
    }
    return trans;
}

static double
run_scenario(const LV2_Descriptor* d, int div, int quantised, float* ports)
{
    LV2_URID_Map map = { NULL, urid_map };
    LV2_Feature  map_feat = { LV2_URID__map, &map };
    const LV2_Feature* features[] = { &map_feat, NULL };

    float inL[BLOCK], inR[BLOCK], outL[BLOCK], outR[BLOCK];
    for (int i = 0; i < BLOCK; ++i) { inL[i] = inR[i] = 1.0f; }
    uint8_t atom_buf[BUFSZ];

    ports[7] = (float)div;

    LV2_Handle inst = d->instantiate(d, SR, ".", features);
    d->connect_port(inst, 0, atom_buf);   /* time_in */
    d->connect_port(inst, 1, inL);
    d->connect_port(inst, 2, inR);
    d->connect_port(inst, 3, outL);
    d->connect_port(inst, 4, outR);
    for (int p = 5; p < NPORTS; ++p) d->connect_port(inst, (uint32_t)p, &ports[p]);
    float dummy_step = 0.0f;
    d->connect_port(inst, 8, &dummy_step);
    d->activate(inst);

    const double bpm = 120.0;
    const long total = (long)(SR * SECS);
    int transitions = 0;
    int64_t frame = 0;
    while (frame < total) {
        double real_beat = (double)frame * bpm / (60.0 * SR);
        double sent_beat = quantised ? floor(real_beat) : real_beat;
        transitions += run_block(d, inst, atom_buf, bpm, sent_beat, 1.0, frame, outL);
        frame += BLOCK;
    }

    d->deactivate(inst);
    d->cleanup(inst);
    return (double)transitions / (double)SECS;
}

int
main(void)
{
    void* h = dlopen("./parkinsound-stepgate.lv2/stepgate.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    const LV2_Descriptor* (*lv2_descriptor)(uint32_t) =
        (const LV2_Descriptor* (*)(uint32_t))dlsym(h, "lv2_descriptor");
    const LV2_Descriptor* d = lv2_descriptor(0);

    /* Pre-register the URIs we use, matching the order the plug-in
     * registers them. (Order doesn't matter for correctness; the map
     * just needs to be consistent between host and plug-in.) */
    U_seq   = urid_map(NULL, LV2_ATOM__Sequence);
    urid_map(NULL, LV2_ATOM__Blank);
    urid_map(NULL, LV2_ATOM__Object);
    urid_map(NULL, LV2_ATOM__Float);
    urid_map(NULL, LV2_ATOM__Double);
    urid_map(NULL, LV2_ATOM__Int);
    urid_map(NULL, LV2_ATOM__Long);
    U_pos   = urid_map(NULL, "http://lv2plug.in/ns/ext/time#Position");
    U_beat  = urid_map(NULL, "http://lv2plug.in/ns/ext/time#beat");
    U_bpm   = urid_map(NULL, "http://lv2plug.in/ns/ext/time#beatsPerMinute");
    U_speed = urid_map(NULL, "http://lv2plug.in/ns/ext/time#speed");
    U_frame = urid_map(NULL, "http://lv2plug.in/ns/ext/time#frame");

    float ports[NPORTS] = {0};
    ports[5] = 0.0f;    /* sync_source = Host Sync */
    ports[6] = 120.0f;  /* tempo fallback */
    for (int s = 0; s < 16; ++s) {
        ports[9  + s * 2] = 1.0f;  /* all steps on */
        ports[10 + s * 2] = 0.0f;  /* tie=0 -> 2 transitions per step */
    }
    ports[41] = 1.0f;  /* enabled */

    static const char* names[6] = { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };

    printf("host_sync with time:Position events:\n\n");
    printf("                  FRACTIONAL beat            QUANTISED beat (integer only)\n");
    printf("division          measured   expected         measured   expected\n");
    printf("--------          --------   --------         --------   --------\n");
    static const double expect[6] = { 1.0, 2.0, 4.0, 8.0, 16.0, 32.0 };
    for (int div = 0; div < 6; ++div) {
        double rA = run_scenario(d, div, 0, ports);
        double rB = run_scenario(d, div, 1, ports);
        printf("  %-5s             %6.2f /s    %4.2f /s          %6.2f /s    %4.2f /s\n",
               names[div], rA, expect[div], rB, expect[div]);
    }

    dlclose(h);
    return 0;
}
