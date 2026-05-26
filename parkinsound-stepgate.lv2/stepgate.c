/*
 * Parkinsound Step Gate - 16-step audio gate sequencer (LV2)
 *
 * Pure audio plugin: each step opens or closes a smoothed gain envelope
 * over the incoming stereo audio.
 *
 * Two sync modes:
 *   - Host Sync: the step position is derived directly from time:beat
 *     (or time:frame * bpm / sr) advertised by the host. Several
 *     instances driven by the same host transport are therefore in
 *     phase sample-accurately.
 *   - Free Run: an internal phase counter, reset to step 1 each time
 *     the lv2:enabled designation transitions from 0 to 1 (i.e. each
 *     time the user un-bypasses the plug-in in mod-ui).
 *
 * When lv2:enabled is 0 the plug-in passes audio through unchanged, as
 * required by the LV2 core spec.
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

#define PLUGIN_URI "https://github.com/pilali/parkinsound/lv2/stepgate"
#define NUM_STEPS  16

typedef enum {
    PORT_TIME_IN      = 0,
    PORT_AUDIO_IN_L   = 1,
    PORT_AUDIO_IN_R   = 2,
    PORT_AUDIO_OUT_L  = 3,
    PORT_AUDIO_OUT_R  = 4,
    PORT_SYNC_SOURCE  = 5,
    PORT_TEMPO        = 6,
    PORT_DIVISION     = 7,
    PORT_CURRENT_STEP = 8,
    PORT_STEP_BASE    = 9
} PortIndex;

#define PORT_ENABLED (PORT_STEP_BASE + NUM_STEPS * 2)
#define NUM_PORTS    (PORT_ENABLED + 1)

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

    const LV2_Atom_Sequence* time_in;
    const float* audio_in_l;
    const float* audio_in_r;
    float*       audio_out_l;
    float*       audio_out_r;
    const float* sync_source;
    const float* tempo;
    const float* division;
    float*       current_step_out;
    const float* step_on[NUM_STEPS];
    const float* step_tie[NUM_STEPS];
    const float* enabled_port;

    double sample_rate;

    /* Host transport state (updated from time:Position events and
     * integrated sample-by-sample in between events). */
    double host_bpm;
    double host_beat;
    double host_speed;

    /* Last beat value we accepted from a time:Position event. Used to
     * detect when the host is just re-emitting the same (possibly
     * integer-quantised) beat value, in which case we let our per-
     * sample integration drive host_beat instead of snapping back. */
    double prev_received_beat;
    int    has_prev_beat;

    /* Free-run state. */
    double free_phase;
    int    free_step;

    /* lv2:enabled transition detection. */
    int    prev_enabled;

    /* Gate smoothing. */
    float  gate;
} StepGate;

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
handle_position(StepGate* self, const LV2_Atom_Object* obj)
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
     * processing block but only quantise time:beat to integer beats
     * - between two integer ticks, every block carries the same beat
     * value, which would freeze host_beat at that integer if we
     * snapped to it blindly. We therefore only adopt time:beat when
     * its value has actually changed since the previous event; in
     * between, the per-sample beat_inc integration drives host_beat.
     *
     * time:frame is continuous (sample-precise) when present and is
     * preferred whenever the host supplies it together with a BPM. */
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

    StepGate* self = (StepGate*)calloc(1, sizeof(StepGate));
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

    self->sample_rate         = rate;
    self->host_bpm            = 0.0;
    self->host_beat           = 0.0;
    /* Assume the host transport is running until it tells us otherwise.
     * mod-host has no explicit play/stop and emits no time:speed=0
     * events, so this default ensures we run freely on a MOD device. */
    self->host_speed          = 1.0;
    self->prev_received_beat  = 0.0;
    self->has_prev_beat       = 0;
    self->free_phase          = 0.0;
    self->free_step           = 0;
    self->prev_enabled        = 1;
    self->gate                = 0.0f;

    return (LV2_Handle)self;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    StepGate* self = (StepGate*)instance;
    switch (port) {
        case PORT_TIME_IN:      self->time_in          = (const LV2_Atom_Sequence*)data; break;
        case PORT_AUDIO_IN_L:   self->audio_in_l       = (const float*)data; break;
        case PORT_AUDIO_IN_R:   self->audio_in_r       = (const float*)data; break;
        case PORT_AUDIO_OUT_L:  self->audio_out_l      = (float*)data; break;
        case PORT_AUDIO_OUT_R:  self->audio_out_r      = (float*)data; break;
        case PORT_SYNC_SOURCE:  self->sync_source      = (const float*)data; break;
        case PORT_TEMPO:        self->tempo            = (const float*)data; break;
        case PORT_DIVISION:     self->division         = (const float*)data; break;
        case PORT_CURRENT_STEP: self->current_step_out = (float*)data; break;
        default:
            if (port == PORT_ENABLED) {
                self->enabled_port = (const float*)data;
            } else if (port >= PORT_STEP_BASE && port < PORT_STEP_BASE + NUM_STEPS * 2u) {
                uint32_t local = port - PORT_STEP_BASE;
                uint32_t step  = local / 2u;
                if ((local & 1u) == 0u) self->step_on[step]  = (const float*)data;
                else                    self->step_tie[step] = (const float*)data;
            }
            break;
    }
}

