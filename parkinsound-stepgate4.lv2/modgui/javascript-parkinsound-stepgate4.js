/*
 * Parkinsound Step Gate 4 - modgui controller.
 *
 * Layout (540x524 viewBox):
 *   Header (y 0..44):
 *     - brand text (drag handle area)
 *     - ENABLED button   (soft bypass, set_port_value 'enabled')
 *     - SYNC button      (HOST / FREE, 'sync_source')
 *     - TEMPO readout    (x-drag, 'tempo')
 *   Four channel rows, each:
 *     - left gutter: "CHn", division selector (click cycles), current step
 *     - 16 step buttons  ('chN_step_M_on')
 *     - 16 tie  buttons  ('chN_step_M_tie')
 *     - ADSR curve with 3 draggable handles ('chN_attack' ...)
 *
 * Everything is built dynamically so the geometry is defined once. The
 * four channels are completely independent in the UI; only ENABLED,
 * SYNC and TEMPO are shared.
 */
function (event, funcs) {
    var NS     = 'http://www.w3.org/2000/svg';
    var NCH    = 4;
    var NSTEPS = 16;

    /* ---- Layout constants -------------------------------------------- */
    var HEADER_H = 44;
    var STEP_X0  = 80, STEP_W = 25, STEP_GAP = 3;
    var STEP_H   = 26, TIE_H = 9;
    var CH_TOP0  = 56, CH_H = 116;

    /* ADSR section pixel widths (sum + AX0 must stay within the grid). */
    var AX0  = STEP_X0;
    var A_PX = 90, D_PX = 80, S_PX = 170, R_PX = 80;
    var ADSR_H = 48;

    var DIV_LABELS = ['1/1', '1/2', '1/4', '1/8', '1/16', '1/32'];

    /* ---- Geometry helpers -------------------------------------------- */
    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    function stepX(s)    { return STEP_X0 + s * (STEP_W + STEP_GAP); }
    function rowTop(ch)  { return CH_TOP0 + ch * CH_H; }
    function stepY(ch)   { return rowTop(ch); }
    function tieY(ch)    { return rowTop(ch) + STEP_H + 2; }
    function adsrTop(ch) { return rowTop(ch) + STEP_H + TIE_H + 8; }
    function adsrBot(ch) { return adsrTop(ch) + ADSR_H; }

    function makeEl(tag, attrs) {
        var el = document.createElementNS(NS, tag);
        for (var k in attrs) {
            if (Object.prototype.hasOwnProperty.call(attrs, k)) {
                el.setAttribute(k, attrs[k]);
            }
        }
        return el;
    }

    function makeText(x, y, txt, cls) {
        var t = makeEl('text', { x: x, y: y, 'class': cls || '' });
        t.textContent = txt;
        return t;
    }

    /* ---- Per-icon state ---------------------------------------------- */
    function defaultState() {
        var adsr = [];
        for (var c = 0; c < NCH; c++) {
            adsr.push({ attack: 0.0, decay: 0.0, sustain: 1.0, release: 0.5 });
        }
        var divs = [];
        for (var d = 0; d < NCH; d++) divs.push(4); /* 1/16 default */
        return { adsr: adsr, div: divs, tempo: 120 };
    }

    /* ---- ADSR geometry ----------------------------------------------- */
    function adsrPts(ch, st) {
        var yt = adsrTop(ch), yb = adsrBot(ch);
        var xa = AX0 + st.attack  * A_PX;
        var xd = xa  + st.decay   * D_PX;
        var xs = xd  + S_PX;
        var xr = xs  + st.release * R_PX;
        var ys = yt + (1.0 - st.sustain) * (yb - yt);
        return [
            [AX0, yb], [xa, yt], [xd, ys], [xs, ys], [xr, yb]
        ];
    }

    function ptsToStr(pts) {
        return pts.map(function (p) {
            return p[0].toFixed(1) + ',' + p[1].toFixed(1);
        }).join(' ');
    }

    function updateADSR(iconEl, ch) {
        var st  = iconEl._pg.adsr[ch];
        var pts = adsrPts(ch, st);
        var g   = iconEl;
        var pl  = g.querySelector('.adsr-curve[data-ch="' + ch + '"]');
        if (pl) pl.setAttribute('points', ptsToStr(pts));
        var hA = g.querySelector('.adsr-ha[data-ch="' + ch + '"]');
        var hD = g.querySelector('.adsr-hd[data-ch="' + ch + '"]');
        var hR = g.querySelector('.adsr-hr[data-ch="' + ch + '"]');
        if (hA) { hA.setAttribute('cx', pts[1][0].toFixed(1)); hA.setAttribute('cy', pts[1][1].toFixed(1)); }
        if (hD) { hD.setAttribute('cx', pts[2][0].toFixed(1)); hD.setAttribute('cy', pts[2][1].toFixed(1)); }
        if (hR) { hR.setAttribute('cx', pts[4][0].toFixed(1)); hR.setAttribute('cy', pts[4][1].toFixed(1)); }
    }

    /* ---- Apply functions --------------------------------------------- */
    function applyEnabled(iconEl, on) {
        var b = iconEl.querySelector('.btn[data-action="enable"]');
        var l = iconEl.querySelector('.btn-label[data-action="enable"]');
        if (b) b.classList.toggle('on', on);
        if (l) { l.classList.toggle('on', on); l.textContent = on ? 'ON' : 'OFF'; }
    }

    function applySync(iconEl, freeRun) {
        var l = iconEl.querySelector('.btn-label[data-action="sync"]');
        if (l) l.textContent = freeRun ? 'FREE' : 'HOST';
    }

    function applyTempo(iconEl, bpm) {
        iconEl._pg.tempo = bpm;
        var t = iconEl.querySelector('.tempo-value');
        if (t) t.textContent = String(Math.round(bpm));
    }

    function applyDivision(iconEl, ch, idx) {
        idx = clamp(Math.round(idx), 0, 5);
        iconEl._pg.div[ch] = idx;
        var l = iconEl.querySelector('.div-value[data-ch="' + ch + '"]');
        if (l) l.textContent = DIV_LABELS[idx];
    }

    function applyStepValue(iconEl, symbol, value) {
        var node = iconEl.querySelector('[data-symbol="' + symbol + '"]');
        if (!node) return;
        var on = parseFloat(value) > 0.5;
        node.classList.toggle('on', on);
        node.classList.toggle('off', !on);
    }

    function highlightStep(iconEl, ch, stepNum) {
        var rects = iconEl.querySelectorAll('.step[data-ch="' + ch + '"]');
        for (var i = 0; i < rects.length; i++) {
            rects[i].classList.toggle(
                'playing',
                parseInt(rects[i].getAttribute('data-step'), 10) === stepNum
            );
        }
    }

    /* ---- Click handlers ---------------------------------------------- */
    function stopMouseDown(e) { e.stopPropagation(); }

    function onToggleClick(e) {
        var symbol  = this.getAttribute('data-symbol');
        var current = this.classList.contains('on') ? 1 : 0;
        var next    = 1 - current;
        if (funcs && typeof funcs.set_port_value === 'function') {
            funcs.set_port_value(symbol, next);
        }
        this.classList.toggle('on',  next === 1);
        this.classList.toggle('off', next === 0);
        e.stopPropagation();
        e.preventDefault();
    }

    /* ---- Drag (ADSR handles + tempo) --------------------------------- */
    function getScale(svgEl) {
        var rect = svgEl.getBoundingClientRect();
        return rect.width > 0 ? 540 / rect.width : 1;
    }

    function startDrag(iconEl, info, e) {
        var svgEl = iconEl.querySelector('.parkinsound-stepgate4-svg');
        iconEl._pgDrag = {
            kind:   info.kind,
            ch:     info.ch,
            startX: e.clientX,
            startY: e.clientY,
            scale:  getScale(svgEl),
            base:   info.base
        };
        e.preventDefault();
        e.stopPropagation();
    }

    function onMove(iconEl) {
        return function (e) {
            var drag = iconEl._pgDrag;
            if (!drag) return;
            var sc = drag.scale;
            var dx = (e.clientX - drag.startX) * sc;
            var dy = (e.clientY - drag.startY) * sc;

            if (drag.kind === 'tempo') {
                var bpm = clamp(drag.base.tempo + dx * 0.5, 20, 300);
                applyTempo(iconEl, bpm);
                if (funcs) funcs.set_port_value('tempo', bpm);
                return;
            }

            var ch = drag.ch;
            var st = iconEl._pg.adsr[ch];
            var n  = ch + 1;
            var yb = adsrBot(ch), yt = adsrTop(ch);

            if (drag.kind === 'attack') {
                st.attack = clamp(drag.base.attack + dx / A_PX, 0, 1);
                if (funcs) funcs.set_port_value('ch' + n + '_attack', st.attack);
                updateADSR(iconEl, ch);
            } else if (drag.kind === 'decay_sustain') {
                st.decay   = clamp(drag.base.decay   + dx / D_PX, 0, 1);
                st.sustain = clamp(drag.base.sustain - dy / (yb - yt), 0, 1);
                if (funcs) {
                    funcs.set_port_value('ch' + n + '_decay',   st.decay);
                    funcs.set_port_value('ch' + n + '_sustain', st.sustain);
                }
                updateADSR(iconEl, ch);
            } else if (drag.kind === 'release') {
                st.release = clamp(drag.base.release + dx / R_PX, 0, 1);
                if (funcs) funcs.set_port_value('ch' + n + '_release', st.release);
                updateADSR(iconEl, ch);
            }
        };
    }

    function onUp(iconEl) { return function () { iconEl._pgDrag = null; }; }

    /* ---- Build the whole UI ------------------------------------------ */
    function build(rootEl) {
        var svg = rootEl.find('.parkinsound-stepgate4-svg')[0];
        if (!svg || svg.getAttribute('data-built') === 'true') return;
        svg.setAttribute('data-built', 'true');

        var iconEl = rootEl[0];
        var g = svg.querySelector('.content');

        /* ----- Header ----- */
        g.appendChild(makeText(12, 22, 'PARKINSOUND', 'brand'));
        g.appendChild(makeText(12, 36, 'STEP GATE 4', 'brand-sub'));

        /* ENABLED button */
        var enBtn = makeEl('rect', {
            x: 178, y: 9, width: 64, height: 26, rx: 4,
            'class': 'btn', 'data-action': 'enable'
        });
        var enLbl = makeText(210, 23, 'OFF', 'btn-label');
        enLbl.setAttribute('data-action', 'enable');
        g.appendChild(enBtn); g.appendChild(enLbl);

        /* SYNC button */
        var syBtn = makeEl('rect', {
            x: 250, y: 9, width: 70, height: 26, rx: 4,
            'class': 'btn', 'data-action': 'sync'
        });
        var syLbl = makeText(285, 23, 'HOST', 'btn-label');
        syLbl.setAttribute('data-action', 'sync');
        g.appendChild(syBtn); g.appendChild(syLbl);

        function headerBtn(node, action) {
            node.addEventListener('mousedown', stopMouseDown);
            node.addEventListener('touchstart', stopMouseDown);
            node.addEventListener('click', function (e) {
                if (action === 'enable') {
                    var on = !enBtn.classList.contains('on');
                    if (funcs) funcs.set_port_value('enabled', on ? 1 : 0);
                    applyEnabled(iconEl, on);
                } else {
                    var free = (syLbl.textContent === 'HOST');
                    if (funcs) funcs.set_port_value('sync_source', free ? 1 : 0);
                    applySync(iconEl, free);
                }
                e.stopPropagation();
                e.preventDefault();
            });
        }
        headerBtn(enBtn, 'enable');
        headerBtn(syBtn, 'sync');

        /* TEMPO readout (draggable) */
        var tmBox = makeEl('rect', {
            x: 360, y: 6, width: 78, height: 32, rx: 4, 'class': 'tempo-box'
        });
        g.appendChild(tmBox);
        g.appendChild(makeText(399, 20, '120', 'tempo-value'));
        g.appendChild(makeText(399, 33, 'BPM', 'tempo-unit'));
        (function () {
            function begin(e) {
                startDrag(iconEl, { kind: 'tempo', base: { tempo: iconEl._pg.tempo } }, e);
            }
            tmBox.addEventListener('mousedown', begin);
            tmBox.addEventListener('touchstart', function (e) {
                if (e.touches.length === 1) begin(e.touches[0]);
            });
        })();

        /* ----- Channel rows ----- */
        for (var ch = 0; ch < NCH; ch++) {
            var n  = ch + 1;
            var rt = rowTop(ch);

            /* separator above each row (except first) */
            if (ch > 0) {
                g.appendChild(makeEl('line', {
                    x1: 6, y1: rt - 8, x2: 534, y2: rt - 8, 'class': 'row-sep'
                }));
            }

            /* gutter: channel label */
            g.appendChild(makeText(8, rt + 12, 'CH' + n, 'ch-label'));

            /* gutter: division selector */
            var dBox = makeEl('rect', {
                x: 8, y: rt + 20, width: 56, height: 20, rx: 3,
                'class': 'div-box', 'data-ch': ch
            });
            g.appendChild(dBox);
            var dVal = makeText(36, rt + 30, DIV_LABELS[4], 'div-value');
            dVal.setAttribute('data-ch', ch);
            g.appendChild(dVal);
            (function (cch, box) {
                function cycle(e) {
                    var next = (iconEl._pg.div[cch] + 1) % 6;
                    applyDivision(iconEl, cch, next);
                    if (funcs) funcs.set_port_value('ch' + (cch + 1) + '_division', next);
                    e.stopPropagation();
                    e.preventDefault();
                }
                box.addEventListener('mousedown', stopMouseDown);
                box.addEventListener('touchstart', stopMouseDown);
                box.addEventListener('click', cycle);
            })(ch, dBox);

            /* gutter: current step readout */
            var cs = makeText(8, rt + 56, 'step -', 'cur-step');
            cs.setAttribute('data-ch', ch);
            g.appendChild(cs);

            /* step + tie buttons */
            for (var s = 0; s < NSTEPS; s++) {
                var x = stepX(s);

                var stepRect = makeEl('rect', {
                    x: x, y: stepY(ch), width: STEP_W, height: STEP_H, rx: 2,
                    'class': 'step off',
                    'data-ch': ch, 'data-step': (s + 1),
                    'data-symbol': 'ch' + n + '_step_' + (s + 1) + '_on'
                });
                g.appendChild(stepRect);

                var tieRect = makeEl('rect', {
                    x: x, y: tieY(ch), width: STEP_W, height: TIE_H, rx: 1.5,
                    'class': 'tie off',
                    'data-ch': ch, 'data-step': (s + 1),
                    'data-symbol': 'ch' + n + '_step_' + (s + 1) + '_tie'
                });
                g.appendChild(tieRect);

                [stepRect, tieRect].forEach(function (node) {
                    node.addEventListener('mousedown', stopMouseDown);
                    node.addEventListener('touchstart', stopMouseDown);
                    node.addEventListener('click', onToggleClick);
                });
            }

            /* ADSR curve + handles */
            var st  = iconEl._pg.adsr[ch];
            var pts = adsrPts(ch, st);
            var curve = makeEl('polyline', {
                'class': 'adsr-curve', 'data-ch': ch, points: ptsToStr(pts)
            });
            g.appendChild(curve);

            var hA = makeEl('circle', { cx: pts[1][0], cy: pts[1][1], r: 4.5, 'class': 'adsr-handle adsr-ha', 'data-ch': ch });
            var hD = makeEl('circle', { cx: pts[2][0], cy: pts[2][1], r: 4.5, 'class': 'adsr-handle adsr-hd', 'data-ch': ch });
            var hR = makeEl('circle', { cx: pts[4][0], cy: pts[4][1], r: 4.5, 'class': 'adsr-handle adsr-hr', 'data-ch': ch });
            g.appendChild(hA); g.appendChild(hD); g.appendChild(hR);

            (function (cch, ha, hd, hr) {
                function bind(el, kind) {
                    function begin(e) {
                        var s2 = iconEl._pg.adsr[cch];
                        startDrag(iconEl, {
                            kind: kind, ch: cch,
                            base: { attack: s2.attack, decay: s2.decay, sustain: s2.sustain, release: s2.release }
                        }, e);
                    }
                    el.addEventListener('mousedown', begin);
                    el.addEventListener('touchstart', function (e) {
                        if (e.touches.length === 1) begin(e.touches[0]);
                    });
                }
                bind(ha, 'attack');
                bind(hd, 'decay_sustain');
                bind(hr, 'release');
            })(ch, hA, hD, hR);
        }

        /* global drag listeners (attached once) */
        if (!iconEl._pgHandlers) {
            iconEl._pgHandlers = true;
            var mm = onMove(iconEl);
            var mu = onUp(iconEl);
            document.addEventListener('mousemove', mm);
            document.addEventListener('mouseup', mu);
            document.addEventListener('touchmove', function (e) {
                if (iconEl._pgDrag && e.touches.length === 1) {
                    mm({ clientX: e.touches[0].clientX, clientY: e.touches[0].clientY });
                    e.preventDefault();
                }
            }, { passive: false });
            document.addEventListener('touchend', mu);
        }
    }

    /* ---- Dispatch a single symbol/value pair ------------------------- */
    function dispatch(iconEl, sym, value) {
        if (sym === 'enabled')     { applyEnabled(iconEl, parseFloat(value) > 0.5); return; }
        if (sym === 'sync_source') { applySync(iconEl, parseFloat(value) > 0.5); return; }
        if (sym === 'tempo')       { applyTempo(iconEl, parseFloat(value)); return; }

        var m;
        if ((m = sym.match(/^ch(\d)_division$/))) {
            applyDivision(iconEl, parseInt(m[1], 10) - 1, parseFloat(value)); return;
        }
        if ((m = sym.match(/^ch(\d)_current_step$/))) {
            var ch = parseInt(m[1], 10) - 1;
            highlightStep(iconEl, ch, parseInt(value, 10));
            var cs = iconEl.querySelector('.cur-step[data-ch="' + ch + '"]');
            if (cs) cs.textContent = 'step ' + parseInt(value, 10);
            return;
        }
        if ((m = sym.match(/^ch(\d)_(attack|decay|sustain|release)$/))) {
            var c2 = parseInt(m[1], 10) - 1;
            iconEl._pg.adsr[c2][m[2]] = parseFloat(value);
            updateADSR(iconEl, c2);
            return;
        }
        if (/^ch\d_step_\d+_(on|tie)$/.test(sym)) {
            applyStepValue(iconEl, sym, value);
            return;
        }
    }

    /* ---- modgui lifecycle -------------------------------------------- */
    var icon   = $(event.icon);
    var iconEl = icon[0];

    if (!iconEl._pg) iconEl._pg = defaultState();

    if (event.type === 'start') {
        build(icon);
        if (event.value) {
            /* First pass: ADSR/division state, so curves render correctly. */
            for (var sym in event.value) {
                if (!Object.prototype.hasOwnProperty.call(event.value, sym)) continue;
                dispatch(iconEl, sym, event.value[sym]);
            }
        }
        return;
    }

    if (event.type === 'change') {
        dispatch(iconEl, event.symbol, event.value);
        return;
    }
}
