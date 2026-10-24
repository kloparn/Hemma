// The upload client (web/wire.js upload(); issues #49 and Hemma 1.1's easier uploads): files dropped or picked
// on the upload page are previewed - where each one goes, sorted by its name - and Start sends them there,
// byte for byte.
import fs from 'node:fs';
import path from 'node:path';
import { expect, test } from '@playwright/test';
import { act } from './helpers.mjs';
import { LIBRARY, WORK } from './where.mjs';

const CLIP = path.join(WORK, 'upload', 'Upload test (2021).mp4'); /* (made by server.mjs: build/test av_test_clips) */
/* an MKV the browser's converter takes: H.264, subtitles as WebVTT (English), PGS (English, "English") and VobSub
   (French, forced) - one of the converter's fuzz seeds (server.mjs) */
const MKV = path.join(WORK, 'mkv', 'h264-webvtt-pgs-vobsub.mkv');
const SUBS = ['.en.srt', '.en.2.sup', '.fr.forced.idx', '.fr.forced.sub']; /* (what its tracks are saved as) */
const same = (a, b) => fs.readFileSync(a).equals(fs.readFileSync(b));
const there = (dir, names) => names.filter((n) => fs.existsSync(path.join(dir, n))).length;

/* The MKV under another name, picked on the upload page; skipped where Hemma was built without the converter */
async function pickMkv(page, name) {
  const from = path.join(WORK, 'mkv-up', name);
  fs.mkdirSync(path.dirname(from), { recursive: true });
  fs.copyFileSync(MKV, from);
  await page.goto('/media/upload');
  test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await page.locator('input[name=files]').setInputFiles(from);
}

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

// The converter in the browser (web/conv.js, av.wasm): an MKV becomes a play-everywhere copy on the sending
// device and only the copy travels, with the subtitles it carried saved beside it.
test('an MKV is converted here: only its copy goes, and its subtitles with it', async ({ page }) => {
  const dir = path.join(LIBRARY, 'Series', 'Mkvshow', 'Season 1');
  await pickMkv(page, 'Mkvshow S01E01.mkv');
  await expect(page.locator('.media-up-group', { hasText: 'Mkvshow' })).toContainText('Mkvshow · S01E01');
  await page.locator('#media-up-start').click();
  const want = ['Mkvshow S01E01.web.mp4', ...SUBS.map((s) => `Mkvshow S01E01${s}`)];
  await expect.poll(() => there(dir, want), { message: 'the copy and its subtitles arrive', timeout: 30_000 }).toBe(want.length);
  expect(fs.existsSync(path.join(dir, 'Mkvshow S01E01.mkv'))).toBe(false); /* (the original stays home) */
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
});

// 1.3: with the memory box ticked the copy waits in memory and goes in pieces (?at=&end=) - and arrives the same.
test('a copy kept in memory goes in pieces and arrives whole', async ({ page }) => {
  const dir = path.join(LIBRARY, 'Series', 'Ramshow', 'Season 1');
  await pickMkv(page, 'Ramshow S01E01.mkv');
  await page.locator('input[name=ram]').check();
  await expect(page.locator('.media-up-group', { hasText: 'Ramshow' })).toContainText('Ramshow · S01E01');
  await page.locator('#media-up-form').evaluate((f) => { f.dataset.part = '1000'; }); /* (tiny pieces: several) */
  await page.locator('#media-up-start').click();
  const copy = path.join(dir, 'Ramshow S01E01.web.mp4');
  await expect.poll(() => there(dir, ['Ramshow S01E01.web.mp4', 'Ramshow S01E01.en.srt']),
    { message: 'the copy arrives', timeout: 30_000 }).toBe(2);
  expect(fs.statSync(copy).size).toBeGreaterThan(3000); /* (more than a few pieces) */
  expect(same(copy, path.join(LIBRARY, 'Series', 'Mkvshow', 'Season 1', 'Mkvshow S01E01.web.mp4'))).toBe(true);
  await page.goto('/media/upload'); /* the box is remembered */
  await expect(page.locator('input[name=ram]')).toBeChecked();
  await page.locator('input[name=ram]').uncheck();
});

