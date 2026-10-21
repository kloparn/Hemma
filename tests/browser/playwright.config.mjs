// Hemma in a real browser (issue #49): the player (resume, progress, the next episode), Live TV (now, no
// pausing or winding - also without wire.js), the upload client and the sign-in pages' languages, against
// a real Hemma with real (tiny) videos. Google Chrome - the bundled Chromium can't play H.264 or AAC.
// Run: npm ci && npx playwright test (after building Hemma: see server.mjs).
import { defineConfig } from '@playwright/test';
import { BASE, SIGNED_IN } from './where.mjs';

export default defineConfig({
  testDir: '.',
  testMatch: '*.spec.mjs',
  timeout: 60_000,
  expect: { timeout: 10_000 },
  workers: 1, /* one Hemma, and Live TV runs on the real clock: one test at a time */
  retries: process.env.CI ? 1 : 0,
  reporter: process.env.CI ? [['list'], ['html', { open: 'never' }]] : 'list',
  globalSetup: './setup.mjs',
  use: {
    baseURL: BASE,
    channel: 'chrome',
    headless: true,
    storageState: SIGNED_IN,
    locale: 'en-GB',
    launchOptions: { args: ['--autoplay-policy=no-user-gesture-required'] },
    trace: 'retain-on-failure',
  },
  webServer: {
    command: 'node server.mjs',
    url: `${BASE}/healthz`,
    reuseExistingServer: false,
    timeout: 120_000,
  },
});
