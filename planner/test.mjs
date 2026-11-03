// Tests for the planner's logic: node planner/test.mjs
import { createRequire } from 'node:module';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';

const require = createRequire(import.meta.url);
const P = require('./logic.js');
const here = path.dirname(fileURLToPath(import.meta.url));
const devicesDir = path.join(here, '..', 'devices');

let passed = 0, failed = 0;
function test(name, fn) {
  try { fn(); passed++; console.log('PASS ' + name); }
  catch (e) { failed++; console.log('FAIL ' + name + '\n     ' + String(e.message).split('\n').join('\n     ')); }
}
const example = name => JSON.parse(readFileSync(path.join(here, 'examples', name), 'utf8'));
const pair = (r, m, t) => r.reach[m] && r.reach[m][t] && r.reach[m][t].ok;
let n = 0;
const machine = (name, outputs, wants, extra) => Object.assign({ id: 'm' + (++n), name, kind: 'computer', outputs, agent: true, wants }, extra);
const monitor = (id, name, inputs, extra) => Object.assign({ id, name, kind: 'monitor', inputs, sog: false, atOnce: 1 }, extra);
const owned = id => P.hardwareFromCatalogue(id);

/* A small reader for the generated config: sections in order, keys with
 * comments stripped the way the hub strips them. */