static void
activate(LV2_Handle instance)
{
    StepGate* self = (StepGate*)instance;
    self->free_phase   = 0.0;
    self->free_step    = 0;
    self->prev_enabled = 1;
    self->gate         = 0.0f;
}

static void
run(LV2_Handle instance, uint32_t n_samples)
{
    StepGate* self = (StepGate*)instance;
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
     * 0 = 1/1 whole       = 4 quarters
     * 1 = 1/2 half        = 2 quarters
     * 2 = 1/4 quarter     = 1 quarter
     * 3 = 1/8 eighth      = 0.5
     * 4 = 1/16 sixteenth  = 0.25
     * 5 = 1/32            = 0.125 */
    static const double div_factor[6] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
    int div = self->division ? (int)lroundf(*self->division) : 4;
    if (div < 0) div = 0;
    if (div > 5) div = 5;
    const double step_in_beats = div_factor[div];

    const double beat_inc = bpm / (60.0 * self->sample_rate);

    /* Free-run only: reset to step 1 when lv2:enabled goes 0 -> 1. */
    if (!host_sync && enabled && !self->prev_enabled) {
        self->free_phase = 0.0;
        self->free_step  = 0;
    }
    self->prev_enabled = enabled;

    /* ~3 ms one-pole smoothing to avoid clicks on gate transitions. */
    const float gate_alpha = 1.0f - expf(-1.0f / (float)(0.003 * self->sample_rate));

    const float* inL  = self->audio_in_l;
    const float* inR  = self->audio_in_r;
    float*       outL = self->audio_out_l;
    float*       outR = self->audio_out_r;

    int display_step = host_sync ? 0 : self->free_step;

    for (uint32_t i = 0; i < n_samples; ++i) {
        float target;
        int   step          = 0;
        double in_step_phase = 0.0;

        if (host_sync) {
            /* Always advance the beat counter while the plug-in is
             * being clocked. mod-host has no JACK transport on a MOD
             * device and emits time:Position with time:speed = 0,
             * which would otherwise freeze the sequencer at step 1.
             * In a DAW this means the pattern keeps cycling while
             * the transport is paused, which is the right behaviour
             * for a tremolo-style step gate. */
            self->host_beat += beat_inc;
            const double seq_pos    = self->host_beat / step_in_beats;
            const double seq_floor  = floor(seq_pos);
            long step_index = (long)seq_floor;
            in_step_phase = seq_pos - seq_floor;
            long mod_step = step_index % NUM_STEPS;
            if (mod_step < 0) mod_step += NUM_STEPS;
            step = (int)mod_step;
        } else if (enabled) {
            step = self->free_step;
            in_step_phase = self->free_phase;
            self->free_phase += beat_inc / step_in_beats;
            if (self->free_phase >= 1.0) {
                self->free_phase -= 1.0;
                self->free_step = (self->free_step + 1) % NUM_STEPS;
            }
        } else {
            /* Free-run + disabled: freeze at step 1 ready for the next
             * enable transition. */
            step = 0;
            in_step_phase = 0.0;
        }

        if (!enabled) {
            /* lv2:enabled = 0 -> transparent pass-through. */
            target = 1.0f;
        } else {
            const int on  = (self->step_on[step]  && *self->step_on[step]  > 0.5f);
            const int tie = (self->step_tie[step] && *self->step_tie[step] > 0.5f);
            if (!on)         target = 0.0f;
            else if (tie)    target = 1.0f;
            else             target = (in_step_phase < 0.5) ? 1.0f : 0.0f;
        }

        self->gate += (target - self->gate) * gate_alpha;

        const float sL = inL ? inL[i] : 0.0f;
        const float sR = inR ? inR[i] : 0.0f;
        if (outL) outL[i] = sL * self->gate;
        if (outR) outR[i] = sR * self->gate;

        display_step = step;
    }

    if (self->current_step_out) {
        *self->current_step_out = (float)(display_step + 1);
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
