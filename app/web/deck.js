// SPDX-License-Identifier: GPL-2.0-or-later
// The NXS2 face in a page: the body export_face.py drew, the LCD over it, and
// the hand on its controls. What a control does is decided where the frames
// come from (link.js describes the link); this file only says what the hand
// did, as JSON events:
//
//   {k: name, d: 1|0}      a face key (layout.py's name) or "rotary_push"
//   {sel: n}               the selector turned n detents, + down the list
//   {touch: [x, y, d]}     a finger on the LCD, in LCD pixels
//   {jog: degrees}         the platter turned by a drag, + clockwise
//   {jogTouch: 1|0}       a hand on (or off) the platter's top plate
//   {tempo: pos}           the tempo fader at pos, 0 (top, '-') to 1 (bottom, '+')
//   {key: keysym, d: 1|0}  the host keyboard, as X11 keysyms
//   {hidden: true}         the page went away: let go of everything held
//
// A key lights while the link's last panel frame (link.onlamps) sets the bit
// the face's lamp table gives it.

"use strict";

const SVG = "http://www.w3.org/2000/svg";

// KeyboardEvent.code to X11 keysym, as gfx.py maps pygame's keys: a printable
// key is its unshifted character, Shift a key of its own.
const KEYSYMS = {
  Backspace: 0xFF08, Tab: 0xFF09, Enter: 0xFF0D, Escape: 0xFF1B, Delete: 0xFFFF,
  Insert: 0xFF63, Home: 0xFF50, ArrowLeft: 0xFF51, ArrowUp: 0xFF52, ArrowRight: 0xFF53,
  ArrowDown: 0xFF54, PageUp: 0xFF55, PageDown: 0xFF56, End: 0xFF57,
  ShiftLeft: 0xFFE1, ShiftRight: 0xFFE2, ControlLeft: 0xFFE3, ControlRight: 0xFFE4,
  CapsLock: 0xFFE5, AltLeft: 0xFFE9, AltRight: 0xFFEA, MetaLeft: 0xFFE7, MetaRight: 0xFFE8,
  NumpadEnter: 0xFF8D, NumpadMultiply: 0xFFAA, NumpadAdd: 0xFFAB, NumpadSubtract: 0xFFAD,
  NumpadDecimal: 0xFFAE, NumpadDivide: 0xFFAF, NumpadEqual: 0xFFBD,
  Space: 0x20, Minus: 0x2D, Equal: 0x3D, BracketLeft: 0x5B, BracketRight: 0x5D,
  Backslash: 0x5C, Semicolon: 0x3B, Quote: 0x27, Comma: 0x2C, Period: 0x2E, Slash: 0x2F,
  Backquote: 0x60,
};
for (let n = 0; n < 10; n++) {
  KEYSYMS["Digit" + n] = 0x30 + n;
  KEYSYMS["Numpad" + n] = 0xFFB0 + n;
}
for (let n = 1; n <= 12; n++) KEYSYMS["F" + n] = 0xFFBD + n;
for (let c = 0; c < 26; c++) KEYSYMS["Key" + String.fromCharCode(65 + c)] = 0x61 + c;
// Left to the browser: reload, full screen, its tools.
const BROWSER_KEYS = new Set(["F5", "F11", "F12"]);

function keysym(code) {
  return BROWSER_KEYS.has(code) ? undefined : KEYSYMS[code];
}

// RGB565 to the canvas's RGBA, as one little-endian word a pixel.
const RGBA = new Uint32Array(65536);
for (let v = 0; v < 65536; v++) {
  const r = ((v >> 11) * 527 + 23) >> 6, g = (((v >> 5) & 63) * 259 + 33) >> 6, b = ((v & 31) * 527 + 23) >> 6;
  RGBA[v] = (0xFF000000 | (b << 16) | (g << 8) | r) >>> 0;
}

function hitTest(face, x, y) {
  // The same order as deck_view.py's down(): the screen, the selector, the keys.
  const [lx0, ly0, lx1, ly1] = face.lcd;
  if (x >= lx0 && x < lx1 && y >= ly0 && y < ly1) return { kind: "lcd" };
  const [sx, sy, sr] = face.select;
  const rs = Math.hypot(x - sx, y - sy);
  if (rs <= sr * 0.62) return { kind: "push" };
  if (rs <= sr + 8) return { kind: "select", angle: Math.atan2(y - sy, x - sx) * 180 / Math.PI, acc: 0 };
  const [jx, jy, jr] = face.jog;
  const rj = Math.hypot(x - jx, y - jy);
  if (rj <= jr) return { kind: "jog", angle: Math.atan2(y - jy, x - jx) * 180 / Math.PI, top: rj <= (face.jog_top ?? 0) };
  if (face.slider_frame) {
    const [fx0, fy0, fx1, fy1] = face.slider_frame;
    if (x >= fx0 && x <= fx1 && y >= fy0 && y <= fy1) return { kind: "slider" };
  }
  for (const k of face.keys) {
    if (k.look === "inert") continue;
    const hit = k.circle ? Math.hypot(x - k.circle[0], y - k.circle[1]) <= k.circle[2]
                         : x >= k.box[0] && x <= k.box[2] && y >= k.box[1] && y <= k.box[3];
    if (hit) return { kind: "key", key: k };
  }
  return null;
}

