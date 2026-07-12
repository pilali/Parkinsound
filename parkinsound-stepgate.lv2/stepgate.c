/*
 * Parkinsound Step Gate - 16-step audio gate sequencer (LV2 wrapper)
 *
 * Pure audio plugin: each step opens or closes a smoothed gain envelope
 * over the incoming stereo audio.
 *
 * This file is now a thin LV2 wrapper: all the signal processing lives in
 * the host-agnostic core (src/stepgate_dsp.{c,h}), which is shared with
 * the JUCE (VST3/AU/Standalone) build. The wrapper only maps LV2 ports
 * and time:Position atoms onto the core's parameter struct and transport
 * update, then calls stepgate_dsp_process().
 *
 * Per-step ADSR envelope shapes each active gate; sustain level and
 * all time parameters are expressed as fractions of the step duration.
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
#include <stdint.h>
#include <math.h>

#include <lv2/lv2plug.in/ns/lv2core/lv2.h>
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/util.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/time/time.h>

#include "stepgate_dsp.h"

#define PLUGIN_URI "https://github.com/pilali/parkinsound/lv2/stepgate"
#define NUM_STEPS  STEPGATE_NUM_STEPS

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
#define PORT_ATTACK  (PORT_ENABLED + 1)
#define PORT_DECAY   (PORT_ATTACK  + 1)
#define PORT_SUSTAIN (PORT_DECAY   + 1)
#define PORT_RELEASE (PORT_SUSTAIN + 1)
/* Appended after the original layout (LV2 forbids renumbering existing
 * ports without a new plugin URI). */
#define PORT_DIV_MOD (PORT_RELEASE + 1)
#define NUM_PORTS    (PORT_DIV_MOD + 1)

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

    StepGateDsp* dsp;

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
    const float* attack_port;
    const float* decay_port;
    const float* sustain_port;
    const float* release_port;
    const float* div_mod_port;
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
    stepgate_dsp_update_position(self->dsp,
                                 bpm   != NULL, get_atom_double(bpm,   uris),
                                 beat  != NULL, get_atom_double(beat,  uris),
                                 speed != NULL, get_atom_double(speed, uris),
                                 frame != NULL, get_atom_double(frame, uris));
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
            if      (port == PORT_ENABLED) self->enabled_port = (const float*)data;
            else if (port == PORT_ATTACK)  self->attack_port  = (const float*)data;
            else if (port == PORT_DECAY)   self->decay_port   = (const float*)data;
            else if (port == PORT_SUSTAIN) self->sustain_port = (const float*)data;
            else if (port == PORT_RELEASE) self->release_port = (const float*)data;
            else if (port == PORT_DIV_MOD) self->div_mod_port = (const float*)data;
            else if (port >= PORT_STEP_BASE && port < PORT_STEP_BASE + NUM_STEPS * 2u) {
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
    stepgate_dsp_reset(self->dsp);
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

    /* Map the LV2 control ports onto the core's parameter struct. Unmapped
     * ports fall back to the .ttl defaults so behaviour is unchanged. */
    StepGateParams p;
    p.sync_source = self->sync_source ? *self->sync_source : 0.0f;
    p.tempo       = self->tempo       ? *self->tempo       : 120.0f;
    p.division    = self->division    ? *self->division    : 4.0f;
    p.division_mod = self->div_mod_port ? *self->div_mod_port : 0.0f;
    p.enabled     = self->enabled_port ? *self->enabled_port : 1.0f;
    p.attack      = self->attack_port  ? *self->attack_port  : 0.0f;
    p.decay       = self->decay_port   ? *self->decay_port   : 0.0f;
    p.sustain     = self->sustain_port ? *self->sustain_port : 1.0f;
    p.release     = self->release_port ? *self->release_port : 0.5f;
    for (int k = 0; k < NUM_STEPS; ++k) {
        p.step_on[k]  = self->step_on[k]  ? *self->step_on[k]  : 0.0f;
        p.step_tie[k] = self->step_tie[k] ? *self->step_tie[k] : 0.0f;
    }

    const int display_step =
        stepgate_dsp_process(self->dsp, &p,
                             self->audio_in_l, self->audio_in_r,
                             self->audio_out_l, self->audio_out_r,
                             n_samples);

    if (self->current_step_out) {
        *self->current_step_out = (float)display_step;
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
    StepGate* self = (StepGate*)instance;
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
