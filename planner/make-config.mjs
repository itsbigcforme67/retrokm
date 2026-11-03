#!/usr/bin/env node
// A retrokm.conf from a plan saved by the planner page:
//   node planner/make-config.mjs my-desk.json [--owned] > retrokm.conf
// --owned leaves out the suggested hardware (only what the plan already has).
import { createRequire } from 'node:module';
import { readFileSync } from 'node:fs';
const require = createRequire(import.meta.url);
const P = require('./logic.js');

const args = process.argv.slice(2);
const file = args.find(a => !a.startsWith('--'));
if (!file) {
  console.error('usage: node planner/make-config.mjs PLAN.json [--owned]');
  process.exit(2);
}
const a = P.analyse(JSON.parse(readFileSync(file, 'utf8')));
const owned = args.includes('--owned');
process.stdout.write(P.makeConfig(owned ? a.plan : a.withSuggestions, owned ? a.now.cables : a.routing.cables));
