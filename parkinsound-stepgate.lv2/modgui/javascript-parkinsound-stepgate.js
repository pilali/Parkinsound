/*
 * Parkinsound Step Gate - modgui controller.
 *
 * Layout (250x360 viewBox, ring centre 125,125):
 *   - Tie ring  : annulus r in [85, 95]   (16 segments, no gap)
 *   - Step ring : annulus r in [50, 80]   (16 segments, 2deg gap)
 *   - Two centre halves: r in [0, 45]
 *       TOP    : toggles enabled (ON / OFF)  — soft bypass via set_port_value
 *       BOTTOM : toggles sync_source (HOST / FREE)
 *   - ADSR curve : white polyline with 3 draggable handles (y=258..348)
 *       Handle A  : x-drag → attack
 *       Handle DS : x-drag → decay, y-drag → sustain
 *       Handle R  : x-drag → release
 *
 * Bypass note: enabled uses set_port_value (soft bypass handled by the
 * plugin's run() function).  The half-click handler captures iconEl
 * directly rather than using closest() to avoid SVG→HTML traversal bugs.
 */
function (event, funcs) {
    var NS         = 'http://www.w3.org/2000/svg';
    var CX         = 125, CY = 125;
    var STEP_OUTER = 80,  STEP_INNER = 50;
    var TIE_OUTER  = 95,  TIE_INNER  = 85;
    var HALF_R     = 45;
    var GAP_DEG    = 2;
    var STEP_DEG   = 22.5;
    var START_DEG  = -90;

    /* ADSR display area */
    var AX0  = 10;                         /* left edge of curve (matches separator) */
    var AYT  = 258, AYB = 348;            /* top (amp=1.0), bottom (amp=0) */
    var A_PX = 50, D_PX = 44, S_PX = 96, R_PX = 40; /* max pixel widths — total 230, end x=240 */

    /* ------------------------------------------------------------------ */
    /* Geometry helpers                                                    */
    /* ------------------------------------------------------------------ */

    function deg2rad(d) { return d * Math.PI / 180; }

    function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }

    function ringPath(rOuter, rInner, startDeg, endDeg) {
        var a0  = deg2rad(startDeg);
        var a1  = deg2rad(endDeg);
        var ox0 = (CX + rOuter * Math.cos(a0)).toFixed(3);
        var oy0 = (CY + rOuter * Math.sin(a0)).toFixed(3);
        var ox1 = (CX + rOuter * Math.cos(a1)).toFixed(3);
        var oy1 = (CY + rOuter * Math.sin(a1)).toFixed(3);
        var ix1 = (CX + rInner * Math.cos(a1)).toFixed(3);
        var iy1 = (CY + rInner * Math.sin(a1)).toFixed(3);
        var ix0 = (CX + rInner * Math.cos(a0)).toFixed(3);
        var iy0 = (CY + rInner * Math.sin(a0)).toFixed(3);
        var large = (endDeg - startDeg) > 180 ? 1 : 0;
        return ['M', ox0, oy0,
                'A', rOuter, rOuter, 0, large, 1, ox1, oy1,
                'L', ix1, iy1,
                'A', rInner, rInner, 0, large, 0, ix0, iy0,
                'Z'].join(' ');
    }

    function halfPath(r, isTop) {
        var sweep = isTop ? 1 : 0;
        return ['M', (CX - r).toFixed(3), CY,
                'A', r, r, 0, 0, sweep, (CX + r).toFixed(3), CY,
                'Z'].join(' ');
    }

    function makeSvgEl(tag, attrs) {
        var el = document.createElementNS(NS, tag);
        for (var k in attrs) {
            if (Object.prototype.hasOwnProperty.call(attrs, k)) {
                el.setAttribute(k, attrs[k]);
            }
        }
        return el;
    }

    function makeText(x, y, txt, cls) {
        var t = document.createElementNS(NS, 'text');
        t.setAttribute('x', x);
        t.setAttribute('y', y);
        t.setAttribute('class', cls || 'centre-label');
        t.textContent = txt;
        return t;
    }

    /* ------------------------------------------------------------------ */
    /* ADSR geometry                                                       */
    /* ------------------------------------------------------------------ */

    function adsrPts(st) {
        var xa = AX0 + st.attack  * A_PX;
        var xd = xa  + st.decay   * D_PX;
        var xs = xd  + S_PX;
        var xr = xs  + st.release * R_PX;
        var ys = AYT + (1.0 - st.sustain) * (AYB - AYT);
        return [
            [AX0, AYB],  /* P0 start          */
            [xa,  AYT],  /* PA attack peak     */
            [xd,  ys ],  /* PD decay end       */
            [xs,  ys ],  /* PS sustain end     */
            [xr,  AYB]   /* PR release end     */
        ];
    }

    function ptsToStr(pts) {
        return pts.map(function(p) {
            return p[0].toFixed(1) + ',' + p[1].toFixed(1);
        }).join(' ');
    }

    function updateADSRCurve(iconEl) {
        var st  = iconEl._pgState;
        var pts = adsrPts(st);
        var pl  = iconEl.querySelector('.adsr-curve');
        if (pl) pl.setAttribute('points', ptsToStr(pts));
        var hA = iconEl.querySelector('.adsr-ha');
        var hD = iconEl.querySelector('.adsr-hd');
        var hR = iconEl.querySelector('.adsr-hr');
        if (hA) { hA.setAttribute('cx', pts[1][0].toFixed(1)); hA.setAttribute('cy', pts[1][1].toFixed(1)); }
        if (hD) { hD.setAttribute('cx', pts[2][0].toFixed(1)); hD.setAttribute('cy', pts[2][1].toFixed(1)); }
        if (hR) { hR.setAttribute('cx', pts[4][0].toFixed(1)); hR.setAttribute('cy', pts[4][1].toFixed(1)); }
    }

    /* ------------------------------------------------------------------ */
    /* Apply functions (ring / centre state)                              */
    /* ------------------------------------------------------------------ */

    function applyEnabled(iconEl, on) {
        var half = iconEl.querySelector('.centre-half.top');
        var lbl  = iconEl.querySelector('.centre-label.label-top');
        if (half) half.classList.toggle('on', on);
        if (lbl)  {
            lbl.classList.toggle('on', on);
            lbl.textContent = on ? 'ON' : 'OFF';
        }
    }

    function applySync(iconEl, freeRun) {
        var half = iconEl.querySelector('.centre-half.bottom');
        var lbl  = iconEl.querySelector('.centre-label.label-bottom');
        if (half) half.classList.toggle('free', freeRun);
        if (lbl)  lbl.textContent = freeRun ? 'FREE' : 'HOST';
    }

    function applyStepValue(iconEl, symbol, value) {
        var node = iconEl.querySelector('[data-symbol="' + symbol + '"]');
        if (!node) return;
        var on = parseFloat(value) > 0.5;
        node.classList.toggle('on',  on);
        node.classList.toggle('off', !on);
    }

    function highlightCurrentStep(iconEl, stepNum) {
        var paths = iconEl.querySelectorAll('.step');
        for (var i = 0; i < paths.length; i++) {
            paths[i].classList.toggle(
                'playing',
                parseInt(paths[i].getAttribute('data-step'), 10) === stepNum
            );
        }
    }

    /* ------------------------------------------------------------------ */
    /* Click handlers (step / tie)                                        */
    /* ------------------------------------------------------------------ */

    function stopMouseDown(e) { e.stopPropagation(); }

    function onStepOrTieClick(e) {
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

    /* ------------------------------------------------------------------ */
    /* Drag interaction (ADSR handles)                                    */
    /* ------------------------------------------------------------------ */

    function getSvgScale(svgEl) {
        var rect = svgEl.getBoundingClientRect();
        return rect.width > 0 ? 250 / rect.width : 1;
    }

    function startDrag(iconEl, symbol, e) {
        var svgEl = iconEl.querySelector('.parkinsound-stepgate-svg');
        var st    = iconEl._pgState;
        iconEl._pgDrag = {
            symbol:  symbol,
            startX:  e.clientX,
            startY:  e.clientY,
            scale:   getSvgScale(svgEl),
            valA:    st.attack,
            valD:    st.decay,
            valS:    st.sustain,
            valR:    st.release
        };
        e.preventDefault();
        e.stopPropagation();
    }

    function onDocMouseMove(iconEl) {
        return function(e) {
            var drag = iconEl._pgDrag;
            if (!drag) return;
            var sc = drag.scale;
            var dx = (e.clientX - drag.startX) * sc;
            var dy = (e.clientY - drag.startY) * sc;
            var st = iconEl._pgState;

            if (drag.symbol === 'attack') {
                st.attack = clamp(drag.valA + dx / A_PX, 0, 1);
                if (funcs) funcs.set_port_value('attack', st.attack);
                updateADSRCurve(iconEl);

            } else if (drag.symbol === 'decay_sustain') {
                st.decay   = clamp(drag.valD + dx / D_PX, 0, 1);
                st.sustain = clamp(drag.valS - dy / (AYB - AYT), 0, 1);
                if (funcs) {
                    funcs.set_port_value('decay',   st.decay);
                    funcs.set_port_value('sustain', st.sustain);
                }
                updateADSRCurve(iconEl);

            } else if (drag.symbol === 'release') {
                st.release = clamp(drag.valR + dx / R_PX, 0, 1);
                if (funcs) funcs.set_port_value('release', st.release);
                updateADSRCurve(iconEl);
            }
        };
    }

    function onDocMouseUp(iconEl) {
        return function() { iconEl._pgDrag = null; };
    }

    /* ------------------------------------------------------------------ */
    /* Handler attachment helpers                                          */
    /* ------------------------------------------------------------------ */

    function attachStepHandlers(node) {
        node.addEventListener('mousedown',  stopMouseDown);
        node.addEventListener('touchstart', stopMouseDown);
        node.addEventListener('click',      onStepOrTieClick);
    }

    /* Capture iconEl directly to avoid SVG→HTML closest() traversal bugs. */
    function attachHalfHandlers(node, iconEl) {
        node.addEventListener('mousedown',  stopMouseDown);
        node.addEventListener('touchstart', stopMouseDown);
        node.addEventListener('click', function(e) {
            var action = node.getAttribute('data-action');
            if (action === 'enable') {
                var on = !node.classList.contains('on');
                if (funcs) funcs.set_port_value('enabled', on ? 1 : 0);
                applyEnabled(iconEl, on);
            } else if (action === 'sync') {
                var free = !node.classList.contains('free');
                if (funcs) funcs.set_port_value('sync_source', free ? 1 : 0);
                applySync(iconEl, free);
            }
            e.stopPropagation();
            e.preventDefault();
        });
    }

    /* ------------------------------------------------------------------ */
    /* Build the SVG                                                       */
    /* ------------------------------------------------------------------ */

    function buildRings(rootEl) {
        var svg = rootEl.find('.parkinsound-stepgate-svg')[0];
        if (!svg || svg.getAttribute('data-built') === 'true') return;
        svg.setAttribute('data-built', 'true');

        var iconEl      = rootEl[0];
        var tieGroup    = svg.querySelector('.tie-ring');
        var stepGroup   = svg.querySelector('.step-ring');
        var halvesGroup = svg.querySelector('.centre-halves');
        var labelsGroup = svg.querySelector('.centre-labels');
        var adsrGroup   = svg.querySelector('.adsr-group');

        /* --- 16 step + 16 tie sectors --- */
        for (var i = 0; i < 16; i++) {
            var n         = i + 1;
            var tieStart  = START_DEG + i * STEP_DEG;
            var tieEnd    = tieStart + STEP_DEG;
            var stepStart = tieStart + GAP_DEG / 2;
            var stepEnd   = tieEnd   - GAP_DEG / 2;

            var tie = document.createElementNS(NS, 'path');
            tie.setAttribute('d',           ringPath(TIE_OUTER, TIE_INNER, tieStart, tieEnd));
            tie.setAttribute('class',       'tie off');
            tie.setAttribute('data-step',   n);
            tie.setAttribute('data-symbol', 'step_' + n + '_tie');
            tieGroup.appendChild(tie);
            attachStepHandlers(tie);

            var step = document.createElementNS(NS, 'path');
            step.setAttribute('d',           ringPath(STEP_OUTER, STEP_INNER, stepStart, stepEnd));
            step.setAttribute('class',       'step off');
            step.setAttribute('data-step',   n);
            step.setAttribute('data-symbol', 'step_' + n + '_on');
            stepGroup.appendChild(step);
            attachStepHandlers(step);
        }

        /* --- 2 centre halves --- */
        var topHalf = document.createElementNS(NS, 'path');
        topHalf.setAttribute('d',           halfPath(HALF_R, true));
        topHalf.setAttribute('class',       'centre-half top');
        topHalf.setAttribute('data-action', 'enable');
        halvesGroup.appendChild(topHalf);
        attachHalfHandlers(topHalf, iconEl);

        var bottomHalf = document.createElementNS(NS, 'path');
        bottomHalf.setAttribute('d',           halfPath(HALF_R, false));
        bottomHalf.setAttribute('class',       'centre-half bottom');
        bottomHalf.setAttribute('data-action', 'sync');
        halvesGroup.appendChild(bottomHalf);
        attachHalfHandlers(bottomHalf, iconEl);

        var topLabel    = makeText(CX, CY - HALF_R * 0.45, 'OFF',  'centre-label label-top');
        var bottomLabel = makeText(CX, CY + HALF_R * 0.45, 'HOST', 'centre-label label-bottom');
        labelsGroup.appendChild(topLabel);
        labelsGroup.appendChild(bottomLabel);

        /* --- Separator line between ring and ADSR section --- */
        adsrGroup.appendChild(makeSvgEl('line', {
            x1: '8', y1: '253', x2: '242', y2: '253',
            class: 'adsr-separator'
        }));

        /* --- ADSR curve --- */
        var st  = iconEl._pgState;
        var pts = adsrPts(st);

        adsrGroup.appendChild(makeSvgEl('polyline', {
            class:  'adsr-curve',
            points: ptsToStr(pts)
        }));

        /* Handles */
        var hA = makeSvgEl('circle', {
            cx: pts[1][0].toFixed(1), cy: pts[1][1].toFixed(1),
            r: 5, class: 'adsr-handle adsr-ha'
        });
        var hD = makeSvgEl('circle', {
            cx: pts[2][0].toFixed(1), cy: pts[2][1].toFixed(1),
            r: 5, class: 'adsr-handle adsr-hd'
        });
        var hR = makeSvgEl('circle', {
            cx: pts[4][0].toFixed(1), cy: pts[4][1].toFixed(1),
            r: 5, class: 'adsr-handle adsr-hr'
        });
        adsrGroup.appendChild(hA);
        adsrGroup.appendChild(hD);
        adsrGroup.appendChild(hR);

        /* ADSR labels (static midpoints of each section) */
        var lblY = 357;
        adsrGroup.appendChild(makeText(AX0 + A_PX * 0.5,                      lblY, 'A', 'adsr-label'));
        adsrGroup.appendChild(makeText(AX0 + A_PX + D_PX * 0.5,               lblY, 'D', 'adsr-label'));
        adsrGroup.appendChild(makeText(AX0 + A_PX + D_PX + S_PX * 0.5,        lblY, 'S', 'adsr-label'));
        adsrGroup.appendChild(makeText(AX0 + A_PX + D_PX + S_PX + R_PX * 0.5, lblY, 'R', 'adsr-label'));

        /* Handle drag events */
        function addHandleDrag(el, symbol) {
            el.addEventListener('mousedown', function(e) {
                startDrag(iconEl, symbol, e);
            });
            el.addEventListener('touchstart', function(e) {
                if (e.touches.length === 1) startDrag(iconEl, symbol, e.touches[0]);
            });
        }
        addHandleDrag(hA, 'attack');
        addHandleDrag(hD, 'decay_sustain');
        addHandleDrag(hR, 'release');

        /* --- Global drag handlers (attached once per icon element) --- */
        if (!iconEl._pgHandlers) {
            iconEl._pgHandlers = true;
            var mmHandler = onDocMouseMove(iconEl);
            var muHandler = onDocMouseUp(iconEl);
            document.addEventListener('mousemove', mmHandler);
            document.addEventListener('mouseup',   muHandler);
            document.addEventListener('touchmove', function(e) {
                if (iconEl._pgDrag && e.touches.length === 1) {
                    mmHandler({ clientX: e.touches[0].clientX, clientY: e.touches[0].clientY });
                    e.preventDefault();
                }
            }, { passive: false });
            document.addEventListener('touchend', muHandler);
        }
    }

    /* ------------------------------------------------------------------ */
    /* modgui lifecycle                                                    */
    /* ------------------------------------------------------------------ */

    var icon   = $(event.icon);
    var iconEl = icon[0];

    /* Persistent parameter state attached to the icon DOM element */
    if (!iconEl._pgState) {
        iconEl._pgState = { attack: 0.0, decay: 0.0, sustain: 1.0, release: 0.5 };
    }

    if (event.type === 'start') {
        buildRings(icon);

        if (event.value) {
            for (var sym in event.value) {
                if (!Object.prototype.hasOwnProperty.call(event.value, sym)) continue;
                var v = parseFloat(event.value[sym]);
                if      (sym === 'current_step') { highlightCurrentStep(iconEl, parseInt(event.value[sym], 10)); }
                else if (sym === 'enabled')      { applyEnabled(iconEl, v > 0.5); }
                else if (sym === 'sync_source')  { applySync(iconEl, v > 0.5); }
                else if (sym === 'attack')       { iconEl._pgState.attack  = v; }
                else if (sym === 'decay')        { iconEl._pgState.decay   = v; }
                else if (sym === 'sustain')      { iconEl._pgState.sustain = v; }
                else if (sym === 'release')      { iconEl._pgState.release = v; }
                else if (sym.indexOf('step_') === 0) { applyStepValue(iconEl, sym, event.value[sym]); }
            }
            updateADSRCurve(iconEl);
        }
        return;
    }

    if (event.type === 'change') {
        var sym2 = event.symbol;
        var v2   = parseFloat(event.value);
        if      (sym2 === 'current_step') { highlightCurrentStep(iconEl, parseInt(event.value, 10)); }
        else if (sym2 === 'enabled')      { applyEnabled(iconEl, v2 > 0.5); }
        else if (sym2 === 'sync_source')  { applySync(iconEl, v2 > 0.5); }
        else if (sym2 === 'attack')       { iconEl._pgState.attack  = v2; updateADSRCurve(iconEl); }
        else if (sym2 === 'decay')        { iconEl._pgState.decay   = v2; updateADSRCurve(iconEl); }
        else if (sym2 === 'sustain')      { iconEl._pgState.sustain = v2; updateADSRCurve(iconEl); }
        else if (sym2 === 'release')      { iconEl._pgState.release = v2; updateADSRCurve(iconEl); }
        else if (sym2 && sym2.indexOf('step_') === 0) { applyStepValue(iconEl, sym2, event.value); }
        return;
    }
}
