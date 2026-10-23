// Picture subtitles (1.2): a Blu-ray (PGS) track beside an episode is in the player's menu ([data-subpic],
// no <track>: browsers can't show pictures as one) and Hemma's player draws it itself - its cue at the cue's
// time, placed and scaled from the subtitles' frame onto the video's SHOWN picture (the 4:3 test clips in a
// 16:9 box: bars at the sides), lifted above the bar while that's up, gone at the cue's end, back after a
// seek. Never beside a text track; the pick goes on to the next episode by its key, as text tracks' do, and
// the viewer's language (Library settings) draws one from the start. The .sup files come from the clips
// generator (modules/av/clips_test.c): two cues each, the first episode's on a 640x480 frame, the second's
// on 720x576 - the frame MakeMKV gives even an NTSC DVD: the frame maps onto the shown picture, whatever
// its shape.
import fs from 'node:fs';
import path from 'node:path';
import { expect, request, test } from '@playwright/test';
import { act, episodes, ready } from './helpers.mjs';
import { BASE, LIBRARY, SIGNED_IN, WORK } from './where.mjs';

const SEASON = path.join(LIBRARY, 'Series', 'Testshow', 'Season 1');
/* each episode's: its file beside the episode, the frame and the cues in it */
const SUP1 = { name: 'Testshow S01E01.en.sup', from: 'pictures.sup', w: 640, h: 480,
  cues: [{ start: 2, end: 9, x: 160, y: 380, w: 320, h: 60 }, { start: 20, end: 24, x: 40, y: 40, w: 200, h: 50 }] };
const SUP2 = { name: 'Testshow S01E02.en.sup', from: 'pictures-576.sup', w: 720, h: 576,
  cues: [{ start: 2, end: 9, x: 180, y: 448, w: 360, h: 74 }, { start: 20, end: 24, x: 45, y: 48, w: 225, h: 60 }] };
const CUES = SUP1.cues;
const TEXT = 'Testshow S01E01.sv.srt';

