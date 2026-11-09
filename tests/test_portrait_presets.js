// tests/test_portrait_presets.js —— 校验新增的人像预设（漫画线稿/剪影/网点）真的按预期量化。
//
// 直接把人像量化函数从内嵌网页里抽出来跑，避免在测试里复制一份算法（复制会漂移）。
const fs = require('fs');
const path = require('path');
const assert = require('assert');

const html = fs.readFileSync(path.join(__dirname, '..', 'main', 'apps', 'home', 'phone_setup.html'), 'utf8');

function extract(name) {
  const start = html.indexOf(`function ${name}(`);
  assert(start >= 0, `${name} not found in phone_setup.html`);
  let depth = 0;
  for (let i = html.indexOf('{', start); i < html.length; i += 1) {
    if (html[i] === '{') depth += 1;
    else if (html[i] === '}') {
      depth -= 1;
      if (depth === 0) return html.slice(start, i + 1);
    }
  }
  throw new Error(`${name} body not closed`);
}

const sandbox = {};
const names = [...html.matchAll(/function (portrait\w+)\(/g)].map((m) => m[1]);
new Function('sandbox', `with (sandbox) {
  var portraitSampleCache = null, portraitPreparedCache = null;
  ${names.map(extract).join('\n')}
  sandbox.levels = portraitLevels;
}`)(sandbox);

const W = 96;
const H = 138;

// 左半白、右半黑的硬边图：用来区分"平涂"和"漫画线稿"对轮廓的处理
function edgeImage() {
  const data = new Uint8ClampedArray(W * H * 4);
  for (let y = 0; y < H; y += 1) {
    for (let x = 0; x < W; x += 1) {
      const v = x < W / 2 ? 230 : 30;
      const i = (y * W + x) * 4;
      data[i] = v; data[i + 1] = v; data[i + 2] = v; data[i + 3] = 255;
    }
  }
  return data;
}

// 纯中灰：网点抖动应该摊到两个相邻阶
function flatImage(v) {
  const data = new Uint8ClampedArray(W * H * 4);
  for (let i = 0; i < W * H; i += 1) {
    data[i * 4] = v; data[i * 4 + 1] = v; data[i * 4 + 2] = v; data[i * 4 + 3] = 255;
  }
  return data;
}

function levelsOf(data, preset) {
  const out = sandbox.levels(data, W, H, false, 0, preset);
  return out.levels;
}

function countIn(levels, from, to, value) {
  let n = 0;
  for (let p = from; p < to; p += 1) if (levels[p] === value) n += 1;
  return n;
}

// 1. 平涂：白区里不应出现最暗阶（不会凭空画线）
const flatEdge = levelsOf(edgeImage(), 2);
const whiteHalf = W / 2;
let whiteInk = 0;
for (let y = 0; y < H; y += 1) whiteInk += countIn(flatEdge, y * W, y * W + whiteHalf, 0);
assert.strictEqual(whiteInk, 0, '平涂不该在白区画墨线');

// 2. 漫画线稿：硬边两侧会出现墨线（最暗阶），靠它勾轮廓
const comic = levelsOf(edgeImage(), 4);
let comicEdgeInk = 0;
for (let y = 0; y < H; y += 1) {
  const base = y * W;
  comicEdgeInk += countIn(comic, base + whiteHalf - 3, base + whiteHalf + 3, 0);
}
assert(comicEdgeInk > H, `漫画线稿应在边缘画墨线，实际 ${comicEdgeInk}`);

// 3. 剪影：只有 0 和 3 两阶；亮处为 3、暗处为 0
const silhouette = levelsOf(edgeImage(), 5);
const seen = new Set(silhouette);
assert.deepStrictEqual([...seen].sort(), [0, 3], '剪影只能有两阶');
assert.strictEqual(silhouette[10], 3, '剪影白区应为浅阶');
assert.strictEqual(silhouette[W - 10], 0, '剪影黑区应为暗阶');

// 4. 网点印刷：中灰要抖成两种阶，而不是一刀切
const screenTone = levelsOf(flatImage(128), 6);
const toneSeen = new Set(screenTone);
assert(toneSeen.has(1) && toneSeen.has(2), `网点中灰应抖成 1/2 两阶，实际 ${[...toneSeen]}`);

// 5. 六种预设都能跑通且都只用 0..3
for (const preset of [1, 2, 3, 4, 5, 6]) {
  const levels = levelsOf(flatImage(140), preset);
  for (const v of levels) assert(v >= 0 && v <= 3, `预设 ${preset} 产生了非法阶 ${v}`);
}

console.log('Portrait presets (dot/flat/hybrid/comic/silhouette/screen): PASS');
