// The upload client (web/wire.js upload(); issues #49 and Hemma 1.1's easier uploads): files dropped or picked
// on the upload page are previewed - where each one goes, sorted by its name - and Start sends them there,
// byte for byte.
import fs from 'node:fs';
import path from 'node:path';
import { expect, test } from '@playwright/test';
import { LIBRARY, WORK } from './where.mjs';

const CLIP = path.join(WORK, 'upload', 'Upload test (2021).mp4'); /* (made by server.mjs: build/test av_test_clips) */
const same = (a, b) => fs.readFileSync(a).equals(fs.readFileSync(b));

test('a dropped episode is previewed in its series folder, and Start puts it there', async ({ page }) => {
  const name = 'Newshow S01E01.mp4', to = path.join(LIBRARY, 'Series', 'Newshow', 'Season 1', name);
  await page.goto('/media/upload');
  const zone = page.locator('[data-drop]');
  await expect(zone).toBeVisible();
  // a drop as the browser makes it: a DataTransfer holding the file
  /* global DataTransfer, File -- (in the page) */
  const dt = await page.evaluateHandle(({ bytes, n }) => {
    const d = new DataTransfer();
    d.items.add(new File([new Uint8Array(bytes)], n, { type: 'video/mp4' }));
    return d;
  }, { bytes: [...fs.readFileSync(CLIP)], n: name });
  await zone.dispatchEvent('dragenter', { dataTransfer: dt });
  await expect(zone).toHaveAttribute('data-over', '');
  await zone.dispatchEvent('drop', { dataTransfer: dt });
  await expect(zone).not.toHaveAttribute('data-over', '');
  const group = page.locator('.media-up-group', { hasText: 'Newshow' });
  await expect(group.locator('.media-up-dir > span')).toHaveText(/Series\s*\/\s*Newshow\s*\/\s*Season 1/);
  await expect(group).toContainText(name);
  await expect(group).toContainText('Newshow · S01E01');
  await expect(page.locator('.media-up-sum')).toContainText('1 file to send');
  await page.locator('#media-up-start').click();
  await expect.poll(() => fs.existsSync(to), { message: 'it arrives', timeout: 30_000 }).toBe(true);
  expect(same(to, CLIP)).toBe(true);
  await expect(page.locator('.media-up-sum')).toHaveCount(0); /* (the preview gives way to the upload's card) */
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
});

test('a picked folder keeps its subtitles and poster with the show', async ({ page }) => {
  const from = path.join(WORK, 'upload-folder'), show = path.join(from, 'Foldershow');
  fs.mkdirSync(path.join(show, 'Season 2'), { recursive: true });
  fs.copyFileSync(CLIP, path.join(show, 'Season 2', 'Foldershow S02E03.mp4'));
  fs.writeFileSync(path.join(show, 'Season 2', 'Foldershow S02E03.en.srt'), '1\n00:00:01,000 --> 00:00:02,000\nHi\n');
  fs.writeFileSync(path.join(show, 'poster.jpg'), 'not really a picture');
  await page.goto('/media/upload');
  await page.locator('input[name=folder]').setInputFiles(show);
  const season = page.locator('.media-up-group', { hasText: 'Season 2' });
  await expect(season.locator('.media-up-dir > span')).toHaveText(/Series\s*\/\s*Foldershow\s*\/\s*Season 2/);
  await expect(season).toContainText('Subtitles for Foldershow · S02E03');
  await expect(page.locator('.media-up-group', { hasText: 'poster.jpg' }).locator('.media-up-dir > span'))
    .toHaveText(/Series\s*\/\s*Foldershow$/);
  await page.locator('#media-up-start').click();
  const into = path.join(LIBRARY, 'Series', 'Foldershow');
  const arrived = ['poster.jpg', 'Season 2/Foldershow S02E03.en.srt', 'Season 2/Foldershow S02E03.mp4'];
  await expect.poll(() => arrived.filter((f) => fs.existsSync(path.join(into, f))).length,
    { message: 'all three arrive', timeout: 30_000 }).toBe(3);
  expect(same(path.join(into, 'Season 2', 'Foldershow S02E03.mp4'), CLIP)).toBe(true);
});

test('"keep the folders\' own layout" puts a file where it was asked to go', async ({ page }) => {
  const to = path.join(LIBRARY, 'Uploads', path.basename(CLIP));
  await page.goto('/media/upload');
  await page.locator('input[name=files]').setInputFiles(CLIP);
  await expect(page.locator('.media-up-group .media-up-dir > span')).toHaveText(/Movies\s*\/\s*Upload test \(2021\)/);
  await page.locator('#media-up-more summary').click();
  await page.locator('input[name=keep]').check();
  await page.locator('input[name=into]').fill('Uploads');
  await expect(page.locator('.media-up-group .media-up-dir > span')).toHaveText(/^\s*Uploads\s*$/);
  await page.locator('#media-up-start').click();
  await expect.poll(() => fs.existsSync(to), { message: 'it arrives', timeout: 30_000 }).toBe(true);
  expect(same(to, CLIP)).toBe(true);
});
