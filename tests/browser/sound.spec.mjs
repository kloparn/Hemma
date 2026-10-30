// Sound languages (2.2): a film with two sound tracks (av_test_clips' "Two Voices (2024)": English at 495 Hz, the
// default, and Svenska at 990 Hz, as the converter writes a copy). Hemma asks a viewer's language once; the player's
// Language menu ([data-snd]) switches the same element to the same film with another sound (?a=<n>), from the same
// place, playing on; what the server sends then holds that one sound track - a download keeps both.
import { expect, test } from '@playwright/test';
import { act, at, ready } from './helpers.mjs';

async function twoVoices(page) {
  await page.goto('/media/movies');
  return page.locator('a[href^="/media/watch/"]', { hasText: 'Two Voices' }).first().getAttribute('href');
}

/* How many sound tracks ('soun' handlers) the header of what an address sends describes */
const sounds = (page, src) => page.evaluate(async (u) => {
  const b = new Uint8Array(await (await fetch(u, { headers: { Range: 'bytes=0-262143' } })).arrayBuffer());
  let s = '';
  for (const c of b) s += String.fromCharCode(c);
  let n = 0;
  for (let i = s.indexOf('hdlr'); i >= 0; i = s.indexOf('hdlr', i + 4)) if (s.substr(i + 12, 4) === 'soun') n++;
  return n;
}, src);

test('asks the language once, then plays it', async ({ page }) => {
  const href = await twoVoices(page);
  expect(href, 'the two-voice film is in the library').toBeTruthy();
  await act(page, 'media.soundlang', { lang: '' }); /* (Ask me) */
  await page.goto(href);
  const dialog = page.locator('.modal[role="dialog"]');
  await expect(dialog).toContainText('Which language do you want to watch in?');
  await dialog.getByRole('button', { name: 'Svenska' }).click();
  await expect(dialog).toHaveCount(0);
  const v = page.locator('#media-video');
  await expect(v).toHaveAttribute('src', /\?a=1/);
  expect(await sounds(page, await v.getAttribute('src')), 'the Swedish view').toBe(1);
  expect(await sounds(page, `${href.replace('/media/watch/', '/media/file/')}?download=1`), 'a download keeps both').toBe(2);
  await page.reload();
  await expect(page.locator('.modal[role="dialog"]')).toHaveCount(0);
  await expect(page.locator('[data-snd="1"]')).toHaveAttribute('aria-checked', 'true');
});

test('the Language menu switches the sound from the same place, playing on', async ({ page }) => {
  await act(page, 'media.soundlang', { lang: 'en' });
  const href = await twoVoices(page);
  await act(page, 'media.sound', { id: href.split('/').pop(), n: '0' }); /* (no pick left for the film) */
  await page.goto(href);
  const v = page.locator('#media-video');
  await expect(v).toHaveAttribute('src', /\?a=0/);
  await ready(v);
  await v.evaluate((m) => { m.muted = true; m.currentTime = 12; return m.play(); });
  await expect.poll(() => at(v)).toBeGreaterThan(12.2);
  const pl = page.locator('[data-player="media-video"]');
  /* the menu by clicks the page gets, without moving a mouse over the picture: in the test browser a hover over a
     playing test clip sends it to its end (the one-sound "Clip" too - not this feature's) */
  const press = (sel) => page.evaluate((s) => document.querySelector(`[data-player="media-video"] ${s}`).click(), sel);
  await press('[data-p="snd"]');
  await expect(pl.locator('[data-snd]')).toHaveText(['English', 'Svenska']);
  await press('[data-snd="1"]');
  await expect(pl.locator('[data-snd="1"]')).toHaveAttribute('aria-checked', 'true');
  await expect(v).toHaveAttribute('src', /\?a=1/);
  await ready(v);
  await expect.poll(() => at(v), { message: 'on from where it was' }).toBeGreaterThan(12);
  expect(await at(v)).toBeLessThan(25);
  await expect.poll(() => v.evaluate((m) => !m.paused), { message: 'still playing' }).toBe(true);
  /* the pick is kept for the film: a new visit plays Swedish, with English as the language elsewhere */
  await expect.poll(async () => {
    await page.goto(href);
    return page.locator('#media-video').getAttribute('src');
  }, { message: 'the pick kept' }).toMatch(/\?a=1/);
  await act(page, 'media.sound', { id: href.split('/').pop(), n: '0' });
});
