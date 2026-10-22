// Downloads (1.1): a film's Download button saves the film - named for people, the size the server said - and
// a series comes as one zip, a real one: it starts like a zip and its central directory lists the episodes.
import fs from 'node:fs';
import { expect, test } from '@playwright/test';
import { episodes, film } from './helpers.mjs';

/* The names in a zip's central directory (the end record says where it is; the clips are small: no zip64) */
function zipNames(buf) {
  const end = buf.length - 22;
  expect(buf.readUInt32LE(end), 'an end of central directory record').toBe(0x06054b50);
  const count = buf.readUInt16LE(end + 10), size = buf.readUInt32LE(end + 12);
  let p = buf.readUInt32LE(end + 16);
  expect(p + size, 'the central directory ends at the end record').toBe(end);
  const names = [];
  for (let i = 0; i < count; i++) {
    expect(buf.readUInt32LE(p), 'a central directory header').toBe(0x02014b50);
    const nl = buf.readUInt16LE(p + 28), xl = buf.readUInt16LE(p + 30), cl = buf.readUInt16LE(p + 32);
    names.push(buf.toString('utf8', p + 46, p + 46 + nl));
    p += 46 + nl + xl + cl;
  }
  return names;
}

test("a film's Download button saves the film", async ({ page }) => {
  await page.goto(await film(page));
  const button = page.locator('a#media-dl');
  await expect(button).toHaveText(/^Download \(\d+ [KM]B\)$/);
  const [dl] = await Promise.all([page.waitForEvent('download'), button.click()]);
  expect(dl.suggestedFilename()).toBe('Clip (2020).mp4');
  const head = await page.request.head(await button.getAttribute('href'));
  expect(head.ok()).toBe(true);
  expect(fs.statSync(await dl.path()).size).toBe(Number(head.headers()['content-length']));
});

test('a series downloads as one zip of its episodes', async ({ page }) => {
  const [e1] = await episodes(page); /* (on the series page now) */
  const all = page.locator('#media-dl a[href^="/media/zip/"]').last();
  await expect(all).toHaveText(/^Download all \(\d+ [KM]B\)$/);
  const [dl] = await Promise.all([page.waitForEvent('download'), all.click()]);
  expect(dl.suggestedFilename()).toBe('Testshow.zip');
  const zip = fs.readFileSync(await dl.path());
  expect(zip.subarray(0, 4).equals(Buffer.from('PK\x03\x04', 'latin1')), 'it starts like a zip').toBe(true);
  expect(zipNames(zip)).toEqual(['Testshow/Season 1/Testshow S01E01.mp4', 'Testshow/Season 1/Testshow S01E02.mp4']);
  /* one season by its address: the length up front, and the same archive */
  const one = await page.request.get(`${await all.getAttribute('href')}?s=1`);
  expect(one.headers()['content-type']).toBe('application/zip');
  expect(Number(one.headers()['content-length'])).toBe((await one.body()).length);
  expect(zipNames(await one.body())).toEqual(zipNames(zip));
  /* and each episode on its own, from its page */
  await page.goto(e1);
  await expect(page.locator('a#media-dl')).toHaveAttribute('href', /\?download=1$/);
});
