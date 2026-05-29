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
 * What is SHARED across the four channels:
 *   - the Time input (host transport)
 *   - the Sync Source (Host Sync / Free Run)
 *   - the Tempo (used when not host-synced)
 *   - the global Enabled (soft bypass)
 *
 * What is PER-CHANNEL (independent):
 *   - one mono audio input and one mono audio output
 *   - the rhythmic Division (each voice can run a different note value
 *     while staying locked to the same master beat)
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

#define PLUGIN_URI   "https://github.com/pilali/parkinsound/lv2/stepgate4"
#define NUM_STEPS    16
#define NUM_CHANNELS 4

/* ---- Port layout ----------------------------------------------------
 * Common ports first, then the audio I/O, then four identical
 * per-channel blocks. Keeping the layout computable (rather than a flat
 * enum of 160-odd entries) makes connect_port() and the .ttl generator
 * agree by construction. */
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

#define NUM_PORTS (PORT_CHANNEL_BASE + NUM_CHANNELS * CH_STRIDE)

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
} URIs;

typedef struct {
    LV2_URID_Map* map;
    URIs uris;

    /* Shared ports. */
    const LV2_Atom_Sequence* time_in;
    const float* sync_source;
    const float* tempo;
    const float* enabled_port;

    /* Per-channel ports. */
    const float* audio_in[NUM_CHANNELS];
    float*       audio_out[NUM_CHANNELS];
    const float* division[NUM_CHANNELS];
    float*       current_step_out[NUM_CHANNELS];
    const float* attack[NUM_CHANNELS];
    const float* decay[NUM_CHANNELS];
    const float* sustain[NUM_CHANNELS];
    const float* release[NUM_CHANNELS];
    const float* step_on[NUM_CHANNELS][NUM_STEPS];
    const float* step_tie[NUM_CHANNELS][NUM_STEPS];

    double sample_rate;

    /* Host transport state (updated from time:Position events and
     * integrated sample-by-sample in between events). */
    double host_bpm;
    double host_beat;
    double host_speed;

    /* Last beat value accepted from a time:Position event, used to
     * detect when the host is re-emitting the same quantised beat. */
    double prev_received_beat;
    int    has_prev_beat;

    /* Free-run master beat counter (shared by all channels). */
    double free_beat;

    /* lv2:enabled transition detection. */
    int    prev_enabled;

    /* Per-channel gate smoothing state. */
    float  gate[NUM_CHANNELS];
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

/* Per-step ADSR envelope, all times expressed as fractions of the step
 * duration. tied_in suppresses Attack and Decay (the note is the
 * continuation of the previous step, already at sustain). tied_out
 * suppresses Release (the next step is a tied continuation, so the gate
 * must stay at sustain across the boundary). If the remaining A+D+R
 * still exceeds 1.0 they are scaled down to fit. */
static inline float
step_env(double phase, float a, float d, float s, float r,
         int tied_in, int tied_out)
{
    if (a < 0.0f) a = 0.0f;
    if (d < 0.0f) d = 0.0f;
    if (r < 0.0f) r = 0.0f;
    if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;

    if (tied_in)  { a = 0.0f; d = 0.0f; }
    if (tied_out) { r = 0.0f; }

    float adr = a + d + r;
    if (adr > 1.0f) {
        float k = 1.0f / adr;
        a *= k; d *= k; r *= k;
    }
    const float p      = (float)phase;
    const float aend   = a;
    const float dend   = a + d;
    const float rstart = 1.0f - r;

    if (p < aend)        return (a > 0.0f) ? p / a : 1.0f;
    else if (p < dend)   return (d > 0.0f) ? 1.0f - (p - aend) / d * (1.0f - s) : s;
    else if (p < rstart) return s;
    else                 return (r > 0.0f) ? s * (1.0f - (p - rstart) / r) : 0.0f;
}

static void
handle_position(StepGate4* self, const LV2_Atom_Object* obj)
{
    const URIs* uris = &self->uris;
    const LV2_Atom* bpm   = NULL;
    const LV2_Atom* beat  = NULL;
    const LV2_Atom* speed = NULL;
    const LV2_Atom* frame = NULL;
    lv2_atom_object_get(obj,
                        uris->time_beatsPerMinute, &bpm,
                        uris->time_beat,           &beat,
                        uris->time_speed,          &speed,
                        uris->time_frame,          &frame,
                        0);
    if (bpm) {
        double v = get_atom_double(bpm, uris);
        if (v > 0.0) self->host_bpm = v;
    }
    if (speed) {
        self->host_speed = get_atom_double(speed, uris);
    }
    /* Resynchronise the local beat counter to the host's absolute
     * position whenever we get a fresh time:Position event.
     *
     * Some hosts (mod-host in particular) emit time:Position every
     * processing block but only quantise time:beat to integer beats.
     * We therefore only adopt time:beat when its value has actually
     * changed since the previous event; in between, the per-sample
     * integration drives host_beat. time:frame is continuous when
     * present and is preferred whenever a BPM is also known. */
    if (frame && self->host_bpm > 0.0) {
        double f = get_atom_double(frame, uris);
        self->host_beat = f * self->host_bpm / (60.0 * self->sample_rate);
    } else if (beat) {
        double v = get_atom_double(beat, uris);
        if (!self->has_prev_beat || v != self->prev_received_beat) {
            self->host_beat = v;
            self->has_prev_beat = 1;
        }
        self->prev_received_beat = v;
    }
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

    self->sample_rate        = rate;
    self->host_bpm           = 0.0;
    self->host_beat          = 0.0;
    /* Assume the host transport is running until told otherwise; mod-host
     * has no explicit play/stop and emits no time:speed=0 events. */
    self->host_speed         = 1.0;
    self->prev_received_beat = 0.0;
    self->has_prev_beat      = 0;
    self->free_beat          = 0.0;
    self->prev_enabled       = 1;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) self->gate[ch] = 0.0f;

    return (LV2_Handle)self;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    StepGate4* self = (StepGate4*)instance;

    if (port == PORT_TIME_IN)     { self->time_in      = (const LV2_Atom_Sequence*)data; return; }
    if (port == PORT_SYNC_SOURCE) { self->sync_source  = (const float*)data; return; }
    if (port == PORT_TEMPO)       { self->tempo        = (const float*)data; return; }
    if (port == PORT_ENABLED)     { self->enabled_port = (const float*)data; return; }

    if (port >= PORT_AUDIO_IN_BASE && port < PORT_AUDIO_IN_BASE + NUM_CHANNELS) {
        self->audio_in[port - PORT_AUDIO_IN_BASE] = (const float*)data;
        return;
    }
    if (port >= PORT_AUDIO_OUT_BASE && port < PORT_AUDIO_OUT_BASE + NUM_CHANNELS) {
        self->audio_out[port - PORT_AUDIO_OUT_BASE] = (float*)data;
        return;
    }

    if (port >= PORT_CHANNEL_BASE && port < (uint32_t)NUM_PORTS) {
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
    self->free_beat    = 0.0;
    self->prev_enabled = 1;
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) self->gate[ch] = 0.0f;
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

    const int   sync       = self->sync_source ? (int)lroundf(*self->sync_source) : 0;
    const float tempo_ctrl = self->tempo       ? *self->tempo                     : 120.0f;
    const int   host_sync  = (sync == 0);
    const int   enabled    = (!self->enabled_port) || (*self->enabled_port > 0.5f);

    double bpm;
    if (host_sync && self->host_bpm > 0.0) {
        bpm = self->host_bpm;
    } else {
        bpm = (double)tempo_ctrl;
    }
    if (bpm < 20.0)  bpm = 20.0;
    if (bpm > 999.0) bpm = 999.0;

    /* division index -> step length expressed in quarter notes
     * 0 = 1/1 whole = 4 quarters ... 5 = 1/32 = 0.125 quarters. */
    static const double div_factor[6] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };

    const double beat_inc = bpm / (60.0 * self->sample_rate);

    /* Free-run only: reset the shared master beat to step 1 when
     * lv2:enabled goes 0 -> 1. All channels reset together, preserving
     * their mutual phase relationship. */
    if (!host_sync && enabled && !self->prev_enabled) {
        self->free_beat = 0.0;
    }
    self->prev_enabled = enabled;

    /* ~3 ms one-pole smoothing to avoid clicks on gate transitions. */
    const float gate_alpha = 1.0f - expf(-1.0f / (float)(0.003 * self->sample_rate));

    /* Snapshot the per-channel controls once per block. LV2 control
     * ports are stable across run(), so tie boundaries can be resolved
     * by looking at neighbouring steps without re-reading every sample. */
    double step_in_beats[NUM_CHANNELS];
    int    son [NUM_CHANNELS][NUM_STEPS];
    int    stie[NUM_CHANNELS][NUM_STEPS];
    float  env_a[NUM_CHANNELS], env_d[NUM_CHANNELS];
    float  env_s[NUM_CHANNELS], env_r[NUM_CHANNELS];
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        int div = self->division[ch] ? (int)lroundf(*self->division[ch]) : 4;
        if (div < 0) div = 0;
        if (div > 5) div = 5;
        step_in_beats[ch] = div_factor[div];

        env_a[ch] = self->attack[ch]  ? *self->attack[ch]  : 0.0f;
        env_d[ch] = self->decay[ch]   ? *self->decay[ch]   : 0.0f;
        env_s[ch] = self->sustain[ch] ? *self->sustain[ch] : 1.0f;
        env_r[ch] = self->release[ch] ? *self->release[ch] : 0.5f;

        for (int k = 0; k < NUM_STEPS; ++k) {
            son [ch][k] = (self->step_on[ch][k]  && *self->step_on[ch][k]  > 0.5f);
            stie[ch][k] = (self->step_tie[ch][k] && *self->step_tie[ch][k] > 0.5f);
        }
    }

    int display_step[NUM_CHANNELS];
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) display_step[ch] = 0;

    for (uint32_t i = 0; i < n_samples; ++i) {
        /* The single master beat shared by every channel. Sampling it
         * BEFORE advancing means master beat 0 lands exactly on step 1
         * / phase 0 for all channels, so the four sequences trigger
         * together. */
        double master;
        if (host_sync) {
            master = self->host_beat;
        } else if (enabled) {
            master = self->free_beat;
        } else {
            master = 0.0;
        }

        for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
            float target;
            int   step = 0;

            if (!enabled) {
                /* lv2:enabled = 0 -> transparent pass-through. */
                target = 1.0f;
            } else {
                const double seq_pos   = master / step_in_beats[ch];
                const double seq_floor = floor(seq_pos);
                long   step_index      = (long)seq_floor;
                double in_step_phase   = seq_pos - seq_floor;
                long   mod_step        = step_index % NUM_STEPS;
                if (mod_step < 0) mod_step += NUM_STEPS;
                step = (int)mod_step;

                const int on        = son[ch][step];
                const int prev_step = (step + NUM_STEPS - 1) % NUM_STEPS;
                const int next_step = (step + 1) % NUM_STEPS;
                const int tied_in   = on && stie[ch][step] && son[ch][prev_step];
                const int tied_out  = on && son[ch][next_step] && stie[ch][next_step];
                if (!on) target = 0.0f;
                else     target = step_env(in_step_phase,
                                           env_a[ch], env_d[ch], env_s[ch], env_r[ch],
                                           tied_in, tied_out);
            }

            self->gate[ch] += (target - self->gate[ch]) * gate_alpha;

            const float s = self->audio_in[ch] ? self->audio_in[ch][i] : 0.0f;
            if (self->audio_out[ch]) self->audio_out[ch][i] = s * self->gate[ch];

            display_step[ch] = step;
        }

        /* Advance the shared counters once per sample, after all four
         * channels have been processed from the same master value.
         * host_beat keeps cycling even while disabled / paused (mod-host
         * has no JACK transport), matching the single-channel plug-in. */
        self->host_beat += beat_inc;
        if (enabled) self->free_beat += beat_inc;
    }

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        if (self->current_step_out[ch]) {
            *self->current_step_out[ch] = (float)(display_step[ch] + 1);
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
    free(instance);
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
