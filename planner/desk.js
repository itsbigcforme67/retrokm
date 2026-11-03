/* RetroKM desk builder: the visual way in.  Machines are tiles, screens
 * are drop targets, and each mode is one layout that must work all at
 * once.  The plan is the same one the detailed planner (index.html) uses,
 * and so is the thinking (logic.js). */
(function () {
  'use strict';
  const P = window.RetroKMPlanner;
  const EXAMPLES = window.RetroKMExamples || {};
  const S = P.SIGNALS;
  const STORE = 'retrokm-planner-plan';           // shared with index.html
  const STORE_MODE = 'retrokm-desk-mode';         // which mode tab is open, this browser only
  const $ = id => document.getElementById(id);
  const HUES = ['#2563eb', '#db2777', '#16a34a', '#d97706', '#7c3aed', '#0d9488', '#dc2626', '#4f46e5', '#65a30d', '#c026d3'];

  let plan = load() || P.normalizePlan({});
  let result = null;
  let modeId = null;
  let picked = null;            // machine id chosen by a tap, waiting for a screen
  let editing = null;           // { kind: 'm' | 't', id }
  let timer = 0;
  try { modeId = localStorage.getItem(STORE_MODE); } catch (e) { modeId = null; }

  // ---- helpers ---------------------------------------------------------------------

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
  const names = list => list.length <= 1 ? list.join('') : list.slice(0, -1).join(', ') + ' and ' + list[list.length - 1];
  const byId = (list, id) => list.find(x => x.id === id);
  const hue = m => HUES[Math.max(0, plan.machines.findIndex(x => x.id === m.id)) % HUES.length];

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
    try { localStorage.setItem(STORE_MODE, modeId || ''); } catch (e) { /* fine */ }
  }
  const download = (name, text, type) => window.RetroKMUI.offer(name, text, type);
  function freshName(name, list) {
    const taken = new Set(list.map(x => x.name.toLowerCase()));
    let v = name, n = 2;
    while (taken.has(v.toLowerCase())) v = name + ' ' + n++;
    return v;
  }

  function mode() {
    if (!plan.modes.length) plan.modes.push({ id: P.newId('x'), name: 'Mode 1', shows: [] });
    let md = byId(plan.modes, modeId);
    if (!md) { md = plan.modes[0]; modeId = md.id; }
    return md;
  }

  /* Something changed: tidy the plan, redraw, think again shortly */
  function changed() {
    plan = P.normalizePlan(plan);
    mode();
    save();
    draw();
    clearTimeout(timer);
    timer = setTimeout(analyse, 120);
  }

  // ---- placing machines on screens --------------------------------------------------

  function place(mId, tId) {
    const md = mode();
    const t = byId(plan.monitors, tId), m = byId(plan.machines, mId);
    if (!t || !m) return;
    md.shows = md.shows.filter(sh => sh.m !== mId);
    const here = md.shows.filter(sh => sh.t === tId);
    if (here.length >= t.atOnce) {           // full: the new one takes the place of the oldest
      md.shows = md.shows.filter(sh => sh !== here[0]);
      say(`${byId(plan.machines, here[0].m).name} taken off ${t.name}.`);
    }
    md.shows.push({ m: mId, t: tId });
    picked = null;
    changed();
  }
  function unplace(mId) {
    const md = mode();
    md.shows = md.shows.filter(sh => sh.m !== mId);
    changed();
  }

  /* Drag with a pointer (mouse, pen or finger); a press that does not move
   * is a tap, which picks the machine for tap-to-place. */
  function draggable(el, mId, onDrop, onTap) {
    onDrop = onDrop || place;
    onTap = onTap || (() => {
      picked = picked === mId ? null : mId;
      draw();
      if (picked) say(`Now tap a screen for ${byId(plan.machines, mId).name}.`);
    });
    el.addEventListener('pointerdown', e => {
      if (e.button || e.target.closest('button')) return;
      const x0 = e.clientX, y0 = e.clientY;
      let ghost = null, over = null, last = null, raf = 0;
      // near the top or bottom edge the page scrolls, so a screen further
      // down can be reached with a finger
      const edge = () => {
        raf = 0;
        if (!ghost || !last) return;
        const zone = 70, y = last.clientY, hgt = window.innerHeight;
        const dy = y > hgt - zone ? (y - (hgt - zone)) / 3 : y < zone ? -(zone - y) / 3 : 0;
        if (dy) {
          window.scrollBy(0, dy);
          hover(last);
          raf = requestAnimationFrame(edge);
        }
      };
      el.setPointerCapture(e.pointerId);
      const target = ev => {
        const under = document.elementFromPoint(ev.clientX, ev.clientY);
        return under && under.closest('[data-drop]');
      };
      const move = ev => {
        if (!ghost && Math.hypot(ev.clientX - x0, ev.clientY - y0) > 6) {
          ghost = el.cloneNode(true);
          ghost.classList.add('ghost');
          document.body.append(ghost);
        }
        if (!ghost) return;
        last = { clientX: ev.clientX, clientY: ev.clientY };
        ghost.style.left = ev.clientX + 'px';
        ghost.style.top = ev.clientY + 'px';
        hover(ev);
        if (!raf) raf = requestAnimationFrame(edge);
      };
      const hover = ev => {
        const t = target(ev);
        if (t !== over) {
          if (over) over.classList.remove('over');
          over = t;
          if (over) over.classList.add('over');
        }
      };
      const up = ev => {
        el.removeEventListener('pointermove', move);
        el.removeEventListener('pointerup', up);
        el.removeEventListener('pointercancel', up);
        if (raf) cancelAnimationFrame(raf);
        if (over) over.classList.remove('over');
        if (ghost) {
          ghost.remove();
          const t = ev.type === 'pointerup' && target(ev);
          if (t) onDrop(mId, t.dataset.drop);
        } else if (ev.type === 'pointerup') onTap();
      };
      el.addEventListener('pointermove', move);
      el.addEventListener('pointerup', up);
      el.addEventListener('pointercancel', up);
    });
    el.addEventListener('keydown', e => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        onTap();
      }
    });
  }

  // ---- drawing ----------------------------------------------------------------------

  function statusOf(mId, tId) {
    if (!result) return '';
    const md = result.modes.find(x => x.id === modeId);
    const sh = md && md.shows.find(x => x.m === mId && x.t === tId);
    return sh ? sh.status : '';
  }

  function tile(m, extra) {
    return h('div', Object.assign({
      class: 'tile' + (picked === m.id ? ' picked' : ''), tabindex: '0', role: 'button',
      'aria-pressed': String(picked === m.id), style: `--hue:${hue(m)}`,
    }, extra || {}),
      h('span', { class: 'swatch', 'aria-hidden': 'true' }),
      h('span', { class: 'tname' }, m.name),
      h('span', { class: 'sigs', 'aria-label': m.outputs.map(s => S[s].label).join(', ') },
        m.outputs.map(s => h('i', { style: `background:${S[s].color}`, title: S[s].label }))));
  }

  function drawTray() {
    const tray = $('tray');
    tray.replaceChildren();
    if (!plan.machines.length) tray.append(h('p', { class: 'empty' }, 'Add your machines below, or load an example.'));
    const md = mode();
    for (const m of plan.machines) {
      const where = md.shows.find(sh => sh.m === m.id);
      const t = where && byId(plan.monitors, where.t);
      const el = tile(m, { 'aria-label': `${m.name}${t ? ', on ' + t.name : ''}. Drag or tap to place.` });
      if (t) el.classList.add('placed');
      el.append(h('span', { class: 'where' }, t ? 'on ' + t.name : ''),
        h('button', { type: 'button', class: 'more', 'aria-label': 'Edit ' + m.name, title: 'Edit',
          onclick: () => { editing = editing && editing.id === m.id ? null : { kind: 'm', id: m.id }; draw(); } }, '⋯'));
      draggable(el, m.id);
      tray.append(el);
    }
    drawMachineEdit();
  }

  function sigChips(list, toggle, keys) {
    return h('div', { class: 'chips' }, (keys || P.SIGNAL_KEYS).map(s => h('button', {
      type: 'button', class: 'chip', 'aria-pressed': String(list.includes(s)), onclick: () => toggle(s),
    }, h('span', { class: 'dot', style: `background:${S[s].color}` }), S[s].label)));
  }

  function drawMachineEdit() {
    const box = $('machine-edit');
    box.replaceChildren();
    const m = editing && editing.kind === 'm' && byId(plan.machines, editing.id);
    if (!m) return;
    box.append(h('div', { class: 'item edit' },
      h('div', { class: 'row head' },
        h('input', { type: 'text', class: 'name', value: m.name, 'aria-label': 'Machine name', maxlength: '40',
          onchange: e => { m.name = e.target.value || m.name; changed(); } }),
        h('select', { 'aria-label': 'Kind', onchange: e => { m.kind = e.target.value; changed(); } },
          h('option', { value: 'computer', selected: m.kind === 'computer' }, 'Computer'),
          h('option', { value: 'console', selected: m.kind === 'console' }, 'Console')),
        h('button', { type: 'button', class: 'x', title: 'Done', 'aria-label': 'Done', onclick: () => { editing = null; draw(); } }, '✓')),
      h('div', { class: 'label' }, 'Video it sends'),
      sigChips(m.outputs, s => { m.outputs = m.outputs.includes(s) ? m.outputs.filter(x => x !== s) : m.outputs.concat(s); changed(); }),
      h('div', { class: 'row' },
        h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: m.agent,
          onchange: e => { m.agent = e.target.checked; changed(); } }), 'Runs a RetroKM agent'),
        h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: m.hub,
          onchange: e => { m.hub = e.target.checked; changed(); } }), 'The hub runs on it'),
        h('button', { type: 'button', class: 'danger', onclick: () => {
          plan.machines = plan.machines.filter(x => x !== m);
          editing = null;
          changed();
        } }, 'Remove machine'))));
  }

  function drawTabs() {
    const tabs = $('tabs');
    tabs.replaceChildren();
    const cur = mode();
    for (const md of plan.modes) {
      const r = result && result.modes.find(x => x.id === md.id);
      tabs.append(h('button', {
        type: 'button', role: 'tab', class: 'tab', 'aria-selected': String(md.id === cur.id),
        onclick: () => { modeId = md.id; picked = null; save(); draw(); },
      }, r && md.shows.length ? h('span', { class: 'led ' + r.status, 'aria-hidden': 'true' }) : null, md.name));
    }
    tabs.append(h('button', { type: 'button', class: 'tab add-tab', onclick: () => {
      const md = { id: P.newId('x'), name: freshName('Mode ' + (plan.modes.length + 1), plan.modes), shows: [] };
      plan.modes.push(md);
      modeId = md.id;
      changed();
    } }, '+ New mode'));

    const bar = $('mode-bar');
    bar.replaceChildren(
      h('input', { type: 'text', class: 'name', value: cur.name, 'aria-label': 'Mode name', maxlength: '40',
        onchange: e => { cur.name = e.target.value || cur.name; changed(); } }),
      h('button', { type: 'button', onclick: () => {
        const md = { id: P.newId('x'), name: freshName(cur.name + ' copy', plan.modes), shows: cur.shows.map(x => Object.assign({}, x)) };
        plan.modes.splice(plan.modes.indexOf(cur) + 1, 0, md);
        modeId = md.id;
        changed();
      } }, 'Duplicate'),
      h('button', { type: 'button', onclick: () => { cur.shows = []; changed(); } }, 'Clear'),
      plan.modes.length > 1 && h('button', { type: 'button', class: 'danger', onclick: () => {
        const drop = () => {
          plan.modes = plan.modes.filter(x => x !== cur);
          modeId = null;
          changed();
        };
        if (cur.shows.length) window.RetroKMUI.ask(`Delete the mode "${cur.name}"?`, 'Delete it', drop);
        else drop();
      } }, 'Delete'));
  }

  function drawDesk() {
    const desk = $('desk');
    desk.replaceChildren();
    const md = mode();
    if (!plan.monitors.length) desk.append(h('p', { class: 'empty' }, 'Add your screens below: monitors, TVs, capture cards.'));
    for (const t of plan.monitors) {
      const here = md.shows.filter(sh => sh.t === t.id);
      const screen = h('div', {
        class: `screen ${t.kind}` + (picked ? ' armed' : ''), tabindex: picked ? '0' : null,
        role: picked ? 'button' : null, 'aria-label': picked ? `Put ${byId(plan.machines, picked).name} on ${t.name}` : null,
        onclick: e => { if (picked && !e.target.closest('.tile')) place(picked, t.id); },
        onkeydown: e => { if (picked && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); place(picked, t.id); } },
      });
      if (!here.length) screen.append(h('span', { class: 'drop-hint' }, picked ? 'Tap to put it here' : 'Drop a machine here'));
      for (const sh of here) {
        const m = byId(plan.machines, sh.m);
        const st = statusOf(sh.m, sh.t);
        const el = tile(m, { 'aria-label': `${m.name} on ${t.name}` + (st ? ': ' + { ok: 'works', plan: 'works with the suggestions', bad: 'not possible yet' }[st] : '') });
        el.classList.add('onscreen');
        if (st) el.classList.add(st);
        el.append(h('span', { class: 'mark', 'aria-hidden': 'true' }, { ok: '✓', plan: '★', bad: '✗' }[st] || ''),
          h('button', { type: 'button', class: 'more', title: 'Take it off', 'aria-label': `Take ${m.name} off ${t.name}`,
            onclick: () => unplace(m.id) }, '×'));
        draggable(el, m.id);
        screen.append(el);
      }
      if (t.atOnce > 1) screen.append(h('span', { class: 'cap' }, `shows ${t.atOnce} at once`));
      desk.append(h('div', { class: 'station', 'data-drop': t.id },      // the label under it counts too
        screen,
        h('div', { class: 'stand' },
          h('span', { class: 'sname' }, t.name),
          h('span', { class: 'sins' }, t.inputs.map(s => S[s].label).join(' · ') || 'no inputs yet'),
          h('button', { type: 'button', class: 'more', 'aria-label': 'Edit ' + t.name, title: 'Edit',
            onclick: () => { editing = editing && editing.id === t.id ? null : { kind: 't', id: t.id }; draw(); } }, '⋯'))));
    }
    drawWhy();
    drawMonitorEdit();
  }

  function drawWhy() {
    const box = $('mode-why');
    box.replaceChildren();
    const md = result && result.modes.find(x => x.id === modeId);
    if (!md || !md.shows.length) return;
    const M = id => byId(result.plan.machines, id).name, T = id => byId(result.plan.monitors, id).name;
    if (md.status === 'ok') box.append(h('p', { class: 'verdict ok' }, '✓ This mode works with what you own.'));
    else if (md.status === 'plan') box.append(h('p', { class: 'verdict plan' },
      `★ Works once you add the suggested hardware in “What it takes” (needed for ${names(md.shows.filter(x => x.status === 'plan').map(x => M(x.m) + ' on ' + T(x.t)))}).`));
    for (const x of md.shows.filter(y => y.status === 'bad')) box.append(h('div', { class: 'miss' }, h('p', {}, '✗ ' + x.reason)));
  }

  function drawMonitorEdit() {
    const box = $('monitor-edit');
    box.replaceChildren();
    const t = editing && editing.kind === 't' && byId(plan.monitors, editing.id);
    if (!t) return;
    box.append(h('div', { class: 'item edit' },
      h('div', { class: 'row head' },
        h('input', { type: 'text', class: 'name', value: t.name, 'aria-label': 'Screen name', maxlength: '40',
          onchange: e => { t.name = e.target.value || t.name; changed(); } }),
        h('select', { 'aria-label': 'Kind', onchange: e => { t.kind = e.target.value; changed(); } },
          [['monitor', 'Monitor'], ['tv', 'TV'], ['capture', 'Capture card']].map(([v, l]) => h('option', { value: v, selected: t.kind === v }, l))),
        h('button', { type: 'button', class: 'x', title: 'Done', 'aria-label': 'Done', onclick: () => { editing = null; draw(); } }, '✓')),
      h('div', { class: 'label' }, 'Inputs'),
      h('div', { class: 'chips' },
        t.inputs.map((s, i) => h('button', { type: 'button', class: 'chip in', 'aria-label': 'Remove input ' + S[s].label,
          onclick: () => { t.inputs.splice(i, 1); changed(); } }, h('span', { class: 'dot', style: `background:${S[s].color}` }), S[s].label, ' ×')),
        h('select', { 'aria-label': 'Add an input', onchange: e => { if (e.target.value) { t.inputs.push(e.target.value); changed(); } } },
          h('option', { value: '' }, '+ input'), P.SIGNAL_KEYS.filter(s => s !== 'sog').map(s => h('option', { value: s }, S[s].label)))),
      h('div', { class: 'row' },
        t.inputs.includes('vga') && h('label', { class: 'check' }, h('input', { type: 'checkbox', checked: t.sog,
          onchange: e => { t.sog = e.target.checked; changed(); } }), 'VGA takes sync-on-green'),
        t.kind !== 'capture' && h('label', { class: 'check' }, 'Shows at once ',
          h('select', { onchange: e => { t.atOnce = +e.target.value; changed(); } }, [1, 2, 3, 4].map(n => h('option', { value: n, selected: t.atOnce === n }, n)))),
        h('button', { type: 'button', class: 'danger', onclick: () => {
          plan.monitors = plan.monitors.filter(x => x !== t);
          editing = null;
          changed();
        } }, 'Remove screen'))));
  }

  function drawOwned() {
    const box = $('owned');
    box.replaceChildren();
    if (!plan.hardware.length) box.append(h('p', { class: 'empty' }, 'Nothing yet.'));
    for (const hw of plan.hardware)
      box.append(h('span', { class: 'chip in' + (hw.planned ? ' to-buy' : '') }, hw.name, hw.planned ? ' (to buy)' : '',
        h('button', { type: 'button', class: 'x small', 'aria-label': 'Remove ' + hw.name,
          onclick: () => { plan.hardware = plan.hardware.filter(x => x !== hw); changed(); } }, '×')));
  }

  function drawVerdict() {
    const box = $('verdict'), sug = $('suggestions');
    box.replaceChildren();
    sug.replaceChildren();
    if (!result) return;
    const used = result.modes.filter(m => m.shows.length);
    if (!used.length) {
      box.append(h('p', { class: 'summary' }, 'Put some machines on screens in a mode to see what it takes.'));
      return;
    }
    for (const md of used) {
      const text = { ok: 'works with what you own', plan: 'works with the suggestions below', bad: 'is not possible yet' }[md.status];
      box.append(h('p', { class: 'verdict-line' }, h('span', { class: 'led ' + md.status, 'aria-hidden': 'true' }),
        h('button', { type: 'button', class: 'linkish', onclick: () => { modeId = md.id; save(); draw(); } }, md.name), ' ' + text + '.'));
    }
    if (result.suggestions.length) sug.append(h('h3', {}, 'What to add'));
    for (const st of result.suggestions) {
      const c = st.item;
      sug.append(h('div', { class: 'sug' },
        h('h3', {}, c.category, ' ', c.driver !== 'manual' ? h('span', { class: 'tag' }, 'the hub can drive it') : null),
        h('p', {}, st.reason),
        h('p', { class: 'eg' }, 'For example: ' + names(c.examples) + '.'),
        c.notes && h('p', { class: 'eg' }, c.notes),
        h('button', { type: 'button', onclick: () => {
          plan.hardware.push(P.hardwareFromCatalogue(c.id, { planned: true, name: c.name }));
          changed();
          say(c.name + ' added to your hardware, marked "to buy".');
        } }, 'Add to my hardware')));
    }
  }

  function draw() {
    drawTray();
    drawTabs();
    drawDesk();
    drawOwned();
    drawVerdict();
  }

  function analyse() {
    result = P.analyse(plan);
    draw();
    if (sim) {                    // the plan changed: the pretend desk starts again
      simBuild();
      if (view === 'try') drawTry();
    }
  }

  // ---- buttons -----------------------------------------------------------------------

  function preset(p) {
    if (!p) return {};
    const c = JSON.parse(JSON.stringify(p));
    delete c.label;
    return c;
  }
  function fill() {
    $('machine-preset').replaceChildren(h('option', { value: '' }, 'Blank machine'),
      ...P.MACHINE_PRESETS.map((p, i) => h('option', { value: i }, p.label)));
    $('monitor-preset').replaceChildren(h('option', { value: '' }, 'Blank screen'),
      ...P.MONITOR_PRESETS.map((p, i) => h('option', { value: i }, p.label)));
    $('hardware-pick').replaceChildren(...P.CATALOGUE.map(c => h('option', { value: c.id }, c.name)));
    for (const [k, ex] of Object.entries(EXAMPLES))
      $('example').append(h('option', { value: k }, ex.name.replace(/^Example: /, '')));
  }

  $('add-machine').addEventListener('click', () => {
    // machines made here want only what the modes give them
    const m = Object.assign({ id: P.newId('m'), name: 'Machine', kind: 'computer', outputs: [], agent: true, wants: [] },
      preset(P.MACHINE_PRESETS[$('machine-preset').value]), { wants: [] });
    m.name = freshName(m.name, plan.machines);
    plan.machines.push(m);
    if (!m.outputs.length) editing = { kind: 'm', id: m.id };
    changed();
  });
  $('add-monitor').addEventListener('click', () => {
    const t = Object.assign({ id: P.newId('t'), name: 'Screen', kind: 'monitor', inputs: [], sog: false, atOnce: 1 },
      preset(P.MONITOR_PRESETS[$('monitor-preset').value]));
    t.name = freshName(t.name, plan.monitors);
    plan.monitors.push(t);
    if (!t.inputs.length) editing = { kind: 't', id: t.id };
    changed();
  });
  $('add-hardware').addEventListener('click', () => {
    const hw = P.hardwareFromCatalogue($('hardware-pick').value);
    if (hw) { plan.hardware.push(hw); changed(); }
  });
  $('example').addEventListener('change', e => {
    const ex = EXAMPLES[e.target.value];
    e.target.value = '';
    if (!ex) return;
    const go = () => {
      plan = P.normalizePlan(ex);
      modeId = null;
      picked = editing = null;
      changed();
      say('Loaded the example.');
    };
    if (plan.machines.length || plan.monitors.length)
      window.RetroKMUI.ask('Replace your plan with the example? Save yours first if you want to keep it.', 'Load the example', go);
    else go();
  });
  $('new').addEventListener('click', () => {
    window.RetroKMUI.ask('Start over with an empty plan?', 'Start over', () => {
      plan = P.normalizePlan({});
      modeId = null;
      picked = editing = null;
      changed();
    });
  });
  $('save').addEventListener('click', () => download('retrokm-plan.json', JSON.stringify(plan, null, 2) + '\n', 'application/json'));
  $('load').addEventListener('change', e => {
    const f = e.target.files[0];
    if (!f) return;
    const rd = new FileReader();
    rd.onload = () => {
      try { plan = P.normalizePlan(JSON.parse(rd.result)); modeId = null; changed(); say(`Opened ${f.name}.`); }
      catch (err) { say(`${f.name} is not a plan file (${err.message}).`); }
    };
    rd.readAsText(f);
    e.target.value = '';
  });
  const config = () => result ? P.makeConfig(result.withSuggestions, result.routing.cables) : '';
  $('download').addEventListener('click', () => { analyse(); download('retrokm.conf', config(), 'text/plain'); });
  $('copy').addEventListener('click', () => {
    analyse();
    const text = config();
    const fallback = () => {
      const ta = h('textarea', { style: 'position:fixed;opacity:0' });
      ta.value = text;
      document.body.append(ta);
      ta.select();
      let ok = false;
      try { ok = document.execCommand('copy'); } catch (e) { ok = false; }
      ta.remove();
      say(ok ? 'Copied.' : 'Could not copy here: use Download.');
    };
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(() => say('Copied.'), fallback);
    else fallback();
  });
  document.addEventListener('keydown', e => { if (e.key === 'Escape' && picked) { picked = null; draw(); } });
  // the other page may change the plan in another tab
  window.addEventListener('storage', e => {
    if (e.key !== STORE) return;
    plan = load() || plan;
    draw();
    clearTimeout(timer);
    timer = setTimeout(analyse, 120);
  });

  // ---- try it out ----------------------------------------------------------------------

  let view = 'design', simOwned = false, sim = null, simPicked = null, active = null, log = [], lastLost = new Set();
  try { simOwned = localStorage.getItem('retrokm-try-owned') === '1'; } catch (e) { simOwned = false; }

  function simBuild() {
    if (!result) return;
    sim = simOwned ? P.simulator(result.plan, result.now.cables) : P.simulator(result.withSuggestions, result.routing.cables);
    log = [];
    active = null;
    simPicked = null;
    lastLost = new Set();
  }
  const sp = () => (sim ? sim.plan : plan);
  const mName = id => byId(sp().machines, id).name;
  const tName = id => byId(sp().monitors, id).name;

  function entry(title, items, cls) {
    log.unshift({ title, items, cls });
    log = log.slice(0, 6);
  }
  function itemsOf(r, mId, tId) {
    if (!r.ok) {
      const fixes = simOwned && result.routing.reach[mId] && result.routing.reach[mId][tId] && result.routing.reach[mId][tId].ok;
      return [{ cls: 'fail', text: fixes
        ? `With what you own there is no way from ${mName(mId)} to ${tName(tId)}. The suggested hardware fixes that: switch to “With the suggestions” above to try it.`
        : r.reason }];
    }
    return r.steps.map(x => ({ cls: x.auto ? 'auto' : 'hand', text: x.text }))
      .concat(r.lost.map(l => ({ cls: 'lost', text: `${tName(l.t)} loses ${mName(l.m)}: ${l.why}.` })));
  }
  function simShow(mId, tId) {
    const r = sim.show(mId, tId);
    lastLost = new Set(r.lost.map(l => l.t));
    entry(`${mName(mId)} on ${tName(tId)}`, itemsOf(r, mId, tId), !r.ok ? 'bad' : r.lost.length ? 'warn' : 'ok');
    simPicked = null;
    if (r.ok) active = tId;
    drawTry();
  }
  function simMode(md) {
    const rs = sim.mode(md);
    const items = [];
    let cls = 'ok';
    lastLost = new Set();
    for (const r of rs) {
      if (!r.ok) cls = 'bad';
      else if (r.lost.length && cls === 'ok') cls = 'warn';
      items.push({ cls: 'head', text: `${mName(r.m)} on ${tName(r.t)}` + (r.ok && !r.kept ? ' (then lost again, below)' : '') });
      items.push(...itemsOf(r, r.m, r.t));
      r.lost.forEach(l => lastLost.add(l.t));
    }
    if (!rs.length) items.push({ cls: 'fail', text: 'This mode has nothing in it yet.' });
    entry(`Switched to "${md.name}"`, items, cls);
    drawTry();
  }

  function controlText() {
    if (!active) return 'Point at a screen (or tap one) to see where the keyboard and mouse go.';
    const c = sim.control(active);
    if (c.kind === 'none') return `${tName(active)} shows nothing: the pointer would just pass over it.`;
    if (c.kind === 'video') return `${mName(c.m)} on ${tName(active)} is video only: no keyboard or mouse there.`;
    return `⌨ Keyboard and mouse → ${mName(c.m)} (on ${tName(active)}). Copy here, paste on another machine: the clipboard follows.`;
  }
  function setActive(tId) {
    if (active === tId) return;
    active = tId;
    for (const el of $('sim-desk').querySelectorAll('.sim-screen')) el.classList.toggle('active', el.dataset.t === tId);
    $('sim-control').textContent = controlText();
    typed();
  }
  function typed() {
    const v = $('sim-type').value;
    const out = $('sim-typed');
    if (!v) { out.textContent = ''; return; }
    const c = active && sim.control(active);
    out.textContent = !c || c.kind === 'none' ? 'Those keys go nowhere: no machine under the pointer.'
      : c.kind === 'video' ? `${mName(c.m)} has no keyboard: those keys go nowhere.`
      : `${mName(c.m)} receives: “${v}”`;
  }

  function drawTry() {
    if (!sim) simBuild();
    if (!sim) return;
    $('sim-owned').setAttribute('aria-checked', String(simOwned));
    $('sim-sug').setAttribute('aria-checked', String(!simOwned));
    const p = sim.plan;

    const modes = $('sim-modes');
    modes.replaceChildren();
    const used = p.modes.filter(md => md.shows.length);
    if (used.length) {
      modes.append(h('span', { class: 'label inline' }, 'Switch to:'));
      for (const md of used) modes.append(h('button', { type: 'button', onclick: () => simMode(md) }, md.name));
    }

    const tray = $('sim-tray');
    tray.replaceChildren();
    for (const m of p.machines) {
      const where = [...sim.on].find(([, v]) => v.m === m.id);
      const el = tile(m, { 'aria-label': `${m.name}. Drag onto a screen, or tap it and then a screen.` });
      el.classList.toggle('picked', simPicked === m.id);
      el.setAttribute('aria-pressed', String(simPicked === m.id));
      if (where) el.append(h('span', { class: 'where' }, 'on ' + tName(where[0])));
      draggable(el, m.id, simShow, () => {
        simPicked = simPicked === m.id ? null : m.id;
        drawTry();
        if (simPicked) say(`Now tap a screen for ${m.name}.`);
      });
      tray.append(el);
    }

    const desk = $('sim-desk');
    desk.replaceChildren(h('div', { class: 'cursor', id: 'cursor', 'aria-hidden': 'true' }));
    for (const t of p.monitors) {
      const v = sim.on.get(t.id);
      const m = v && byId(p.machines, v.m);
      const screen = h('div', {
        class: `sim-screen ${t.kind}` + (active === t.id ? ' active' : '') + (simPicked ? ' armed' : '') + (lastLost.has(t.id) ? ' lost' : ''),
        'data-t': t.id, tabindex: '0', role: 'button',
        'aria-label': simPicked ? `Show ${mName(simPicked)} on ${t.name}` : `${t.name}: ${m ? m.name : 'no signal'}`,
        onclick: () => { if (simPicked) simShow(simPicked, t.id); else setActive(t.id); },
        onkeydown: e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); if (simPicked) simShow(simPicked, t.id); else setActive(t.id); } },
      });
      if (m) {
        screen.append(h('div', { class: 'pic', style: `--hue:${hue(m)}` },
          m.kind === 'computer' ? [h('span', { class: 'menubar' }), h('span', { class: 'win' })] : null,
          h('b', {}, m.name),
          h('small', {}, m.kind === 'console' ? '▶ playing' : m.agent || m.hub ? 'desktop' : 'video only')));
      } else screen.append(h('span', { class: 'nosig' }, 'NO SIGNAL'));
      const via = v ? v.hops.filter(x => x.dev).map(x => byId(p.hardware, x.dev).name) : [];
      desk.append(h('div', { class: 'station', 'data-drop': t.id },
        screen,
        h('div', { class: 'stand' }, h('span', { class: 'sname' }, t.name),
          v && h('button', { type: 'button', class: 'more', title: 'Turn off', 'aria-label': `Clear ${t.name}`,
            onclick: () => { sim.clear(t.id); entry(`${t.name} cleared`, [], 'ok'); drawTry(); } }, '×'),
          h('span', { class: 'via' }, via.length ? 'through ' + via.join(' → ') : v ? 'cabled straight in' : ''))));
    }
    $('sim-control').textContent = controlText();
    typed();

    // the last thing that happened, next to the screens (the full log is further down)
    const last = $('sim-last');
    last.replaceChildren();
    if (log.length) {
      const e = log[0];
      const hand = e.items.filter(i => i.cls === 'hand').length, auto = e.items.filter(i => i.cls === 'auto').length;
      const bad = e.items.filter(i => i.cls === 'lost' || i.cls === 'fail');
      last.className = 'sim-last ' + e.cls;
      last.append(h('strong', {}, { ok: '✓ ', warn: '⚠ ', bad: '✗ ' }[e.cls] + e.title),
        (auto || hand) ? h('span', {}, ` — ${auto ? auto + ' done by the hub' : ''}${auto && hand ? ', ' : ''}${hand ? hand + ' by hand' : ''}.`) : null,
        bad.map(i => h('div', {}, i.text)),
        h('a', { href: '#h-log' }, 'All steps'));
    }

    const lg = $('sim-log');
    lg.replaceChildren();
    if (!log.length) lg.append(h('p', { class: 'empty' }, 'Nothing yet: put a machine on a screen, or switch to a mode.'));
    for (const e of log)
      lg.append(h('div', { class: 'log-entry ' + e.cls }, h('h3', {}, e.title),
        e.items.length ? h('ul', {}, e.items.map(it => it.cls === 'head' ? h('li', { class: 'head' }, h('strong', {}, it.text))
          : h('li', { class: it.cls }, it.text))) : null));

    const lim = $('sim-limits');
    lim.replaceChildren();
    const ls = P.limits(result, simOwned);
    if (!ls.length) lim.append(h('li', {}, 'Nothing stands out: everything you asked for works.'));
    for (const l of ls) lim.append(h('li', {}, l));
  }

  // the pretend pointer: follows the mouse over the desk, and the screen under it gets the keyboard
  $('sim-desk').addEventListener('pointermove', e => {
    if (e.pointerType !== 'mouse') return;
    const desk = $('sim-desk'), cur = $('cursor'), r = desk.getBoundingClientRect();
    if (cur) { cur.style.display = 'block'; cur.style.left = (e.clientX - r.left) + 'px'; cur.style.top = (e.clientY - r.top) + 'px'; }
    const under = document.elementFromPoint(e.clientX, e.clientY);
    const scr = under && under.closest('.sim-screen');
    if (scr) setActive(scr.dataset.t);
  });
  $('sim-desk').addEventListener('pointerleave', () => { const cur = $('cursor'); if (cur) cur.style.display = 'none'; });
  $('sim-type').addEventListener('input', typed);
  $('sim-owned').addEventListener('click', () => { simOwned = true; try { localStorage.setItem('retrokm-try-owned', '1'); } catch (e) { /* fine */ } simBuild(); drawTry(); });
  $('sim-sug').addEventListener('click', () => { simOwned = false; try { localStorage.setItem('retrokm-try-owned', '0'); } catch (e) { /* fine */ } simBuild(); drawTry(); });
  $('sim-reset').addEventListener('click', () => { simBuild(); drawTry(); say('All screens off.'); });

  function setView(v) {
    view = v;
    $('design-view').hidden = v !== 'design';
    $('try-view').hidden = v !== 'try';
    $('tab-design').setAttribute('aria-selected', String(v === 'design'));
    $('tab-try').setAttribute('aria-selected', String(v === 'try'));
    if (v === 'try') { if (!sim) simBuild(); drawTry(); }
    try {
      if (location.hash !== (v === 'try' ? '#try' : '')) history.replaceState(null, '', v === 'try' ? '#try' : location.pathname);
    } catch (e) { /* some frames refuse: the view still changes */ }
  }
  $('tab-design').addEventListener('click', () => setView('design'));
  $('tab-try').addEventListener('click', () => setView('try'));

  fill();
  mode();
  draw();
  analyse();
  if (location.hash === '#try') setView('try');
  window.addEventListener('hashchange', () => setView(location.hash === '#try' ? 'try' : 'design'));
})();
