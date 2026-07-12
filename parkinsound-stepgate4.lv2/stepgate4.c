/*
 * Parkinsound Step Gate 4 - 4-channel 16-step audio gate sequencer (LV2)
 *
 * This is the 4-voice sibling of the single Parkinsound Step Gate. It
 * exists to solve a synchronisation problem: when several independent
 * rhythmic-gate plug-ins are instantiated separately under mod-host /
 * mod-ui, there is no guarantee that their step clocks stay locked to
 * one another sample-for-sample. Different instances can drift, start
 * on different blocks, or resynchronise to the host transport at
 * slightly different moments.
 *
 * By folding four independent gate voices into a *single* plug-in we
 * guarantee perfect mutual synchronisation: all four channels are
 * processed inside the same run() call, advanced by the same shared
 * beat counter, and they all take their step position from one common
 * "master beat" origin. At master beat 0 every channel sits exactly at
 * step 1 / phase 0, so the four sequences trigger simultaneously and
 * stay phase-locked forever.
 *
 * This file is now a thin LV2 wrapper: all the signal processing lives
 * in the host-agnostic core (src/stepgate_dsp.{c,h}), shared with the
 * single-channel plug-in and the JUCE build, via its multi-voice entry
 * point stepgate_dsp_process_multi(). The wrapper only maps LV2 ports
 * and time:Position atoms onto the core's parameter structs.
 *
 * What is SHARED across the four channels:
 *   - the Time input (host transport)
 *   - the Sync Source (Host Sync / Free Run)
 *   - the Tempo (used when not host-synced)
 *   - the global Enabled (soft bypass)
 *
 * What is PER-CHANNEL (independent):
 *   - one mono audio input and one mono audio output
 *   - the rhythmic Division and its Feel modifier (straight / dotted /
 *     triplet); each voice can run a different note value while staying
 *     locked to the same master beat
 *   - the 16 step On/Tie toggles
 *   - the ADSR envelope (attack / decay / sustain / release)
 *
 * Two sync modes (common to all channels):
 *   - Host Sync: the master beat is derived directly from the host's
 *     time:beat / time:frame, so several instances of this plug-in (or
 *     other host-synced plug-ins) on the same transport are in phase.
 *   - Free Run: an internal master beat counter, reset to 0 (step 1)
 *     each time the lv2:enabled designation transitions from 0 to 1.
 *
 * When lv2:enabled is 0 the plug-in passes all four channels through
 * unchanged, as required by the LV2 core spec.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/util.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/time/time.h>

#include "stepgate_dsp.h"

#define PLUGIN_URI   "https://github.com/pilali/parkinsound/lv2/stepgate4"
#define NUM_STEPS    STEPGATE_NUM_STEPS
#define NUM_CHANNELS 4

/* ---- Port layout ----------------------------------------------------
 * Common ports first, then the audio I/O, then four identical
 * per-channel blocks. Keeping the layout computable (rather than a flat
 * enum of 160-odd entries) makes connect_port() and the .ttl generator
 * agree by construction.
 *
 * Ports added after the original release MUST be appended after the
 * last historical index (LV2 forbids renumbering existing ports without
 * a new plugin URI), so they live outside the per-channel blocks. */
enum {
    PORT_TIME_IN        = 0,
    PORT_SYNC_SOURCE    = 1,
    PORT_TEMPO          = 2,
    PORT_ENABLED        = 3,
    PORT_AUDIO_IN_BASE  = 4,                              /* 4 mono inputs : 4..7  */
    PORT_AUDIO_OUT_BASE = PORT_AUDIO_IN_BASE + NUM_CHANNELS, /* 4 mono outs : 8..11 */
    PORT_CHANNEL_BASE   = PORT_AUDIO_OUT_BASE + NUM_CHANNELS /* 12 */
};

/* Offsets within one per-channel block. */
enum {
    CH_DIVISION     = 0,
    CH_CURRENT_STEP = 1,
    CH_ATTACK       = 2,
    CH_DECAY        = 3,
    CH_SUSTAIN      = 4,
    CH_RELEASE      = 5,
    CH_STEP_BASE    = 6,                       /* 32 step ports: on/tie x 16 */
    CH_STRIDE       = CH_STEP_BASE + NUM_STEPS * 2
};

/* Appended ports (v1.2): per-channel division feel modifier, shared
 * bar-pattern / meter controls, per-channel active-steps outputs. */
