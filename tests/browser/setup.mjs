// Before the tests (Playwright's globalSetup, after the server is up): the admin signs in, the test clips
// become the library, and the scan has found them. The sign-in is kept for the tests (SIGNED_IN).
import fs from 'node:fs';
import path from 'node:path';
import { expect, request } from '@playwright/test';
import { ADMIN, BASE, LIBRARY, SIGNED_IN } from './where.mjs';

export default async function setup() {
  const api = await request.newContext({ baseURL: BASE, extraHTTPHeaders: { Origin: BASE, Referer: `${BASE}/` } });
  const r = await api.post('/login', { form: { username: ADMIN.user, password: ADMIN.password, remember: '1' } });
  expect(r.ok(), 'the admin signs in').toBe(true);
  const add = await api.post('/a/media.roots_add', { form: { dir: LIBRARY } });
  expect(add.ok(), 'the clips become the library').toBe(true);
  await expect.poll(async () => (await (await api.get('/media/series')).text()).includes('Testshow'),
    { message: 'the scan finds the test series', timeout: 30_000 }).toBe(true);
  fs.mkdirSync(path.dirname(SIGNED_IN), { recursive: true });
  await api.storageState({ path: SIGNED_IN });
  await api.dispose();
}