async function startDeck(root, statusLine, makeLink, deck, faceUrl) {
  const face = await (await fetch(faceUrl)).json();
  const [W, H] = face.units;
  const [PW, PH] = face.lcd_pixels;
  const pct = (v, of) => (100 * v / of) + "%";
  const body = root.querySelector("#body");
  const lcd = root.querySelector("#lcd");
  const hits = root.querySelector("#hits");
  const bodyW = Math.round(W * face.scale), bodyH = Math.round(H * face.scale);
  body.src = face.body;
  root.style.aspectRatio = `${W} / ${H}`;

  const [lx0, ly0, lx1, ly1] = face.lcd;
  Object.assign(lcd.style, { left: pct(lx0, W), top: pct(ly0, H), width: pct(lx1 - lx0, W), height: pct(ly1 - ly0, H) });
  lcd.width = PW;
  lcd.height = PH;
  const ctx = lcd.getContext("2d");
  const image = ctx.createImageData(PW, PH);
  const out = new Uint32Array(image.data.buffer);

  // Each working key's looks: pressed while it is held, lit while its lamp is.
  const sprites = {}, held = {}, lit = {};
  for (const k of face.keys) {
    if (!k.sprites) continue;
    sprites[k.name] = {};
    for (const [state, [src, x, y]] of Object.entries(k.sprites)) {
      const img = document.createElement("img");
      img.className = "pressed";
      img.draggable = false;
      img.onload = () => {
        Object.assign(img.style, { left: pct(x, bodyW), top: pct(y, bodyH),
                                   width: pct(img.naturalWidth, bodyW), height: pct(img.naturalHeight, bodyH) });
      };
      img.src = src;
      root.insertBefore(img, hits);
      sprites[k.name][state] = img;
    }
  }

  function show(name) {
    if (!sprites[name]) return;
    const state = (lit[name] ? "lit" : "") + (lit[name] && held[name] ? "_" : "") + (held[name] ? "down" : "");
    for (const [s, img] of Object.entries(sprites[name])) img.classList.toggle("down", s === state);
  }

  // The hit layer: titles name each key and its keyboard key.
  hits.setAttribute("viewBox", `0 0 ${W} ${H}`);
  for (const k of face.keys) {
    if (k.look === "inert") continue;
    const el = document.createElementNS(SVG, k.circle ? "circle" : "rect");
    if (k.circle) {
      el.setAttribute("cx", k.circle[0]); el.setAttribute("cy", k.circle[1]); el.setAttribute("r", k.circle[2]);
    } else {
      el.setAttribute("x", k.box[0]); el.setAttribute("y", k.box[1]);
      el.setAttribute("width", k.box[2] - k.box[0]); el.setAttribute("height", k.box[3] - k.box[1]);
    }
    const title = document.createElementNS(SVG, "title");
    title.textContent = k.name.replace(/_/g, " ") + (k.keycap ? "  ·  key " + k.keycap : "");
    el.appendChild(title);
    hits.appendChild(el);
  }

  const link = makeLink(face);
  let linkState = "", frames = [], dirty = null;
  link.onstatus = (text) => { linkState = text; };
  link.onframe = (rgb565) => { dirty = rgb565; };
  link.onlamps = (panel) => {
    for (const k of face.keys) {
      const lamp = k.lamp && face.lamps[k.lamp];
      if (!lamp) continue;
      const on = Boolean(panel[lamp[0]] & lamp[1]);
      if (on !== Boolean(lit[k.name])) {
        lit[k.name] = on;
        show(k.name);
      }
    }
  };

  function draw(now) {
    if (dirty) {
      for (let i = 0, j = 0; i < out.length; i++, j += 2) out[i] = RGBA[(dirty[j] << 8) | dirty[j + 1]];
      ctx.putImageData(image, 0, 0);
      dirty = null;
      frames.push(now);
    }
    while (frames.length && frames[0] < now - 2000) frames.shift();
    requestAnimationFrame(draw);
  }
  requestAnimationFrame(draw);
  setInterval(() => {
    // A screen that changes now and then (a stand-in, a paused deck) gets no rate.
    const fps = frames.length > 2 ? `  ·  ${(frames.length / 2).toFixed(1)} fps` : "";
    statusLine.textContent = `deck ${deck}  ·  ${linkState}${fps}`;
  }, 500);

  // Pointers: one grab per pointerId, so two fingers work at once.
  const grabs = new Map();
  const units = (e) => {
    const r = root.getBoundingClientRect();
    return [(e.clientX - r.left) * W / r.width, (e.clientY - r.top) * H / r.height];
  };
  const lcdPixel = (x, y) => [(x - lx0) * PW / (lx1 - lx0), (y - ly0) * PH / (ly1 - ly0)];

  function press(name, down) {
    link.send({ k: name, d: down ? 1 : 0 });
    held[name] = down;
    show(name);
  }

  // The fader's position along the cap's travel.
  function slide(y) {
    const [, top, bottom] = face.slider;
    link.send({ tempo: Math.min(Math.max((y - top) / (bottom - top), 0), 1) });
  }

  function letGo(g) {
    if (g.kind === "jog") link.send({ jogTouch: 0 });
    else if (g.kind === "lcd") link.send({ touch: [...g.at, 0] });
    else if (g.kind === "push") press("rotary_push", false);
    else if (g.kind === "key") press(g.key.name, false);
  }

  root.addEventListener("pointerdown", (e) => {
    const [x, y] = units(e);
    const g = hitTest(face, x, y);
    if (!g) return;
    e.preventDefault();
    root.setPointerCapture(e.pointerId);
    grabs.set(e.pointerId, g);
    if (g.kind === "lcd") {
      g.at = lcdPixel(x, y);
      link.send({ touch: [...g.at, 1] });
    } else if (g.kind === "push") {
      press("rotary_push", true);
    } else if (g.kind === "jog") {
      if (g.top) link.send({ jogTouch: 1 });
    } else if (g.kind === "slider") {
      slide(y);
    } else if (g.kind === "key") {
      press(g.key.name, true);
    }
  });

  root.addEventListener("pointermove", (e) => {
    const g = grabs.get(e.pointerId);
    if (!g) return;
    const [x, y] = units(e);
    if (g.kind === "lcd") {
      g.at = lcdPixel(x, y);
      link.send({ touch: [...g.at, 1] });
    } else if (g.kind === "select") {
      // One detent per 15 degrees of drag, clockwise down the list.
      const a = Math.atan2(y - face.select[1], x - face.select[0]) * 180 / Math.PI;
      g.acc += ((a - g.angle + 540) % 360) - 180;
      g.angle = a;
      const n = Math.trunc(g.acc / 15);
      if (n) {
        g.acc -= n * 15;
        link.send({ sel: n });
      }
    } else if (g.kind === "jog") {
      const a = Math.atan2(y - face.jog[1], x - face.jog[0]) * 180 / Math.PI;
      link.send({ jog: ((a - g.angle + 540) % 360) - 180 });
      g.angle = a;
    } else if (g.kind === "slider") {
      slide(y);
    }
  });

  const up = (e) => {
    const g = grabs.get(e.pointerId);
    if (!g) return;
    grabs.delete(e.pointerId);
    letGo(g);
  };
  root.addEventListener("pointerup", up);
  root.addEventListener("pointercancel", up);

  // A wheel notch away from the platter turns the selector; up moves up the list.
  let wheel = 0;
  root.addEventListener("wheel", (e) => {
    const [x, y] = units(e);
    if (Math.hypot(x - face.jog[0], y - face.jog[1]) <= face.jog[2]) return;
    e.preventDefault();
    wheel += e.deltaMode === 0 ? e.deltaY / 100 : e.deltaY;
    const n = Math.trunc(wheel);
    if (n) {
      wheel -= n;
      link.send({ sel: n });
    }
  }, { passive: false });

  // The keyboard is the deck's only while the face is on screen.
  const shown = () => root.getClientRects().length > 0;
  const keysHeld = new Set();
  window.addEventListener("keydown", (e) => {
    const sym = keysym(e.code);
    if (sym === undefined || !shown()) return;
    e.preventDefault();
    if (e.repeat || keysHeld.has(e.code)) return;
    keysHeld.add(e.code);
    link.send({ key: sym, d: 1 });
  });
  window.addEventListener("keyup", (e) => {
    const sym = keysym(e.code);
    if (sym === undefined || !keysHeld.has(e.code)) return;
    e.preventDefault();
    keysHeld.delete(e.code);
    link.send({ key: sym, d: 0 });
  });

  // A hidden page cannot let go of anything, so it lets go of everything now.
  function hidden() {
    for (const name of Object.keys(held)) {
      held[name] = false;
      show(name);
    }
    grabs.clear();
    keysHeld.clear();
    link.send({ hidden: true });
  }
  document.addEventListener("visibilitychange", () => { if (document.hidden) hidden(); });
  window.addEventListener("blur", hidden);
}
