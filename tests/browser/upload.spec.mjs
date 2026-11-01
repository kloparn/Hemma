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

// 1.3: a read of the original that fails (a browser short of memory: "readAsArrayBuffer ... could not be read" sent
// 13 episodes up unconverted) puts the job back in line - the copy still goes, not the original.
test('a conversion whose reads failed is tried again, and its copy goes', async ({ page }) => {
  const dir = path.join(LIBRARY, 'Series', 'Flakyshow', 'Season 1');
  await pickMkv(page, 'Flakyshow S01E01.mkv');
  await expect(page.locator('.media-up-group', { hasText: 'Flakyshow' })).toContainText('Flakyshow · S01E01');
  await page.locator('#media-up-form').evaluate((f) => { f.dataset.flaky = '2'; }); /* (its first try loses both reads) */
  await page.locator('#media-up-start').click();
  await expect.poll(() => there(dir, ['Flakyshow S01E01.web.mp4', 'Flakyshow S01E01.en.srt']),
    { message: 'the copy arrives', timeout: 30_000 }).toBe(2);
  expect(fs.existsSync(path.join(dir, 'Flakyshow S01E01.mkv'))).toBe(false); /* (not the original) */
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
  await expect(page.locator('.media-up-asis')).toHaveCount(0);
});

// 1.4: a network that's gone for a while is waited out (a PC's Wi-Fi dropped for ten minutes at night, and the
// upload stopped after a minute of tries): sends that got no answer at all aren't tries, and the file arrives whole.
test('an upload waits out a dropped network', async ({ page }) => {
  const from = path.join(WORK, 'netdrop', 'Netdrop (2022).mp4'), to = path.join(LIBRARY, 'Movies', 'Netdrop (2022)', 'Netdrop (2022).mp4');
  fs.mkdirSync(path.dirname(from), { recursive: true });
  fs.copyFileSync(CLIP, from);
  let cut = 12; /* (more than the 8 tries a send gets when the server answers) */
  await page.route(/\/media\/upload\/[^/?]+\/\d+/, (route) =>
    route.request().method() === 'PUT' && cut-- > 0 ? route.abort('internetdisconnected') : route.continue());
  await page.goto('/media/upload');
  await page.locator('input[name=files]').setInputFiles(from);
  await expect(page.locator('.media-up-group .media-up-dir > span')).toHaveText(/Movies\s*\/\s*Netdrop \(2022\)/);
  await page.locator('#media-up-form').evaluate((f) => { f.dataset.netpause = '20'; }); /* (ask again at once) */
  await page.locator('#media-up-start').click();
  await expect.poll(() => fs.existsSync(to), { message: 'it arrives', timeout: 30_000 }).toBe(true);
  expect(cut).toBeLessThan(0);
  expect(same(to, from)).toBe(true);
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
});