function readConf(text) {
  const secs = [];
  let cur = null;
  text.split('\n').forEach((raw, i) => {
    let line = raw.trim();
    if (!line || line[0] === '#' || line[0] === ';') return;
    const cut = line.search(/\s[#;]/);
    if (cut >= 0) line = line.slice(0, cut).trim();
    const sec = line.match(/^\[(\w+)(?:\s+(.+))?\]$/);
    if (sec) { secs.push(cur = { kind: sec[1], name: sec[2] || '', keys: [] }); return; }
    const eq = line.indexOf('=');
    assert.ok(eq > 0 && cur, `line ${i + 1} is not key = value inside a section: ${raw}`);
    cur.keys.push([line.slice(0, eq).trim(), line.slice(eq + 1).trim()]);
  });
  const get = (s, k) => (s.keys.find(x => x[0] === k) || [])[1];
  return { secs, get, of: kind => secs.filter(s => s.kind === kind) };
}

/* What the hub checks when it loads a config, done here too */
function checkConf(text) {
  const c = readConf(text);
  const devs = new Map();
  const allNames = new Set();
  for (const s of c.secs) {
    if (['device', 'screen', 'monitor'].includes(s.kind)) {
      assert.match(s.name, /^[a-z][a-z0-9-]{0,30}$/, 'a name the hub takes: ' + s.name);
      const key = s.kind + ':' + s.name;
      assert.ok(!allNames.has(key), 'names are unique: ' + key);
      allNames.add(key);
    }
  }
  for (const d of c.of('device')) {
    let ins = +c.get(d, 'inputs') || 0, outs = +c.get(d, 'outputs') || 0;
    const model = c.get(d, 'model');
    if (model) {
      const file = P.parseDeviceFile(readFileSync(path.join(devicesDir, model + '.conf'), 'utf8'), model);
      ins = ins || file.ins; outs = outs || file.outs;
    } else assert.ok(['extron-sis', 'text'].includes(c.get(d, 'driver')), 'a driver the hub has');
    for (const [k] of d.keys) assert.ok(['model', 'driver', 'port', 'baud', 'poll', 'inputs', 'outputs', 'tie', 'untie', 'read', 'reply', 'tie_command', 'read_command'].includes(k), 'device key the hub knows: ' + k);
    devs.set(d.name, { ins, outs });
  }
  const port = (v, side) => {
    const m = v.match(/^([a-z0-9-]+)\.(in|out)(\d+)$/);
    assert.ok(m, 'a port: ' + v);
    assert.equal(m[2], side, `${v} is an ${side === 'in' ? 'input' : 'output'}`);
    const d = devs.get(m[1]);
    assert.ok(d, 'a [device] called ' + m[1]);
    assert.ok(+m[3] >= 1 && +m[3] <= (side === 'in' ? d.ins : d.outs), 'port number in range: ' + v);
  };
  for (const w of c.of('wire')) for (const [a, b] of w.keys) { port(a, 'out'); port(b, 'in'); }
  const screens = new Set(c.of('screen').map(s => s.name));
  for (const s of c.of('screen')) {
    if (c.get(s, 'video')) port(c.get(s, 'video'), 'in');
    if (c.get(s, 'via')) assert.ok(screens.has(c.get(s, 'via')), 'via names a screen: ' + c.get(s, 'via'));
  }
  for (const m of c.of('monitor')) {
    const v = c.get(m, 'video'), sc = c.get(m, 'screen');
    assert.ok(v || sc, `monitor ${m.name} has video = or screen =`);
    if (v) port(v, 'out');
    if (sc) assert.ok(screens.has(sc), 'screen = names a screen');
    if (c.get(m, 'shared')) assert.ok(screens.has(c.get(m, 'shared')), 'shared = names a screen');
  }
  assert.ok(screens.size >= 1, 'at least one [screen]');
  return c;
}

// ---- the catalogue and the device files --------------------------------------

test('every catalogue entry becomes hardware', () => {
  const ids = new Set();
  for (const c of P.CATALOGUE) {
    assert.ok(!ids.has(c.id), 'unique id ' + c.id);
    ids.add(c.id);
    const hw = P.hardwareFromCatalogue(c.id);
    assert.ok(hw, c.id);
    assert.ok(c.need && c.examples.length, c.id + ' has a reason phrase and examples');
    assert.equal(hw.driver, c.driver);
  }
});

test('catalogue entries agree with their device files', () => {
  const files = readdirSync(devicesDir).filter(f => f.endsWith('.conf')).map(f => f.replace(/\.conf$/, ''));
  for (const c of P.CATALOGUE.filter(x => x.deviceFile)) {
    assert.ok(files.includes(c.deviceFile), `devices/${c.deviceFile}.conf exists`);
    const f = P.parseDeviceFile(readFileSync(path.join(devicesDir, c.deviceFile + '.conf'), 'utf8'), c.deviceFile + '.conf');
    assert.equal(f.driver, c.driver, c.id + ' driver');
    assert.equal(f.deviceFile, c.deviceFile);
    for (const s of f.accepts) assert.ok(c.accepts.includes(s), `${c.id} carries ${s} like its file says`);
    if (c.deviceFile !== 'template-text') {      // the template is any size
      assert.equal(f.ins, c.ins, c.id + ' inputs');
      assert.equal(f.outs, c.outs, c.id + ' outputs');
    }
  }
});

test('a device file reads as hardware', () => {
  const hw = P.parseDeviceFile('name = Box  # a comment\nsignal = hdmi\ninputs = 6\noutputs = 2\ntie = x{in}{out}\n', 'my-box.conf');
  assert.equal(hw.name, 'Box');
  assert.equal(hw.driver, 'text');
  assert.deepEqual([hw.ins, hw.outs], [6, 2]);
  assert.deepEqual(hw.accepts, ['hdmi', 'dvi']);
  assert.equal(hw.deviceFile, 'my-box');
});

// ---- signals and routing -----------------------------------------------------

test('DVI and HDMI meet; sync-on-green needs a monitor that takes it', () => {
  assert.ok(P.takes(['hdmi'], 'dvi') && P.takes(['dvi'], 'hdmi'));
  assert.ok(!P.takes(['vga'], 'sog'));
  const plan = P.normalizePlan({
    machines: [machine('Octane', ['sog'], ['a', 'b'])],
    monitors: [monitor('a', 'Plain LCD', ['vga']), monitor('b', 'Sony CRT', ['vga'], { sog: true })],
  });
  const r = P.route(plan);
  assert.ok(!pair(r, plan.machines[0].id, 'a'), 'not into a plain VGA input');
  assert.ok(pair(r, plan.machines[0].id, 'b'), 'into a VGA input that takes SOG');
});

test('owned hardware is used and cabled', () => {
  const plan = P.normalizePlan(example('retro-desk.json'));
  const r = P.route(plan);
  assert.ok(pair(r, 'pc', 'crt') && pair(r, 'pc', 'tall'), 'PC on both VGA monitors through the Extron');
  assert.ok(pair(r, 'ps2', 'tv') && pair(r, 'ps2', 'cap'), 'PS2 through the 5X and the matrix');
  assert.ok(!pair(r, 'octane', 'cap'), 'nothing owned turns SOG into HDMI');
  const lines = P.wiringList(plan, r.cables);
  assert.ok(lines.some(l => /^PlayStation 2 Component\s+->\s+RetroTINK 5X Pro component/.test(l)), lines.join('\n'));
  assert.ok(lines.some(l => /^RetroTINK 5X Pro hdmi\s+->\s+HDMI matrix/.test(l)), 'the 5X feeds the matrix, so it reaches both HDMI screens');
  // a port has one cable
  const tos = r.cables.map(c => c.to.t + c.to.id + c.to.port), froms = r.cables.map(c => c.from.t + c.from.id + c.from.port);
  assert.equal(new Set(tos).size, tos.length);
  assert.equal(new Set(froms).size, froms.length);
});

// ---- suggestions -------------------------------------------------------------

test('suggests a SOG scaler for the Octane, with the reason', () => {
  const a = P.analyse(example('retro-desk.json'));
  const first = a.suggestions[0];
  assert.ok(first, 'a suggestion');
  assert.ok(['ossc', 'retrotink-4k'].includes(first.item.id), 'a scaler that takes SOG, got ' + first.item.id);
  assert.match(first.reason, /SGI Octane sends sync-on-green VGA; Dell CRT can show it, but .*Capture card need/);
  assert.match(first.reason, /turned into HDMI/);
  assert.ok(first.unlocks.some(u => u.m === 'octane' && u.t === 'cap'));
  assert.equal(a.missing.length, 0, 'everything reachable after the suggestions: ' + JSON.stringify(a.missing));
  assert.ok(a.routing.okCount === a.pairs);
});

test('too few inputs: suggests a switch, and says so', () => {
  const plan = {
    machines: [machine('PC', ['vga'], ['crt']), machine('Quadra', ['vga'], ['crt']), machine('G4', ['vga'], ['crt'])],
    monitors: [monitor('crt', 'Dell CRT', ['vga'])],
  };
  const a = P.analyse(plan);
  assert.equal(a.now.okCount, 1, 'one machine gets the one input');
  assert.equal(a.suggestions.length, 1);
  assert.equal(a.suggestions[0].item.kind, 'switch');
  assert.ok(a.suggestions[0].item.accepts.includes('vga'));
  assert.match(a.suggestions[0].reason, /Dell CRT has one VGA input and more machines want it \(PC, Quadra and G4\)/);
  assert.equal(a.missing.length, 0);
});

test('a cheap fix wins when it does the same job', () => {
  const plan = {
    machines: [machine('Octane', ['sog'], ['lcd'])],
    monitors: [monitor('lcd', 'LCD', ['vga'])],
  };
  const a = P.analyse(plan);
  assert.equal(a.suggestions[0].item.id, 'sog-splitter');
  assert.match(a.suggestions[0].reason, /sync-on-green VGA turned into VGA/);
});

test('consoles to an HDMI TV get a retro scaler; several share it', () => {
  const plan = {
    machines: [machine('PS2', ['component'], ['tv'], { kind: 'console', agent: false }),
               machine('N64', ['svideo', 'composite'], ['tv'], { kind: 'console', agent: false })],
    monitors: [monitor('tv', 'TV', ['hdmi'], { kind: 'tv' })],
  };
  const a = P.analyse(plan);
  assert.equal(a.suggestions.length, 1, a.suggestions.map(s => s.item.id).join());
  assert.equal(a.suggestions[0].item.id, 'retrotink-5x');
  assert.equal(a.suggestions[0].unlocks.length, 2);
});

test('a box that can serve more of what is missing is picked on a tie', () => {
  // a VGA-to-HDMI adapter fixes PC -> capture as well, but the OSSC can also
  // take the Octane's sync-on-green, so with a switch in front it serves both
  const plan = {
    machines: [machine('Octane', ['sog'], ['crt', 'cap']), machine('PC', ['vga'], ['crt', 'cap'])],
    monitors: [monitor('crt', 'CRT', ['vga'], { sog: true }), monitor('cap', 'Capture', ['hdmi'], { kind: 'capture' })],
  };
  const a = P.analyse(plan);
  assert.equal(a.suggestions[0].item.id, 'ossc', a.suggestions.map(s => s.item.id).join());
  assert.equal(a.missing.length, 0, JSON.stringify(a.missing));
  assert.ok(a.suggestions.every(s => s.gain >= 1));
  assert.ok(!/lets them share/.test(a.suggestions[0].reason), 'the scaler is not credited with sharing: ' + a.suggestions[0].reason);
});

test('impossible pairs are explained', () => {
  const plan = {
    machines: [machine('PS2', ['component'], ['crt'], { kind: 'console' })],
    monitors: [monitor('crt', 'PC CRT', ['vga'])],
  };
  const a = P.analyse(plan);
  assert.equal(a.missing.length, 1);
  assert.match(a.missing[0].reason, /PS2 sends component; PC CRT takes VGA/);
  assert.match(a.missing[0].reason, /15 kHz/);
});

test('nothing wanted, nothing suggested', () => {
  const a = P.analyse({ machines: [machine('PC', ['vga'], [])], monitors: [monitor('a', 'A', ['vga'])] });
  assert.equal(a.pairs, 0);
  assert.equal(a.suggestions.length, 0);
});

// ---- the config --------------------------------------------------------------

test('config for the example desk', () => {
  const a = P.analyse(example('retro-desk.json'));
  const text = P.makeConfig(a.withSuggestions, a.routing.cables);
  const c = checkConf(text);
  const sec = (k, nm) => c.secs.find(s => s.kind === k && s.name === nm);
  assert.equal(c.get(sec('device', 'mvx'), 'model'), 'extron-mvx88');
  assert.equal(c.get(sec('device', 'matrix'), 'model'), 'template-text');
  assert.equal(c.get(sec('device', 'matrix'), 'inputs'), '4');
  assert.equal(c.get(sec('screen', 'sgi-octane'), 'video'), 'mvx.in1');
  assert.equal(c.get(sec('screen', 'playstation-2'), 'via'), 'tink');
  assert.equal(c.get(sec('screen', 'playstation-2'), 'agent'), 'no');
  assert.equal(c.get(sec('screen', 'laptop'), 'local'), 'yes');
  assert.match(c.get(sec('screen', 'tink'), 'video'), /^matrix\.in\d$/);
  assert.match(c.get(sec('monitor', 'capture-card'), 'video'), /^matrix\.out\d$/);
  assert.equal(c.get(sec('monitor', 'capture-card'), 'capture'), 'yes');
  // the scaler the Extron feeds is looked through: a wire from the Extron into the matrix
  const wires = c.of('wire').flatMap(w => w.keys);
  assert.ok(wires.some(([a2, b]) => /^mvx\.out\d$/.test(a2) && /^matrix\.in\d$/.test(b)), JSON.stringify(wires));
  assert.match(text, /# through OSSC/);
  assert.match(text, /^ctrl\+alt\+1 = goto laptop$/m);
});

test('config with only what is owned', () => {
  const a = P.analyse(example('retro-desk.json'));
  const text = P.makeConfig(a.plan, a.now.cables);
  checkConf(text);
  assert.ok(!/OSSC/.test(text));
});

test('config: a hand switch is a screen its machines go via; a direct cable is screen =', () => {
  const a = P.analyse(example('small-desk.json'));
  assert.equal(a.missing.length, 0);
  const c = checkConf(P.makeConfig(a.withSuggestions, a.routing.cables));
  const sec = (k, nm) => c.secs.find(s => s.kind === k && s.name === nm);
  assert.equal(c.get(sec('screen', 'laptop'), 'via'), 'hdmi-switch');
  assert.equal(c.get(sec('screen', 'hdmi-switch'), 'agent'), 'no');
  assert.equal(c.get(sec('monitor', 'tv'), 'screen'), 'hdmi-switch');
  assert.equal(c.get(sec('monitor', 'pc-crt'), 'screen'), 'dos-pc');
  assert.equal(c.of('device').length, 0, 'nothing the hub drives');
});

test('config names stay safe and unique', () => {
  const plan = {
    name: 'odd # names ; here',
    machines: [machine('PC #1', ['vga'], ['a']), machine('PC  1', ['vga'], ['a']), machine('9000', ['hdmi'], ['b']),
               machine('Ünïcode Mac', ['vga'], ['a']), machine('hub', ['hdmi'], ['b'])],
    monitors: [monitor('a', 'Left; CRT', ['vga']), monitor('b', 'TV', ['hdmi', 'hdmi'])],
    hardware: [owned('extron-mvx88')],
  };
  const a = P.analyse(plan);
  const text = P.makeConfig(a.withSuggestions, a.routing.cables);
  const c = checkConf(text);
  assert.deepEqual(c.of('screen').map(s => s.name).slice(0, 5), ['pc-1', 'pc-12', 'm9000', 'unicode-mac', 'hub2']);
  for (const s of c.of('screen')) assert.ok(!/[#;]/.test(c.get(s, 'label') || ''));
});

test('a monitor nothing reaches is left commented out', () => {
  const text = P.makeConfig({ machines: [machine('PC', ['vga'], [])], monitors: [monitor('a', 'Lonely TV', ['hdmi'])] }, []);
  checkConf(text);
  assert.match(text, /^# \[monitor lonely-tv\]/m);
});

// ---- plans as files ----------------------------------------------------------

test('a plan survives JSON and junk is ignored', () => {
  const plan = P.normalizePlan(example('retro-desk.json'));
  assert.deepEqual(P.normalizePlan(JSON.parse(JSON.stringify(plan))), plan);
  const junk = P.normalizePlan({ machines: [null, 3, { outputs: ['laser'] }], monitors: 'no', hardware: [{ kind: 'switch' }] });
  assert.equal(junk.machines.length, 1);
  assert.deepEqual(junk.machines[0].outputs, []);
  assert.equal(junk.monitors.length, 0);
  assert.equal(junk.hardware.length, 0);
  assert.deepEqual(P.normalizePlan(null).machines, []);
});

test('examples.js has the same plans as examples/*.json', () => {
  const embedded = require('./examples.js');
  const files = readdirSync(path.join(here, 'examples')).filter(f => f.endsWith('.json')).map(f => f.replace(/\.json$/, '')).sort();
  assert.deepEqual(Object.keys(embedded).sort(), files);
  for (const f of files) assert.deepEqual(embedded[f], example(f + '.json'), f);
});

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
