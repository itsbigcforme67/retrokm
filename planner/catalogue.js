/* RetroKM planner: the kinds of hardware the planner knows about.
 *
 * Each entry is either something you can say you own, or a kind of box the
 * planner can suggest (suggest: true), or both.  Entries with a deviceFile
 * match a file in devices/ (the hub reads those too); test.mjs checks that
 * their ports and signals agree with the file.
 *
 * Signals: composite, svideo, component, rgb (15 kHz RGB, usually SCART),
 * vga, sog (VGA with sync on green), dvi, hdmi.
 *
 * A switch passes a signal from any input to any output that can carry it.
 * A converter turns what goes into one of its jacks into what comes out.
 * short: its name in a generated retrokm.conf.
 * driver: extron-sis or text = the hub can set it; manual = buttons, a
 * remote or moving plugs (the hub cannot set it yet).
 *
 * weight: which to suggest first when two help equally: lower first.
 * Roughly how big a step buying one is (1 a small adapter, 6 a premium
 * scaler), except that a cheap AV converter comes after the retro scalers
 * because it looks so much worse.  Not a price.
 *
 * Plain classic script (no modules) so the page works when opened from disk.
 */
(function (root) {
  'use strict';

  const ANALOG_AV = ['composite', 'svideo', 'component'];

  const CATALOGUE = [
    // ---- switchers -------------------------------------------------------
    {
      id: 'extron-mvx88', short: 'mvx', name: 'Extron MVX 88 VGA A', category: 'VGA matrix switcher',
      kind: 'switch', driver: 'extron-sis', deviceFile: 'extron-mvx88',
      ins: 8, outs: 8, accepts: ['vga', 'sog', 'rgb', 'component', 'svideo', 'composite'],
      need: 'a VGA matrix switcher (any input to any output, the hub drives it over RS-232)',
      examples: ['Extron MVX 88 VGA A', 'Extron CrossPoint 300', 'Kramer VP-series VGA matrices'],
      notes: 'Its jacks are 15-pin VGA; composite, S-Video, component and SCART RGB need breakout cables.',
      weight: 3, suggest: true,
    },
    {
      id: 'hdmi-matrix-4x4', short: 'matrix', name: 'HDMI matrix 4x4 (RS-232 or network)', category: 'HDMI matrix switcher',
      kind: 'switch', driver: 'text', deviceFile: 'template-text',
      ins: 4, outs: 4, accepts: ['hdmi', 'dvi'],
      need: 'an HDMI matrix (several HDMI machines to several screens, the hub drives it)',
      examples: ['OREI and AV Access 4x4 HDMI matrices', 'any HDMI matrix with an RS-232 or network port'],
      notes: 'Pick one with RS-232 or a network (telnet) port so the hub can drive it; its commands go in a device file.',
      weight: 3, suggest: true,
    },
    {
      id: 'hdmi-matrix-8x8', short: 'matrix', name: 'HDMI matrix 8x8 (RS-232 or network)', category: 'HDMI matrix switcher',
      kind: 'switch', driver: 'text', deviceFile: 'template-text',
      ins: 8, outs: 8, accepts: ['hdmi', 'dvi'],
      need: 'a large HDMI matrix',
      examples: ['8x8 HDMI matrices with RS-232 or a network port'],
      notes: 'Its commands go in a device file (copy devices/template-text.conf).',
      weight: 4, suggest: false,
    },
    {
      id: 'hdmi-switch-4x1', short: 'hdmi-switch', name: 'HDMI switch 4 in, 1 out', category: 'HDMI switch',
      kind: 'switch', driver: 'manual', ins: 4, outs: 1, accepts: ['hdmi', 'dvi'],
      need: 'an HDMI switch (several HDMI machines share one screen input)',
      examples: ['any 4-port HDMI switch with a remote'],
      notes: 'Switched with its button or remote; the hub cannot set it yet.',
      weight: 1, suggest: true,
    },
    {
      id: 'vga-switch-4x1', short: 'vga-switch', name: 'VGA switch 4 in, 1 out', category: 'VGA switch',
      kind: 'switch', driver: 'manual', ins: 4, outs: 1, accepts: ['vga'],
      need: 'a VGA switch (several VGA machines share one monitor input)',
      examples: ['a 4-port VGA switch or VGA KVM switch'],
      notes: 'Cheap ones often blur high resolutions and may not pass sync-on-green.',
      weight: 1, suggest: true,
    },
    {
      id: 'scart-switch', short: 'scart-switch', name: 'SCART switch 8 in, 1 out', category: 'SCART RGB switch',
      kind: 'switch', driver: 'manual', ins: 8, outs: 1, accepts: ['rgb', 'composite'],
      need: 'a SCART switch (several RGB consoles share one input)',
      examples: ['gscartsw', 'Extron MPS 112 for mixed signals'],
      notes: 'Automatic SCART switches pick whichever console is turned on.',
      weight: 2, suggest: true,
    },
    {
      id: 'av-switch', short: 'av-switch', name: 'Composite / S-Video / component switch', category: 'AV switch',
      kind: 'switch', driver: 'manual', ins: 4, outs: 1, accepts: ANALOG_AV,
      need: 'an AV switch (several consoles share one composite, S-Video or component input)',
      examples: ['Extron MPS 112 (several small switchers in one box)', 'a 4-way component / AV switch'],
      notes: 'Each signal has its own jacks; one console per input.',
      weight: 1, suggest: true,
    },

    // ---- scalers and converters ------------------------------------------
    {
      id: 'retrotink-5x', short: 'tink', name: 'RetroTINK 5X Pro', category: 'Retro scaler (to HDMI)',
      kind: 'converter', driver: 'manual',
      ins: [
        { name: 'scart', accepts: ['rgb'] },
        { name: 'component', accepts: ['component', 'composite'] },   // the green jack takes composite
        { name: 'svideo', accepts: ['svideo'] },
      ],
      outs: [{ name: 'hdmi', gives: 'hdmi' }],
      need: 'a retro scaler (consoles to HDMI, with low lag and good 240p)',
      examples: ['RetroTINK 5X Pro', 'RetroTINK 4K'],
      notes: 'One picture at a time: whichever jack is selected.',
      weight: 3, suggest: true,
    },
    {
      id: 'ossc', short: 'ossc', name: 'OSSC (Open Source Scan Converter)', category: 'Scaler that accepts sync-on-green',
      kind: 'converter', driver: 'manual',
      ins: [
        { name: 'scart', accepts: ['rgb'] },
        { name: 'component', accepts: ['component'] },
        { name: 'vga', accepts: ['vga', 'sog'] },
      ],
      outs: [{ name: 'hdmi', gives: 'hdmi' }],
      need: 'a scaler that accepts RGB and sync-on-green (workstations and consoles to HDMI)',
      examples: ['OSSC', 'RetroTINK 4K'],
      notes: 'No composite or S-Video. Line-doubles with almost no lag.',
      weight: 3.5, suggest: true,
    },
    {
      id: 'retrotink-4k', short: 'tink4k', name: 'RetroTINK 4K', category: 'Scaler that accepts everything analog',
      kind: 'converter', driver: 'manual',
      ins: [
        { name: 'scart', accepts: ['rgb'] },
        { name: 'component', accepts: ['component', 'composite'] },
        { name: 'svideo', accepts: ['svideo'] },
        { name: 'vga', accepts: ['vga', 'sog'] },
      ],
      outs: [{ name: 'hdmi', gives: 'hdmi' }],
      need: 'a scaler that takes every analog signal, sync-on-green included',
      examples: ['RetroTINK 4K'],
      notes: 'One picture at a time.',
      weight: 6, suggest: true,
    },
    {
      id: 'av-to-hdmi', short: 'av2hdmi', name: 'Composite / S-Video to HDMI converter', category: 'Basic AV to HDMI converter',
      kind: 'converter', driver: 'manual',
      ins: [{ name: 'composite', accepts: ['composite'] }, { name: 'svideo', accepts: ['svideo'] }],
      outs: [{ name: 'hdmi', gives: 'hdmi' }],
      need: 'a basic AV to HDMI converter',
      examples: ['any small composite / S-Video to HDMI box'],
      notes: 'Cheap, but adds lag and often mangles 240p games; a retro scaler looks much better.',
      weight: 5, suggest: true,
    },
    {
      id: 'vga-to-hdmi', short: 'vga2hdmi', name: 'VGA to HDMI converter', category: 'VGA to HDMI converter',
      kind: 'converter', driver: 'manual',
      ins: [{ name: 'vga', accepts: ['vga'] }],
      outs: [{ name: 'hdmi', gives: 'hdmi' }],
      need: 'a VGA to HDMI converter',
      examples: ['any powered VGA to HDMI adapter'],
      notes: 'Fine for PC resolutions; most do not take sync-on-green or 15 kHz.',
      weight: 1, suggest: true,
    },
    {
      id: 'hdmi-to-vga', short: 'hdmi2vga', name: 'HDMI to VGA adapter', category: 'HDMI to VGA adapter',
      kind: 'converter', driver: 'manual',
      ins: [{ name: 'hdmi', accepts: ['hdmi', 'dvi'] }],
      outs: [{ name: 'vga', gives: 'vga' }],
      need: 'an HDMI to VGA adapter (a modern machine on a VGA monitor or CRT)',
      examples: ['any active HDMI to VGA adapter'],
      notes: 'Set the machine to a resolution the CRT can show.',
      weight: 1, suggest: true,
    },
    {
      id: 'sog-splitter', short: 'sog-splitter', name: 'Sync-on-green separator', category: 'Sync-on-green separator',
      kind: 'converter', driver: 'manual',
      ins: [{ name: 'in', accepts: ['sog'] }],
      outs: [{ name: 'out', gives: 'vga' }],
      need: 'a sync-on-green separator (turns SOG into ordinary VGA for monitors that cannot sync to it)',
      examples: ['SOG to RGBHV sync separator boards from retro hobby shops'],
      notes: 'Many LCDs show no picture from sync-on-green without one.',
      weight: 1, suggest: true,
    },
    {
      id: 'rgb-to-component', short: 'transcoder', name: 'RGB to component transcoder', category: 'RGB to component transcoder',
      kind: 'converter', driver: 'manual',
      ins: [{ name: 'scart', accepts: ['rgb'] }],
      outs: [{ name: 'component', gives: 'component' }],
      need: 'an RGB to component transcoder (SCART consoles on a TV with only component)',
      examples: ['any SCART RGB to YPbPr transcoder'],
      notes: 'Keeps the picture analog: no lag.',
      weight: 2, suggest: true,
    },
  ];

  const api = { CATALOGUE };
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.RetroKMCatalogue = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