// 2.6: Hemma asks its open pages to load again when it changed (a new version, at the socket's return) - a page that
// was uploading did, and the browser's "leave the page?" question stopped it, a night's converting with it, until someone
// answered. It loads again once the upload is done.
test('a page asked to load again while uploading does so after the upload', async ({ page }) => {
  const from = path.join(WORK, 'reload', 'Reloadfilm (2023).mp4');
  const to = path.join(LIBRARY, 'Movies', 'Reloadfilm (2023)', 'Reloadfilm (2023).mp4');
  fs.mkdirSync(path.dirname(from), { recursive: true });
  fs.copyFileSync(CLIP, from);
  let sock = null, held = 0, release;
  const hold = new Promise((r) => { release = r; });
  await page.routeWebSocket(/\/ws$/, (ws) => { sock = ws; ws.connectToServer(); });
  await page.route(/\/media\/upload\/[^/?]+\/\d+/, async (route) => {
    if (route.request().method() === 'PUT') { held++; await hold; }
    await route.continue();
  });
  const asked = [];
  page.on('dialog', (d) => { asked.push(d.type()); d.dismiss(); });
  await page.goto('/media/upload');
  await page.locator('input[name=files]').setInputFiles(from);
  await expect(page.locator('.media-up-group .media-up-dir > span')).toHaveText(/Movies\s*\/\s*Reloadfilm \(2023\)/);
  await page.evaluate(() => { globalThis.hemmaSame = 1; });
  await page.locator('#media-up-start').click();
  await expect.poll(() => held, { message: 'the file is being sent' }).toBeGreaterThan(0);
  sock.send(JSON.stringify([['reload'], ['toast', 0, 'Reload asked']])); /* (a toast after it: the reload was seen) */
  await expect(page.locator('#toasts')).toContainText('Reload asked');
  expect(asked).toEqual([]);
  expect(await page.evaluate(() => globalThis.hemmaSame)).toBe(1);
  release();
  await expect.poll(() => fs.existsSync(to), { message: 'it arrives', timeout: 30_000 }).toBe(true);
  await expect.poll(() => page.evaluate(() => globalThis.hemmaSame).catch(() => 1),
    { message: 'then the page loads again', timeout: 15_000 }).toBeUndefined();
  expect(same(to, from)).toBe(true);
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

// 2.1: an AV1 picture is copied into the MP4 (it went up as it was before, "the converter can't change yet"), and
// the Dolby Digital Plus beside it is made AAC.
test('an MKV with an AV1 picture is converted here too', async ({ page }) => {
  const from = path.join(WORK, 'mkv-up', 'Avshow S01E01.mkv');
  fs.mkdirSync(path.dirname(from), { recursive: true });
  fs.copyFileSync(path.join(WORK, 'mkv', 'av1-eac3.mkv'), from);
  await page.goto('/media/upload');
  test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await page.locator('input[name=files]').setInputFiles(from);
  await expect(page.locator('.media-up-group', { hasText: 'Avshow' })).toContainText('Avshow · S01E01');
  await page.locator('#media-up-start').click();
  const dir = path.join(LIBRARY, 'Series', 'Avshow', 'Season 1'), copy = path.join(dir, 'Avshow S01E01.web.mp4');
  await expect.poll(() => fs.existsSync(copy), { message: 'the copy arrives', timeout: 30_000 }).toBe(true);
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
  await expect(page.locator('.media-up')).not.toContainText('sent as it is');
  expect(fs.existsSync(path.join(dir, 'Avshow S01E01.mkv'))).toBe(false);
  const mp4 = fs.readFileSync(copy).toString('latin1');
  for (const box of ['av01', 'av1C', 'mp4a']) expect(mp4).toContain(box); /* (AV1 copied, the sound made AAC) */
});

// 2.5 (the owner: "the videos on hemma that are not converted"): an XviD picture (MPEG-4 Part 2, B-VOPs, quarter samples)
// plays nowhere - the browser's converter decodes it and makes H.264, its MP3 copied. The copy goes, not the original.
test('an MKV with an XviD picture is converted here too', async ({ page }) => {
  const from = path.join(WORK, 'mkv-up', 'Xvidshow S01E01.mkv');
  fs.mkdirSync(path.dirname(from), { recursive: true });
  fs.copyFileSync(path.join(WORK, 'mkv', 'mpeg4-asp-qpel-mp3.mkv'), from);
  await page.goto('/media/upload');
  test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await page.locator('input[name=files]').setInputFiles(from);
  await expect(page.locator('.media-up-group', { hasText: 'Xvidshow' })).toContainText('Xvidshow · S01E01');
  await page.locator('#media-up-start').click();
  const dir = path.join(LIBRARY, 'Series', 'Xvidshow', 'Season 1'), copy = path.join(dir, 'Xvidshow S01E01.web.mp4');
  await expect.poll(() => fs.existsSync(copy), { message: 'the copy arrives', timeout: 30_000 }).toBe(true);
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
  await expect(page.locator('.media-up')).not.toContainText('sent as it is');
  expect(fs.existsSync(path.join(dir, 'Xvidshow S01E01.mkv'))).toBe(false);
  const mp4 = fs.readFileSync(copy).toString('latin1');
  for (const box of ['avc1', 'avcC', 'mp4a']) expect(mp4).toContain(box); /* (made H.264, the MP3 copied) */
});

// 2.7 (the owner: "Yes, off by default"): with "Make HEVC videos play everywhere too" ticked, an HEVC upload is made
// H.264 on the sending computer - one pass, not up, back down and up again with "On this device" afterwards. Without the
// box (the 2.6 default) the HEVC picture is copied as it is: 'hvc1'.
test('an HEVC upload is made H.264 when the box is ticked', async ({ page }) => {
  const run = async (name, tick) => {
    const from = path.join(WORK, 'mkv-up', `${name} S01E01.mkv`);
    fs.mkdirSync(path.dirname(from), { recursive: true });
    fs.copyFileSync(path.join(WORK, 'mkv', 'hevc-random-10bit-aac.mkv'), from);
    await page.goto('/media/upload');
    test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
    await page.locator('input[name=files]').setInputFiles(from);
    await expect(page.locator('.media-up-group', { hasText: name })).toContainText(`${name} · S01E01`);
    if (tick) {
      await page.locator('#media-up-more summary').click();
      await page.locator('input[name=h264]').check();
    }
    await page.locator('#media-up-start').click();
    const copy = path.join(LIBRARY, 'Series', name, 'Season 1', `${name} S01E01.web.mp4`);
    await expect.poll(() => fs.existsSync(copy), { message: 'the copy arrives', timeout: 60_000 }).toBe(true);
    await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
    return fs.readFileSync(copy).toString('latin1');
  };
  const made = await run('Hevcup', true);
  for (const box of ['avc1', 'avcC', 'mp4a']) expect(made).toContain(box); /* (made H.264, the AAC copied) */
  expect(made).not.toContain('hvcC');
  const copied = await run('Hevckeep', false);
  expect(copied).toContain('hvcC'); /* (the HEVC copied as it is) */
});

// 2.7 (the owner: "Full throttle ... processes the videos in the most effective way"): this device on Full throttle,
// fewer videos than it makes at once - one video made in parts side by side in its own workers (the server's plan says
// which: {"part": [from, to]}, here parts of 300 ms - server.mjs), each sent to its own file, and the server joins them
// into the copy. The copy is H.264; no part is left behind.
test('a library HEVC video is made in parts on this device on full throttle', async ({ page }) => {
  const dir = path.join(LIBRARY, 'Series', 'Partshow', 'Season 1'), from = path.join(dir, 'Partshow S01E01.mkv');
  fs.mkdirSync(dir, { recursive: true });
  fs.copyFileSync(path.join(WORK, 'mkv', 'hevc-three-keyframes-aac.mkv'), from);
  await page.goto('/media/upload');
  test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await act(page, 'media.power', { where: 'here', tier: 'full' });
  await act(page, 'media.rescan', {});
  const row = page.locator('li.row', { hasText: 'Partshow' });
  await expect.poll(async () => {
    await page.goto('/media'); /* (Watch's pages carry the question its browser answers - its cores too) */
    await page.waitForLoadState('networkidle');
    await page.goto('/media/settings/converting');
    return (await row.count()) && row.locator('form[data-here][data-workers]').count();
  }, { message: 'listed, with how many this device makes at once', timeout: 30_000 }).toBeGreaterThan(0);
  test.skip(Number(await row.locator('form[data-here]').getAttribute('data-workers')) < 2, 'a device of one core: no parts');
  const puts = [];
  page.on('request', (r) => { if (r.method() === 'PUT') puts.push(r.url()); });
  await row.locator('form[data-here] button').click();
  const copy = path.join(dir, 'Partshow S01E01.web.mp4');
  await expect.poll(() => fs.existsSync(copy), { message: 'the joined copy arrives', timeout: 60_000 }).toBe(true);
  const mp4 = fs.readFileSync(copy).toString('latin1');
  for (const box of ['avc1', 'avcC', 'mp4a']) expect(mp4).toContain(box); /* (made H.264, the AAC copied) */
  expect(puts.some((u) => /\/1\?/.test(u))).toBe(true); /* (a part went: entry 1 - the video itself is entry 0) */
  expect(fs.readdirSync(dir).filter((n) => n.endsWith('.conv.part'))).toEqual([]);
  await act(page, 'media.power', { where: 'here', tier: 'medium' });
});

// 2.6 (the owner: "select a series i want to convert ... where they can pick 'on hemma device' or 'on logged device'"): a
// library video whose picture is HEVC is made H.264 in this browser - the very file read from the server, its copy sent
// back beside the original as an upload's copy would be.
test('a library HEVC video is made H.264 on this device', async ({ page }) => {
  const dir = path.join(LIBRARY, 'Series', 'Hevcshow', 'Season 1'), from = path.join(dir, 'Hevcshow S01E01.mkv');
  fs.mkdirSync(dir, { recursive: true });
  fs.copyFileSync(path.join(WORK, 'mkv', 'hevc-random-10bit-aac.mkv'), from);
  await act(page, 'media.rescan', {});
  const row = page.locator('li.row', { hasText: 'Hevcshow' });
  await expect.poll(async () => {
    await page.goto('/media/settings/converting');
    return row.count();
  }, { message: 'the scan lists it with the HEVC videos', timeout: 30_000 }).toBeGreaterThan(0);
  test.skip(!(await row.locator('form[data-here][data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await row.locator('form[data-here] button').click();
  const copy = path.join(dir, 'Hevcshow S01E01.web.mp4');
  await expect.poll(() => fs.existsSync(copy), { message: 'its copy arrives', timeout: 60_000 }).toBe(true);
  expect(fs.existsSync(from)).toBe(true); /* (the original stays) */
  const mp4 = fs.readFileSync(copy).toString('latin1');
  for (const box of ['avc1', 'avcC', 'mp4a']) expect(mp4).toContain(box); /* (made H.264, the AAC copied) */
  expect(mp4).not.toContain('hvcC');
});

// 2.6: the video is read from Hemma while it's converted - a network that's gone a while is waited out, as an upload's
// sending is (1.4), not taken for a file that can't be read (which would skip a night's videos within seconds).
test('converting a library video here waits out a dropped network', async ({ page }) => {
  const dir = path.join(LIBRARY, 'Series', 'Netshow', 'Season 1'), from = path.join(dir, 'Netshow S01E01.mkv');
  fs.mkdirSync(dir, { recursive: true });
  fs.copyFileSync(path.join(WORK, 'mkv', 'hevc-random-10bit-aac.mkv'), from);
  let cut = 8; /* (each try reads twice before it gives up; more than the 2 tries again a failed read gets) */
  await page.route(/\/media\/file\/[0-9a-f]+\?src=1/, (route) => (cut-- > 0 ? route.abort('internetdisconnected') : route.continue()));
  await act(page, 'media.rescan', {});
  const row = page.locator('li.row', { hasText: 'Netshow' });
  await expect.poll(async () => {
    await page.goto('/media/settings/converting');
    return row.count();
  }, { message: 'the scan lists it with the HEVC videos', timeout: 30_000 }).toBeGreaterThan(0);
  test.skip(!(await row.locator('form[data-here][data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await row.locator('form[data-here]').evaluate((f) => { f.dataset.netpause = '20'; }); /* (ask again at once) */
  await row.locator('form[data-here] button').click();
  const copy = path.join(dir, 'Netshow S01E01.web.mp4');
  await expect.poll(() => fs.existsSync(copy), { message: 'its copy arrives', timeout: 60_000 }).toBe(true);
  expect(cut).toBeLessThan(0);
  expect(fs.readFileSync(copy).toString('latin1')).toContain('avcC');
});

// 2.5 (the owner: "Its sound is Opus, which the converter can't change yet. So we should support that"): Opus sound is
// copied into the MP4 ('Opus' + dOps) - the copy goes, not the original.
test('an MKV with Opus sound is converted here too', async ({ page }) => {
  const from = path.join(WORK, 'mkv-up', 'Opushow S01E01.mkv');
  fs.mkdirSync(path.dirname(from), { recursive: true });
  fs.copyFileSync(path.join(WORK, 'mkv', 'h264-opus.mkv'), from);
  await page.goto('/media/upload');
  test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await page.locator('input[name=files]').setInputFiles(from);
  await expect(page.locator('.media-up-group', { hasText: 'Opushow' })).toContainText('Opushow · S01E01');
  await page.locator('#media-up-start').click();
  const dir = path.join(LIBRARY, 'Series', 'Opushow', 'Season 1'), copy = path.join(dir, 'Opushow S01E01.web.mp4');
  await expect.poll(() => fs.existsSync(copy), { message: 'the copy arrives', timeout: 30_000 }).toBe(true);
  await expect(page.locator('.media-up h3')).toContainText('Finished · 1 file sent');
  await expect(page.locator('.media-up')).not.toContainText('sent as it is');
  expect(fs.existsSync(path.join(dir, 'Opushow S01E01.mkv'))).toBe(false);
  const mp4 = fs.readFileSync(copy).toString('latin1');
  for (const box of ['avc1', 'Opus', 'dOps']) expect(mp4).toContain(box);
});

// 2.5 (the owner's DuckTales: MP4s with Dolby sound, silent in Chrome): an MP4 is read by the converter here too - one
// with Dolby gets a copy with AAC sound, one that plays everywhere already goes as it is, without a word.
test('an MP4 with Dolby sound is converted here, one that plays goes as it is', async ({ page }) => {
  const dolby = path.join(WORK, 'mp4-up', 'Dolbyshow S01E01.mp4'), plays = path.join(WORK, 'mp4-up', 'Playsshow S01E01.mp4');
  fs.mkdirSync(path.dirname(dolby), { recursive: true });
  fs.copyFileSync(path.join(WORK, 'upload', 'Dolby test (2019).mp4'), dolby); /* (H.264 + AC-3: av_test_clips) */
  fs.copyFileSync(CLIP, plays); /* (H.264 + AAC) */
  await page.goto('/media/upload');
  test.skip(!(await page.locator('form[data-wasm]').count()), 'this Hemma has no converter for browsers (av.wasm)');
  await page.locator('input[name=files]').setInputFiles([dolby, plays]);
  await expect(page.locator('.media-up-group', { hasText: 'Dolbyshow' })).toContainText('Converted here if needed');
  await page.locator('#media-up-start').click();
  const d = path.join(LIBRARY, 'Series', 'Dolbyshow', 'Season 1'), p = path.join(LIBRARY, 'Series', 'Playsshow', 'Season 1');
  const copy = path.join(d, 'Dolbyshow S01E01.web.mp4');
  await expect.poll(() => fs.existsSync(copy) && fs.existsSync(path.join(p, 'Playsshow S01E01.mp4')),
    { message: 'both arrive', timeout: 30_000 }).toBe(true);
  await expect(page.locator('.media-up h3')).toContainText('Finished · 2 files sent');
  await expect(page.locator('.media-up')).not.toContainText('sent as it is');
  expect(fs.existsSync(path.join(d, 'Dolbyshow S01E01.mp4'))).toBe(false);
  const mp4 = fs.readFileSync(copy).toString('latin1');
  expect(mp4).toContain('mp4a'); /* (the sound made AAC) */
  expect(mp4).not.toContain('dac3');
  expect(same(path.join(p, 'Playsshow S01E01.mp4'), plays), 'the MP4 that plays: byte for byte').toBe(true);
  expect(fs.existsSync(path.join(p, 'Playsshow S01E01.web.mp4'))).toBe(false);
});

// 2.4 (owner: "the year of a movie to be almost mandatory"): a film without a year gets a box on the preview, Start
// asks first while it's empty, and with the year typed the film goes into Movies/<Title (Year)>/ under its own name.
test('a film without a year is asked for it, and goes in with it', async ({ page }) => {
  const from = path.join(WORK, 'noyear', 'Yearless.1080p.BluRay.x264.mp4');
  const to = path.join(LIBRARY, 'Movies', 'Yearless (2019)', path.basename(from));
  fs.mkdirSync(path.dirname(from), { recursive: true });
  fs.copyFileSync(CLIP, from);
  await page.goto('/media/upload');
  await page.locator('input[name=files]').setInputFiles(from);
  const box = page.locator('.media-up-year input');
  await expect(box).toHaveCount(1);
  await expect(page.locator('form[data-upload]')).toHaveAttribute('data-confirm', '1 film has no year. Upload it anyway?');
  let asked = '';
  page.once('dialog', (d) => { asked = d.message(); d.dismiss(); });
  await page.locator('#media-up-start').click();
  await expect.poll(() => asked, { message: 'Start asks first' }).toContain('no year');
  expect(fs.existsSync(to)).toBe(false); /* (not now: the question was turned down) */
  await box.fill('2019');
  await expect(page.locator('.media-up-group .media-up-dir > span')).toHaveText(/Movies\s*\/\s*Yearless \(2019\)/);
  await expect(page.locator('form[data-upload]')).not.toHaveAttribute('data-confirm', /./);
  await page.locator('#media-up-start').click();
  await expect.poll(() => fs.existsSync(to), { message: 'it arrives', timeout: 30_000 }).toBe(true);
  expect(same(to, CLIP)).toBe(true);
});
