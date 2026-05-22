/*
 * Parkinsound Step Gate - modgui controller.
 *
 * Layout (250x250 viewBox, centre 125,125):
 *   - Tie ring    : annulus r in [85, 95]   (16 segments, no gap)
 *   - Step ring   : annulus r in [50, 80]   (16 segments, 2deg gap)
 *   - Quarter pies: r in [0, 45]            (4 buttons in centre)
 *
 * Centre quarter buttons:
 *   TL (180-270)  : toggles the lv2:enabled port
 *   TR (270-360)  : toggles sync_source (Host Sync <-> Free Run)
 *   BL (90-180)   : preset prev   (TODO: no documented modgui API yet)
 *   BR (0-90)     : preset next   (TODO)
 *
 * Each path attaches its own click / mousedown / touchstart so we
 * never depend on bubbling through SVG containers that may have
 * pointer-events:none.
 */
function (event, funcs) {
    var NS         = 'http://www.w3.org/2000/svg';
    var CX         = 125, CY = 125;
    var STEP_OUTER = 80,  STEP_INNER = 50;
    var TIE_OUTER  = 95,  TIE_INNER  = 85;
    var QTR_RADIUS = 45;
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

    function piePath(r, startDeg, endDeg) {
        var a0 = deg2rad(startDeg);
        var a1 = deg2rad(endDeg);
        var x0 = (CX + r * Math.cos(a0)).toFixed(3);
        var y0 = (CY + r * Math.sin(a0)).toFixed(3);
        var x1 = (CX + r * Math.cos(a1)).toFixed(3);
        var y1 = (CY + r * Math.sin(a1)).toFixed(3);
        var large = (endDeg - startDeg) > 180 ? 1 : 0;
        return ['M', CX, CY,
                'L', x0, y0,
                'A', r, r, 0, large, 1, x1, y1,
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

    function onQuarterClick(e) {
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
        } else if (action === 'preset-prev' || action === 'preset-next') {
            /* TODO: mod-ui's jsFuncs (set_port_value, patch_set) does
             * not expose a documented way to navigate presets from a
             * custom modgui. Leaving the visual + click in place; the
             * wiring will land once we identify the right hook. */
        }

        e.stopPropagation();
        e.preventDefault();
    }

    function applyEnabled(iconEl, on) {
        var q = iconEl.querySelector('.quarter.q-tl');
        var dot = iconEl.querySelector('.label-tl');
        if (q)   q.classList.toggle('on', on);
        if (dot) dot.classList.toggle('on', on);
    }

    function applySync(iconEl, freeRun) {
        var q = iconEl.querySelector('.quarter.q-tr');
        var lbl = iconEl.querySelector('.label-tr');
        if (q) q.classList.toggle('free', freeRun);
        if (lbl) lbl.textContent = freeRun ? 'F' : 'H';
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

    function attachQuarterHandlers(node) {
        node.addEventListener('mousedown',  stopMouseDown);
        node.addEventListener('touchstart', stopMouseDown);
        node.addEventListener('click',      onQuarterClick);
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

        /* --- 4 centre quarter buttons --- */
        var qGroup   = svg.querySelector('.centre-quarters');
        var lblGroup = svg.querySelector('.centre-labels');

        var QUARTERS = [
            { cls: 'q-tl', action: 'enable',      start: 180, end: 270,
              labelCls: 'label-tl', labelTxt: '●' },               /* solid dot */
            { cls: 'q-tr', action: 'sync',        start: 270, end: 360,
              labelCls: 'label-tr', labelTxt: 'H' },
            { cls: 'q-bl', action: 'preset-prev', start: 90,  end: 180,
              labelCls: 'label-bl', labelTxt: '‹' },               /* single < */
            { cls: 'q-br', action: 'preset-next', start: 0,   end: 90,
              labelCls: 'label-br', labelTxt: '›' }                /* single > */
        ];

        for (var k = 0; k < QUARTERS.length; k++) {
            var q = QUARTERS[k];
            var p = document.createElementNS(NS, 'path');
            p.setAttribute('d', piePath(QTR_RADIUS, q.start, q.end));
            p.setAttribute('class', 'quarter ' + q.cls);
            p.setAttribute('data-action', q.action);
            qGroup.appendChild(p);
            attachQuarterHandlers(p);

            /* Label at the centroid of each quarter (mid-angle, r = 22) */
            var midDeg = (q.start + q.end) / 2;
            var mr     = QTR_RADIUS * 0.55;
            var lx     = CX + mr * Math.cos(deg2rad(midDeg));
            var ly     = CY + mr * Math.sin(deg2rad(midDeg));
            var lbl    = makeText(lx, ly, q.labelTxt, 'centre-label ' + q.labelCls);
            lblGroup.appendChild(lbl);
        }
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
