// The player's household rules (web/wire.js media(); issue #49: they lived only in JavaScript, untested):
// where you are is reported as you watch (every 15 s of the video, and at a pause), the page resumes there -
// also without wire.js (the address's #t=) - but not in the last 5 seconds, and the next episode comes on
// in the same player (so a full screen stays full screen).
import { expect, test } from '@playwright/test';
import { at, episodes, film, ready } from './helpers.mjs';

test('the player reports where you are, and the page resumes there', async ({ page }) => {
  const [e1] = await episodes(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  await v.evaluate((m) => { m.muted = true; m.currentTime = 20; return m.play(); }); /* (15 s on: a report) */
  await page.waitForTimeout(1500);
  await v.evaluate((m) => m.pause());
  await expect.poll(async () => {
    await page.reload();
    return page.locator('#media-video').getAttribute('data-resume');
  }, { message: 'the page offers the spot to resume at' }).toMatch(/^2[0-2]$/);
  const w = page.locator('#media-video');
  await expect(w).toHaveAttribute('src', /#t=2[0-2]$/);
  await ready(w);
  expect(await at(w)).toBeGreaterThan(19);
});

test('without wire.js the page still resumes, from the address', async ({ page }) => {
  const [e1] = await episodes(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  await v.evaluate((m) => { m.muted = true; m.currentTime = 25; return m.play(); });
  await page.waitForTimeout(1000);
  await v.evaluate((m) => m.pause());
  await page.waitForTimeout(500);
  await page.route('**/static/wire.js*', (r) => r.abort());
  await page.goto(e1);
  const w = page.locator('#media-video');
  await expect(w).toHaveAttribute('src', /#t=2[4-7]$/);
  await ready(w);
  expect(await at(w)).toBeGreaterThan(23);
});

test('a paused video lets go of its file when another page comes', async ({ page }) => {
  /* (it kept it: a keyboard's play key or the computer's media controls played it on, out of sight) */
  const [e1] = await episodes(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  await v.evaluate((m) => { m.muted = true; return m.play(); });
  await page.waitForTimeout(500);
  await v.evaluate((m) => { m.pause(); window.hemmaOld = m; });
  await page.locator('a[href="/media"]:visible').first().click();
  await expect(page).toHaveURL(/\/media$/);
  expect(await page.evaluate(() => window.hemmaOld.isConnected)).toBe(false); /* (the same document: wire.js's nav) */
  expect(await page.evaluate(() => { const m = window.hemmaOld; return [m.hasAttribute('src'), m.networkState]; }))
    .toEqual([false, 0]); /* NETWORK_EMPTY: nothing left to play */
});

test('a spot in the last 5 seconds starts the video from the top', async ({ page }) => {
  const clip = await film(page);
  await page.goto(clip);
  const v = page.locator('#media-video');
  await ready(v);
  await v.evaluate((m) => { m.muted = true; m.currentTime = 35.3; return m.play(); }); /* (of 40: not yet "watched") */
  await page.waitForTimeout(400);
  await v.evaluate((m) => m.pause());
  await page.waitForTimeout(500);
  await page.reload();
  const w = page.locator('#media-video');
  await expect(w).not.toHaveAttribute('data-resume', /./);
  await ready(w);
  expect(await at(w)).toBeLessThan(1);
});

test('the next episode comes on in the same player', async ({ page }) => {
  const [e1, e2] = await episodes(page);
  await page.goto(e1);
  const v = page.locator('#media-video');
  await ready(v);
  await v.evaluate((m) => { m._mark = 'the same element'; m.muted = true; m.currentTime = 94; return m.play(); });
  const card = page.locator('[data-upnext]');
  await expect(card).toBeVisible(); /* (its last 30 seconds; only for a video over 90) */
  await expect(card.locator('[data-count]')).toHaveText(/^\d+$/);
  await expect(page).toHaveURL(new RegExp(`${e2}\\?auto=1$`), { timeout: 15_000 });
  const w = page.locator('#media-video');
  expect(await w.evaluate((m) => m._mark)).toBe('the same element'); /* morphed, not rebuilt: full screen stays */
  await expect(w).toHaveAttribute('data-id', e2.split('/').pop());
  await expect.poll(() => w.evaluate((m) => !m.paused && m.currentTime > 0), { message: 'it plays' }).toBe(true);
});
