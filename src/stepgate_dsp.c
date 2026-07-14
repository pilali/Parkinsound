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

    /* Host meter state (time:beatsPerBar / time:beatUnit or the JUCE
     * time signature). 0 = the host never told us. */
    double    host_beats_per_bar;
    int       host_beat_unit;
    /* Bar reference: absolute transport beat of a known bar start,
     * derived from (received beat - received barBeat) so both values
     * share the host's quantisation, or taken verbatim from JUCE's
     * ppqPositionOfLastBarStart. */
    double    host_bar_start;
    int       has_bar_ref;
    long long host_bar;          /* bar counter, -1 = unknown */

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

static inline int
clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Wrap x into [0, period). */
static inline double
wrap_pos(double x, double period)
{
    double r = fmod(x, period);
    return r < 0.0 ? r + period : r;
}

/* Block-level resolution of the meter / pattern-length settings. All
 * lengths are in quarter notes. */
typedef struct {
    int    pattern_mode;   /* 0 fixed-16, 1 one bar, 2 two bars */
    double qs;             /* quarters per transport beat (host sync) */
    double bar_len_q;      /* bar length in quarters */
    double period_q;       /* pattern period (bar_len_q * nbars) */
    double origin_q;       /* pattern origin in quarters (absolute), already
                              shifted for the bar parity in two-bar mode */
    int    use_bar_ref;    /* origin_q valid (else wrap from beat 0) */
} MeterInfo;

static void
resolve_meter(const StepGateDsp* self, int host_sync,
              float pattern_mode, float meter_source,
              float meter_num, float meter_denom,
              MeterInfo* mi)
{
    mi->pattern_mode = clampi((int)lroundf(pattern_mode), 0, 2);

    /* Transport-beat -> quarter-note normalisation. In a 6/8 host,
     * time:beat counts eighth notes, so one transport beat is 4/8 of a
     * quarter. Free-run counters are kept in quarters by construction. */
    const int    bu  = (self->host_beat_unit > 0) ? self->host_beat_unit : 4;
    const double hqs = 4.0 / (double)bu;
    mi->qs = host_sync ? hqs : 1.0;

    const int manual = ((int)lroundf(meter_source) == 1);
    const int mnum   = clampi((int)lroundf(meter_num), 1, 16);
    const int mden   = clampi((int)lroundf(meter_denom), 1, 16);

    /* Auto follows the host meter whenever the host has announced one,
     * including in Free Run (the free clock is ours but the signature
     * is still musical information worth following); the manual ports
     * are the fallback for silent hosts. */
    if (!manual && self->host_beats_per_bar > 0.0) {
        mi->bar_len_q = self->host_beats_per_bar * hqs;
    } else {
        mi->bar_len_q = (double)mnum * 4.0 / (double)mden;
    }
    if (mi->bar_len_q <= 0.0) mi->bar_len_q = 4.0;

    const int nbars = (mi->pattern_mode == 2) ? 2 : 1;
    mi->period_q = mi->bar_len_q * (double)nbars;

    /* Pattern origin: a host-provided bar reference when available (and
     * meaningful: host sync + auto meter), otherwise bars are counted
     * arithmetically from transport beat 0 (mod-host starts there). */
    mi->use_bar_ref = 0;
    mi->origin_q    = 0.0;
    if (host_sync && !manual && self->has_bar_ref) {
        double origin = self->host_bar_start * mi->qs;
        if (nbars == 2) {
            long long bar = self->host_bar;
            if (bar < 0) {
                /* No bar counter: infer parity arithmetically, assuming
                 * a constant meter since beat 0. */
                bar = (long long)floor(origin / mi->bar_len_q + 0.5);
            }
            if (bar % 2 != 0) origin -= mi->bar_len_q;
        }
        mi->origin_q    = origin;
        mi->use_bar_ref = 1;
    }
}

/* Effective pattern length in steps for one voice. */
static int
active_steps_for(const MeterInfo* mi, double step_in_beats)
{
    if (mi->pattern_mode == 0) return NUM_STEPS;
    long n = lround(mi->period_q / step_in_beats);
    if (n < 1) n = 1;
    if (n > NUM_STEPS) n = NUM_STEPS;
    return (int)n;
}

/* Step index + phase of one voice at absolute position beat_q (quarter
 * notes). In the bar modes the position is taken relative to the
 * pattern origin and wrapped into the pattern period, so step 1 always
 * lands on a bar start. */
