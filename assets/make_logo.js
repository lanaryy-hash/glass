// Génère assets/glass.ico (logo de Glass) : carré arrondi en dégradé néon, reflet de verre, double croche blanche.
// Usage : node assets/make_logo.js   (le .ico est ensuite intégré à Glass.exe via glass.rc ; glass-1024.png sert à l'icône Mac)
const fs = require('fs'), zlib = require('zlib'), path = require('path');

const mix = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);
const clamp = (v, a = 0, b = 1) => Math.min(b, Math.max(a, v));
const PINK = [1.0, 0.31, 0.85], VIOLET = [0.55, 0.36, 1.0], CYAN = [0.18, 0.89, 1.0];

function sdRoundBox(x, y, cx, cy, hw, hh, r) {
  const qx = Math.abs(x - cx) - hw + r, qy = Math.abs(y - cy) - hh + r;
  return Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) + Math.min(Math.max(qx, qy), 0) - r;
}
function sdCapsule(x, y, ax, ay, bx, by, r) {
  const pax = x - ax, pay = y - ay, bax = bx - ax, bay = by - ay;
  const h = clamp((pax * bax + pay * bay) / (bax * bax + bay * bay));
  return Math.hypot(pax - bax * h, pay - bay * h) - r;
}
function sdEllipse(x, y, cx, cy, rx, ry, ang) {  // approximation suffisante pour un logo
  const c = Math.cos(ang), s = Math.sin(ang), dx = x - cx, dy = y - cy;
  const u = dx * c + dy * s, v = -dx * s + dy * c;
  return (Math.hypot(u / rx, v / ry) - 1) * Math.min(rx, ry);
}
function sdNote(x, y) {  // croche : tête penchée, hampe, crochet
  return Math.min(
    sdEllipse(x, y, 0.462, 0.598, 0.072, 0.054, -0.38),
    sdCapsule(x, y, 0.522, 0.585, 0.522, 0.372, 0.019),
    sdCapsule(x, y, 0.522, 0.372, 0.598, 0.430, 0.019));
}
const squircle = (x, y) => Math.pow(Math.abs(x - 0.5) / 0.44, 5) + Math.pow(Math.abs(y - 0.5) / 0.44, 5) - 1;  // < 0 : dedans (forme des icônes iPhone)
const conic = a => {  // couleur de l'anneau selon l'angle : rose -> violet -> cyan -> rose
  const t = (a / (2 * Math.PI) + 1.125) % 1 * 3, i = Math.floor(t), f = t - i, cs = [PINK, VIOLET, CYAN, PINK];
  return mix(cs[i], cs[i + 1], f * f * (3 - 2 * f));
};

// Couleur (simple « over ») d'un point du logo dans [0,1]² : fond sombre, anneau de verre en dégradé, croche blanche.
function sample(x, y) {
  const sq = squircle(x, y);
  if (sq > 0) return [0, 0, 0, 0];
  let c = mix([0.11, 0.07, 0.21], [0.03, 0.025, 0.07], clamp(x * 0.4 + y * 0.6));
  const dx = x - 0.5, dy = y - 0.5, r = Math.hypot(dx, dy), ang = Math.atan2(dy, dx);
  c = mix(c, conic(ang), 0.45 * Math.pow(clamp(1 - Math.abs(r - 0.27) / 0.2), 2));  // halo coloré autour de l'anneau
  if (r < 0.245) {  // disque de verre dépoli, reflet en haut à gauche
    c = mix(c, [1, 1, 1], 0.07 + 0.16 * Math.pow(clamp(1 - Math.hypot(x - 0.4, y - 0.36) / 0.22), 1.5));
  }
  const ring = Math.abs(r - 0.27) - 0.034;
  if (ring < 0) {
    c = conic(ang);
    c = mix(c, [1, 1, 1], 0.55 * Math.pow(clamp(-(dx + dy) / r * 0.7), 3) * clamp(-ring / 0.02));  // lumière sur le haut gauche de l'anneau
  }
  if (sq > -0.06) c = mix(c, [1, 1, 1], 0.12 * clamp(1 + sq / 0.06));  // liseré fin du verre
  const sh = sdNote(x - 0.008, y - 0.014);
  if (sh < 0.012) c = mix(c, [0.05, 0.02, 0.12], 0.4 * clamp(1 - (sh + 0.012) / 0.024));  // ombre de la note
  if (sdNote(x, y) < 0) c = [1, 1, 1];
  return [...c, 1];
}

function render(n) {
  const ss = n <= 32 ? 6 : 4, px = Buffer.alloc(n * n * 4);
  for (let py = 0; py < n; py++) for (let pxx = 0; pxx < n; pxx++) {
    let r = 0, g = 0, b = 0, a = 0;
    for (let sy = 0; sy < ss; sy++) for (let sx = 0; sx < ss; sx++) {
      const s = sample((pxx + (sx + 0.5) / ss) / n, (py + (sy + 0.5) / ss) / n);
      r += s[0] * s[3]; g += s[1] * s[3]; b += s[2] * s[3]; a += s[3];
    }
    const i = (py * n + pxx) * 4, k = ss * ss;
    px[i] = a ? Math.round(r / a * 255) : 0; px[i + 1] = a ? Math.round(g / a * 255) : 0; px[i + 2] = a ? Math.round(b / a * 255) : 0;
    px[i + 3] = Math.round(a / k * 255);
  }
  return px;
}

const crcTable = Array.from({length: 256}, (_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c >>> 0; });
const crc32 = buf => { let c = 0xffffffff; for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]), crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(n, px) {
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(n, 0); ihdr.writeUInt32BE(n, 4); ihdr[8] = 8; ihdr[9] = 6;
  const raw = Buffer.alloc((n * 4 + 1) * n);
  for (let y = 0; y < n; y++) px.copy(raw, y * (n * 4 + 1) + 1, y * n * 4, (y + 1) * n * 4);
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, {level: 9})), chunk('IEND', Buffer.alloc(0))]);
}

const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
const pngs = sizes.map(n => png(n, render(n)));
const head = Buffer.alloc(6); head.writeUInt16LE(0, 0); head.writeUInt16LE(1, 2); head.writeUInt16LE(sizes.length, 4);
let offset = 6 + 16 * sizes.length;
const dir = sizes.map((n, i) => {
  const e = Buffer.alloc(16);
  e[0] = n >= 256 ? 0 : n; e[1] = n >= 256 ? 0 : n; e.writeUInt16LE(1, 4); e.writeUInt16LE(32, 6);
  e.writeUInt32LE(pngs[i].length, 8); e.writeUInt32LE(offset, 12); offset += pngs[i].length;
  return e;
});
fs.writeFileSync(path.join(__dirname, 'glass.ico'), Buffer.concat([head, ...dir, ...pngs]));
fs.writeFileSync(path.join(__dirname, 'glass-256.png'), pngs[pngs.length - 1]);
fs.writeFileSync(path.join(__dirname, 'glass-1024.png'), png(1024, render(1024)));  // icône Mac (Glass.icns, fabriquée par la compilation Mac)
console.log('glass.ico :', sizes.join(', '));
