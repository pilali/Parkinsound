/*
 * Parkinsound Step Gate - modgui controller.
 *
 * Layout (250x250 viewBox, centre 125,125):
 *   - Tie ring  : annulus r in [85, 95]   (16 segments, no gap)
 *   - Step ring : annulus r in [50, 80]   (16 segments, 2deg gap)
 *   - Two centre halves: r in [0, 45]
 *       TOP    : toggles lv2:enabled (disable / enable)
 *       BOTTOM : toggles sync_source (HOST / FREE)
 *
 * Each interactive element attaches its own click / mousedown /
 * touchstart so we never depend on bubbling through SVG containers
 * that may have pointer-events:none.
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

    function deg2rad(d) { return d * Math.PI / 180; }

    function ringPath(rOuter, rInner, startDeg, endDeg) {
        var a0 = deg2rad(startDeg);
        var a1 = deg2rad(endDeg);
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

    /* Top half-disc: arc goes from (CX-R, CY) CW through 270 (top) to
     * (CX+R, CY), then Z closes back along the diameter.
     * Bottom half-disc: arc goes CCW through 90 (bottom) instead. */
    function halfPath(r, isTop) {
        var sweep = isTop ? 1 : 0;
        return ['M', (CX - r).toFixed(3), CY,
                'A', r, r, 0, 0, sweep, (CX + r).toFixed(3), CY,
                'Z'].join(' ');
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
    /* Click handlers                                                     */
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

    function onHalfClick(e) {
        var action = this.getAttribute('data-action');
        var icon   = this.closest('.parkinsound-stepgate-pedal');

        if (action === 'enable') {
            var enabled = !this.classList.contains('on');
            if (funcs && typeof funcs.set_port_value === 'function') {
                funcs.set_port_value('enabled', enabled ? 1 : 0);
            }
            applyEnabled(icon, enabled);
        } else if (action === 'sync') {
            /* sync_source: 0 = Host Sync, 1 = Free Run */
            var freeRun = !this.classList.contains('free');
            if (funcs && typeof funcs.set_port_value === 'function') {
                funcs.set_port_value('sync_source', freeRun ? 1 : 0);
            }
            applySync(icon, freeRun);
        }

        e.stopPropagation();
        e.preventDefault();
    }

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
    /* Build the SVG                                                      */
    /* ------------------------------------------------------------------ */

    function attachStepHandlers(node) {
        node.addEventListener('mousedown',  stopMouseDown);
        node.addEventListener('touchstart', stopMouseDown);
        node.addEventListener('click',      onStepOrTieClick);
    }

    function attachHalfHandlers(node) {
        node.addEventListener('mousedown',  stopMouseDown);
        node.addEventListener('touchstart', stopMouseDown);
        node.addEventListener('click',      onHalfClick);
    }

    function buildRings(rootEl) {
        var svg = rootEl.find('.parkinsound-stepgate-svg')[0];
        if (!svg || svg.getAttribute('data-built') === 'true') return;
        svg.setAttribute('data-built', 'true');

        var tieGroup  = svg.querySelector('.tie-ring');
        var stepGroup = svg.querySelector('.step-ring');

        /* --- 16 step + 16 tie sectors --- */
        for (var i = 0; i < 16; i++) {
            var n         = i + 1;
            var tieStart  = START_DEG + i * STEP_DEG;
            var tieEnd    = tieStart + STEP_DEG;
            var stepStart = tieStart + GAP_DEG / 2;
            var stepEnd   = tieEnd   - GAP_DEG / 2;

            var tie = document.createElementNS(NS, 'path');
            tie.setAttribute('d',     ringPath(TIE_OUTER, TIE_INNER, tieStart, tieEnd));
            tie.setAttribute('class', 'tie off');
            tie.setAttribute('data-step',   n);
            tie.setAttribute('data-symbol', 'step_' + n + '_tie');
            tieGroup.appendChild(tie);
            attachStepHandlers(tie);

            var step = document.createElementNS(NS, 'path');
            step.setAttribute('d',     ringPath(STEP_OUTER, STEP_INNER, stepStart, stepEnd));
            step.setAttribute('class', 'step off');
            step.setAttribute('data-step',   n);
            step.setAttribute('data-symbol', 'step_' + n + '_on');
            stepGroup.appendChild(step);
            attachStepHandlers(step);
        }

        /* --- 2 centre halves --- */
        var halvesGroup = svg.querySelector('.centre-halves');
        var labelsGroup = svg.querySelector('.centre-labels');

        var topHalf = document.createElementNS(NS, 'path');
        topHalf.setAttribute('d', halfPath(HALF_R, true));
        topHalf.setAttribute('class', 'centre-half top');
        topHalf.setAttribute('data-action', 'enable');
        halvesGroup.appendChild(topHalf);
        attachHalfHandlers(topHalf);

        var bottomHalf = document.createElementNS(NS, 'path');
        bottomHalf.setAttribute('d', halfPath(HALF_R, false));
        bottomHalf.setAttribute('class', 'centre-half bottom');
        bottomHalf.setAttribute('data-action', 'sync');
        halvesGroup.appendChild(bottomHalf);
        attachHalfHandlers(bottomHalf);

        /* Labels: TOP shows ON/OFF, BOTTOM shows HOST/FREE.
         * y offsets are roughly half the radius so they sit nicely
         * inside each half-disc. */
        var topLabel    = makeText(CX, CY - HALF_R * 0.45, 'OFF',
                                   'centre-label label-top');
        var bottomLabel = makeText(CX, CY + HALF_R * 0.45, 'HOST',
                                   'centre-label label-bottom');
        labelsGroup.appendChild(topLabel);
        labelsGroup.appendChild(bottomLabel);
    }

    /* ------------------------------------------------------------------ */
    /* modgui lifecycle                                                   */
    /* ------------------------------------------------------------------ */

    if (event.type === 'start') {
        var icon    = $(event.icon);
        var iconEl  = icon[0];
        buildRings(icon);

        if (event.value) {
            for (var sym in event.value) {
                if (sym === 'current_step') {
                    highlightCurrentStep(iconEl, parseInt(event.value[sym], 10));
                } else if (sym === 'enabled') {
                    applyEnabled(iconEl, parseFloat(event.value[sym]) > 0.5);
                } else if (sym === 'sync_source') {
                    applySync(iconEl, parseFloat(event.value[sym]) > 0.5);
                } else if (sym.indexOf('step_') === 0) {
                    applyStepValue(iconEl, sym, event.value[sym]);
                }
            }
        }
        return;
    }

    if (event.type === 'change') {
        var iconEl2 = $(event.icon)[0];
        if (event.symbol === 'current_step') {
            highlightCurrentStep(iconEl2, parseInt(event.value, 10));
        } else if (event.symbol === 'enabled') {
            applyEnabled(iconEl2, parseFloat(event.value) > 0.5);
        } else if (event.symbol === 'sync_source') {
            applySync(iconEl2, parseFloat(event.value) > 0.5);
        } else if (event.symbol && event.symbol.indexOf('step_') === 0) {
            applyStepValue(iconEl2, event.symbol, event.value);
        }
        return;
    }
}