static inline int
voice_step_at(const MeterInfo* mi, double beat_q, double step_in_beats,
              int active_steps, double* phase_out)
{
    double seq_pos;
    if (mi->pattern_mode == 0) {
        seq_pos = beat_q / step_in_beats;
    } else {
        seq_pos = wrap_pos(beat_q - mi->origin_q, mi->period_q) / step_in_beats;
    }
    const double seq_floor = floor(seq_pos);
    long step_index = (long)seq_floor;
    *phase_out = seq_pos - seq_floor;
    long mod_step = step_index % active_steps;
    if (mod_step < 0) mod_step += active_steps;
    return (int)mod_step;
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
    self->host_beats_per_bar  = 0.0;
    self->host_beat_unit      = 0;
    self->host_bar_start      = 0.0;
    self->has_bar_ref         = 0;
    self->host_bar            = -1;
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
stepgate_dsp_update_position(StepGateDsp* self, const StepGatePosition* p)
{
    if (p->have_bpm) {
        if (p->bpm > 0.0) self->host_bpm = p->bpm;
    }
    if (p->have_speed) {
        self->host_speed = p->speed;
    }
    if (p->have_beat_unit && p->beat_unit > 0) {
        self->host_beat_unit = p->beat_unit;
    }
    if (p->have_beats_per_bar && p->beats_per_bar > 0.0) {
        self->host_beats_per_bar = p->beats_per_bar;
    }
    if (p->have_bar) {
        self->host_bar = p->bar;
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
    int frame_drove_beat = 0;
    if (p->have_frame && self->host_bpm > 0.0) {
        self->host_beat = p->frame * self->host_bpm / (60.0 * self->sample_rate);
        frame_drove_beat = 1;
    } else if (p->have_beat) {
        double v = p->beat;
        if (!self->has_prev_beat || v != self->prev_received_beat) {
            self->host_beat = v;
            self->has_prev_beat = 1;
        }
        self->prev_received_beat = v;
    }

    /* Bar reference for the bar-aligned pattern modes.
     *
     * JUCE hands us the bar start directly. LV2 hosts give barBeat; the
     * anchor must live on the same axis as whatever drives host_beat:
     *
     * - Frame-driven hosts (frame preferred above): anchor on the beat
     *   just derived from time:frame. This is REQUIRED for mod-host,
     *   whose time:beat is not a global counter at all - it forges
     *   (pos.beat - 1), i.e. the beat WITHIN the bar, so subtracting
     *   barBeat from it would pin every bar to ~0 instead of the real
     *   bar start.
     * - Beat-driven hosts: use (received beat - received barBeat), NOT
     *   the integrated host_beat, so a host that quantises time:beat
     *   to integers (and time:barBeat the same way) cancels its own
     *   quantisation and the difference is the exact bar start.
     * - Hosts like Ardour omit time:beat entirely (frame + barBeat
     *   only): the frame-driven anchor covers them too. */
    if (p->have_bar_start) {
        self->host_bar_start = p->bar_start;
        self->has_bar_ref    = 1;
    } else if (p->have_bar_beat) {
        if (!frame_drove_beat && p->have_beat) {
            self->host_bar_start = p->beat - p->bar_beat;
        } else {
            self->host_bar_start = self->host_beat - p->bar_beat;
        }
        self->has_bar_ref = 1;
    }
}

int
stepgate_dsp_process(StepGateDsp* self, const StepGateParams* params,
                     const float* inL, const float* inR,
                     float* outL, float* outR,
                     int* active_steps_out, uint32_t n_samples)
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

    MeterInfo mi;
    resolve_meter(self, host_sync,
                  params->pattern_mode, params->meter_source,
                  params->meter_num, params->meter_denom, &mi);
    const int active_steps = active_steps_for(&mi, step_in_beats);
    if (active_steps_out) *active_steps_out = active_steps;

    const double beat_inc = bpm / (60.0 * self->sample_rate);

    /* Free-run only: reset to step 1 when lv2:enabled goes 0 -> 1. */
    if (!host_sync && enabled && !self->prev_enabled) {
        self->free_phase = 0.0;
        self->free_step  = 0;
        self->free_beat  = 0.0;
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
            const double beat_q = self->host_beat * mi.qs;
            step = voice_step_at(&mi, beat_q, step_in_beats,
                                 active_steps, &in_step_phase);
        } else if (enabled) {
            if (mi.pattern_mode == 0) {
                step = self->free_step;
                in_step_phase = self->free_phase;
            } else {
                /* Bar modes need an absolute position; the free-run
                 * master beat (in quarters, reset on enable) provides
                 * it, with bars counted arithmetically from 0. */
                step = voice_step_at(&mi, self->free_beat, step_in_beats,
                                     active_steps, &in_step_phase);
            }
            /* Advance both free-run counters so switching pattern_mode
             * mid-flight stays continuous. */
            self->free_beat  += beat_inc;
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
            /* Neighbours wrap at the effective pattern length, so ties
             * behave at the loop seam of a shortened (bar-mode) pattern. */
            const int prev_step = (step + active_steps - 1) % active_steps;
            const int next_step = (step + 1) % active_steps;
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
                           int* active_steps_out,
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

    MeterInfo mi;
    resolve_meter(self, host_sync,
                  shared->pattern_mode, shared->meter_source,
                  shared->meter_num, shared->meter_denom, &mi);

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
    int    active[STEPGATE_MAX_VOICES];
    int    son [STEPGATE_MAX_VOICES][NUM_STEPS];
    int    stie[STEPGATE_MAX_VOICES][NUM_STEPS];
    for (int v = 0; v < num_voices; ++v) {
        step_in_beats[v] =
            step_length_in_beats(voices[v].division, voices[v].division_mod);
        active[v] = active_steps_for(&mi, step_in_beats[v]);
        if (active_steps_out) active_steps_out[v] = active[v];
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
                /* Free-run counters are already in quarters; the host
                 * beat is normalised by the block-constant qs factor. */
                const double beat_q = host_sync ? master * mi.qs : master;
                double in_step_phase;
                step = voice_step_at(&mi, beat_q, step_in_beats[v],
                                     active[v], &in_step_phase);

                const int on        = son[v][step];
                const int prev_step = (step + active[v] - 1) % active[v];
                const int next_step = (step + 1) % active[v];
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
