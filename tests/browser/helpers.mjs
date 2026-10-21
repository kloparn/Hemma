// What the tests share: finding the test videos' pages, waiting for a video, reading where it is.
import { expect } from '@playwright/test';
import { BASE } from './where.mjs';

/* The test series' episode pages, in order: ["/media/watch/<id>", ...] */
export async function episodes(page) {
  await page.goto('/media/series');
  const series = await page.locator('a[href^="/media/series/"]', { hasText: 'Testshow' }).first().getAttribute('href');
  await page.goto(series);
  const hrefs = await page.locator('a[href^="/media/watch/"]').evaluateAll((as) => as.map((a) => a.getAttribute('href')));
  return [...new Set(hrefs.map((h) => h.split('?')[0]))];
}

/* The test film's page */
export async function film(page) {
  await page.goto('/media/movies');
  return page.locator('a[href^="/media/watch/"]', { hasText: 'Clip' }).first().getAttribute('href');
}

/* Once the video knows its length (it can be seeked) */
export async function ready(video) {
  await expect.poll(() => video.evaluate((m) => m.readyState >= 1 && m.duration > 0), { message: 'the video loads' }).toBe(true);
}

export const at = (video) => video.evaluate((m) => m.currentTime);

/* An action, as a page's form sends it without the wire (POST /a/<action>, same origin) */
export async function act(page, action, form) {
  const r = await page.request.post(`/a/${action}`, { form, headers: { Origin: BASE, Referer: `${BASE}/` } });
  expect(r.ok(), action).toBe(true);
  return r;
}
