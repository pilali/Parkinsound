/*
 * Parkinsound Step Gate 4 - modgui controller.
 *
 * Reuses the single-channel Step Gate visual language for four voices.
 *
 * Layout (552x360 viewBox):
 *   Header (y 0..46):
 *     - brand text (drag handle area)
 *     - ENABLED button   (soft bypass, 'enabled')
 *     - SYNC button      (HOST / FREE, 'sync_source')
 *     - TEMPO readout    (x-drag, 'tempo')
 *   Four channel lines, each:
 *     - 16 tie buttons on top of 16 step buttons
 *     - left gutter: "CHn" label + division selector (click cycles)
 *   One ADSR line below: the four voices' ADSR curves side by side,
 *     each with three draggable handles.
 *
 * Only ENABLED, SYNC and TEMPO are shared; everything else is per voice.
 */
function (event, funcs) {
    var NS     = 'http://www.w3.org/2000/svg';
    var NCH    = 4;
    var NSTEPS = 16;

    /* ---- Layout constants -------------------------------------------- */
    var STEP_X0 = 92, STEP_PITCH = 28, STEP_W = 24;
    var TIE_H = 9, STEP_H = 26, TIE_STEP_GAP = 3;
    var CH_TOP0 = 54, CH_H = 50;

    var ADSR_SEP_Y = 250;
    var ADSR_TOP   = 258;
    var COL_X0 = 16, COL_W = 130;
    var A_PX = 24, D_PX = 20, S_PX = 46, R_PX = 20;

    var DIV_LABELS = ['1/1', '1/2', '1/4', '1/8', '1/16', '1/32'];

    /* ---- Geometry helpers -------------------------------------------- */
    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    function stepX(s)   { return STEP_X0 + s * STEP_PITCH; }
    function tieY(ch)   { return CH_TOP0 + ch * CH_H; }
    function stepY(ch)  { return tieY(ch) + TIE_H + TIE_STEP_GAP; }

    function colX(ch)   { return COL_X0 + ch * COL_W; }
    function adsrAx0(ch){ return colX(ch) + 12; }
    function adsrYT()   { return ADSR_TOP + 18; }
    function adsrYB()   { return ADSR_TOP + 70; }

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
        var adsr = [], divs = [];
        for (var c = 0; c < NCH; c++) {
            adsr.push({ attack: 0.0, decay: 0.0, sustain: 1.0, release: 0.5 });
            divs.push(4); /* 1/16 default */
        }
        return { adsr: adsr, div: divs, tempo: 120 };
    }

    /* ---- ADSR geometry ----------------------------------------------- */
    function adsrPts(ch, st) {
        var ax0 = adsrAx0(ch), yt = adsrYT(), yb = adsrYB();
        var xa = ax0 + st.attack  * A_PX;
        var xd = xa  + st.decay   * D_PX;
        var xs = xd  + S_PX;
        var xr = xs  + st.release * R_PX;
        var ys = yt + (1.0 - st.sustain) * (yb - yt);
        return [ [ax0, yb], [xa, yt], [xd, ys], [xs, ys], [xr, yb] ];
    }

    function ptsToStr(pts) {
        return pts.map(function (p) {
            return p[0].toFixed(1) + ',' + p[1].toFixed(1);
        }).join(' ');
    }

    function updateADSR(iconEl, ch) {
        var st  = iconEl._pg.adsr[ch];
        var pts = adsrPts(ch, st);
        var pl  = iconEl.querySelector('.adsr-curve[data-ch="' + ch + '"]');
        if (pl) pl.setAttribute('points', ptsToStr(pts));
        var hA = iconEl.querySelector('.adsr-ha[data-ch="' + ch + '"]');
        var hD = iconEl.querySelector('.adsr-hd[data-ch="' + ch + '"]');
        var hR = iconEl.querySelector('.adsr-hr[data-ch="' + ch + '"]');
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
        return rect.width > 0 ? 552 / rect.width : 1;
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
            var span = adsrYB() - adsrYT();

            if (drag.kind === 'attack') {
                st.attack = clamp(drag.base.attack + dx / A_PX, 0, 1);
                if (funcs) funcs.set_port_value('ch' + n + '_attack', st.attack);
                updateADSR(iconEl, ch);
            } else if (drag.kind === 'decay_sustain') {
                st.decay   = clamp(drag.base.decay   + dx / D_PX, 0, 1);
                st.sustain = clamp(drag.base.sustain - dy / span, 0, 1);
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
        g.appendChild(makeText(12, 37, 'STEP GATE 4', 'brand-sub'));

        var enBtn = makeEl('rect', { x: 184, y: 10, width: 64, height: 26, rx: 4, 'class': 'btn', 'data-action': 'enable' });
        var enLbl = makeText(216, 24, 'OFF', 'btn-label'); enLbl.setAttribute('data-action', 'enable');
        g.appendChild(enBtn); g.appendChild(enLbl);

        var syBtn = makeEl('rect', { x: 256, y: 10, width: 70, height: 26, rx: 4, 'class': 'btn', 'data-action': 'sync' });
        var syLbl = makeText(291, 24, 'HOST', 'btn-label'); syLbl.setAttribute('data-action', 'sync');
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

        var tmBox = makeEl('rect', { x: 360, y: 7, width: 78, height: 32, rx: 4, 'class': 'tempo-box' });
        g.appendChild(tmBox);
        g.appendChild(makeText(399, 21, '120', 'tempo-value'));
        g.appendChild(makeText(399, 34, 'BPM', 'tempo-unit'));
        (function () {
            function begin(e) { startDrag(iconEl, { kind: 'tempo', base: { tempo: iconEl._pg.tempo } }, e); }
            tmBox.addEventListener('mousedown', begin);
            tmBox.addEventListener('touchstart', function (e) { if (e.touches.length === 1) begin(e.touches[0]); });
        })();

        /* ----- Channel lines: tie row on top of step row ----- */
        for (var ch = 0; ch < NCH; ch++) {
            var n = ch + 1;

            /* gutter: channel label + division selector */
            g.appendChild(makeText(8, tieY(ch) + 14, 'CH' + n, 'ch-label'));
            var dBox = makeEl('rect', { x: 8, y: tieY(ch) + 20, width: 64, height: 17, rx: 3, 'class': 'div-box', 'data-ch': ch });
            g.appendChild(dBox);
            var dVal = makeText(40, tieY(ch) + 29, DIV_LABELS[4], 'div-value'); dVal.setAttribute('data-ch', ch);
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

            for (var s = 0; s < NSTEPS; s++) {
                var x = stepX(s);

                var tieRect = makeEl('rect', {
                    x: x, y: tieY(ch), width: STEP_W, height: TIE_H, rx: 1.5,
                    'class': 'tie off',
                    'data-ch': ch, 'data-step': (s + 1),
                    'data-symbol': 'ch' + n + '_step_' + (s + 1) + '_tie'
                });
                g.appendChild(tieRect);

                var stepRect = makeEl('rect', {
                    x: x, y: stepY(ch), width: STEP_W, height: STEP_H, rx: 2,
                    'class': 'step off',
                    'data-ch': ch, 'data-step': (s + 1),
                    'data-symbol': 'ch' + n + '_step_' + (s + 1) + '_on'
                });
                g.appendChild(stepRect);

                [tieRect, stepRect].forEach(function (node) {
                    node.addEventListener('mousedown', stopMouseDown);
                    node.addEventListener('touchstart', stopMouseDown);
                    node.addEventListener('click', onToggleClick);
                });
            }
        }

        /* ----- Separator + ADSR line (four voices side by side) ----- */
        g.appendChild(makeEl('line', { x1: 8, y1: ADSR_SEP_Y, x2: 544, y2: ADSR_SEP_Y, 'class': 'adsr-sep' }));

        for (var c2 = 0; c2 < NCH; c2++) {
            var nn  = c2 + 1;
            var cx  = colX(c2) + COL_W / 2;
            g.appendChild(makeText(cx, ADSR_TOP + 10, 'CH' + nn + ' ADSR', 'col-label'));

            var st  = iconEl._pg.adsr[c2];
            var pts = adsrPts(c2, st);
            g.appendChild(makeEl('polyline', { 'class': 'adsr-curve', 'data-ch': c2, points: ptsToStr(pts) }));

            var hA = makeEl('circle', { cx: pts[1][0], cy: pts[1][1], r: 4.5, 'class': 'adsr-handle adsr-ha', 'data-ch': c2 });
            var hD = makeEl('circle', { cx: pts[2][0], cy: pts[2][1], r: 4.5, 'class': 'adsr-handle adsr-hd', 'data-ch': c2 });
            var hR = makeEl('circle', { cx: pts[4][0], cy: pts[4][1], r: 4.5, 'class': 'adsr-handle adsr-hr', 'data-ch': c2 });
            g.appendChild(hA); g.appendChild(hD); g.appendChild(hR);

            /* A/D/S/R section labels, like the base GUI */
            var lblY = adsrYB() + 12;
            var ax0  = adsrAx0(c2);
            g.appendChild(makeText(ax0 + A_PX * 0.5,                       lblY, 'A', 'adsr-label'));
            g.appendChild(makeText(ax0 + A_PX + D_PX * 0.5,                lblY, 'D', 'adsr-label'));
            g.appendChild(makeText(ax0 + A_PX + D_PX + S_PX * 0.5,         lblY, 'S', 'adsr-label'));
            g.appendChild(makeText(ax0 + A_PX + D_PX + S_PX + R_PX * 0.5,  lblY, 'R', 'adsr-label'));

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
                    el.addEventListener('touchstart', function (e) { if (e.touches.length === 1) begin(e.touches[0]); });
                }
                bind(ha, 'attack');
                bind(hd, 'decay_sustain');
                bind(hr, 'release');
            })(c2, hA, hD, hR);
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
            highlightStep(iconEl, parseInt(m[1], 10) - 1, parseInt(value, 10)); return;
        }
        if ((m = sym.match(/^ch(\d)_(attack|decay|sustain|release)$/))) {
            var c = parseInt(m[1], 10) - 1;
            iconEl._pg.adsr[c][m[2]] = parseFloat(value);
            updateADSR(iconEl, c);
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
