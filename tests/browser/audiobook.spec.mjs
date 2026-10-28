// Audiobooks (1.6; web/wire.js "the listener"): Hemma's own controls take over the <audio>, a chapter's link plays
// from there, the place in the book is reported and the page resumes there, a book in parts goes on from part to part
// in the same element, the speed is this device's, the sleep timer stops at the chapter's end, the lock screen (the
// Media Session) knows the book and the chapter, and a page going out of sight (a phone being locked) still sends the
// place - as a plain POST.
import { expect, test } from '@playwright/test';

async function bookPage(page, title) {
  await page.goto('/media/books');
  const href = await page.locator('a[href^="/media/book/"]', { hasText: title }).first().getAttribute('href');
  await page.goto(href);
  return href;
}
const audio = (page) => page.locator('#media-audio');
async function loaded(a) {
  await expect.poll(() => a.evaluate((m) => m.readyState >= 1 && m.duration > 0), { message: 'the book loads' }).toBe(true);
}
const bookAt = (a) => a.evaluate((m) => (m._parts && m._parts[m._part] ? m._parts[m._part].at : 0) + m.currentTime);

test('the listener plays a book by its chapters, and the page resumes where you were', async ({ page }) => {
  await bookPage(page, 'Test Story');
  const a = audio(page);
  await expect(page.locator('[data-listen]')).toBeVisible();
  await loaded(a);
  expect(await a.evaluate((m) => m.controls)).toBe(false); /* (ours, not the browser's) */
  await a.evaluate((m) => { m.muted = true; });
  await page.locator('#media-chapters a', { hasText: 'The Middle' }).click();
  await expect.poll(() => a.evaluate((m) => !m.paused && m.currentTime >= 16), { message: 'it plays from the chapter' }).toBe(true);
  await expect(page.locator('[data-b="chapter"]')).toHaveText('Chapter 1 - The Middle');
  await expect(page.locator('#media-chapters a', { hasText: 'The Middle' })).toHaveAttribute('aria-current', 'true');
  /* the lock screen knows the book and the chapter */
  await expect.poll(() => page.evaluate(() => navigator.mediaSession.metadata && navigator.mediaSession.metadata.title))
    .toBe('Chapter 1 - The Middle');
  expect(await page.evaluate(() => navigator.mediaSession.metadata.artist)).toBe('Ann Author');
  expect(await page.evaluate(() => navigator.mediaSession.metadata.album)).toBe('Test Story');
  await page.locator('[data-b="play"]').click(); /* a pause: reported */
  await expect.poll(async () => {
    await page.reload();
    return Number(await page.locator('#media-audio').getAttribute('data-resume'));
  }, { message: 'the page offers the place to resume at' }).toBeGreaterThanOrEqual(16);
  const b = audio(page);
  await loaded(b);
  expect(await bookAt(b)).toBeGreaterThan(15);
});

test('a book in parts goes on from part to part in the same element', async ({ page }) => {
  await bookPage(page, 'Parted Book');
  const a = audio(page);
  await loaded(a);
  const len = Number(await page.locator('[data-b="seek"]').getAttribute('max'));
  expect(len).toBeGreaterThan(await a.evaluate((m) => m.duration) + 2); /* (the seek bar is the whole book's) */
  await a.evaluate((m) => { m._mark = 'the same element'; m.muted = true; m.currentTime = m.duration - 1; return m.play(); });
  await expect.poll(() => a.evaluate((m) => m._part), { message: 'on to part two' }).toBe(1);
  expect(await a.evaluate((m) => m._mark)).toBe('the same element');
  await expect.poll(() => a.evaluate((m) => !m.paused && m.currentTime > 0), { message: 'it plays' }).toBe(true);
  await expect(page.locator('[data-b="chapter"]')).toHaveText('Part Two');
  await page.locator('[data-b="back"]').click(); /* 15 s back from just into part two: part one */
  await expect.poll(() => a.evaluate((m) => m._part)).toBe(0);
});