#define PORT_DIV_MOD_BASE      (PORT_CHANNEL_BASE + NUM_CHANNELS * CH_STRIDE) /* 164 */
#define PORT_PATTERN_MODE      (PORT_DIV_MOD_BASE + NUM_CHANNELS)            /* 168 */
#define PORT_METER_SOURCE      (PORT_PATTERN_MODE + 1)                       /* 169 */
#define PORT_METER_NUM         (PORT_METER_SOURCE + 1)                       /* 170 */
#define PORT_METER_DENOM       (PORT_METER_NUM + 1)                          /* 171 */
#define PORT_ACTIVE_STEPS_BASE (PORT_METER_DENOM + 1)                        /* 172..175 */
#define NUM_PORTS              (PORT_ACTIVE_STEPS_BASE + NUM_CHANNELS)       /* 176 */

typedef struct {
    LV2_URID atom_Blank;
    LV2_URID atom_Object;
    LV2_URID atom_Float;
    LV2_URID atom_Double;
    LV2_URID atom_Int;
    LV2_URID atom_Long;
    LV2_URID time_Position;
    LV2_URID time_beat;
    LV2_URID time_beatsPerMinute;
    LV2_URID time_speed;
    LV2_URID time_frame;
    LV2_URID time_beatsPerBar;
    LV2_URID time_beatUnit;
    LV2_URID time_bar;
    LV2_URID time_barBeat;
} URIs;

typedef struct {
    LV2_URID_Map* map;
    URIs uris;

    StepGateDsp* dsp;

    /* Shared ports. */
    const LV2_Atom_Sequence* time_in;
    const float* sync_source;
    const float* tempo;
    const float* enabled_port;
    const float* pattern_mode_port;
    const float* meter_source_port;
    const float* meter_num_port;
    const float* meter_denom_port;

    /* Per-channel ports. */
    const float* audio_in[NUM_CHANNELS];
    float*       audio_out[NUM_CHANNELS];
    const float* division[NUM_CHANNELS];
    const float* div_mod[NUM_CHANNELS];
    float*       current_step_out[NUM_CHANNELS];
    const float* attack[NUM_CHANNELS];
    const float* decay[NUM_CHANNELS];
    const float* sustain[NUM_CHANNELS];
    const float* release[NUM_CHANNELS];
    const float* step_on[NUM_CHANNELS][NUM_STEPS];
    const float* step_tie[NUM_CHANNELS][NUM_STEPS];
    float*       active_steps_out[NUM_CHANNELS];
} StepGate4;

static inline double
get_atom_double(const LV2_Atom* atom, const URIs* uris)
{
    if (!atom) return 0.0;
    if (atom->type == uris->atom_Float)  return ((const LV2_Atom_Float*)atom)->body;
    if (atom->type == uris->atom_Double) return ((const LV2_Atom_Double*)atom)->body;
    if (atom->type == uris->atom_Int)    return ((const LV2_Atom_Int*)atom)->body;
    if (atom->type == uris->atom_Long)   return (double)((const LV2_Atom_Long*)atom)->body;
    return 0.0;
}

