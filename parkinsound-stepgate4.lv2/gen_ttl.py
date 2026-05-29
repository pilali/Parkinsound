#!/usr/bin/env python3
"""
Generate stepgate4.ttl for the Parkinsound Step Gate 4 plug-in.

The plug-in exposes 164 ports laid out exactly as the C source expects:

    0  time_in        (atom sequence, host transport)
    1  sync_source    (shared: Host Sync / Free Run)
    2  tempo          (shared)
    3  enabled        (shared soft bypass)
    4..7   in_1..in_4   (mono audio inputs)
    8..11  out_1..out_4 (mono audio outputs)
    12..   four per-channel blocks of 38 ports each:
        +0  chN_division
        +1  chN_current_step (output)
        +2  chN_attack
        +3  chN_decay
        +4  chN_sustain
        +5  chN_release
        +6.. chN_step_M_on / chN_step_M_tie  (M = 1..16)

Keeping the .ttl machine-generated guarantees it stays consistent with
the computable port layout in stepgate4.c. Run from the bundle dir:

    python3 gen_ttl.py > stepgate4.ttl
"""

NUM_STEPS = 16
NUM_CHANNELS = 4
PLUGIN_URI = "https://github.com/pilali/parkinsound/lv2/stepgate4"
PROJECT_URI = "https://github.com/pilali/parkinsound"

DIV_SCALE = [
    ("1/1", 0), ("1/2", 1), ("1/4", 2),
    ("1/8", 3), ("1/16", 4), ("1/32", 5),
]

ports = []  # each entry is a list of TTL lines (without the leading "[" / trailing "]")


def add(lines):
    ports.append(lines)


def control_in(idx, symbol, name, default, minimum, maximum, props=None, scale=None, unit=None):
    out = [
        "a lv2:ControlPort , lv2:InputPort ;",
        f"lv2:index {idx} ;",
        f'lv2:symbol "{symbol}" ;',
        f'lv2:name "{name}" ;',
        f"lv2:default {default} ; lv2:minimum {minimum} ; lv2:maximum {maximum} ;",
    ]
    if props:
        out.append("lv2:portProperty " + " , ".join(props) + " ;")
    if unit:
        out.append(f"units:unit {unit} ;")
    if scale:
        for label, value in scale:
            out.append(f'lv2:scalePoint [ rdfs:label "{label}" ; rdf:value {value} ] ;')
    return out


# ---- index 0: time input -------------------------------------------------
add([
    "a lv2:InputPort , atom:AtomPort ;",
    "atom:bufferType atom:Sequence ;",
    "atom:supports time:Position ;",
    "lv2:designation lv2:control ;",
    "lv2:index 0 ;",
    'lv2:symbol "time_in" ;',
    'lv2:name "Time" ;',
])

# ---- index 1: sync source (shared) --------------------------------------
add(control_in(
    1, "sync_source", "Sync Source", 0, 0, 1,
    props=["lv2:enumeration", "lv2:integer"],
    scale=[("Host Sync", 0), ("Free Run", 1)],
))

# ---- index 2: tempo (shared) --------------------------------------------
add(control_in(2, "tempo", "Tempo", "120.0", "20.0", "300.0", unit="units:bpm"))

# ---- index 3: enabled (shared soft bypass) ------------------------------
add(control_in(
    3, "enabled", "Enabled", 1, 0, 1,
    props=["lv2:toggled", "lv2:integer", "lv2:notOnGUI"],
))

# ---- indices 4..7: mono audio inputs ------------------------------------
for ch in range(NUM_CHANNELS):
    idx = 4 + ch
    add([
        "a lv2:AudioPort , lv2:InputPort ;",
        f"lv2:index {idx} ;",
        f'lv2:symbol "in_{ch + 1}" ;',
        f'lv2:name "Audio In {ch + 1}" ;',
    ])

# ---- indices 8..11: mono audio outputs ----------------------------------
for ch in range(NUM_CHANNELS):
    idx = 8 + ch
    add([
        "a lv2:AudioPort , lv2:OutputPort ;",
        f"lv2:index {idx} ;",
        f'lv2:symbol "out_{ch + 1}" ;',
        f'lv2:name "Audio Out {ch + 1}" ;',
    ])