test('the speed is this device\'s, and the sleep timer stops at the end of the chapter', async ({ page }) => {
  await bookPage(page, 'Test Story');
  let a = audio(page);
  await loaded(a);
  await page.locator('[data-b="speed"]').click();
  await page.locator('[data-speed="1.5"]').click();
  expect(await a.evaluate((m) => m.playbackRate)).toBe(1.5);
  await page.reload();
  a = audio(page);
  await loaded(a);
  expect(await a.evaluate((m) => m.playbackRate)).toBe(1.5);
  await expect(page.locator('[data-b="speed-t"]')).toHaveText('1.5×');
  await a.evaluate((m) => { m.muted = true; m.currentTime = 13.5; });
  await page.locator('[data-b="sleep"]').click();
  await page.locator('[data-sleep="ch"]').click();
  await page.locator('[data-b="play"]').click();
  await expect.poll(() => a.evaluate((m) => m.paused && m.currentTime > 15), { message: 'it stops', timeout: 10_000 }).toBe(true);
  expect(await a.evaluate((m) => m.currentTime)).toBeLessThan(17.5); /* (the chapter ends at 16 s) */
  await page.evaluate(() => localStorage.removeItem('hemma.speed'));
});

test('a paused book taken off the page leaves the lock screen, its buttons and its file', async ({ page }) => {
  /* (the lock screen's play, a keyboard's play key or the computer's media controls played it on, out of sight) */
  await page.addInitScript(() => {
    const ms = navigator.mediaSession, set = ms.setActionHandler.bind(ms);
    window.hemmaHandlers = {};
    ms.setActionHandler = (what, fn) => { window.hemmaHandlers[what] = fn; set(what, fn); };
  });
  await bookPage(page, 'Test Story');
  const a = audio(page);
  await loaded(a);
  await a.evaluate((m) => { m.muted = true; return m.play(); });
  await expect.poll(() => page.evaluate(() => !!navigator.mediaSession.metadata)).toBe(true);
  await page.locator('[data-b="play"]').click(); /* paused, on the page */
  await a.evaluate((m) => { window.hemmaOld = m; });
  await page.locator('a[href="/media/books"]:visible').first().click();
  await expect(page).toHaveURL(/\/media\/books$/);
  expect(await page.evaluate(() => window.hemmaOld.isConnected)).toBe(false); /* (the same document: wire.js's nav) */
  expect(await page.evaluate(() => navigator.mediaSession.metadata)).toBe(null);
  expect(await page.evaluate(() => typeof window.hemmaHandlers.play)).not.toBe('function');
  expect(await page.evaluate(() => { const m = window.hemmaOld; return [m.hasAttribute('src'), m.networkState, m.paused]; }))
    .toEqual([false, 0, true]);
});

test('a book playing when another page comes keeps its place', async ({ page }) => {
  /* (letting go of the file sets the element's time to 0 - that must not be reported as the place) */
  const href = await bookPage(page, 'Test Story');
  const a = audio(page);
  await loaded(a);
  await a.evaluate((m) => { m.muted = true; m.currentTime = 22; return m.play(); });
  await expect.poll(() => a.evaluate((m) => !m.paused && m.currentTime > 22.5)).toBe(true);
  await page.locator('a[href="/media/books"]:visible').first().click();
  await expect(page).toHaveURL(/\/media\/books$/);
  await page.waitForTimeout(1000);
  await page.goto(href);
  expect(Number(await audio(page).getAttribute('data-resume'))).toBeGreaterThanOrEqual(22);
});

test('a page going out of sight sends the place as a plain POST', async ({ page }) => {
  await bookPage(page, 'Test Story');
  const a = audio(page);
  await loaded(a);
  const posted = page.waitForRequest((r) => r.method() === 'POST' && r.url().endsWith('/a/media.progress'));
  await a.evaluate((m) => { m.muted = true; m.currentTime = 3; return m.play(); });
  await page.evaluate(() => {
    Object.defineProperty(document, 'visibilityState', { value: 'hidden', configurable: true });
    document.dispatchEvent(new Event('visibilitychange'));
  });
  const r = await posted;
  expect(r.postData()).toMatch(/pos=[3-5]/);
});
