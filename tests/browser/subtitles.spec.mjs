// Subtitles (1.2): two in one language are two choices. Every <track> has its key as its id
// ("sub-en", "sub-en.sdh"), the player's menu picks by that key ([data-sub]), and the pick goes on to
// the next episode - the same key, else the first in the same language, else none.
import fs from 'node:fs';
import path from 'node:path';
import { expect, request, test } from '@playwright/test';
import { episodes, ready } from './helpers.mjs';
import { BASE, LIBRARY, SIGNED_IN } from './where.mjs';

const SEASON = path.join(LIBRARY, 'Series', 'Testshow', 'Season 1');
const srt = (text) => `1\n00:00:00,000 --> 00:01:40,000\n${text}\n`; /* (the whole 100-second episode) */
const SUBS = {
  'Testshow S01E01.en.srt': srt('one'),
  'Testshow S01E01.en.sdh.srt': srt('one, for the hard of hearing'),
  'Testshow S01E01.sv.forced.srt': srt('ett, bara främmande språk'),
  'Testshow S01E01.de.srt': srt('eins'),
  'Testshow S01E02.en.srt': srt('two'),
  'Testshow S01E02.en.sdh.srt': srt('two, for the hard of hearing'),
  'Testshow S01E02.sv.srt': srt('två'),
};

