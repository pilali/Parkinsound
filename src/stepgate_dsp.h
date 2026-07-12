/*
 * Parkinsound Step Gate - host-agnostic DSP core.
 *
 * This module contains the entire signal-processing engine of the Step
 * Gate, with ZERO dependency on any plug-in format (no lv2.h, no JUCE).
 * It is consumed both from C (the LV2 wrapper, stepgate.c) and from C++
 * (the JUCE wrapper) via the extern "C" boundary below.
 *
 * The arithmetic here is a verbatim extraction of the original LV2
 * run()/activate()/instantiate() bodies; the output is bit-identical to
 * the pre-refactor plug-in.
 */
#ifndef STEPGATE_DSP_H
#define STEPGATE_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STEPGATE_NUM_STEPS  16
#define STEPGATE_MAX_VOICES 4

/* One value per control port, copied verbatim from the LV2 control
 * ports / JUCE parameters. Raw values are accepted: the rounding,
 * thresholding and clamping happen inside stepgate_dsp_process(),
 * exactly as the original run() did, so the sound is unchanged. */
typedef struct {
    float sync_source;                      /* 0 = Host Sync, 1 = Free Run */
    float tempo;                            /* bpm control (free-run / fallback) */
    float division;                         /* 0..5 -> 1/1,1/2,1/4,1/8,1/16,1/32 */
    float division_mod;                     /* 0 = straight, 1 = dotted (x1.5),
                                               2 = triplet (x2/3) */
    float step_on[STEPGATE_NUM_STEPS];      /* per-step gate on/off */
    float step_tie[STEPGATE_NUM_STEPS];     /* per-step tie flag */
    float enabled;                          /* lv2:enabled (1 = active) */
    float attack;                           /* ADSR, fractions of step length */
    float decay;
    float sustain;
    float release;
} StepGateParams;

/* Multi-voice variant (Step Gate 4): every voice reads its step position
 * from ONE shared master beat, so the voices are phase-locked
 * sample-for-sample. Shared controls first, then one block per voice. */
typedef struct {
    float sync_source;                      /* 0 = Host Sync, 1 = Free Run */
    float tempo;                            /* bpm control (free-run / fallback) */
    float enabled;                          /* shared soft bypass */
} StepGateSharedParams;

typedef struct {
    float division;                         /* 0..5, same scale as above */
    float division_mod;                     /* 0 straight, 1 dotted, 2 triplet */
    float step_on[STEPGATE_NUM_STEPS];
    float step_tie[STEPGATE_NUM_STEPS];
    float attack;
    float decay;
    float sustain;
    float release;
} StepGateVoiceParams;

typedef struct StepGateDsp StepGateDsp;     /* opaque state */

StepGateDsp* stepgate_dsp_new(double sample_rate);
void         stepgate_dsp_free(StepGateDsp*);
void         stepgate_dsp_reset(StepGateDsp*);   /* = activate(): clears phase/gate */

/* Feed host transport, mirroring the original time:Position handling.
 * Pass have_<field> = 0 for any field the host did not supply this block;
 * the corresponding value is then ignored. Call zero or more times (once
 * per transport event, in order) before stepgate_dsp_process(). */
void stepgate_dsp_update_position(StepGateDsp*,
                                  int have_bpm,   double bpm,
                                  int have_beat,  double beat,
                                  int have_speed, double speed,
                                  int have_frame, double frame);

/* Process n frames. inL/inR may be NULL (treated as silence); outL/outR
 * may be NULL (that channel is skipped). The same gate is applied to both
 * channels. Returns the 1-based current step (for the "current_step"
 * display / monitored output). */
int stepgate_dsp_process(StepGateDsp*, const StepGateParams*,
                         const float* inL, const float* inR,
                         float* outL, float* outR, uint32_t n);

/* Multi-voice processing (Step Gate 4). All num_voices voices (at most
 * STEPGATE_MAX_VOICES) are advanced from one shared master beat: the
 * master value is sampled BEFORE it is advanced, so master beat 0 lands
 * exactly on step 1 / phase 0 for every voice and the sequences trigger
 * simultaneously. ins[v]/outs[v] may be NULL (silence in / voice
 * skipped). current_steps (may be NULL) receives the 1-based current
 * step of each voice. */
void stepgate_dsp_process_multi(StepGateDsp*,
                                const StepGateSharedParams*,
                                const StepGateVoiceParams* voices,
                                int num_voices,
                                const float* const* ins,
                                float* const* outs,
                                int* current_steps,
                                uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* STEPGATE_DSP_H */
