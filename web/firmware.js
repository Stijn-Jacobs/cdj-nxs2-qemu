// SPDX-License-Identifier: GPL-2.0-or-later
// Turn the visitor's own CDJ-2000NXS2 v1.87 update (C2KNXS2.UPD) into the
// images the emulator boots, in the browser, with nothing uploaded. This is
// launcher/firmware.py and the scripts/firmware/ steps it runs, producing the
// same bytes; every image is checked against the digest the profile
// (models/cdj2000nxs2.conf) holds for it.

export const UPD_NAME = 'C2KNXS2.UPD';
export const FW_VERSION = '1.87';
export const UPD_SHA256 = 'f211191a573b7a5c0e694936871f76011a58d5861dd061301cdb0d8b83a8b3e5';
export const EXPECTED = {
  'main_unpacked.bin': '78182ce6e552ff4ba65c8c845a62317cecbec86c35e312c6ac9e9af9796a2076',
  'gui_unpacked.bin': '20b52af38f29b2a59255e1ce4238cab896f28edca2ad5707650a8c8bb9d0457e',
  'flash.bin': '481d42e5843bc70d3a93df640878adb0d7035497e3b233b5cc85c14030a82563',
  'resblob.bin': 'cb61896e8d6b3d3fe317e2af14a05c5729593beb084d0e53b98968181272d768',
  'artblob.bin': '929372309ea24f8d30255876ad46f7b684470009a3b022eed03179db3d4161a7',
  'settings.bin': '73af511542c0637da543f6bfec8513cf92befe0053552471411bb4e31854b56d',
};

const MAIN_SECTION = 3;
const GUI_SECTION = 1;
const MAIN_LZSS = 0x50004;
const GUI_STREAM_OFF = 0x24;
// The font archive's declared span at 0x0DF00000; the decoder runs 0.3% past it.
const RES_REGION = 0x440000;
const RES_SPAN = 0x57C099;
// The artwork window at 0x0CD00000 cannot reach the glyph window.
const ART_REGION = 0x150000;
const ART_MAXSPAN = 0x0DF00000 - 0x0CD00000;

const LABEL_SIZE = 32;
const FLASH_SIZE = 0x800000;
const MAIN_END = 0x7E0000;
const SETTINGS_OFF = 0x7F6000;
const SETTINGS_SIZE = 0x2000;
// Key 0x7101 must be non-zero, or main() sees the zeroed DRAM cache agree
// with the lookup and reboots in a loop.
const SETTINGS = [[0x7101, 0x0001]];

export class FirmwareError extends Error {}

export async function sha256(bytes) {
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, (b) => b.toString(16).padStart(2, '0')).join('');
}

// A manifest of CRLF-terminated decimal lengths, then the sections in order.
export function splitUpdate(upd) {
  const sizes = [];
  let pos = 0;
  for (;;) {
    let end = pos;
    while (end < upd.length && end - pos < 30 && upd[end] >= 0x30 && upd[end] <= 0x39) end++;
    if (end === pos || upd[end] !== 0x0D || upd[end + 1] !== 0x0A) break;
    sizes.push(Number(new TextDecoder().decode(upd.subarray(pos, end))));
    pos = end + 2;
  }
  return sizes.map((size) => {
    const section = upd.subarray(pos, pos + size);
    pos += size;
    return section;
  });
}

const HEX = new Int16Array(256).fill(-1);
for (let i = 0; i < 16; i++) {
  HEX['0123456789abcdef'.charCodeAt(i)] = i;
  HEX['0123456789ABCDEF'.charCodeAt(i)] = i;
}

// [address, data] of every S1/S2/S3 record after the section's label.
export function srecords(section) {
  const records = [];
  let line = LABEL_SIZE;
  while (line < section.length) {
    let eol = line;
    while (eol < section.length && section[eol] !== 0x0A) eol++;
    let end = eol > line && section[eol - 1] === 0x0D ? eol - 1 : eol;
    const type = section[line + 1] - 0x30;
    if (section[line] === 0x53 && type >= 1 && type <= 3) {
      const rec = new Uint8Array((end - line - 2) >> 1);
      let sum = 0;
      for (let i = 0, p = line + 2; i < rec.length; i++, p += 2) {
        const hi = HEX[section[p]], lo = HEX[section[p + 1]];
        if (hi < 0 || lo < 0) throw new FirmwareError(`bad S-record at offset ${line}`);
        sum += rec[i] = (hi << 4) | lo;
      }
      if ((sum & 0xFF) !== 0xFF) throw new FirmwareError(`bad S-record checksum at offset ${line}`);
      const abytes = type + 1;
      let addr = 0;
      for (let i = 1; i <= abytes; i++) addr = addr * 256 + rec[i];
      records.push([addr, rec.subarray(1 + abytes, rec.length - 1)]);
    }
    line = eol + 1;
  }
  return records;
}