/* The library is the other specs' too: the subtitles come for these tests and go after them. */
async function rescan(wanted) {
  const api = await request.newContext({ baseURL: BASE, storageState: SIGNED_IN, extraHTTPHeaders: { Origin: BASE, Referer: `${BASE}/` } });
  const r = await api.post('/a/media.rescan', { form: {} });
  expect(r.ok(), 'a rescan').toBe(true);
  await expect.poll(async () => {
    const series = await (await api.get('/media/series')).text();
    const href = (series.match(/href="(\/media\/series\/[0-9a-f]{16})"/) || [])[1];
    const page = href ? await (await api.get(href)).text() : '';
    const watch = (page.match(/href="(\/media\/watch\/[0-9a-f]{16})/) || [])[1];
    return watch ? (await (await api.get(watch)).text()).includes('id="sub-en.sdh"') : null;
  }, { message: 'the scan finds the subtitles', timeout: 30_000 }).toBe(wanted);
  await api.dispose();
}

test.beforeAll(async () => {
  for (const [name, text] of Object.entries(SUBS)) fs.writeFileSync(path.join(SEASON, name), text);
  await rescan(true);
  /* no subtitles by default - which also forgets the picks other specs left for the series (1.3) */
  const api = await request.newContext({ baseURL: BASE, storageState: SIGNED_IN, extraHTTPHeaders: { Origin: BASE, Referer: `${BASE}/` } });
  expect((await api.post('/a/media.prefs', { form: { sublang: '' } })).ok()).toBe(true);
  await api.dispose();
});

test.afterAll(async () => {
  for (const name of Object.keys(SUBS)) fs.rmSync(path.join(SEASON, name), { force: true });
  await rescan(false);
});

/* What shows: the key of the track showing (or ''), and its first cue's words once loaded */
const showing = (v) => v.evaluate((m) => {
  const t = [...m.textTracks].find((x) => x.mode === 'showing');
  return t ? `${t.id.replace(/^sub-/, '')}: ${t.cues && t.cues.length ? t.cues[0].text : '…'}` : '';
});

/* [S01E01, S01E02]: once watched, the series page's "Next up" may come first - the first episode is
   the one whose video goes on to another */
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

test('two subtitles in one language are two choices, each shows its own', async ({ page }) => {
  const [e1] = await inOrder(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  /* every one is a <track> with its key, and the menu has them in the same order */
  const ids = await v.locator('track').evaluateAll((ts) => ts.map((t) => t.id));
  expect(ids).toEqual(['sub-de', 'sub-en', 'sub-en.sdh', 'sub-sv.forced']);
  const keys = await page.locator('[data-player] [data-sub]').evaluateAll((bs) => bs.map((b) => b.dataset.sub));
  expect(keys).toEqual(['', 'de', 'en', 'en.sdh', 'sv.forced']);
  await expect(page.locator('[data-sub="en.sdh"]')).toHaveText('English (for the hard of hearing)');
  expect(await showing(v)).toBe('');
  await pick(page, 'en.sdh');
  await expect.poll(() => showing(v)).toBe('en.sdh: one, for the hard of hearing');
  await expect(page.locator('[data-sub="en"]')).toHaveAttribute('aria-checked', 'false');
  await pick(page, 'en');
  await expect.poll(() => showing(v)).toBe('en: one');
  await expect(page.locator('[data-sub="en.sdh"]')).toHaveAttribute('aria-checked', 'false');
  await pick(page, '');
  await expect.poll(() => showing(v)).toBe('');
});

/* The next episode comes on in the same player (player.spec.mjs): the pick goes with it */
async function nextEpisode(page, key) {
  const [e1, e2] = await inOrder(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  await pick(page, key);
  await v.evaluate((m) => { m.muted = true; m.currentTime = 97; return m.play(); });
  await expect(page).toHaveURL(new RegExp(`${e2}\\?auto=1$`), { timeout: 15_000 });
  await expect(page.locator('#media-video')).toHaveAttribute('data-id', e2.split('/').pop());
  return page.locator('#media-video');
}

test('the pick goes on to the next episode by its key', async ({ page }) => {
  const v = await nextEpisode(page, 'en.sdh');
  await expect.poll(() => showing(v)).toBe('en.sdh: two, for the hard of hearing');
  await expect(page.locator('[data-sub="en.sdh"]')).toHaveAttribute('aria-checked', 'true');
});

test('... else to the first in its language, else none', async ({ page }) => {
  let v = await nextEpisode(page, 'sv.forced'); /* (the next has "sv" only) */
  await expect.poll(() => showing(v)).toBe('sv: två');
  await expect(page.locator('[data-sub="sv"]')).toHaveAttribute('aria-checked', 'true');
  v = await nextEpisode(page, 'de'); /* (the next has no German) */
  await expect.poll(() => showing(v)).toBe('');
  await expect(page.locator('[data-sub=""]')).toHaveAttribute('aria-checked', 'true');
});

// 1.3 (owner: "if enabled on an episode, carry on to the next episode with the same language if it exists", and
// "c" for subtitles): C turns them off and on again; the pick is the viewer's for the series, so the next
// episode opened afresh - a new page, not the same player - starts with it, and one turned off starts off.
test('C turns subtitles on and off, and the pick follows the series into a new page', async ({ page }) => {
  const [e1, e2] = await inOrder(page);
  await page.goto(e1);
  let v = page.locator('#media-video');
  await ready(v);
  await expect(page.locator('html')).toHaveAttribute('data-ws', 'on');
  await pick(page, '');
  await expect.poll(() => showing(v)).toBe('');
  await page.keyboard.press('c'); /* on again: none shown here before, so the first - German */
  await expect.poll(() => showing(v)).toBe('de: eins');
  await page.keyboard.press('c');
  await expect.poll(() => showing(v)).toBe('');
  await pick(page, 'en.sdh');
  await expect.poll(() => showing(v)).toBe('en.sdh: one, for the hard of hearing');
  await page.goto(e2); /* a new page: the series' pick */
  v = page.locator('#media-video');
  await ready(v);
  await expect.poll(() => showing(v)).toBe('en.sdh: two, for the hard of hearing');
  await expect(page.locator('[data-sub="en.sdh"]')).toHaveAttribute('aria-checked', 'true');
  await expect(page.locator('html')).toHaveAttribute('data-ws', 'on');
  await page.keyboard.press('c'); /* off - and the series stays off */
  await expect.poll(() => showing(v)).toBe('');
  await page.goto(e1);
  v = page.locator('#media-video');
  await ready(v);
  await expect.poll(() => showing(v)).toBe('');
  await expect(page.locator('[data-sub=""]')).toHaveAttribute('aria-checked', 'true');
  await expect(page.locator('[data-player] [data-p="subs"]')).toHaveAttribute('aria-label', 'Subtitles (C)');
});
