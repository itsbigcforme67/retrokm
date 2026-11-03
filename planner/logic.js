/* RetroKM planner: the thinking part, with no page code in it.
 *
 *   route(plan)        how every machine reaches the monitors it should,
 *                      laying cables between what the plan has
 *   analyse(plan)      route + suggestions for what to add, and why
 *   makeConfig(plan, cables)   a starter retrokm.conf
 *
 * A plan is plain JSON (see normalizePlan).  Used by index.html (as a
 * classic script, so it works opened from disk) and by test.mjs (require).
 */
(function (root) {
  'use strict';

  const CAT = (typeof require === 'function')
    ? require('./catalogue.js').CATALOGUE
    : root.RetroKMCatalogue.CATALOGUE;

  // ---- signals -------------------------------------------------------------

  const SIGNALS = {
    composite: { label: 'Composite', says: 'composite', color: '#d9a400' },
    svideo:    { label: 'S-Video', says: 'S-Video', color: '#c2410c' },
    component: { label: 'Component', says: 'component', color: '#16a34a' },
    rgb:       { label: 'RGB / SCART', says: 'RGB (SCART)', color: '#db2777' },
    vga:       { label: 'VGA', says: 'VGA', color: '#2563eb' },
    sog:       { label: 'VGA sync-on-green', says: 'sync-on-green VGA', color: '#0d9488' },
    dvi:       { label: 'DVI', says: 'DVI', color: '#7c3aed' },
    hdmi:      { label: 'HDMI', says: 'HDMI', color: '#9333ea' },
  };
  const SIGNAL_KEYS = Object.keys(SIGNALS);

  /* Can a port that takes `accepts` take `sig`?  DVI and HDMI meet with a
   * plain adapter cable. */
  function takes(accepts, sig) {
    if (accepts.includes(sig)) return true;
    if (sig === 'dvi' && accepts.includes('hdmi')) return true;
    if (sig === 'hdmi' && accepts.includes('dvi')) return true;
    return false;
  }

  // ---- presets -------------------------------------------------------------

  const MACHINE_PRESETS = [
    { name: 'PC (90s, VGA)', kind: 'computer', outputs: ['vga'], agent: true },
    { name: 'Modern PC or laptop', kind: 'computer', outputs: ['hdmi'], agent: true },
    { name: 'SGI workstation', kind: 'computer', outputs: ['sog'], agent: true },
    { name: 'Sun workstation', kind: 'computer', outputs: ['sog'], agent: true },
    { name: 'Power Mac G4', kind: 'computer', outputs: ['vga', 'dvi'], agent: true },
    { name: '68k Mac', kind: 'computer', outputs: ['vga'], agent: true },
    { name: 'Amiga', kind: 'computer', outputs: ['rgb', 'composite'], agent: false },
    { name: '16-bit console', kind: 'console', outputs: ['rgb', 'composite'], agent: false },
    { name: 'N64', kind: 'console', outputs: ['svideo', 'composite'], agent: false },
    { name: 'PlayStation 2', kind: 'console', outputs: ['component', 'rgb', 'svideo', 'composite'], agent: false },
    { name: 'Dreamcast', kind: 'console', outputs: ['vga', 'rgb', 'svideo', 'composite'], agent: false },
    { name: 'GameCube', kind: 'console', outputs: ['component', 'svideo', 'composite'], agent: false },
    { name: 'Xbox', kind: 'console', outputs: ['component', 'svideo', 'composite'], agent: false },
    { name: 'HDMI console', kind: 'console', outputs: ['hdmi'], agent: false },
  ];

  const MONITOR_PRESETS = [
    { name: 'PC CRT', kind: 'monitor', inputs: ['vga'], sog: false },
    { name: 'PC CRT (takes sync-on-green)', kind: 'monitor', inputs: ['vga'], sog: true },
    { name: 'LCD monitor', kind: 'monitor', inputs: ['vga', 'dvi', 'hdmi'], sog: false },
    { name: 'Modern TV', kind: 'tv', inputs: ['hdmi', 'hdmi', 'hdmi'], sog: false },
    { name: 'CRT TV (SCART)', kind: 'tv', inputs: ['rgb', 'composite'], sog: false },
    { name: 'CRT TV (AV)', kind: 'tv', inputs: ['composite', 'svideo'], sog: false },
    { name: 'PVM / BVM', kind: 'tv', inputs: ['rgb', 'component', 'svideo', 'composite'], sog: false },
    { name: 'Capture card', kind: 'capture', inputs: ['hdmi'], sog: false },
  ];

  // ---- the plan ------------------------------------------------------------

  let idSeq = 0;
  function newId(prefix) {
    idSeq += 1;
    return prefix + Date.now().toString(36).slice(-4) + idSeq.toString(36);
  }

  function clone(x) { return JSON.parse(JSON.stringify(x)); }

  /* Fill in anything missing, drop what makes no sense.  Never throws on a
   * loaded file: a bad plan becomes an emptier one. */
  function normalizePlan(p) {
    p = (p && typeof p === 'object') ? clone(p) : {};
    const out = { version: 1, name: typeof p.name === 'string' ? p.name : 'My desk', machines: [], monitors: [], hardware: [] };
    const ids = new Set();
    const uniq = (id, prefix) => {
      let v = (typeof id === 'string' && id) ? id : newId(prefix);
      while (ids.has(v)) v = newId(prefix);
      ids.add(v);
      return v;
    };
    for (const m of Array.isArray(p.monitors) ? p.monitors : []) {
      if (!m || typeof m !== 'object') continue;
      out.monitors.push({
        id: uniq(m.id, 't'),
        name: String(m.name || 'Monitor'),
        kind: ['monitor', 'tv', 'capture'].includes(m.kind) ? m.kind : 'monitor',
        inputs: (Array.isArray(m.inputs) ? m.inputs : []).filter(s => SIGNALS[s] && s !== 'sog'),
        sog: !!m.sog,
        atOnce: Math.max(1, Math.min(4, parseInt(m.atOnce, 10) || 1)),
      });
    }
    const monIds = new Set(out.monitors.map(m => m.id));
    for (const m of Array.isArray(p.machines) ? p.machines : []) {
      if (!m || typeof m !== 'object') continue;
      out.machines.push({
        id: uniq(m.id, 'm'),
        name: String(m.name || 'Machine'),
        kind: m.kind === 'console' ? 'console' : 'computer',
        outputs: (Array.isArray(m.outputs) ? m.outputs : []).filter(s => SIGNALS[s]),
        agent: m.agent === undefined ? m.kind !== 'console' : !!m.agent,
        hub: !!m.hub,
        wants: Array.isArray(m.wants) ? m.wants.filter(id => monIds.has(id)) : null,
      });
    }
    for (const h of Array.isArray(p.hardware) ? p.hardware : []) {
      if (!h || typeof h !== 'object') continue;
      const hw = normalizeHardware(h);
      if (!hw) continue;
      hw.id = uniq(h.id, 'h');
      out.hardware.push(hw);
    }
    return out;
  }

  function normalizeHardware(h) {
    const kind = h.kind === 'converter' ? 'converter' : 'switch';
    const hw = {
      id: h.id, name: String(h.name || 'Box'), kind,
      driver: ['extron-sis', 'text'].includes(h.driver) ? h.driver : 'manual',
      catalog: typeof h.catalog === 'string' ? h.catalog : '',
      deviceFile: typeof h.deviceFile === 'string' ? h.deviceFile.replace(/[^A-Za-z0-9_.-]/g, '') : '',
      planned: !!h.planned,
      notes: typeof h.notes === 'string' ? h.notes : '',
    };
    if (kind === 'switch') {
      hw.ins = clampInt(h.ins, 1, 64, 4);
      hw.outs = clampInt(h.outs, 1, 64, 1);
      hw.accepts = (Array.isArray(h.accepts) ? h.accepts : []).filter(s => SIGNALS[s]);
      if (!hw.accepts.length) return null;
    } else {
      hw.ins = (Array.isArray(h.ins) ? h.ins : [])
        .map(j => ({ name: String(j && j.name || 'in'), accepts: (j && Array.isArray(j.accepts) ? j.accepts : []).filter(s => SIGNALS[s]) }))
        .filter(j => j.accepts.length);
      hw.outs = (Array.isArray(h.outs) ? h.outs : [])
        .map(j => ({ name: String(j && j.name || 'out'), gives: SIGNALS[j && j.gives] ? j.gives : '' }))
        .filter(j => j.gives);
      if (!hw.ins.length || !hw.outs.length) return null;
    }
    return hw;
  }

  function clampInt(v, lo, hi, dflt) {
    const n = parseInt(v, 10);
    return Number.isFinite(n) ? Math.max(lo, Math.min(hi, n)) : dflt;
  }

  /* A piece of hardware for the plan, from the catalogue */
  function hardwareFromCatalogue(catId, extra) {
    const c = CAT.find(x => x.id === catId);
    if (!c) return null;
    const hw = normalizeHardware(Object.assign(clone(c), { catalog: c.id }, extra || {}));
    hw.id = (extra && extra.id) || newId('h');
    return hw;
  }

  /* A device file (devices/*.conf) as hardware for the plan.  The file's
   * signal names are the hub's: hdmi stands for HDMI and DVI. */
  function parseDeviceFile(text, fileName) {
    const kv = {};
    for (let line of String(text).split(/\r?\n/)) {
      line = line.trim();
      if (!line || line[0] === '#' || line[0] === ';') continue;
      const cut = line.search(/\s[#;]/);
      if (cut >= 0) line = line.slice(0, cut).trim();
      const eq = line.indexOf('=');
      if (eq < 0) continue;
      kv[line.slice(0, eq).trim().toLowerCase()] = line.slice(eq + 1).trim();
    }
    const sigs = [];
    for (const s of (kv.signal || '').toLowerCase().split(/[\s,]+/)) {
      if (s === 'hdmi') sigs.push('hdmi', 'dvi');
      else if (s === 'vga') sigs.push('vga', 'rgb');      // RGBS rides on a VGA matrix with an adapter
      else if (SIGNALS[s]) sigs.push(s);
    }
    const file = String(fileName || '').replace(/^.*[\\/]/, '').replace(/\.conf$/i, '');
    const hw = normalizeHardware({
      name: kv.name || file || 'Device', kind: 'switch',
      driver: kv.driver || (kv.tie ? 'text' : 'manual'),
      ins: kv.inputs, outs: kv.outputs, accepts: [...new Set(sigs)],
      deviceFile: file, notes: kv.notes || '',
    });
    if (hw) { hw.id = newId('h'); hw.fileKind = kv.kind || ''; }
    return hw;
  }

  /* The ports of a piece of hardware, numbered from 1 for switches (in1,
   * out1 ...: what the hub calls them) */
  function portsOf(hw) {
    if (hw.kind === 'switch') {
      const ins = [], outs = [];
      for (let i = 1; i <= hw.ins; i++) ins.push({ name: 'in' + i, accepts: hw.accepts });
      for (let j = 1; j <= hw.outs; j++) outs.push({ name: 'out' + j, gives: hw.accepts });
      return { ins, outs };
    }
    return {
      ins: hw.ins.map(j => ({ name: j.name, accepts: j.accepts })),
      outs: hw.outs.map(j => ({ name: j.name, gives: [j.gives] })),
    };
  }

  function monitorAccepts(mon, input) {
    return input === 'vga' && mon.sog ? ['vga', 'sog'] : [input];
  }

  function wantedPairs(plan) {
    const pairs = [];
    for (const m of plan.machines) {
      const wants = m.wants === null ? plan.monitors.map(t => t.id) : m.wants;
      for (const t of plan.monitors) if (wants.includes(t.id)) pairs.push({ m: m.id, t: t.id });
    }
    return pairs;
  }

  // ---- routing -------------------------------------------------------------

  /*
   * Cables are laid one wanted pair at a time, as a cheapest-path search
   * over what exists: an existing cable is almost free, a new one costs 1
   * (5 when a machine would take a monitor input that other machines want
   * too, 3 for a box doing that, so a switch gets used), a conversion
   * costs 2, going through hardware not bought yet 1.5.  Ports hold one
   * cable each.  The order matters for a greedy search, so it is run again
   * with the failed pairs first and the better result kept.
   */
  function route(plan, opts) {
    opts = opts || {};
    const ctx = buildCtx(plan);
    const pairs = wantedPairs(plan);
    let best = runOrder(ctx, pairs);
    if (best.failed.length && !opts.quick) {
      const failedKeys = new Set(best.failed.map(f => f.m + '|' + f.t));
      const failedFirst = pairs.filter(p => failedKeys.has(p.m + '|' + p.t)).concat(pairs.filter(p => !failedKeys.has(p.m + '|' + p.t)));
      for (const order of [failedFirst, pairs.slice().reverse()]) {
        const again = runOrder(ctx, order);
        if (again.okCount > best.okCount || (again.okCount === best.okCount && again.cables.length < best.cables.length)) best = again;
        if (!best.failed.length) break;
      }
    }
    return best;
  }

  function buildCtx(plan) {
    const devs = new Map();
    for (const h of plan.hardware) devs.set(h.id, { hw: h, ports: portsOf(h) });
    const demand = new Map();
    for (const p of wantedPairs(plan)) demand.set(p.t, (demand.get(p.t) || 0) + 1);
    return {
      plan, devs, demand,
      machines: new Map(plan.machines.map(m => [m.id, m])),
      monitors: new Map(plan.monitors.map(t => [t.id, t])),
    };
  }

  const fromKey = f => f.t + ':' + f.id + ':' + f.port;
  const toKey = t => t.t + ':' + t.id + ':' + t.port;

  function runOrder(ctx, pairs) {
    const st = { cables: [], byFrom: new Map(), toUsed: new Set() };
    const reach = {};
    const failed = [];
    let okCount = 0;
    for (const p of pairs) {
      const r = search(ctx, st, p.m, p.t);
      (reach[p.m] = reach[p.m] || {})[p.t] = r ? { ok: true, hops: r.hops } : { ok: false };
      if (r) {
        okCount++;
        for (const c of r.newCables) {
          st.cables.push(c);
          st.byFrom.set(fromKey(c.from), c);
          st.toUsed.add(toKey(c.to));
        }
      } else failed.push(p);
    }
    return { cables: st.cables, reach, failed, okCount, pairs };
  }

  function search(ctx, st, mId, tId) {
    const mach = ctx.machines.get(mId), target = ctx.monitors.get(tId);
    if (!mach || !target) return null;
    const shared = (ctx.demand.get(tId) || 0) > 1;
    const open = [];
    const done = new Set();
    mach.outputs.forEach((sig, k) => open.push({ cost: 0, from: { t: 'm', id: mId, port: k }, sig, prev: null }));

    // the devices already on this path, so a path never loops through one
    const onPath = (s, devId) => {
      for (; s; s = s.prev) if (s.dev === devId) return true;
      return false;
    };
    const enter = (s, devId, inPort, sig, cost, cable) => {
      const d = ctx.devs.get(devId);
      if (onPath(s, devId)) return;
      d.ports.outs.forEach((o, j) => {
        let g = null, conv = false;
        if (d.hw.kind === 'switch') { if (takes(o.gives, sig)) g = sig; }
        else { g = o.gives[0]; conv = g !== sig; }
        if (!g) return;
        open.push({
          cost: cost + 0.2 + (conv ? 2 : 0) + (d.hw.planned ? 1.5 : 0), from: { t: 'd', id: devId, port: j }, sig: g, prev: s,
          dev: devId, inPort, outPort: j, inSig: sig, conv, cable,
        });
      });
    };

    while (open.length) {
      let bi = 0;
      for (let i = 1; i < open.length; i++) if (open[i].cost < open[bi].cost) bi = i;
      const s = open.splice(bi, 1)[0];
      if (s.goal) return finish(s);
      const key = fromKey(s.from) + '/' + s.sig;
      if (done.has(key)) continue;
      done.add(key);

      const existing = st.byFrom.get(fromKey(s.from));
      if (existing) {
        const to = existing.to;
        if (to.t === 't') {
          if (to.id === tId && takes(monitorAccepts(target, target.inputs[to.port]), s.sig))
            open.push({ cost: s.cost + 0.1, goal: true, prev: s, monInput: to.port, sig: s.sig, cable: null });
        } else {
          const d = ctx.devs.get(to.id);
          if (d && takes(d.ports.ins[to.port].accepts, s.sig)) enter(s, to.id, to.port, s.sig, s.cost + 0.1, null);
        }
        continue;
      }
      // a new cable from this port: to the monitor, or into any free device input
      target.inputs.forEach((inp, i) => {
        const to = { t: 't', id: tId, port: i };
        if (st.toUsed.has(toKey(to)) || !takes(monitorAccepts(target, inp), s.sig)) return;
        // a monitor several machines want is better fed from a switch, which
        // keeps this port free for the others
        const c = !shared ? 1 : s.from.t === 'm' ? 5 : 3;
        open.push({ cost: s.cost + c + (inp === s.sig ? 0 : 0.3), goal: true, prev: s, monInput: i, sig: s.sig,
                    cable: { from: s.from, to, sig: s.sig } });
      });
      for (const [devId, d] of ctx.devs) {
        d.ports.ins.forEach((inp, i) => {
          const to = { t: 'd', id: devId, port: i };
          if (st.toUsed.has(toKey(to)) || !takes(inp.accepts, s.sig)) return;
          enter(s, devId, i, s.sig, s.cost + 1, { from: s.from, to, sig: s.sig });
        });
      }
    }
    return null;

    function finish(goal) {
      const newCables = [], hops = [];
      for (let s = goal; s; s = s.prev) {
        if (s.cable) newCables.unshift(s.cable);
        if (s.goal) hops.unshift({ monitor: tId, input: s.monInput, sig: s.sig });
        else if (s.dev) hops.unshift({ dev: s.dev, inPort: s.inPort, outPort: s.outPort, sig: s.inSig, outSig: s.sig, conv: s.conv });
        else hops.unshift({ machine: mId, port: s.from.port, sig: s.sig });
      }
      return { newCables, hops };
    }
  }

  // ---- suggestions ----------------------------------------------------------

  function okSet(r) {
    const s = new Set();
    for (const m in r.reach) for (const t in r.reach[m]) if (r.reach[m][t].ok) s.add(m + '|' + t);
    return s;
  }

  function signalsInPlay(plan) {
    const s = new Set();
    for (const m of plan.machines) m.outputs.forEach(x => s.add(x));
    for (const h of plan.hardware) for (const o of portsOf(h).outs) o.gives.forEach(x => s.add(x));
    return s;
  }

  /* Add one kind of box at a time, keep the one that lets the most pairs
   * through (the smaller step when two help equally), and repeat. */
  function suggest(plan, maxSteps) {
    maxSteps = maxSteps || 6;
    let cur = clone(plan);
    let curR = route(cur);
    const steps = [];
    for (let n = 0; n < maxSteps && curR.failed.length; n++) {
      const inPlay = signalsInPlay(cur);
      let best = null;
      for (const c of CAT) {
        if (!c.suggest) continue;
        const ins = c.kind === 'switch' ? c.accepts : c.ins.flatMap(j => j.accepts);
        if (!ins.some(s => inPlay.has(s) || (s === 'hdmi' && inPlay.has('dvi')) || (s === 'dvi' && inPlay.has('hdmi')))) continue;
        const trial = clone(cur);
        const hw = hardwareFromCatalogue(c.id, { planned: true, name: c.name });
        trial.hardware.push(hw);
        const r = route(trial);
        const gain = r.okCount - curR.okCount;
        if (gain > 0 && (!best || gain > best.gain || (gain === best.gain && c.weight < best.cat.weight)))
          best = { gain, cat: c, trial, r, hw };
      }
      if (!best) break;
      const before = okSet(curR), after = okSet(best.r);
      const unlocks = [...after].filter(k => !before.has(k)).map(k => { const [m, t] = k.split('|'); return { m, t }; });
      steps.push({ item: best.cat, hardwareId: best.hw.id, unlocks, reason: explain(cur, curR, best, unlocks) });
      cur = best.trial;
      curR = best.r;
    }
    return { steps, plan: cur, routing: curR };
  }

  function names(list) {
    if (list.length <= 1) return list.join('');
    return list.slice(0, -1).join(', ') + ' and ' + list[list.length - 1];
  }

  function sigList(sigs) { return names([...new Set(sigs)].map(s => SIGNALS[s].says)); }

  /* The reason for a suggestion, in plain words, from what failed before */
  function explain(plan, before, best, unlocks) {
    const M = new Map(plan.machines.map(m => [m.id, m]));
    const T = new Map(plan.monitors.map(t => [t.id, t]));
    const need = best.cat.need.replace(/ \(.*\)$/, '');
    const bySignal = new Map(), byPorts = new Map();
    for (const u of unlocks) {
      const m = M.get(u.m), t = T.get(u.t);
      const native = t.inputs.some(inp => m.outputs.some(s => takes(monitorAccepts(t, inp), s)));
      if (native) { if (!byPorts.has(u.t)) byPorts.set(u.t, []); byPorts.get(u.t).push(u.m); }
      else { if (!bySignal.has(u.m)) bySignal.set(u.m, []); bySignal.get(u.m).push(u.t); }
    }
    const parts = [];
    for (const [mId, tIds] of bySignal) {
      const m = M.get(mId);
      const got = Object.entries(before.reach[mId] || {}).filter(([, v]) => v.ok).map(([t]) => T.get(t).name);
      const targets = tIds.map(t => T.get(t).name);
      // what the new box does on the way, from the route it made possible
      const r = best.r.reach[mId] && best.r.reach[mId][tIds[0]];
      const hop = r && r.ok && r.hops.find(h => h.dev === best.hw.id);
      const what = hop && hop.conv ? `its ${SIGNALS[hop.sig].says} turned into ${SIGNALS[hop.outSig].says}, which ${need} does`
        : need;
      const verb = targets.length > 1 ? 'need' : 'needs';
      if (got.length)
        parts.push(`${m.name} sends ${sigList(m.outputs)}; ${names(got)} can show it, but ${names(targets)} ${verb} ${what}.`);
      else
        parts.push(`${m.name} sends ${sigList(m.outputs)}, which ${names(targets)} cannot take: ${targets.length > 1 ? 'they need' : 'it needs'} ${what}.`);
    }
    for (const [tId, mIds] of byPorts) {
      const t = T.get(tId);
      const wanting = plan.machines.filter(m => (m.wants === null || m.wants.includes(tId)));
      const fits = mIds.map(m => M.get(m));
      const kinds = [...new Set(t.inputs.filter(inp => fits.some(m => m.outputs.some(s => takes(monitorAccepts(t, inp), s)))))];
      const count = t.inputs.filter(inp => kinds.includes(inp)).length;
      parts.push(`${t.name} has ${count === 1 ? 'one' : count} ${names(kinds.map(k => SIGNALS[k].label))} input${count === 1 ? '' : 's'}` +
        ` and more machines want it (${names(wanting.map(m => m.name))}): ${need} lets them share.`);
    }
    return parts.join(' ');
  }

  /* Why a pair still cannot be done */
  function diagnose(plan, routing, mId, tId) {
    const m = plan.machines.find(x => x.id === mId), t = plan.monitors.find(x => x.id === tId);
    if (!m.outputs.length) return `${m.name} has no video outputs in the plan yet.`;
    if (!t.inputs.length) return `${t.name} has no inputs in the plan yet.`;
    const native = t.inputs.some(inp => m.outputs.some(s => takes(monitorAccepts(t, inp), s)));
    if (native) return `${t.name}'s matching inputs are all in use, and nothing in the plan lets ${m.name} share one.`;
    const tIns = names([...new Set(t.inputs.flatMap(i => monitorAccepts(t, i)))].map(s => SIGNALS[s].says));
    let hint = '';
    if (t.inputs.every(i => i === 'vga') && m.outputs.every(s => ['composite', 'svideo', 'component', 'rgb'].includes(s)))
      hint = ' A PC monitor cannot show 15 kHz console video; a TV or a scaler into an HDMI screen can.';
    return `${m.name} sends ${sigList(m.outputs)}; ${t.name} takes ${tIns}, and no common box joins the two well.${hint}`;
  }

  /* Things worth knowing that are not missing hardware */
  function notes(plan, cables) {
    const out = [];
    const H = new Map(plan.hardware.map(h => [h.id, h]));
    for (const t of plan.monitors) {
      const feeds = cables.filter(c => c.to.t === 't' && c.to.id === t.id);
      if (feeds.length > t.atOnce)
        out.push(`${t.name} has ${feeds.length} cables in: you pick its input with its buttons or remote` +
          (t.kind === 'monitor' ? ' (or DDC/CI, which the hub can do for many monitors: ddc = in the config).' : '.'));
    }
    const breakout = new Set();
    for (const c of cables) {
      if (c.to.t !== 'd') continue;
      const h = H.get(c.to.id);
      if (h && h.catalog === 'extron-mvx88' && !['vga', 'sog'].includes(c.sig)) breakout.add(c.sig);
    }
    if (breakout.size) out.push(`The Extron's jacks are VGA: ${sigList([...breakout])} need${breakout.size === 1 ? 's' : ''} breakout cables (VGA to BNC or RCA).`);
    if (cables.some(c => c.sig === 'dvi') || cables.some(c => c.sig === 'hdmi' && isDviPort(plan, H, c.to)))
      out.push('DVI and HDMI meet with a plain adapter cable (picture only, no sound).');
    const manual = plan.hardware.filter(h => h.driver === 'manual' && cables.some(c => c.to.id === h.id));
    if (manual.length)
      out.push(`${names(manual.map(h => h.name))} ${manual.length > 1 ? 'are' : 'is'} switched by hand or remote; the hub knows which machine is plugged in when you tell it (plug on the panel).`);
    return out;
  }

  function isDviPort(plan, H, to) {
    if (to.t === 't') { const t = plan.monitors.find(x => x.id === to.id); return t && t.inputs[to.port] === 'dvi'; }
    return false;
  }

  /* Everything the page shows */
  function analyse(plan) {
    plan = normalizePlan(plan);
    const now = route(plan);
    const sug = suggest(plan);
    const missing = sug.routing.failed.map(f => ({ m: f.m, t: f.t, reason: diagnose(sug.plan, sug.routing, f.m, f.t) }));
    return {
      plan, now, suggestions: sug.steps, withSuggestions: sug.plan, routing: sug.routing, missing,
      notes: notes(sug.plan, sug.routing.cables), pairs: now.pairs.length,
    };
  }

  // ---- descriptions --------------------------------------------------------

  function portName(plan, end, side) {
    if (end.t === 'm') {
      const m = plan.machines.find(x => x.id === end.id);
      return { owner: m.name, port: SIGNALS[m.outputs[end.port]].label };
    }
    if (end.t === 't') {
      const t = plan.monitors.find(x => x.id === end.id);
      const same = t.inputs.filter(i => i === t.inputs[end.port]).length;
      const nth = t.inputs.slice(0, end.port + 1).filter(i => i === t.inputs[end.port]).length;
      return { owner: t.name, port: SIGNALS[t.inputs[end.port]].label + (same > 1 ? ' ' + nth : '') };
    }
    const h = plan.hardware.find(x => x.id === end.id);
    const ports = portsOf(h)[side === 'from' ? 'outs' : 'ins'];
    return { owner: h.name, port: ports[end.port].name };
  }

  /* The cables, one line each, for people with cables in their hands */
  function wiringList(plan, cables) {
    return cables.map(c => {
      const a = portName(plan, c.from, 'from'), b = portName(plan, c.to, 'to');
      return `${a.owner} ${a.port}  ->  ${b.owner} ${b.port}   (${SIGNALS[c.sig].label})`;
    });
  }

  function describeHops(plan, hops) {
    const parts = [];
    for (const h of hops) {
      if (h.machine) parts.push(plan.machines.find(x => x.id === h.machine).name + ' (' + SIGNALS[h.sig].says + ')');
      else if (h.dev) {
        const d = plan.hardware.find(x => x.id === h.dev);
        parts.push(d.name + (h.conv ? ' (to ' + SIGNALS[h.outSig].says + ')' : ''));
      } else parts.push(plan.monitors.find(x => x.id === h.monitor).name);
    }
    return parts.join(' → ');
  }

  // ---- the config ----------------------------------------------------------

  function slugger() {
    const used = new Set(['hub', 'claude', 'main']);
    return function (name, fallback) {
      let s = String(name).toLowerCase().normalize('NFKD').replace(/[̀-ͯ]/g, '')
        .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 20).replace(/-+$/, '');
      if (!s) s = fallback;
      if (/^[0-9]/.test(s)) s = fallback[0] + s;
      let v = s, n = 2;
      while (used.has(v)) v = s + n++;
      used.add(v);
      return v;
    };
  }

  // a label goes after "label =": no comment characters, and short
  const cleanLabel = s => String(s).replace(/[#;\r\n]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 40);

  /*
   * The hub sees only what it can drive: switchers with driver extron-sis
   * or text are [device]s; a box it cannot set (a scaler, a hand switch)
   * is either looked through, when a switcher feeds it, or a [screen] with
   * "agent = no" that the machines plugged into it name with "via =".
   */
  function makeConfig(plan, cables, opts) {
    opts = opts || {};
    plan = normalizePlan(plan);
    const slug = slugger();
    const H = new Map(plan.hardware.map(h => [h.id, h]));
    const driven = h => h && h.kind === 'switch' && h.driver !== 'manual';
    const used = new Set(cables.flatMap(c => [c.from.t + c.from.id, c.to.t + c.to.id]));

    const devName = new Map(), scrName = new Map(), monName = new Map();
    // a box still called what the catalogue calls it gets its short name (mvx, matrix ...)
    const short = h => {
      const c = CAT.find(x => x.id === h.catalog);
      return c && c.short && c.name === h.name ? c.short : h.name;
    };
    for (const h of plan.hardware) if (driven(h)) devName.set(h.id, slug(short(h), 'switcher'));
    for (const m of plan.machines) scrName.set(m.id, slug(m.name, 'machine'));

    // boxes the hub cannot set, that machines (or such boxes) plug into
    const viaBox = new Set();
    for (let changed = true; changed;) {
      changed = false;
      for (const c of cables) {
        if (c.to.t !== 'd' || driven(H.get(c.to.id)) || viaBox.has(c.to.id)) continue;
        if (c.from.t === 'm' || (c.from.t === 'd' && viaBox.has(c.from.id))) { viaBox.add(c.to.id); changed = true; }
      }
    }
    for (const id of viaBox) scrName.set(id, slug(short(H.get(id)), 'box'));
    for (const t of plan.monitors) monName.set(t.id, slug(t.name, 'monitor'));

    const outsOf = (t, id) => cables.filter(c => c.from.t === t && c.from.id === id);
    /* where a cable ends up for the hub: a switcher input, a monitor, or a
     * box that is a screen; looking through other boxes on the way */
    function exits(c, through, seen) {
      seen = seen || new Set();
      if (c.to.t === 't') return [{ kind: 'mon', id: c.to.id, through }];
      const h = H.get(c.to.id);
      if (driven(h)) return [{ kind: 'dev', id: h.id, port: c.to.port + 1, through }];
      if (viaBox.has(h.id) && c.from.t !== 'd') return [{ kind: 'box', id: h.id, through }];
      if (seen.has(h.id)) return [];
      seen.add(h.id);
      return outsOf('d', h.id).flatMap(n => exits(n, through.concat(h.name), seen));
    }

    const L = [];
    const say = s => L.push(s);
    say('# RetroKM hub configuration, from the setup planner (planner/index.html).');
    say('# A starting point: check the ports (port =) and the [hotkeys], then');
    say('#   ./hub/retrokm-hub -c retrokm.conf');
    say('# retrokm.conf.example explains every option.');
    if (plan.name) say('# Plan: ' + String(plan.name).replace(/[\r\n]/g, ' ').slice(0, 100));
    say('');
    say('[hub]');
    say('device = auto          # every keyboard and mouse');
    say('on_tie = follow        # control follows what the switcher shows');
    say('');

    // ---- devices
    const drivenList = plan.hardware.filter(driven);
    let serialN = 0, tcpN = 50;
    if (drivenList.length) {
      say('# ---- switchers the hub drives -------------------------------------');
      for (const h of drivenList) {
        say('');
        say(`[device ${devName.get(h.id)}]` + (h.planned ? '          # suggested: not bought yet' : ''));
        say('# ' + cleanLabel(h.name));
        if (h.deviceFile && h.deviceFile !== 'template-text') {
          say(`model = ${h.deviceFile}` + (h.catalog ? '' : `          # put ${h.deviceFile}.conf in devices/ or next to this file`));
        } else if (h.driver === 'text') {
          say('model = template-text  # the commands there are examples: copy it and fill in your box\'s');
        } else {
          say('driver = ' + h.driver);
        }
        if (h.driver === 'text') say(`port = tcp:192.168.1.${tcpN++}:23   # its address, or a serial port`);
        else say(`port = /dev/ttyUSB${serialN++}        # better: its /dev/serial/by-id/... name`);
        const fromFile = (h.catalog === 'extron-mvx88');
        if (!fromFile) { say('inputs = ' + h.ins); say('outputs = ' + h.outs); }
      }
      say('');
    }

    // ---- wires and monitor feeds
    const wireLines = [], wiredTo = new Map();
    const monFeeds = new Map(plan.monitors.map(t => [t.id, { video: [], direct: [] }]));
    for (const h of drivenList) {
      for (const c of outsOf('d', h.id)) {
        const port = `${devName.get(h.id)}.out${c.from.port + 1}`;
        for (const x of exits(c, [])) {
          const thru = x.through.length ? '   # through ' + names(x.through.map(cleanLabel)) : '';
          if (x.kind === 'dev') {
            const to = `${devName.get(x.id)}.in${x.port}`;
            if (wiredTo.has(to)) wireLines.push(`#${port} = ${to}${thru}  (a box picks one: ${wiredTo.get(to)} is the main one)`);
            else { wiredTo.set(to, port); wireLines.push(`${port} = ${to}${thru}`); }
          } else if (x.kind === 'mon') monFeeds.get(x.id).video.push({ port, thru });
          // (a switcher into a box that is a [screen]: the box's own "video =" covers it)
        }
      }
    }
    if (wireLines.length) {
      say('# ---- cables between switchers -------------------------------------');
      say('[wire]');
      wireLines.forEach(say);
      say('');
    }

    // ---- screens: machines, then boxes they plug into
    say('# ---- machines --------------------------------------------------------');
    const hotkeys = [];
    let gotoN = 1, showN = 1;
    function screenLines(kind, id) {
      const lines = [];
      const starts = kind === 'm' ? outsOf('m', id) : outsOf('d', id);
      let video = null, via = null;
      const extra = [];
      for (const c of starts) {
        for (const x of exits(c, [])) {
          const thru = x.through.length ? '   # through ' + names(x.through.map(cleanLabel)) : '';
          if (x.kind === 'dev' && !video && !via) video = `${devName.get(x.id)}.in${x.port}${thru}`;
          else if (x.kind === 'box' && !video && !via) via = scrName.get(x.id);
          else if (x.kind === 'mon') monFeeds.get(x.id).direct.push(scrName.get(id));
          else if (x.kind === 'dev') extra.push(`${devName.get(x.id)}.in${x.port}`);
          else if (x.kind === 'box') extra.push('the ' + H.get(x.id).name);
        }
      }
      if (video) lines.push('video = ' + video);
      if (via) lines.push('via = ' + via + '          # plugged into it by hand (plug on the panel)');
      if (extra.length) lines.push('# also cabled to ' + names(extra) + ' (the hub uses the first)');
      return { lines, video: !!video, via: !!via };
    }
    for (const m of plan.machines) {
      const name = scrName.get(m.id);
      say('');
      say(`[screen ${name}]`);
      say('label = ' + cleanLabel(m.name));
      if (m.hub) say('local = yes            # the hub runs on this machine');
      if (!m.agent && !m.hub) say('agent = no             # video only, no keyboard');
      const s = screenLines('m', m.id);
      s.lines.forEach(say);
      if (!s.video && !s.via && !used.has('m' + m.id)) say('# not cabled to anything in the plan yet');
      if (m.agent || m.hub) hotkeys.push(`ctrl+alt+${gotoN++} = goto ${name}`);
      if (s.video && showN <= 12) hotkeys.push(`ctrl+alt+f${showN++} = show ${name}`);
    }
    for (const id of viaBox) {
      const h = H.get(id);
      say('');
      say(`[screen ${scrName.get(id)}]` + (h.planned ? '          # suggested: not bought yet' : ''));
      say('label = ' + cleanLabel(h.name));
      say('agent = no             # a box the hub cannot set: its machines say via = ' + scrName.get(id));
      screenLines('d', id).lines.forEach(say);
    }
    say('');

    // ---- monitors
    say('# ---- monitors, left to right ----------------------------------------');
    plan.monitors.forEach((t, i) => {
      const f = monFeeds.get(t.id);
      const direct = [...new Set(f.direct)];
      say('');
      if (!f.video.length && !direct.length) {
        say(`# [monitor ${monName.get(t.id)}]   ${cleanLabel(t.name)}: nothing reaches it in this plan yet`);
        return;
      }
      say(`[monitor ${monName.get(t.id)}]`);
      say('label = ' + cleanLabel(t.name));
      say(`pos = ${i},0`);
      if (t.kind === 'capture') say('capture = yes          # on the panel, never part of the desk');
      if (f.video.length) {
        say('video = ' + f.video[0].port + f.video[0].thru);
        if (direct.length) say('shared = ' + direct[0] + '          # its other input: pick it on the panel');
      } else say('screen = ' + direct[0]);
      const more = f.video.slice(1).map(v => v.port).concat(direct.slice(1));
      if (more.length) say('# also cabled: ' + names(more) + ' (pick those on the monitor itself)');
    });
    say('');

    say('# ---- hotkeys -----------------------------------------------------------');
    say('[hotkeys]');
    hotkeys.forEach(say);
    say('ctrl+alt+l = lock');
    return L.join('\n') + '\n';
  }

  const api = {
    SIGNALS, SIGNAL_KEYS, MACHINE_PRESETS, MONITOR_PRESETS, CATALOGUE: CAT,
    takes, normalizePlan, normalizeHardware, hardwareFromCatalogue, parseDeviceFile, portsOf,
    wantedPairs, route, suggest, analyse, diagnose, wiringList, describeHops, makeConfig, newId,
  };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.RetroKMPlanner = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