# ---- per-channel blocks --------------------------------------------------
CH_BASE = 12
CH_STRIDE = 6 + NUM_STEPS * 2
for ch in range(NUM_CHANNELS):
    base = CH_BASE + ch * CH_STRIDE
    n = ch + 1
    # division
    add(control_in(
        base + 0, f"ch{n}_division", f"Ch{n} Division", 4, 0, 5,
        props=["lv2:enumeration", "lv2:integer"], scale=DIV_SCALE,
    ))
    # current step (output)
    add([
        "a lv2:ControlPort , lv2:OutputPort ;",
        f"lv2:index {base + 1} ;",
        f'lv2:symbol "ch{n}_current_step" ;',
        f'lv2:name "Ch{n} Current Step" ;',
        "lv2:minimum 1 ; lv2:maximum 16 ;",
        "lv2:portProperty lv2:integer ;",
    ])
    # ADSR
    add(control_in(base + 2, f"ch{n}_attack",  f"Ch{n} Attack",  "0.0", "0.0", "1.0"))
    add(control_in(base + 3, f"ch{n}_decay",   f"Ch{n} Decay",   "0.0", "0.0", "1.0"))
    add(control_in(base + 4, f"ch{n}_sustain", f"Ch{n} Sustain", "1.0", "0.0", "1.0"))
    add(control_in(base + 5, f"ch{n}_release", f"Ch{n} Release", "0.5", "0.0", "1.0"))
    # 16 steps: on + tie
    for m in range(NUM_STEPS):
        on_idx  = base + 6 + m * 2
        tie_idx = on_idx + 1
        on_default = 1 if (ch == 0 and m == 0) else 0
        add(control_in(
            on_idx, f"ch{n}_step_{m + 1}_on", f"Ch{n} Step {m + 1} On",
            on_default, 0, 1, props=["lv2:toggled", "lv2:integer"],
        ))
        add(control_in(
            tie_idx, f"ch{n}_step_{m + 1}_tie", f"Ch{n} Step {m + 1} Tie",
            1, 0, 1, props=["lv2:toggled", "lv2:integer"],
        ))


def render():
    header = """@prefix doap:   <http://usefulinc.com/ns/doap#> .
@prefix foaf:   <http://xmlns.com/foaf/0.1/> .
@prefix lv2:    <http://lv2plug.in/ns/lv2core#> .
@prefix atom:   <http://lv2plug.in/ns/ext/atom#> .
@prefix urid:   <http://lv2plug.in/ns/ext/urid#> .
@prefix time:   <http://lv2plug.in/ns/ext/time#> .
@prefix units:  <http://lv2plug.in/ns/extensions/units#> .
@prefix modgui: <http://moddevices.com/ns/modgui#> .
@prefix rdf:    <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs:   <http://www.w3.org/2000/01/rdf-schema#> .

<%(project)s>
    a doap:Project ;
    doap:name "Parkinsound" .

<%(uri)s>
    a lv2:Plugin , lv2:FilterPlugin ;
    doap:name "Parkinsound Step Gate 4" ;
    doap:license <http://opensource.org/licenses/isc> ;
    lv2:project <%(project)s> ;
    lv2:minorVersion 1 ;
    lv2:microVersion 0 ;
    lv2:requiredFeature urid:map ;
    lv2:optionalFeature lv2:hardRTCapable ;
""" % {"uri": PLUGIN_URI, "project": PROJECT_URI}

    port_section = "    lv2:port\n" + " ,\n".join(
        "    [\n        " + "\n        ".join(p) + "\n    ]" for p in ports
    ) + " ;\n"

    monitored = " ,\n        ".join(
        f'[ lv2:symbol "ch{ch + 1}_current_step" ]' for ch in range(NUM_CHANNELS)
    )

    modgui = """
    modgui:gui [
        a modgui:Gui ;
        modgui:resourcesDirectory <modgui> ;
        modgui:iconTemplate    <modgui/icon-parkinsound-stepgate4.html> ;
        modgui:stylesheet      <modgui/stylesheet-parkinsound-stepgate4.css> ;
        modgui:javascript      <modgui/javascript-parkinsound-stepgate4.js> ;
        modgui:screenshot      <modgui/screenshot-parkinsound-stepgate4.png> ;
        modgui:thumbnail       <modgui/thumbnail-parkinsound-stepgate4.png> ;
        modgui:brand           "Parkinsound" ;
        modgui:label           "Step Gate 4" ;
        modgui:model           "" ;
        modgui:panel           "" ;
        modgui:color           "white" ;
        modgui:knob            "" ;
        modgui:monitoredOutputs %(mon)s
    ] .
""" % {"mon": monitored}

    return header + port_section + modgui


if __name__ == "__main__":
    import sys
    sys.stdout.write(render())