static void
handle_position(StepGate4* self, const LV2_Atom_Object* obj)
{
    const URIs* uris = &self->uris;
    const LV2_Atom* bpm      = NULL;
    const LV2_Atom* beat     = NULL;
    const LV2_Atom* speed    = NULL;
    const LV2_Atom* frame    = NULL;
    const LV2_Atom* bpb      = NULL;
    const LV2_Atom* bunit    = NULL;
    const LV2_Atom* bar      = NULL;
    const LV2_Atom* bar_beat = NULL;
    lv2_atom_object_get(obj,
                        uris->time_beatsPerMinute, &bpm,
                        uris->time_beat,           &beat,
                        uris->time_speed,          &speed,
                        uris->time_frame,          &frame,
                        uris->time_beatsPerBar,    &bpb,
                        uris->time_beatUnit,       &bunit,
                        uris->time_bar,            &bar,
                        uris->time_barBeat,        &bar_beat,
                        0);

    StepGatePosition pos;
    memset(&pos, 0, sizeof(pos));
    pos.have_bpm           = (bpm      != NULL); pos.bpm           = get_atom_double(bpm,      uris);
    pos.have_beat          = (beat     != NULL); pos.beat          = get_atom_double(beat,     uris);
    pos.have_speed         = (speed    != NULL); pos.speed         = get_atom_double(speed,    uris);
    pos.have_frame         = (frame    != NULL); pos.frame         = get_atom_double(frame,    uris);
    pos.have_beats_per_bar = (bpb      != NULL); pos.beats_per_bar = get_atom_double(bpb,      uris);
    pos.have_beat_unit     = (bunit    != NULL); pos.beat_unit     = (int)get_atom_double(bunit, uris);
    pos.have_bar           = (bar      != NULL); pos.bar           = (long long)get_atom_double(bar, uris);
    pos.have_bar_beat      = (bar_beat != NULL); pos.bar_beat      = get_atom_double(bar_beat, uris);
    stepgate_dsp_update_position(self->dsp, &pos);
}

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor,
            double                rate,
            const char*           bundle_path,
            const LV2_Feature* const* features)
{
    (void)descriptor;
    (void)bundle_path;

    StepGate4* self = (StepGate4*)calloc(1, sizeof(StepGate4));
    if (!self) return NULL;

    LV2_URID_Map* map = NULL;
    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            map = (LV2_URID_Map*)features[i]->data;
        }
    }
    if (!map) {
        free(self);
        return NULL;
    }
    self->map = map;

    URIs* u = &self->uris;
    u->atom_Blank          = map->map(map->handle, LV2_ATOM__Blank);
    u->atom_Object         = map->map(map->handle, LV2_ATOM__Object);
    u->atom_Float          = map->map(map->handle, LV2_ATOM__Float);
    u->atom_Double         = map->map(map->handle, LV2_ATOM__Double);
    u->atom_Int            = map->map(map->handle, LV2_ATOM__Int);
    u->atom_Long           = map->map(map->handle, LV2_ATOM__Long);
    u->time_Position       = map->map(map->handle, LV2_TIME__Position);
    u->time_beat           = map->map(map->handle, LV2_TIME__beat);
    u->time_beatsPerMinute = map->map(map->handle, LV2_TIME__beatsPerMinute);
    u->time_speed          = map->map(map->handle, LV2_TIME__speed);
    u->time_frame          = map->map(map->handle, LV2_TIME__frame);
    u->time_beatsPerBar    = map->map(map->handle, LV2_TIME__beatsPerBar);
    u->time_beatUnit       = map->map(map->handle, LV2_TIME__beatUnit);
    u->time_bar            = map->map(map->handle, LV2_TIME__bar);
    u->time_barBeat        = map->map(map->handle, LV2_TIME__barBeat);

    self->dsp = stepgate_dsp_new(rate);
    if (!self->dsp) {
        free(self);
        return NULL;
    }

    return (LV2_Handle)self;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    StepGate4* self = (StepGate4*)instance;

    if (port == PORT_TIME_IN)     { self->time_in      = (const LV2_Atom_Sequence*)data; return; }
    if (port == PORT_SYNC_SOURCE) { self->sync_source  = (const float*)data; return; }
    if (port == PORT_TEMPO)       { self->tempo        = (const float*)data; return; }
    if (port == PORT_ENABLED)      { self->enabled_port      = (const float*)data; return; }
    if (port == PORT_PATTERN_MODE) { self->pattern_mode_port = (const float*)data; return; }
    if (port == PORT_METER_SOURCE) { self->meter_source_port = (const float*)data; return; }
    if (port == PORT_METER_NUM)    { self->meter_num_port    = (const float*)data; return; }
    if (port == PORT_METER_DENOM)  { self->meter_denom_port  = (const float*)data; return; }

    if (port >= PORT_AUDIO_IN_BASE && port < PORT_AUDIO_IN_BASE + NUM_CHANNELS) {
        self->audio_in[port - PORT_AUDIO_IN_BASE] = (const float*)data;
        return;
    }
    if (port >= PORT_AUDIO_OUT_BASE && port < PORT_AUDIO_OUT_BASE + NUM_CHANNELS) {
        self->audio_out[port - PORT_AUDIO_OUT_BASE] = (float*)data;
        return;
    }

    if (port >= PORT_DIV_MOD_BASE && port < PORT_DIV_MOD_BASE + NUM_CHANNELS) {
        self->div_mod[port - PORT_DIV_MOD_BASE] = (const float*)data;
        return;
    }
    if (port >= PORT_ACTIVE_STEPS_BASE && port < (uint32_t)NUM_PORTS) {
        self->active_steps_out[port - PORT_ACTIVE_STEPS_BASE] = (float*)data;
        return;
    }

    if (port >= PORT_CHANNEL_BASE && port < (uint32_t)PORT_DIV_MOD_BASE) {
        uint32_t rel = port - PORT_CHANNEL_BASE;
        uint32_t ch  = rel / CH_STRIDE;
        uint32_t off = rel % CH_STRIDE;
        if (ch >= NUM_CHANNELS) return;
        switch (off) {
            case CH_DIVISION:     self->division[ch]         = (const float*)data; return;
            case CH_CURRENT_STEP: self->current_step_out[ch] = (float*)data;       return;
            case CH_ATTACK:       self->attack[ch]           = (const float*)data; return;
            case CH_DECAY:        self->decay[ch]            = (const float*)data; return;
            case CH_SUSTAIN:      self->sustain[ch]          = (const float*)data; return;
            case CH_RELEASE:      self->release[ch]          = (const float*)data; return;
            default: {
                uint32_t local = off - CH_STEP_BASE;   /* 0..31 */
                uint32_t step  = local / 2u;
                if ((local & 1u) == 0u) self->step_on[ch][step]  = (const float*)data;
                else                    self->step_tie[ch][step] = (const float*)data;
                return;
            }
        }
    }
}

