Stepgate plugin LV2 plugin with modgui. 

<img width="250" height="250" alt="screenshot-parkinsound-stepgate" src="https://github.com/user-attachments/assets/d1811acf-d645-4b58-b76b-488c935b438c" />

This repository contains two plug-ins:

- **parkinsound-stepgate.lv2** – the original single (stereo) 16-step audio gate sequencer.
- **parkinsound-stepgate4.lv2** – a 4-channel, sample-locked variant (see below).

## Parkinsound Step Gate 4 (4 channels)

`mod-host` / `mod-ui` give no guarantee that several *separate* rhythmic-gate
plug-ins stay locked to one another sample-for-sample: independent instances
can start on different blocks or re-synchronise to the host transport at
slightly different moments, so they drift.

Step Gate 4 solves this by folding **four independent gate voices into a single
plug-in**. All four channels are processed inside the same `run()` call and
advanced from one shared *master beat*, so at master beat 0 every channel sits
exactly on step 1 / phase 0. The four sequences therefore trigger
simultaneously and stay phase-locked forever.

- **4 mono audio inputs** (`in_1..in_4`) and **4 mono audio outputs** (`out_1..out_4`).
- **Shared** across all channels: Sync Source (Host Sync / Free Run), Tempo, and the global Enabled (soft bypass).
- **Per channel** (independent): the rhythmic Division, the 16 step On/Tie toggles, and the ADSR envelope.

Because every voice runs a different Division but is locked to the same master
beat, the plug-in is ideal for tight polyrhythmic gating (see the `Polyrhythm`
factory preset).

## Build locally

```
git clone https://github.com/pilali/Parkinsound.git
cd Parkinsound
make -j4            # builds both bundles
```

Useful targets: `make stepgate`, `make stepgate4`, `make install-all`
(`make install` installs only the original single-channel bundle).

## Tests

```
gcc -O2 -Wall -o test/divcheck test/divcheck.c -ldl -lm && ./test/divcheck
gcc -O2 -Wall -o test/sync4    test/sync4.c    -ldl -lm && ./test/sync4
```

`test/sync4` verifies the 4-channel sample-accurate synchronisation: identical
channels produce bit-identical output, and a slower channel's step boundaries
land on the exact same samples as a faster channel's.

## Build with mod-plugin-builder

(https://github.com/mod-audio/mod-plugin-builder)

Copy the content of `plugins/package/parkinsound-stepgate` (and/or
`plugins/package/parkinsound-stepgate4`) into
`mod-plugin-builder/plugins/package/`.

Then run `./build my_platform parkinsound-stepgate` (or
`parkinsound-stepgate4`).
