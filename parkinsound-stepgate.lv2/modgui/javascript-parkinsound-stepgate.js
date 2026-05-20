/*
 * Parkinsound Step Gate - modgui controller.
 *
 * The visible SVG ring is cosmetic: each step and tie path is bound
 * to the matching hidden .mod-toggle-image widget (declared in the
 * HTML template), and clicking a path simply $.triggers('click') on
 * its hook. mod-ui's built-in toggle handler then writes the new
 * value to mod-host. We listen for the 'change' callback to keep the
 * ring's visual state in sync with the host.
 *
 * Step 1 begins just past 12 o'clock and the ring advances clockwise.
 */
function (event) {
    var NS         = 'http://www.w3.org/2000/svg';
    var CX         = 100, CY = 100;
    var STEP_OUTER = 80,  STEP_INNER = 50;
    var TIE_OUTER  = 95,  TIE_INNER  = 85;
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

    function buildRings(rootEl) {
        var svg = rootEl.find('.parkinsound-stepgate-svg')[0];
        if (!svg || svg.getAttribute('data-built') === 'true') return;
        svg.setAttribute('data-built', 'true');

        var tieGroup  = svg.querySelector('.tie-ring');
        var stepGroup = svg.querySelector('.step-ring');

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

            var step = document.createElementNS(NS, 'path');
            step.setAttribute('d',     ringPath(STEP_OUTER, STEP_INNER, stepStart, stepEnd));
            step.setAttribute('class', 'step off');
            step.setAttribute('data-step',   n);
            step.setAttribute('data-symbol', 'step_' + n + '_on');
            stepGroup.appendChild(step);
        }
    }

    function applyValue(rootEl, symbol, value) {
        var node = rootEl[0].querySelector('[data-symbol="' + symbol + '"]');
        if (!node) return;
        var on = parseFloat(value) > 0.5;
        node.classList.toggle('on',  on);
        node.classList.toggle('off', !on);
    }

    function highlightCurrentStep(rootEl, stepNum) {
        var paths = rootEl[0].querySelectorAll('.step');
        for (var i = 0; i < paths.length; i++) {
            paths[i].classList.toggle(
                'playing',
                parseInt(paths[i].getAttribute('data-step'), 10) === stepNum
            );
        }
    }

    function wireClicks(rootEl) {
        var svg = rootEl.find('.parkinsound-stepgate-svg')[0];
        svg.addEventListener('click', function (e) {
            var node = e.target.closest('[data-symbol]');
            if (!node) return;
            var symbol = node.getAttribute('data-symbol');
            /* Forward the click to mod-ui's standard toggle widget.
             * That widget owns the value state and the host comms. */
            var hook = rootEl.find(
                '.mod-toggle-image[mod-role="input-control-port"]' +
                '[mod-port-symbol="' + symbol + '"]'
            );
            if (hook.length) {
                hook.trigger('click');
            }
            e.stopPropagation();
            e.preventDefault();
        });
    }

    if (event.type === 'start') {
        var icon = $(event.icon);
        buildRings(icon);
        wireClicks(icon);

        /* Apply the snapshot of port values mod-ui gave us at start. */
        if (event.value) {
            for (var sym in event.value) {
                if (sym === 'current_step') {
                    highlightCurrentStep(icon, parseInt(event.value[sym], 10));
                } else if (sym.indexOf('step_') === 0) {
                    applyValue(icon, sym, event.value[sym]);
                }
            }
        }
        return;
    }

    if (event.type === 'change') {
        var icon2 = $(event.icon);
        if (event.symbol === 'current_step') {
            highlightCurrentStep(icon2, parseInt(event.value, 10));
        } else {
            applyValue(icon2, event.symbol, event.value);
        }
        return;
    }
}
