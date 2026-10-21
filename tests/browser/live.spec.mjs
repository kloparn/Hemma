// Live TV (web/wire.js onair(); issue #49): a channel plays where its programme is now and can't be paused,
// wound or sped up - it jumps back to now. Without wire.js the address's #t= still starts it at now.
// Each test makes a channel of its own: its first programme starts the moment it's made (a 100 s episode).
import { expect, test } from '@playwright/test';
import { act, at, ready } from './helpers.mjs';

async function channel(page, number) {
  await act(page, 'tv.save', { number: String(number), name: `Test ${number}`, all: '1', order: 'rotate', align: 'none' });
  return Date.now();
}

test('Live TV plays where the programme is now, and keeps it there', async ({ page }) => {
  const made = await channel(page, 21);
  const now = () => (Date.now() - made) / 1000;
  await page.waitForTimeout(3000);
  await page.goto('/tv/21');
  const v = page.locator('#tv-video');
  await ready(v);
  await expect.poll(() => v.evaluate((m) => !m.paused && m.currentTime > 0), { message: 'it plays' }).toBe(true);
  expect(Math.abs((await at(v)) - now())).toBeLessThan(2.5);

  await v.evaluate((m) => m.pause()); /* no pausing: it plays on */
  await expect.poll(() => v.evaluate((m) => m.paused)).toBe(false);
  await v.evaluate((m) => { m.currentTime = 0; }); /* no winding: back to now */
  await expect.poll(async () => Math.abs((await at(v)) - now())).toBeLessThan(2.5);
  await v.evaluate((m) => { m.playbackRate = 2; }); /* no hurrying */
  await expect.poll(() => v.evaluate((m) => m.playbackRate)).toBe(1);
});

test('without wire.js, Live TV still starts where the programme is now', async ({ page }) => {
  const made = await channel(page, 22);
  await page.waitForTimeout(4000);
  await page.route('**/static/wire.js*', (r) => r.abort());
  await page.goto('/tv/22');
  const v = page.locator('#tv-video');
  await expect(v).toHaveAttribute('src', /#t=\d+$/);
  await ready(v);
  expect(Math.abs((await at(v)) - (Date.now() - made) / 1000)).toBeLessThan(3);
});
