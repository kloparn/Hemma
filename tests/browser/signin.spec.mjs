// The sign-in page's language (issue #46): the browser's own, and the links that pick another one
// (a cookie, kept through a full page load - wire.js must not turn those links into socket navigations).
import { expect, test } from '@playwright/test';

test.use({ storageState: { cookies: [], origins: [] } }); /* signed out */

test.describe('a Spanish browser', () => {
  test.use({ locale: 'es-ES' });
  test('gets the sign-in page in Spanish', async ({ page }) => {
    await page.goto('/login');
    await expect(page.locator('html')).toHaveAttribute('lang', 'es');
    await expect(page.locator('h1')).toHaveText('Iniciar sesión');
  });
});

test('the language links switch it, and it stays', async ({ page }) => {
  await page.goto('/login');
  await expect(page.locator('h1')).toHaveText('Sign in');
  await page.getByRole('link', { name: 'Svenska' }).click();
  await expect(page.locator('h1')).toHaveText('Logga in');
  await expect(page.locator('html')).toHaveAttribute('lang', 'sv');
  await page.goto('/forgot');
  await expect(page.locator('html')).toHaveAttribute('lang', 'sv');
});
