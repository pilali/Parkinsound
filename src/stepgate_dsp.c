/*
 * Parkinsound Step Gate - host-agnostic DSP core (implementation).
 *
 * Extracted verbatim from the original LV2 stepgate.c so the output is
 * bit-identical. The only changes are mechanical: control ports become
 * fields of StepGateParams, time:Position atoms become arguments to
 * stepgate_dsp_update_position(), and the "current_step" output port
 * becomes the return value of stepgate_dsp_process().
 */

#include "stepgate_dsp.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NUM_STEPS STEPGATE_NUM_STEPS

struct StepGateDsp {
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

    /* Free-run state (single-voice path). */
    double free_phase;
    int    free_step;

    /* Free-run master beat counter (multi-voice path, shared by all
     * voices so their mutual phase relationship is preserved). */
    double free_beat;

    /* lv2:enabled transition detection. */
    int    prev_enabled;

    /* Gate smoothing. */
    float  gate;                            /* single-voice path  */
    float  gates[STEPGATE_MAX_VOICES];      /* multi-voice path   */
};

/* division index -> step length expressed in quarter notes
 * 0 = 1/1 whole       = 4 quarters
 * 1 = 1/2 half        = 2 quarters
 * 2 = 1/4 quarter     = 1 quarter
 * 3 = 1/8 eighth      = 0.5
 * 4 = 1/16 sixteenth  = 0.25
 * 5 = 1/32            = 0.125
 * The modifier then scales the step length:
 * 0 = straight (x1), 1 = dotted (x1.5), 2 = triplet (x2/3). */
