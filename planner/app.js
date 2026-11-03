/* RetroKM planner: the page.  All the thinking is in logic.js; this file
 * draws the editors and the results and keeps the plan in localStorage. */
(function () {
  'use strict';
  const P = window.RetroKMPlanner;
  const EXAMPLES = window.RetroKMExamples || {};
  const S = P.SIGNALS;
  const STORE = 'retrokm-planner-plan';
  const $ = id => document.getElementById(id);

  let plan = load() || P.normalizePlan({});
  let result = null;
  let selected = null;          // machine id whose ways are shown under the table
  let timer = 0;

  // ---- small helpers -----------------------------------------------------------

  /* h('div', {class: 'x', onclick: f}, child, 'text', ...) */
  function h(tag, attrs, ...kids) {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === null || v === undefined || v === false) continue;
      if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else if (k === 'class') el.className = v;
      else if (k in el && typeof v !== 'string') el[k] = v;
      else el.setAttribute(k, v === true ? '' : v);
    }
    for (const kid of kids.flat()) if (kid !== null && kid !== undefined && kid !== false)
      el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
    return el;
  }
  const svgNS = 'http://www.w3.org/2000/svg';
  function sv(tag, attrs, ...kids) {
    const el = document.createElementNS(svgNS, tag);
    for (const [k, v] of Object.entries(attrs || {})) if (v !== null && v !== undefined) el.setAttribute(k, v);
    for (const kid of kids.flat()) if (kid !== null && kid !== undefined)
      el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
    return el;
  }
  const names = list => list.length <= 1 ? list.join('') : list.slice(0, -1).join(', ') + ' and ' + list[list.length - 1];

  function say(text) {
    $('status').textContent = text;
    clearTimeout(say.t);
    say.t = setTimeout(() => { $('status').textContent = ''; }, 4000);
  }

  function load() {
    try {
      const raw = localStorage.getItem(STORE);
      return raw ? P.normalizePlan(JSON.parse(raw)) : null;
    } catch (e) { return null; }
  }
  function save() {
    try { localStorage.setItem(STORE, JSON.stringify(plan)); } catch (e) { /* private window: fine */ }
  }

  function download(name, text, type) {
    const url = URL.createObjectURL(new Blob([text], { type }));
    const a = h('a', { href: url, download: name });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  const byId = (list, id) => list.find(x => x.id === id);

  /* Something changed.  Editors are redrawn only when their shape changed,
   * so typing in a name field keeps its focus. */
  function changed(redraw) {
    if (redraw) {                // tidy (drop wants of removed monitors ...), then new editors
      plan = P.normalizePlan(plan);
      drawEditors();
    }
    save();
    clearTimeout(timer);
    timer = setTimeout(analyse, 150);
  }

  // ---- editors -------------------------------------------------------------------

  function signalChips(selectedSigs, onToggle, allow) {
    return h('div', { class: 'chips' }, (allow || P.SIGNAL_KEYS).map(s =>
      h('button', { type: 'button', class: 'chip', 'aria-pressed': String(selectedSigs.includes(s)), onclick: () => onToggle(s) },
        h('span', { class: 'dot', style: `background:${S[s].color}` }), S[s].label)));
  }

  function nameInput(obj, label) {
    return h('input', {
      type: 'text', class: 'name', value: obj.name, 'aria-label': label, maxlength: '40',
      oninput: e => { obj.name = e.target.value; changed(false); },
      onchange: () => changed(true),
    });
  }

  function drawMachines() {
    const box = $('machines');
    box.replaceChildren();
    if (!plan.machines.length) box.append(h('p', { class: 'empty' }, 'No machines yet.'));
    for (const m of plan.machines) {
      const wants = m.wants === null ? plan.monitors.map(t => t.id) : m.wants;
      box.append(h('div', { class: 'item' },
        h('div', { class: 'row head' },
          nameInput(m, 'Machine name'),
          h('select', { 'aria-label': 'Kind', onchange: e => { m.kind = e.target.value; changed(true); } },
            h('option', { value: 'computer', selected: m.kind === 'computer' }, 'Computer'),
            h('option', { value: 'console', selected: m.kind === 'console' }, 'Console')),
          h('button', { type: 'button', class: 'x', 'aria-label': 'Remove ' + m.name, title: 'Remove',
            onclick: () => { plan.machines = plan.machines.filter(x => x !== m); changed(true); } }, '×')),
        h('div', { class: 'label' }, 'Video it sends'),
        signalChips(m.outputs, s => {
          m.outputs = m.outputs.includes(s) ? m.outputs.filter(x => x !== s) : m.outputs.concat(s);
          changed(true);
        }),
        h('div', { class: 'row' },
          h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: m.agent,
            onchange: e => { m.agent = e.target.checked; changed(true); } }), 'Runs a RetroKM agent (keyboard and mouse)'),
          h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: m.hub,
            onchange: e => { m.hub = e.target.checked; changed(true); } }), 'The hub runs on it')),
        h('div', { class: 'label' }, 'Should show on'),
        plan.monitors.length
          ? h('div', { class: 'chips' }, plan.monitors.map(t => h('button', {
              type: 'button', class: 'chip', 'aria-pressed': String(wants.includes(t.id)),
              onclick: () => { m.wants = wants.includes(t.id) ? wants.filter(x => x !== t.id) : wants.concat(t.id); changed(true); },
            }, t.name)))
          : h('p', { class: 'empty' }, 'Add monitors in step 2.'),
        plan.machines.length > 1 && [
          h('div', { class: 'label' }, 'On screen at the same time as'),
          h('div', { class: 'chips' }, plan.machines.filter(o => o !== m).map(o => h('button', {
            type: 'button', class: 'chip', 'aria-pressed': String(m.alongside.includes(o.id)),
            title: 'Both on screens at once, side by side',
            onclick: () => {
              const on = !m.alongside.includes(o.id);
              m.alongside = on ? m.alongside.concat(o.id) : m.alongside.filter(x => x !== o.id);
              o.alongside = on ? o.alongside.concat(m.id) : o.alongside.filter(x => x !== m.id);
              changed(true);
            },
          }, o.name))),
        ]));
    }
  }

  function drawMonitors() {
    const box = $('monitors');
    box.replaceChildren();
    if (!plan.monitors.length) box.append(h('p', { class: 'empty' }, 'No monitors yet.'));
    for (const t of plan.monitors) {
      const addInput = h('select', { 'aria-label': 'Add an input', onchange: e => {
        if (e.target.value) { t.inputs.push(e.target.value); changed(true); }
      } }, h('option', { value: '' }, '+ input'),
        P.SIGNAL_KEYS.filter(s => s !== 'sog').map(s => h('option', { value: s }, S[s].label)));
      box.append(h('div', { class: 'item' },
        h('div', { class: 'row head' },
          nameInput(t, 'Monitor name'),
          h('select', { 'aria-label': 'Kind', onchange: e => { t.kind = e.target.value; changed(true); } },
            [['monitor', 'Monitor'], ['tv', 'TV'], ['capture', 'Capture card']].map(([v, l]) =>
              h('option', { value: v, selected: t.kind === v }, l))),
          h('button', { type: 'button', class: 'x', 'aria-label': 'Remove ' + t.name, title: 'Remove',
            onclick: () => { plan.monitors = plan.monitors.filter(x => x !== t); changed(true); } }, '×')),
        h('div', { class: 'label' }, 'Inputs'),
        h('div', { class: 'chips' },
          t.inputs.map((s, i) => h('button', { type: 'button', class: 'chip in', title: 'Remove this input',
            'aria-label': 'Remove input ' + S[s].label,
            onclick: () => { t.inputs.splice(i, 1); changed(true); } },
            h('span', { class: 'dot', style: `background:${S[s].color}` }), S[s].label, ' ×')),
          addInput),
        h('div', { class: 'row' },
          t.inputs.includes('vga') && h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: t.sog,
            onchange: e => { t.sog = e.target.checked; changed(true); } }), 'Its VGA input takes sync-on-green'),
          t.kind !== 'capture' && h('label', { class: 'check' }, 'Shows at once ',
            h('select', { onchange: e => { t.atOnce = +e.target.value; changed(true); } },
              [1, 2, 3, 4].map(n => h('option', { value: n, selected: t.atOnce === n }, n)))))));
    }
  }

  function jackText(hw) {
    return hw.ins.map(j => `${j.name} (${j.accepts.map(s => S[s].label).join(', ')})`).join(', ') +
      ' → ' + hw.outs.map(o => S[o.gives].label).join(', ');
  }

  function drawHardware() {
    const box = $('hardware');
    box.replaceChildren();
    if (!plan.hardware.length) box.append(h('p', { class: 'empty' }, 'Nothing yet: the planner will suggest what to get.'));
    for (const hw of plan.hardware) {
      const kindTag = hw.kind === 'switch' ? `Switch ${hw.ins}×${hw.outs}` : 'Converter';
      const custom = !hw.catalog && !hw.deviceFile;
      box.append(h('div', { class: 'item' + (hw.planned ? ' planned' : '') },
        h('div', { class: 'row head' },
          nameInput(hw, 'Hardware name'),
          h('span', { class: 'tag' }, kindTag),
          hw.planned && h('span', { class: 'tag plan' }, 'to buy'),
          h('button', { type: 'button', class: 'x', 'aria-label': 'Remove ' + hw.name, title: 'Remove',
            onclick: () => { plan.hardware = plan.hardware.filter(x => x !== hw); changed(true); } }, '×')),
        hw.kind === 'switch' && h('div', { class: 'row' },
          h('select', { 'aria-label': 'How it is switched', onchange: e => { hw.driver = e.target.value; changed(true); } },
            [['manual', 'Switched by hand or remote'], ['extron-sis', 'The hub drives it: Extron SIS'],
             ['text', 'The hub drives it: text commands']].map(([v, l]) => h('option', { value: v, selected: hw.driver === v }, l))),
          h('label', { class: 'check' }, 'In ', h('input', { type: 'number', min: 1, max: 64, value: hw.ins,
            onchange: e => { hw.ins = +e.target.value; changed(true); } })),
          h('label', { class: 'check' }, 'Out ', h('input', { type: 'number', min: 1, max: 64, value: hw.outs,
            onchange: e => { hw.outs = +e.target.value; changed(true); } }))),
        hw.kind === 'switch' && h('div', { class: 'label' }, 'Signals it carries'),
        hw.kind === 'switch' && signalChips(hw.accepts, s => {
          const next = hw.accepts.includes(s) ? hw.accepts.filter(x => x !== s) : hw.accepts.concat(s);
          if (next.length) { hw.accepts = next; changed(true); }
        }),
        hw.kind === 'converter' && !custom && h('div', { class: 'label' }, 'Jacks: ' + jackText(hw)),
        hw.kind === 'converter' && custom && [
          h('div', { class: 'label' }, 'Takes'),
          signalChips(hw.ins.flatMap(j => j.accepts), s => {
            const has = hw.ins.some(j => j.accepts.includes(s));
            const next = has ? hw.ins.filter(j => !j.accepts.includes(s)) : hw.ins.concat({ name: s, accepts: [s] });
            if (next.length) { hw.ins = next; changed(true); }
          }),
          h('div', { class: 'row' }, h('label', { class: 'check' }, 'Gives ',
            h('select', { onchange: e => { hw.outs = [{ name: e.target.value, gives: e.target.value }]; changed(true); } },
              P.SIGNAL_KEYS.filter(s => s !== 'sog').map(s => h('option', { value: s, selected: hw.outs[0].gives === s }, S[s].label))))),
        ],
        hw.notes && h('div', { class: 'label' }, hw.notes),
        hw.planned && h('div', { class: 'row' }, h('button', { type: 'button',
          onclick: () => { hw.planned = false; changed(true); } }, 'I have it now'))));
    }
  }

  function drawEditors() {
    drawMachines();
    drawMonitors();
    drawHardware();
  }

  function fillPickers() {
    $('machine-preset').replaceChildren(h('option', { value: '' }, 'Blank machine'),
      ...P.MACHINE_PRESETS.map((p, i) => h('option', { value: i }, p.label + ' — ' + p.outputs.map(s => S[s].label).join(', '))));
    $('monitor-preset').replaceChildren(h('option', { value: '' }, 'Blank monitor'),
      ...P.MONITOR_PRESETS.map((p, i) => h('option', { value: i }, p.label + ' — ' + p.inputs.map(s => S[s].label).join(', '))));
    $('hardware-pick').replaceChildren(
      h('optgroup', { label: 'Known boxes' }, P.CATALOGUE.map(c => h('option', { value: 'cat:' + c.id }, c.name))),
      h('optgroup', { label: 'Describe your own' },
        h('option', { value: 'custom:switch' }, 'A switch or matrix'),
        h('option', { value: 'custom:converter' }, 'A scaler or converter')));
    for (const [k, ex] of Object.entries(EXAMPLES))
      $('example').append(h('option', { value: k }, ex.name.replace(/^Example: /, '')));
  }

  // ---- results ---------------------------------------------------------------------

  function analyse() {
    result = P.analyse(plan);
    drawResults();
  }

  function shown() {
    const sug = $('with-suggestions').checked;
    return sug ? { plan: result.withSuggestions, routing: result.routing } : { plan: result.plan, routing: result.now };
  }

  function drawResults() {
    const r = result;
    const sum = $('summary');
    sum.replaceChildren();
    if (!plan.machines.length || !plan.monitors.length) {
      sum.append(h('p', { class: 'summary' }, 'Add some machines and monitors (or load an example) to see a plan.'));
    } else if (!r.pairs) {
      sum.append(h('p', { class: 'summary' }, 'Pick the monitors each machine should show on (step 1).'));
    } else {
      const now = r.now.okCount, then = r.routing.okCount, all = r.pairs;
      sum.append(
        h('div', { class: 'meter', role: 'img', 'aria-label': `${now} of ${all} work now, ${then} with the suggestions` },
          h('span', { class: 'now', style: `width:${100 * now / all}%` }),
          h('span', { class: 'then', style: `width:${100 * (then - now) / all}%` })),
        h('p', { class: 'summary' },
          now === all ? `All ${all} connections work with what you have.`
            : `With what you have, ${now} of ${all} connections work.` +
              (r.suggestions.length ? ` With the suggestions below, ${then} of ${all}.` : '')));
    }

    const sugBox = $('suggestions');
    sugBox.replaceChildren();
    if (r.suggestions.length) sugBox.append(h('h3', {}, 'What to add'));
    for (const st of r.suggestions) {
      const c = st.item;
      sugBox.append(h('div', { class: 'sug' },
        h('h3', {}, c.category, ' ', c.driver !== 'manual' ? h('span', { class: 'tag' }, 'the hub can drive it') : null),
        h('p', {}, st.reason),
        h('p', { class: 'eg' }, 'For example: ' + names(c.examples) + '. Fixes ' + st.gain +
          (st.gain === 1 ? ' connection.' : ' connections.')),
        c.notes && h('p', { class: 'eg' }, c.notes),
        st.alternatives.length ? h('p', { class: 'eg' }, 'Also does the job: ' +
          names([...new Set(st.alternatives.map(a => a.category))]) + '.') : null,
        h('button', { type: 'button', onclick: () => {
          plan.hardware.push(P.hardwareFromCatalogue(c.id, { planned: true, name: c.name }));
          changed(true);
          say(c.name + ' added to your hardware, marked "to buy".');
        } }, 'Add to my hardware')));
    }

    const miss = $('missing');
    miss.replaceChildren();
    if (r.missing.length) {
      miss.append(h('h3', {}, 'Still not possible'));
      for (const m of r.missing) miss.append(h('div', { class: 'miss' }, h('p', {}, m.reason)));
    }
    const side = $('side');
    side.replaceChildren();
    if (r.routing.together.length) {
      const M = id => byId(r.withSuggestions.machines, id).name;
      const T = id => byId(r.withSuggestions.monitors, id).name;
      side.append(h('h3', {}, 'Side by side'));
      for (const x of r.routing.together) {
        const before = r.now.together.find(y => y.a === x.a && y.b === x.b);
        const pair = `${M(x.a)} and ${M(x.b)}`;
        if (before && before.ok)
          side.append(h('p', { class: 'note' }, h('span', { class: 'ok' }, '✓ '), `${pair}: ${M(x.a)} on ${T(before.ta)}, ${M(x.b)} on ${T(before.tb)}.`));
        else if (x.ok)
          side.append(h('p', { class: 'note' }, h('span', { class: 'plan' }, '★ '), `${pair}, with the suggestions: ${M(x.a)} on ${T(x.ta)}, ${M(x.b)} on ${T(x.tb)}.`));
        else {
          const why = r.apart.find(y => y.a === x.a && y.b === x.b);
          side.append(h('div', { class: 'miss' }, h('p', {}, why ? why.reason : pair + ' cannot show at the same time yet.')));
        }
      }
    }
    const notes = $('notes');
    notes.replaceChildren();
    if (r.notes.length) {
      notes.append(h('h3', {}, 'Good to know'));
      r.notes.forEach(n => notes.append(h('p', { class: 'note' }, n)));
    }

    const v = shown();
    drawPicture(v.plan, v.routing.cables);
    drawReach();
    $('wiring').textContent = v.routing.cables.length ? P.wiringList(v.plan, v.routing.cables).join('\n') : 'No cables yet.';
    $('config').textContent = P.makeConfig(v.plan, v.routing.cables);
  }

  function drawReach() {
    const r = result, box = $('reach');
    box.replaceChildren();
    if (!plan.machines.length || !plan.monitors.length) { $('route-detail').replaceChildren(); return; }
    const cell = (m, t) => {
      const wants = m.wants === null || m.wants.includes(t.id);
      if (!wants) return h('td', { class: 'no', title: 'not wanted' }, '·');
      const now = r.now.reach[m.id] && r.now.reach[m.id][t.id];
      const then = r.routing.reach[m.id] && r.routing.reach[m.id][t.id];
      if (now && now.ok) return h('td', { class: 'ok', title: 'works now' }, '✓');
      if (then && then.ok) return h('td', { class: 'plan', title: 'with the suggestions' }, '★');
      return h('td', { class: 'bad', title: 'not possible yet' }, '✗');
    };
    box.append(h('table', { class: 'reach' },
      h('thead', {}, h('tr', {}, h('th', {}, ''), plan.monitors.map(t => h('th', { scope: 'col' }, t.name)))),
      h('tbody', {}, plan.machines.map(m => h('tr', {
        class: selected === m.id ? 'sel' : null, tabindex: '0',
        onclick: () => { selected = m.id; drawReach(); },
        onkeydown: e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); selected = m.id; drawReach(); } },
      }, h('th', { scope: 'row' }, m.name), plan.monitors.map(t => cell(m, t)))))),
    );
    box.append(h('p', { class: 'note' }, '✓ works now   ★ with the suggestions   ✗ not yet   · not wanted'));

    const det = $('route-detail');
    det.replaceChildren();
    const m = selected && byId(r.withSuggestions.machines, selected);
    if (!m) return;
    for (const t of r.withSuggestions.monitors) {
      const way = r.routing.reach[m.id] && r.routing.reach[m.id][t.id];
      if (!way) continue;
      const miss = r.missing.find(x => x.m === m.id && x.t === t.id);
      det.append(h('p', {}, h('strong', {}, t.name + ': '),
        way.ok ? P.describeHops(r.withSuggestions, way.hops) : (miss ? miss.reason : 'no way yet')));
    }
  }

  // ---- the picture -------------------------------------------------------------------

  function drawPicture(pl, cables) {
    const box = $('picture'), legend = $('legend');
    box.replaceChildren();
    legend.replaceChildren();
    if (!pl.machines.length && !pl.monitors.length) {
      box.append(h('p', { class: 'empty' }, 'The picture appears when the plan has machines and monitors.'));
      return;
    }
    const W = 172, GAP = 96, VGAP = 14, PAD = 12;
    const node = new Map();       // key -> {key, col, label, sub, planned, ins:[], outs:[]}
    const key = (t, id) => t + ':' + id;
    pl.machines.forEach(m => node.set(key('m', m.id), { key: key('m', m.id), col: 0, label: m.name,
      sub: (m.kind === 'console' ? 'Console' : 'Computer') + (m.agent || m.hub ? ' · agent' : ''), order: 0 }));
    pl.hardware.forEach(hw => node.set(key('d', hw.id), { key: key('d', hw.id), col: 1, label: hw.name,
      sub: (hw.kind === 'switch' ? `Switch ${hw.ins}×${hw.outs}` : 'Converter') + (hw.driver !== 'manual' ? ' · hub' : ''),
      planned: hw.planned }));
    // columns: a box sits one to the right of whatever feeds it
    for (let pass = 0; pass < 8; pass++)
      for (const c of cables)
        if (c.to.t === 'd') {
          const a = node.get(key(c.from.t, c.from.id)), b = node.get(key('d', c.to.id));
          if (a && b && b.col < a.col + 1 && a.col + 1 < 8) b.col = a.col + 1;
        }
    const last = Math.max(1, ...[...node.values()].map(n => n.col)) + 1;
    pl.monitors.forEach(t => node.set(key('t', t.id), { key: key('t', t.id), col: last, label: t.name,
      sub: { monitor: 'Monitor', tv: 'TV', capture: 'Capture card' }[t.kind] }));

    const ends = cables.map(c => ({ c, a: node.get(key(c.from.t, c.from.id)), b: node.get(key(c.to.t, c.to.id)) }))
      .filter(e => e.a && e.b);
    for (const e of ends) { (e.a.outs = e.a.outs || []).push(e); (e.b.ins = e.b.ins || []).push(e); }

    // rows: each column in the order of what feeds it, so fewer lines cross
    const cols = [];
    for (const n of node.values()) (cols[n.col] = cols[n.col] || []).push(n);
    let y0 = 0;
    const heightOf = n => 46 + 10 * Math.max(0, Math.max((n.ins || []).length, (n.outs || []).length) - 3);
    cols.forEach((col, ci) => {
      if (!col) return;
      if (ci > 0) {
        col.forEach((n, i) => {
          const from = (n.ins || []).map(e => e.a.y).filter(y => y !== undefined);
          n.bary = from.length ? from.reduce((s, y) => s + y, 0) / from.length : 1e6 + i;
        });
        col.sort((a, b) => a.bary - b.bary);
      }
      let y = PAD;
      for (const n of col) { n.y = y; n.h = heightOf(n); n.x = PAD + ci * (W + GAP); y += n.h + VGAP; }
      y0 = Math.max(y0, y);
    });
    const width = PAD * 2 + (cols.length - 1) * (W + GAP) + W, height = y0 + PAD - VGAP;

    const svg = sv('svg', { viewBox: `0 0 ${width} ${height}`, role: 'img',
      'aria-label': 'Machines, hardware and monitors with the cables between them' });
    svg.append(sv('style', {}, `
      .n rect { fill: var(--node); stroke: var(--line); stroke-width: 1.5; }
      .n.planned rect { stroke: var(--plan); stroke-dasharray: 5 4; }
      .n .t { fill: var(--ink); font: 600 13px system-ui, sans-serif; }
      .n .s { fill: var(--muted); font: 11px system-ui, sans-serif; }
      .p { fill: var(--muted); font: 10px ui-monospace, Menlo, monospace; }
      .w { fill: none; stroke-width: 2.5; }`));

    // cables: spread along each side in the order of the far end
    for (const n of node.values()) {
      (n.outs || []).sort((p, q) => (p.b.y || 0) - (q.b.y || 0));
      (n.ins || []).sort((p, q) => (p.a.y || 0) - (q.a.y || 0));
    }
    const at = (n, list, e) => n.y + (n.h * (list.indexOf(e) + 1)) / (list.length + 1);
    const H = new Map(pl.hardware.map(x => [x.id, x]));
    for (const e of ends) {
      const x1 = e.a.x + W, y1 = at(e.a, e.a.outs, e), x2 = e.b.x, y2 = at(e.b, e.b.ins, e);
      const dx = Math.max(30, (x2 - x1) / 2);
      const planned = e.a.planned || e.b.planned;
      svg.append(sv('path', { class: 'w', d: `M${x1},${y1} C${x1 + dx},${y1} ${x2 - dx},${y2} ${x2},${y2}`,
        stroke: S[e.c.sig].color, 'stroke-dasharray': planned ? '6 5' : null },
        sv('title', {}, S[e.c.sig].label)));
      if (e.c.from.t === 'd') {
        const hw = H.get(e.c.from.id);
        svg.append(sv('text', { class: 'p', x: x1 + 4, y: y1 - 4 }, P.portsOf(hw).outs[e.c.from.port].name));
      }
      if (e.c.to.t === 'd') {
        const hw = H.get(e.c.to.id);
        svg.append(sv('text', { class: 'p', x: x2 - 4, y: y2 - 4, 'text-anchor': 'end' }, P.portsOf(hw).ins[e.c.to.port].name));
      }
    }
    const clip = (s, n) => s.length > n ? s.slice(0, n - 1) + '…' : s;
    for (const n of node.values()) {
      svg.append(sv('g', { class: 'n' + (n.planned ? ' planned' : '') },
        sv('title', {}, n.label),
        sv('rect', { x: n.x, y: n.y, width: W, height: n.h, rx: 8 }),
        sv('text', { class: 't', x: n.x + 10, y: n.y + 20 }, clip(n.label, 22)),
        sv('text', { class: 's', x: n.x + 10, y: n.y + 36 }, clip((n.planned ? 'To buy · ' : '') + n.sub, 28))));
    }
    if (width < 560) svg.style.maxWidth = width + 'px';
    svg.style.minWidth = Math.min(width, 560) + 'px';
    box.append(svg);

    const used = [...new Set(cables.map(c => c.sig))];
    used.forEach(s => legend.append(h('span', {}, h('i', { style: `background:${S[s].color}` }), S[s].label)));
  }

  // ---- buttons -----------------------------------------------------------------------

  function setPlan(p, message) {
    plan = P.normalizePlan(p);
    selected = null;
    changed(true);
    if (message) say(message);
  }

  /* "CRT", then "CRT 2" ... */
  function freshName(name, list) {
    const taken = new Set(list.map(x => x.name.toLowerCase()));
    let v = name, n = 2;
    while (taken.has(v.toLowerCase())) v = name + ' ' + n++;
    return v;
  }
  function preset(p) {
    if (!p) return {};
    const c = JSON.parse(JSON.stringify(p));
    delete c.label;
    return c;
  }
  $('add-machine').addEventListener('click', () => {
    const m = Object.assign({ id: P.newId('m'), name: 'Machine', kind: 'computer', outputs: [], agent: true, wants: null },
      preset(P.MACHINE_PRESETS[$('machine-preset').value]));
    m.name = freshName(m.name, plan.machines);
    plan.machines.push(m);
    changed(true);
  });
  $('add-monitor').addEventListener('click', () => {
    const t = Object.assign({ id: P.newId('t'), name: 'Monitor', kind: 'monitor', inputs: [], sog: false, atOnce: 1 },
      preset(P.MONITOR_PRESETS[$('monitor-preset').value]));
    t.name = freshName(t.name, plan.monitors);
    plan.monitors.push(t);
    changed(true);
  });
  $('add-hardware').addEventListener('click', () => {
    const v = $('hardware-pick').value;
    let hw = null;
    if (v.startsWith('cat:')) hw = P.hardwareFromCatalogue(v.slice(4));
    else if (v === 'custom:switch')
      hw = P.normalizeHardware({ name: 'My switch', kind: 'switch', ins: 4, outs: 1, accepts: ['vga'] });
    else if (v === 'custom:converter')
      hw = P.normalizeHardware({ name: 'My converter', kind: 'converter', ins: [{ name: 'composite', accepts: ['composite'] }],
        outs: [{ name: 'hdmi', gives: 'hdmi' }] });
    if (!hw) return;
    hw.id = hw.id || P.newId('h');
    plan.hardware.push(hw);
    changed(true);
  });
  $('device-file').addEventListener('change', e => {
    const f = e.target.files[0];
    if (!f) return;
    const rd = new FileReader();
    rd.onload = () => {
      const hw = P.parseDeviceFile(rd.result, f.name);
      if (!hw) { say(`${f.name}: no "signal =" or ports the planner understands.`); return; }
      plan.hardware.push(hw);
      changed(true);
      say(`Read ${f.name}: ${hw.name}.`);
    };
    rd.readAsText(f);
    e.target.value = '';
  });
  $('example').addEventListener('change', e => {
    const ex = EXAMPLES[e.target.value];
    e.target.value = '';
    if (!ex) return;
    if ((plan.machines.length || plan.monitors.length) && !confirm('Replace your plan with the example? Save yours first if you want to keep it.')) return;
    setPlan(ex, 'Loaded the example.');
  });
  $('new').addEventListener('click', () => {
    if (confirm('Start over with an empty plan?')) setPlan({}, 'Empty plan.');
  });
  $('save').addEventListener('click', () => {
    const name = (plan.name || 'desk').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'desk';
    download(`retrokm-plan-${name}.json`.slice(0, 60).replace(/-?\.?json$/, '') + '.json', JSON.stringify(plan, null, 2) + '\n', 'application/json');
  });
  $('load').addEventListener('change', e => {
    const f = e.target.files[0];
    if (!f) return;
    const rd = new FileReader();
    rd.onload = () => {
      try { setPlan(JSON.parse(rd.result), `Opened ${f.name}.`); }
      catch (err) { say(`${f.name} is not a plan file (${err.message}).`); }
    };
    rd.readAsText(f);
    e.target.value = '';
  });
  $('with-suggestions').addEventListener('change', () => result && drawResults());
  $('copy').addEventListener('click', () => {
    const text = $('config').textContent;
    const fallback = () => {
      const ta = h('textarea', { style: 'position:fixed;opacity:0' });
      ta.value = text;
      document.body.append(ta);
      ta.select();
      let ok = false;
      try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
      ta.remove();
      say(ok ? 'Copied.' : 'Could not copy: select the text and copy it by hand.');
    };
    if (navigator.clipboard && navigator.clipboard.writeText)
      navigator.clipboard.writeText(text).then(() => say('Copied.'), fallback);
    else fallback();
  });
  $('download').addEventListener('click', () => download('retrokm.conf', $('config').textContent, 'text/plain'));

  fillPickers();
  drawEditors();
  analyse();
})();
