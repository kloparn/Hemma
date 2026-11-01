// What your devices play (2.5, media/devices.c): a browser that opens Watch answers the page's list of media types
// (canPlayType, one letter each) as the live media.caps; Library settings then lists it with what it plays.
import { expect, test } from '@playwright/test';

test('a browser that opens Watch says what it plays', async ({ page }) => {
  await page.goto('/media');
  const list = await page.locator('[data-caps]').getAttribute('data-caps');
  expect(list.split('|').length).toBeGreaterThan(10);
  expect(list).toContain('video/mp4; codecs="avc1.640028"');
  // the answer travels over the socket: Library settings shows it once it's there
  await expect.poll(async () => {
    await page.goto('/media/settings/devices');
    return page.locator('#media-devices li').count();
  }, { message: 'this browser listed as a device', timeout: 15_000 }).toBeGreaterThan(0);
  const devices = page.locator('#media-devices');
  await expect(devices).toContainText('Anna'); /* (signed in by setup.mjs: its browser has no name the session knows) */
  await expect(devices).toContainText(/plays (them all|\d+ of \d+)/);
  await page.goto('/media/settings'); /* (2.7: the counts are Overview's) */
  await expect(page.locator('.media-stats')).toContainText('Play on your devices');
});
