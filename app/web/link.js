// SPDX-License-Identifier: GPL-2.0-or-later
// The page's line to app/web_deck.py. deck.js draws the face and reads the
// hand; it sees only this object, so another source of frames (an emulator
// running in the page) can stand in for the server:
//
//   link.onframe(rgb565, width, height)  a whole LCD frame, big-endian RGB565
//                                        bytes as the display board scans out
//   link.onstatus(text)                  the line's state, in a few words
//   link.onlamps(panel)                  MAIN's 40-byte frame to the panel MCU
//   link.send(event)                     a JSON event from the face (deck.js)
//
// Over the WebSocket a frame is "F", u32 seq, u8 kind (little-endian), then
// zlib: kind 0 the whole frame, kind 1 its XOR with the last one sent here.

"use strict";

function serverLink(deck, face) {
  const [width, height] = face.lcd_pixels;
  const link = { onframe: null, onstatus: null, onlamps: null, send };
  let ws = null;
  let frame = null;               // the last whole frame, as the board's bytes
  let queue = Promise.resolve();  // frames inflate in order

  function send(event) {
    if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(event));
  }

  function status(text) {
    if (link.onstatus) link.onstatus(text);
  }

  async function inflate(data) {
    const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream("deflate"));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  async function onFrame(buf) {
    const head = new DataView(buf, 0, 6);
    if (head.getUint8(0) !== 0x46) return;
    const kind = head.getUint8(5);
    const bytes = await inflate(buf.slice(6));
    if (bytes.length !== width * height * 2) return;
    if (kind === 0) {
      frame = bytes;
    } else {
      if (!frame) return;
      const a = new Uint32Array(frame.buffer), d = new Uint32Array(bytes.buffer);
      for (let i = 0; i < a.length; i++) a[i] ^= d[i];
    }
    if (link.onframe) link.onframe(frame, width, height);
  }

  function connect() {
    const q = new URLSearchParams(location.search);
    const dir = location.pathname.replace(/[^/]*$/, "");
    const url = (location.protocol === "https:" ? "wss://" : "ws://") + location.host + dir +
                "ws?deck=" + deck + (q.get("fps") ? "&fps=" + encodeURIComponent(q.get("fps")) : "");
    status("connecting");
    ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    ws.onopen = () => status("connected");
    ws.onclose = () => {
      frame = null;
      status("disconnected, retrying");
      setTimeout(connect, 1000);
    };
    ws.onmessage = (e) => {
      if (typeof e.data !== "string") queue = queue.then(() => onFrame(e.data)).catch(() => {});
    };
  }

  setTimeout(connect, 0);
  return link;
}
