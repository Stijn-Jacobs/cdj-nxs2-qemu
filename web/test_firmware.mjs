// SPDX-License-Identifier: GPL-2.0-or-later
// Run the browser's firmware preparation on a real update in Node and print,
// as JSON, the sha256 of every image, the digests the module expects and how
// long the unpack took. tests/test_web_firmware.py compares them with the
// profile the Python setup verifies against.
//
//   usage: node web/test_firmware.mjs <C2KNXS2.UPD>
import { readFileSync } from 'node:fs';
import { EXPECTED, UPD_SHA256, sha256, unpack } from './firmware.js';

const upd = new Uint8Array(readFileSync(process.argv[2]));
const t0 = performance.now();
const images = unpack(upd);
const ms = Math.round(performance.now() - t0);
const got = {};
for (const [name, bytes] of Object.entries(images)) got[name] = await sha256(bytes);
console.log(JSON.stringify({ upd: await sha256(upd), ms, got, expected: EXPECTED, upd_expected: UPD_SHA256 }, null, 1));
