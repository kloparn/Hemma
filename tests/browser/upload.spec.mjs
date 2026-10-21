// The upload client (web/wire.js upload(); issue #49): a file picked on the upload page goes up - the plan,
// then its bytes - and lands in the library folder byte for byte.
import fs from 'node:fs';
import path from 'node:path';
import { expect, test } from '@playwright/test';
import { LIBRARY, WORK } from './where.mjs';

test('an upload puts the file into the library', async ({ page }) => {
  const name = 'Upload test (2021).mp4', from = path.join(WORK, 'upload', name), to = path.join(LIBRARY, 'Uploads', name);
  await page.goto('/media/upload');
  await page.locator('input[name=files]').setInputFiles(from);
  await page.locator('input[name=into]').fill('Uploads');
  await page.locator('form[data-upload] .modal-f button').click();
  await expect.poll(() => fs.existsSync(to), { message: 'it arrives', timeout: 30_000 }).toBe(true);
  expect(fs.readFileSync(to).equals(fs.readFileSync(from))).toBe(true);
});