/* The library is the other specs' too: the subtitles come for these tests and go after them. */
async function rescan(wanted) {
  const api = await request.newContext({ baseURL: BASE, storageState: SIGNED_IN, extraHTTPHeaders: { Origin: BASE, Referer: `${BASE}/` } });
  const r = await api.post('/a/media.rescan', { form: {} });
  expect(r.ok(), 'a rescan').toBe(true);
  await expect.poll(async () => {
    const series = await (await api.get('/media/series')).text();
    const href = (series.match(/href="(\/media\/series\/[0-9a-f]{16})"/) || [])[1];
    const page = href ? await (await api.get(href)).text() : '';
    const watch = [...page.matchAll(/href="(\/media\/watch\/[0-9a-f]{16})/g)].map((m) => m[1]);
    let n = 0;
    for (const w of new Set(watch)) n += (await (await api.get(w)).text()).includes('data-subpic') ? 1 : 0;
    return n;
  }, { message: 'the scan finds the picture subtitles', timeout: 30_000 }).toBe(wanted ? 2 : 0);
  await api.dispose();
}

test.beforeAll(async () => {
  for (const s of [SUP1, SUP2]) fs.copyFileSync(path.join(WORK, 'sidecars', s.from), path.join(SEASON, s.name));
  fs.writeFileSync(path.join(SEASON, TEXT), '1\n00:00:00,000 --> 00:01:40,000\nett\n');
  await rescan(true);
});

test.afterAll(async () => {
  for (const name of [SUP1.name, SUP2.name, TEXT]) fs.rmSync(path.join(SEASON, name), { force: true });
  await rescan(false);
});

/* [S01E01, S01E02]: the first is the one whose video goes on to another (see subtitles.spec.mjs) */
async function inOrder(page) {
  const eps = await episodes(page);
  for (const e of eps) {
    await page.goto(e);
    if (await page.locator('#media-video[data-next]').count()) return [e, eps.find((x) => x !== e)];
  }
  throw new Error('no episode goes on to the next');
}

async function pick(page, key) {
  const pl = page.locator('[data-player="media-video"]');
  await pl.hover();
  await pl.locator('[data-p="subs"]').click();
  await pl.locator(`[data-sub="${key}"]`).click();
  await expect(pl.locator(`[data-sub="${key}"]`)).toHaveAttribute('aria-checked', 'true');
}

const seek = (v, s) => v.evaluate((m, to) => new Promise((ok) => {
  m.addEventListener('seeked', ok, { once: true });
  m.currentTime = to;
}), s);

/* The video's shown picture (object-fit: contain), the drawn picture (null when none shows - or not yet
   loaded), the bar's top edge while it's up, the text track showing */
const look = (page) => page.evaluate(() => {
  const v = document.getElementById('media-video'), r = v.getBoundingClientRect();
  const s = Math.min(r.width / v.videoWidth, r.height / v.videoHeight), w = v.videoWidth * s, h = v.videoHeight * s;
  const shown = { left: r.left + (r.width - w) / 2, top: r.top + (r.height - h) / 2, w, h };
  const layer = document.querySelector('.pl-subpic'), img = layer && layer.querySelector('img');
  const drawn = !!img && !layer.hidden && img.style.visibility !== 'hidden' && img.complete && img.naturalWidth > 0;
  const ir = drawn ? img.getBoundingClientRect() : null, pl = document.querySelector('[data-player="media-video"]');
  const edge = pl.hasAttribute('data-on') ? pl.querySelector('.pl-bar').firstElementChild.getBoundingClientRect().top : null;
  const text = [...v.textTracks].find((t) => t.mode === 'showing');
  return {
    shown, edge, text: text ? text.id : '', aria: layer ? layer.getAttribute('aria-hidden') : null,
    pic: ir && { left: ir.left, top: ir.top, w: ir.width, h: ir.height, src: img.getAttribute('src'), natural: img.naturalWidth },
  };
});
const drawn = async (page) => ((await look(page)).pic || {}).src || '';

/* Cue k of a .sup (SUP1, SUP2) where it belongs on the shown picture - its frame mapped onto it: left,
   width and height always, top too unless lifted */
function placed(l, k, lifted, sup = SUP1) {
  const c = sup.cues[k], sx = l.shown.w / sup.w, sy = l.shown.h / sup.h;
  expect(Math.abs(l.pic.left - (l.shown.left + c.x * sx)), 'left').toBeLessThan(1.5);
  expect(Math.abs(l.pic.w - c.w * sx), 'width').toBeLessThan(1.5);
  expect(Math.abs(l.pic.h - c.h * sy), 'height').toBeLessThan(1.5);
  expect(l.pic.natural).toBe(c.w); /* (the cue's own picture, its own size) */
  if (!lifted) expect(Math.abs(l.pic.top - (l.shown.top + c.y * sy)), 'top').toBeLessThan(1.5);
  expect(l.pic.left).toBeGreaterThanOrEqual(l.shown.left - 1); /* on the picture, not on the bars beside it */
  expect(l.pic.left + l.pic.w).toBeLessThanOrEqual(l.shown.left + l.shown.w + 1);
  expect(l.pic.top).toBeGreaterThanOrEqual(l.shown.top - 1);
}

test('a picture track is drawn at its cue\'s time and place, over the shown picture', async ({ page }) => {
  const [e1] = await inOrder(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  /* in the menu, after the text ones' language order - and no <track>: pictures aren't text */
  await expect(page.locator('[data-player] [data-sub="en"]')).toHaveAttribute('data-subpic', /\.json/);
  expect(await v.locator('track').evaluateAll((ts) => ts.map((t) => t.id))).toEqual(['sub-sv']);
  await pick(page, 'en');
  await seek(v, 1);
  await page.waitForTimeout(300);
  expect(await drawn(page)).toBe(''); /* (before the first cue) */
  await seek(v, 3);
  await expect.poll(() => drawn(page), { message: 'the first cue' }).toMatch(/\/0\.png(\?|$)/);
  let l = await look(page);
  expect(l.aria).toBe('true'); /* pictures of words: nothing for a screen reader */
  expect(l.edge, 'paused: the bar is up').not.toBeNull();
  await expect.poll(async () => { /* (it slides up: once it's there) */
    const m = await look(page);
    return m.pic.top + m.pic.h <= m.edge + 0.5;
  }, { message: 'above the bar' }).toBe(true);
  placed(await look(page), 0, true);

  /* playing, the bar goes after 3 still seconds: the picture comes down to its own place */
  await page.mouse.move(2, 2);
  await seek(v, 2.2);
  await v.evaluate((m) => { m.muted = true; return m.play(); });
  await expect.poll(async () => { const m = await look(page); return m.edge === null && !!m.pic; },
    { message: 'the bar goes, the cue stays', timeout: 6000 }).toBe(true);
  await page.waitForTimeout(400); /* (its slide down) */
  l = await look(page);
  expect(await v.evaluate((m) => m.currentTime)).toBeLessThan(CUES[0].end);
  placed(l, 0, false);
  /* ... and goes at the cue's end */
  await v.evaluate((m) => new Promise((ok) => { const t = () => (m.currentTime > 9.3 ? ok() : requestAnimationFrame(t)); t(); }));
  expect(await drawn(page)).toBe('');
  await v.evaluate((m) => m.pause());

  /* back after a seek; the next cue in its own place */
  await seek(v, 4);
  await expect.poll(() => drawn(page), { message: 'the first cue again' }).toMatch(/\/0\.png(\?|$)/);
  await seek(v, 21);
  await expect.poll(() => drawn(page), { message: 'the second cue' }).toMatch(/\/1\.png(\?|$)/);
  placed(await look(page), 1, false); /* (at the top: the bar's no matter) */
  await seek(v, 30);
  await expect.poll(() => drawn(page)).toBe('');
});

test('a text track and a picture track never show at once', async ({ page }) => {
  const [e1] = await inOrder(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  await seek(v, 3);
  await pick(page, 'sv');
  await expect.poll(async () => (await look(page)).text).toBe('sub-sv');
  expect(await drawn(page)).toBe('');
  await pick(page, 'en');
  await expect.poll(() => drawn(page)).toMatch(/\/0\.png(\?|$)/);
  expect((await look(page)).text).toBe('');
  await expect(page.locator('[data-sub="sv"]')).toHaveAttribute('aria-checked', 'false');
  await pick(page, '');
  await expect.poll(() => drawn(page)).toBe('');
  expect((await look(page)).text).toBe('');
});

test('the pick goes on to the next episode, and its own pictures are drawn', async ({ page }) => {
  const [e1, e2] = await inOrder(page);
  await page.goto(e1);
  let v = page.locator('#media-video');
  await ready(v);
  await pick(page, 'en');
  await v.evaluate((m) => { m.muted = true; m.currentTime = 97; return m.play(); });
  await expect(page).toHaveURL(new RegExp(`${e2}\\?auto=1$`), { timeout: 15_000 });
  v = page.locator('#media-video');
  await expect(v).toHaveAttribute('data-id', e2.split('/').pop());
  await expect(page.locator('[data-sub="en"]')).toHaveAttribute('aria-checked', 'true');
  const list = await page.locator('[data-sub="en"]').getAttribute('data-subpic');
  const base = new URL(list, BASE).pathname.replace(/\.json$/, '/');
  await expect.poll(() => drawn(page), { message: 'its first cue, from its own list', timeout: 15_000 })
    .toContain(`${base}0.png`);
  placed(await look(page), 0, true, SUP2); /* (720x576 onto the 4:3 picture; the bar may be up: its top aside) */
  await v.evaluate((m) => m.pause());
  await seek(v, 21);
  await expect.poll(() => drawn(page)).toContain(`${base}1.png`);
  placed(await look(page), 1, false, SUP2);
});

test('the viewer\'s language as a picture track is on from the start', async ({ page }) => {
  await act(page, 'media.prefs', { sublang: 'en' }); /* (Library settings: English - here only as pictures) */
  try {
    const [e1] = await inOrder(page);
    await page.goto(e1);
    const v = page.locator('#media-video');
    await ready(v);
    await expect(page.locator('[data-sub="en"]')).toHaveAttribute('aria-checked', 'true');
    await seek(v, 3); /* (nothing picked here) */
    await expect.poll(() => drawn(page), { message: 'drawn without a pick' }).toMatch(/\/0\.png(\?|$)/);
    expect((await look(page)).text).toBe('');
  } finally {
    await act(page, 'media.prefs', { sublang: '' });
  }
});

test.describe('on a phone', () => {
  test.use({ viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true, deviceScaleFactor: 3 });
  test('the picture is scaled down onto the phone\'s picture', async ({ page }) => {
    const [e1] = await inOrder(page);
    await page.goto(e1);
    const v = page.locator('#media-video');
    await ready(v);
    await pick(page, 'en');
    await seek(v, 3); /* (paused: the bar is up, and the low cue goes above it) */
    await expect.poll(() => drawn(page)).toMatch(/\/0\.png(\?|$)/);
    await expect.poll(async () => { const m = await look(page); return m.pic.top + m.pic.h <= m.edge + 0.5; }).toBe(true);
    placed(await look(page), 0, true);
    await seek(v, 21);
    await expect.poll(() => drawn(page)).toMatch(/\/1\.png(\?|$)/);
    placed(await look(page), 1, false);
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(390);
  });
});

test('in full screen the picture follows the full-screen picture', async ({ page }) => {
  const [e1] = await inOrder(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  await pick(page, 'en');
  await seek(v, 21);
  await expect.poll(() => drawn(page)).toMatch(/\/1\.png(\?|$)/);
  const before = (await look(page)).pic.w;
  /* full screen is the player's box (the bar and the card show there): the picture is scaled again */
  await page.locator('[data-player="media-video"]').hover();
  await page.locator('#media-box-fs').click();
  await expect.poll(() => page.evaluate(() => (document.fullscreenElement || {}).id || '')).toBe('media-box');
  await expect.poll(async () => {
    const l = await look(page);
    return l.pic && Math.abs(l.pic.w - CUES[1].w * l.shown.w / SUP1.w) < 1.5;
  }, { message: 'scaled to the full-screen picture' }).toBe(true);
  const l = await look(page);
  placed(l, 1, false);
  expect(l.pic.w).toBeGreaterThan(before);
  await page.evaluate(() => document.exitFullscreen());
  await expect.poll(async () => Math.round((await look(page)).pic.w), { message: 'and back' }).toBe(Math.round(before));
});