static void
activate(LV2_Handle instance)
{
    StepGate4* self = (StepGate4*)instance;
    stepgate_dsp_reset(self->dsp);
}

static void
run(LV2_Handle instance, uint32_t n_samples)
{
    StepGate4* self = (StepGate4*)instance;
    const URIs* uris = &self->uris;

    if (self->time_in) {
        LV2_ATOM_SEQUENCE_FOREACH(self->time_in, ev) {
            if (ev->body.type == uris->atom_Object || ev->body.type == uris->atom_Blank) {
                const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
                if (obj->body.otype == uris->time_Position) {
                    handle_position(self, obj);
                }
            }
        }
    }

    /* Map the LV2 control ports onto the core's parameter structs.
     * Unmapped ports fall back to the .ttl defaults. */
    StepGateSharedParams shared;
    shared.sync_source = self->sync_source  ? *self->sync_source  : 0.0f;
    shared.tempo       = self->tempo        ? *self->tempo        : 120.0f;
    shared.enabled     = self->enabled_port ? *self->enabled_port : 1.0f;
    shared.pattern_mode = self->pattern_mode_port ? *self->pattern_mode_port : 0.0f;
    shared.meter_source = self->meter_source_port ? *self->meter_source_port : 0.0f;
    shared.meter_num    = self->meter_num_port    ? *self->meter_num_port    : 4.0f;
    shared.meter_denom  = self->meter_denom_port  ? *self->meter_denom_port  : 4.0f;

    StepGateVoiceParams voices[NUM_CHANNELS];
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        StepGateVoiceParams* v = &voices[ch];
        v->division     = self->division[ch] ? *self->division[ch] : 4.0f;
        v->division_mod = self->div_mod[ch]  ? *self->div_mod[ch]  : 0.0f;
        v->attack       = self->attack[ch]   ? *self->attack[ch]   : 0.0f;
        v->decay        = self->decay[ch]    ? *self->decay[ch]    : 0.0f;
        v->sustain      = self->sustain[ch]  ? *self->sustain[ch]  : 1.0f;
        v->release      = self->release[ch]  ? *self->release[ch]  : 0.5f;
        for (int k = 0; k < NUM_STEPS; ++k) {
            v->step_on[k]  = self->step_on[ch][k]  ? *self->step_on[ch][k]  : 0.0f;
            v->step_tie[k] = self->step_tie[ch][k] ? *self->step_tie[ch][k] : 0.0f;
        }
    }

    int current_steps[NUM_CHANNELS];
    int active_steps[NUM_CHANNELS];
    stepgate_dsp_process_multi(self->dsp, &shared, voices, NUM_CHANNELS,
                               (const float* const*)self->audio_in,
                               (float* const*)self->audio_out,
                               current_steps, active_steps, n_samples);

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        if (self->current_step_out[ch]) {
            *self->current_step_out[ch] = (float)current_steps[ch];
        }
        if (self->active_steps_out[ch]) {
            *self->active_steps_out[ch] = (float)active_steps[ch];
        }
    }
}

static void
deactivate(LV2_Handle instance)
{
    (void)instance;
}

static void
cleanup(LV2_Handle instance)
{
    StepGate4* self = (StepGate4*)instance;
    if (self) {
        stepgate_dsp_free(self->dsp);
        free(self);
    }
}

static const void*
extension_data(const char* uri)
{
    (void)uri;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    PLUGIN_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
    return (index == 0) ? &descriptor : NULL;
}