// 1.3: an episode the library has stays behind - but the subtitles it carries and the library's lacks go, read
// out of it here, and land next to the library's own video.
test('a video the library has gives only the subtitles it lacks', async ({ page }) => {
  const dir = path.join(LIBRARY, 'Series', 'Subshow', 'Season 1');
  fs.mkdirSync(dir, { recursive: true });
  fs.copyFileSync(CLIP, path.join(dir, 'Subshow S01E02.mp4'));
  await act(page, 'media.rescan', {});
  await expect.poll(async () => (await (await page.request.get('/media/series')).text()).includes('Subshow'),
    { message: 'the library has the episode', timeout: 30_000 }).toBe(true);
  await pickMkv(page, 'Subshow S01E02 720p.mkv');
  await expect(page.locator('.media-up-pv')).toContainText('Only its subtitles');
  await page.locator('#media-up-start').click();
  const want = SUBS.map((s) => `Subshow S01E02${s}`);
  await expect.poll(() => there(dir, want), { message: 'its subtitles arrive', timeout: 30_000 }).toBe(want.length);
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
  expect(fs.readdirSync(dir).filter((n) => /720p|web\.mp4/.test(n))).toEqual([]); /* (no video went) */
});

// 1.3: copies and files that go as they are, in one upload - each sent when it's ready, all of them arrive.
test('copies made here and files sent as they are go in one upload', async ({ page }) => {
  const from = path.join(WORK, 'upload-mixed', 'Mixshow');
  fs.mkdirSync(path.join(from, 'Season 1'), { recursive: true });
  for (const e of ['01', '03']) fs.copyFileSync(MKV, path.join(from, 'Season 1', `Mixshow S01E${e}.mkv`));
  fs.copyFileSync(CLIP, path.join(from, 'Season 1', 'Mixshow S01E02.mp4'));
  fs.writeFileSync(path.join(from, 'Season 1', 'Mixshow S01E02.sv.srt'), '1\n00:00:01,000 --> 00:00:02,000\nHej\n');
  await page.goto('/media/upload');
  test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await page.locator('input[name=folder]').setInputFiles(from);
  await expect(page.locator('.media-up-sum')).toContainText('4 files');
  await page.locator('#media-up-start').click();
  const dir = path.join(LIBRARY, 'Series', 'Mixshow', 'Season 1');
  const want = ['Mixshow S01E01.web.mp4', 'Mixshow S01E02.mp4', 'Mixshow S01E02.sv.srt', 'Mixshow S01E03.web.mp4',
    'Mixshow S01E01.en.srt', 'Mixshow S01E03.fr.forced.sub'];
  await expect.poll(() => there(dir, want), { message: 'everything arrives', timeout: 30_000 }).toBe(want.length);
  await expect(page.locator('.media-up h3')).toContainText('Finished · 4 files sent');
  expect(same(path.join(dir, 'Mixshow S01E02.mp4'), CLIP)).toBe(true);
});

// 1.3: DTS sound (Blu-ray rips) is converted in the browser too - the copy goes, not the original.
test('an MKV with DTS sound is converted here as well', async ({ page }) => {
  const from = path.join(WORK, 'mkv-up', 'Dtsshow S01E01.mkv');
  fs.mkdirSync(path.dirname(from), { recursive: true });
  fs.copyFileSync(path.join(WORK, 'mkv', 'h264-dts.mkv'), from);
  await page.goto('/media/upload');
  test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await page.locator('input[name=files]').setInputFiles(from);
  await expect(page.locator('.media-up-group', { hasText: 'Dtsshow' })).toContainText('Dtsshow · S01E01');
  await page.locator('#media-up-start').click();
  const dir = path.join(LIBRARY, 'Series', 'Dtsshow', 'Season 1');
  await expect.poll(() => fs.existsSync(path.join(dir, 'Dtsshow S01E01.web.mp4')), { message: 'the copy arrives', timeout: 30_000 }).toBe(true);
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
  await expect(page.locator('.media-up')).not.toContainText('sent as it is');
  expect(fs.existsSync(path.join(dir, 'Dtsshow S01E01.mkv'))).toBe(false);
});