// The records in a zero-filled image spanning their lowest to highest address.
// Each record brings its checksum byte along, as srec_coverage.py has always
// done: the next record overwrites it, but one that ends a run leaves it in
// the gap, and the MAIN image's digest depends on those bytes.
function sparseImage(records) {
  const withSum = records.map(([addr, data]) => [addr, new Uint8Array(data.buffer, data.byteOffset, data.length + 1)]);
  let lo = Infinity, hi = 0;
  for (const [addr, data] of withSum) {
    lo = Math.min(lo, addr);
    hi = Math.max(hi, addr + data.length);
  }
  const img = new Uint8Array(hi - lo);
  for (const [addr, data] of withSum) img.set(data, addr - lo);
  return img;
}

// Pioneer's LZSS: a 4 KiB ring filled with spaces, writing from 4078; one flag
// byte (LSB first, 1 = literal) per eight items; a match is an absolute ring
// position (12 bits) and a length of 3..18.
export function unlzss(src, start, end = src.length, limit = Infinity) {
  const ring = new Uint8Array(4096).fill(0x20);
  let r = 4078;
  let out = new Uint8Array(Math.min(Math.max((end - start) * 2, 4096), limit + 18));
  let n = 0;
  let i = start;
  let f = 0;
  while (i < end) {
    if (out.length - n < 18) {
      const grown = new Uint8Array(out.length * 2);
      grown.set(out);
      out = grown;
    }
    f >>= 1;
    if (!(f & 0x100)) f = src[i++] | 0xFF00;
    if (f & 1) {
      if (i >= end) break;
      const b = src[i++];
      out[n++] = b;
      ring[r] = b;
      r = (r + 1) & 0xFFF;
    } else {
      if (i + 1 >= end) break;
      const b1 = src[i], b2 = src[i + 1];
      i += 2;
      const off = b1 | ((b2 & 0xF0) << 4);
      const len = (b2 & 0x0F) + 3;
      for (let k = 0; k < len; k++) {
        const b = ring[(off + k) & 0xFFF];
        out[n++] = b;
        ring[r] = b;
        r = (r + 1) & 0xFFF;
      }
    }
    if (n >= limit) break;
  }
  return out.slice(0, Math.min(n, limit));
}

function be32(bytes, off) {
  return ((bytes[off] << 24) | (bytes[off + 1] << 16) | (bytes[off + 2] << 8) | bytes[off + 3]) >>> 0;
}

export function makeSettings() {
  const sector = new Uint8Array(SETTINGS_SIZE).fill(0xFF);
  const view = new DataView(sector.buffer);
  SETTINGS.forEach(([key, value], i) => {
    view.setUint16(i * 4, key, true);
    view.setUint16(i * 4 + 2, value, true);
  });
  return sector;
}

// The MAIN section laid in as the firmware's own updater does it, every record
// at its flash offset, so the board boots through Pioneer's bootloader.
export function makeFlash(records, settings) {
  const img = new Uint8Array(FLASH_SIZE).fill(0xFF);
  for (const [addr, data] of records) {
    if (addr + data.length > MAIN_END) {
      throw new FirmwareError(`MAIN record at 0x${addr.toString(16)} runs past 0x${MAIN_END.toString(16)}`);
    }
    img.set(data, addr);
  }
  img.set(settings, SETTINGS_OFF);
  return img;
}

// Every image, keyed by the file name the native setup writes, unverified.
export function unpack(upd) {
  const sections = splitUpdate(upd);
  const main = sections[MAIN_SECTION - 1];
  const gui = sections[GUI_SECTION - 1];
  if (!new TextDecoder('latin1').decode(main.subarray(0, LABEL_SIZE)).includes('MAIN')) {
    throw new FirmwareError(`section ${MAIN_SECTION} is not the MAIN section`);
  }

  const records = srecords(main);
  // The bounded stream: the GUI section's 2-byte trailer is a checksum.
  const guiImage = unlzss(gui, GUI_STREAM_OFF, Math.min(gui.length, GUI_STREAM_OFF + be32(gui, 0x20)));
  const settings = makeSettings();
  return {
    'main_unpacked.bin': unlzss(sparseImage(records), MAIN_LZSS),
    'gui_unpacked.bin': guiImage,
    'flash.bin': makeFlash(records, settings),
    'resblob.bin': unlzss(guiImage, RES_REGION + 4, guiImage.length, RES_SPAN),
    'artblob.bin': unlzss(guiImage, ART_REGION + 4, ART_REGION + 4 + be32(guiImage, ART_REGION), ART_MAXSPAN),
    'settings.bin': settings,
  };
}

// unpack() behind the checks setup makes: the update must be v1.87 and every
// image must match its digest, else FirmwareError.
export async function prepare(upd) {
  const got = await sha256(upd);
  if (got !== UPD_SHA256) {
    throw new FirmwareError(`not the CDJ-2000NXS2 v${FW_VERSION} update (sha256 ${got}); only v${FW_VERSION} is supported`);
  }
  const images = unpack(upd);
  for (const [name, want] of Object.entries(EXPECTED)) {
    const sum = await sha256(images[name]);
    if (sum !== want) throw new FirmwareError(`${name} does not match (sha256 ${sum}, want ${want})`);
  }
  return images;
}