static inline double
step_length_in_beats(float division, float division_mod)
{
    static const double div_factor[6] = { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
    static const double mod_factor[3] = { 1.0, 1.5, 2.0 / 3.0 };
    int div = (int)lroundf(division);
    if (div < 0) div = 0;
    if (div > 5) div = 5;
    int mod = (int)lroundf(division_mod);
    if (mod < 0) mod = 0;
    if (mod > 2) mod = 2;
    return div_factor[div] * mod_factor[mod];
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

StepGateDsp*
stepgate_dsp_new(double sample_rate)
{
    StepGateDsp* self = (StepGateDsp*)calloc(1, sizeof(StepGateDsp));
    if (!self) return NULL;

    self->sample_rate         = sample_rate;
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
    self->free_beat           = 0.0;
    self->prev_enabled        = 1;
    self->gate                = 0.0f;
    for (int v = 0; v < STEPGATE_MAX_VOICES; ++v) self->gates[v] = 0.0f;

    return self;
}

void
stepgate_dsp_free(StepGateDsp* self)
{
    free(self);
}

void
stepgate_dsp_reset(StepGateDsp* self)
{
    self->free_phase   = 0.0;
    self->free_step    = 0;
    self->free_beat    = 0.0;
    self->prev_enabled = 1;
    self->gate         = 0.0f;
    for (int v = 0; v < STEPGATE_MAX_VOICES; ++v) self->gates[v] = 0.0f;
}

void
stepgate_dsp_update_position(StepGateDsp* self,
                             int have_bpm,   double bpm,
                             int have_beat,  double beat,
                             int have_speed, double speed,
                             int have_frame, double frame)
{
    if (have_bpm) {
        if (bpm > 0.0) self->host_bpm = bpm;
    }
    if (have_speed) {
        self->host_speed = speed;
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
    if (have_frame && self->host_bpm > 0.0) {
        self->host_beat = frame * self->host_bpm / (60.0 * self->sample_rate);
    } else if (have_beat) {
        double v = beat;
        if (!self->has_prev_beat || v != self->prev_received_beat) {
            self->host_beat = v;
            self->has_prev_beat = 1;
        }
        self->prev_received_beat = v;
    }
}

int
stepgate_dsp_process(StepGateDsp* self, const StepGateParams* params,
                     const float* inL, const float* inR,
                     float* outL, float* outR, uint32_t n_samples)
{
    const int   sync       = (int)lroundf(params->sync_source);
    const float tempo_ctrl = params->tempo;
    const int   host_sync  = (sync == 0);
    const int   enabled    = (params->enabled > 0.5f);

    const float env_a = params->attack;
    const float env_d = params->decay;
    const float env_s = params->sustain;
    const float env_r = params->release;

    double bpm;
    if (host_sync && self->host_bpm > 0.0) {
        bpm = self->host_bpm;
    } else {
        bpm = (double)tempo_ctrl;
    }
    if (bpm < 20.0)  bpm = 20.0;
    if (bpm > 999.0) bpm = 999.0;

    const double step_in_beats =
        step_length_in_beats(params->division, params->division_mod);

    const double beat_inc = bpm / (60.0 * self->sample_rate);

    /* Free-run only: reset to step 1 when lv2:enabled goes 0 -> 1. */
    if (!host_sync && enabled && !self->prev_enabled) {
        self->free_phase = 0.0;
        self->free_step  = 0;
    }
    self->prev_enabled = enabled;

    /* ~3 ms one-pole smoothing to avoid clicks on gate transitions. */
    const float gate_alpha = 1.0f - expf(-1.0f / (float)(0.003 * self->sample_rate));

    /* Snapshot the per-step on/tie controls once per block. LV2 control
     * ports are stable across run(), so we can resolve tie boundaries
     * by looking at the previous and next steps without re-reading on
     * every sample. */
    int step_on_b[NUM_STEPS];
    int step_tie_b[NUM_STEPS];
    for (int k = 0; k < NUM_STEPS; ++k) {
        step_on_b[k]  = (params->step_on[k]  > 0.5f);
        step_tie_b[k] = (params->step_tie[k] > 0.5f);
    }

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
            const int on        = step_on_b[step];
            const int prev_step = (step + NUM_STEPS - 1) % NUM_STEPS;
            const int next_step = (step + 1) % NUM_STEPS;
            /* tie has an anchor only if the previous step was on.
             * Otherwise the tied step retriggers a fresh envelope. */
            const int tied_in   = on && step_tie_b[step] && step_on_b[prev_step];
            /* The boundary to the next step is held when the next step
             * is itself a tied-on step. */
            const int tied_out  = on && step_on_b[next_step] && step_tie_b[next_step];
            if (!on) target = 0.0f;
            else     target = step_env(in_step_phase,
                                       env_a, env_d, env_s, env_r,
                                       tied_in, tied_out);
        }

        self->gate += (target - self->gate) * gate_alpha;

        const float sL = inL ? inL[i] : 0.0f;
        const float sR = inR ? inR[i] : 0.0f;
        if (outL) outL[i] = sL * self->gate;
        if (outR) outR[i] = sR * self->gate;

        display_step = step;
    }

    return display_step + 1;
}

void
stepgate_dsp_process_multi(StepGateDsp* self,
                           const StepGateSharedParams* shared,
                           const StepGateVoiceParams* voices,
                           int num_voices,
                           const float* const* ins,
                           float* const* outs,
                           int* current_steps,
                           uint32_t n_samples)
{
    if (num_voices > STEPGATE_MAX_VOICES) num_voices = STEPGATE_MAX_VOICES;
    if (num_voices < 0)                   num_voices = 0;

    const int   sync       = (int)lroundf(shared->sync_source);
    const float tempo_ctrl = shared->tempo;
    const int   host_sync  = (sync == 0);
    const int   enabled    = (shared->enabled > 0.5f);

    double bpm;
    if (host_sync && self->host_bpm > 0.0) {
        bpm = self->host_bpm;
    } else {
        bpm = (double)tempo_ctrl;
    }
    if (bpm < 20.0)  bpm = 20.0;
    if (bpm > 999.0) bpm = 999.0;

    const double beat_inc = bpm / (60.0 * self->sample_rate);

    /* Free-run only: reset the shared master beat to step 1 when
     * lv2:enabled goes 0 -> 1. All voices reset together, preserving
     * their mutual phase relationship. */
    if (!host_sync && enabled && !self->prev_enabled) {
        self->free_beat = 0.0;
    }
    self->prev_enabled = enabled;

    /* ~3 ms one-pole smoothing to avoid clicks on gate transitions. */
    const float gate_alpha = 1.0f - expf(-1.0f / (float)(0.003 * self->sample_rate));

    /* Snapshot the per-voice controls once per block. Control values are
     * stable across one process call, so tie boundaries can be resolved
     * by looking at neighbouring steps without re-reading every sample. */
    double step_in_beats[STEPGATE_MAX_VOICES];
    int    son [STEPGATE_MAX_VOICES][NUM_STEPS];
    int    stie[STEPGATE_MAX_VOICES][NUM_STEPS];
    for (int v = 0; v < num_voices; ++v) {
        step_in_beats[v] =
            step_length_in_beats(voices[v].division, voices[v].division_mod);
        for (int k = 0; k < NUM_STEPS; ++k) {
            son [v][k] = (voices[v].step_on[k]  > 0.5f);
            stie[v][k] = (voices[v].step_tie[k] > 0.5f);
        }
    }

    int display_step[STEPGATE_MAX_VOICES];
    for (int v = 0; v < num_voices; ++v) display_step[v] = 0;

    for (uint32_t i = 0; i < n_samples; ++i) {
        /* The single master beat shared by every voice. Sampling it
         * BEFORE advancing means master beat 0 lands exactly on step 1
         * / phase 0 for all voices, so the sequences trigger together. */
        double master;
        if (host_sync) {
            master = self->host_beat;
        } else if (enabled) {
            master = self->free_beat;
        } else {
            master = 0.0;
        }

        for (int v = 0; v < num_voices; ++v) {
            float target;
            int   step = 0;

            if (!enabled) {
                /* lv2:enabled = 0 -> transparent pass-through. */
                target = 1.0f;
            } else {
                const double seq_pos   = master / step_in_beats[v];
                const double seq_floor = floor(seq_pos);
                long   step_index      = (long)seq_floor;
                double in_step_phase   = seq_pos - seq_floor;
                long   mod_step        = step_index % NUM_STEPS;
                if (mod_step < 0) mod_step += NUM_STEPS;
                step = (int)mod_step;

                const int on        = son[v][step];
                const int prev_step = (step + NUM_STEPS - 1) % NUM_STEPS;
                const int next_step = (step + 1) % NUM_STEPS;
                const int tied_in   = on && stie[v][step] && son[v][prev_step];
                const int tied_out  = on && son[v][next_step] && stie[v][next_step];
                if (!on) target = 0.0f;
                else     target = step_env(in_step_phase,
                                           voices[v].attack, voices[v].decay,
                                           voices[v].sustain, voices[v].release,
                                           tied_in, tied_out);
            }

            self->gates[v] += (target - self->gates[v]) * gate_alpha;

            const float s = ins && ins[v] ? ins[v][i] : 0.0f;
            if (outs && outs[v]) outs[v][i] = s * self->gates[v];

            display_step[v] = step;
        }

        /* Advance the shared counters once per sample, after all voices
         * have been processed from the same master value. host_beat
         * keeps cycling even while disabled / paused (mod-host has no
         * JACK transport), matching the single-voice path. */
        self->host_beat += beat_inc;
        if (enabled) self->free_beat += beat_inc;
    }

    if (current_steps) {
        for (int v = 0; v < num_voices; ++v) {
            current_steps[v] = display_step[v] + 1;
        }
    }
}
