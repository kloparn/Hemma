// Hemma for the browser tests (Playwright's webServer starts this): a fresh folder under the system's temp
// with test clips made by Hemma's own encoders (build/test av_test_clips), an admin from the settings file,
// then the program itself on 127.0.0.1:18093 until Playwright is done. Needs build/hemma and build/test
// (make, or ./build.ps1 build + ./build.ps1 test on Windows).
import { execFileSync, spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { ADMIN, PORT, ROOT, WORK } from './where.mjs';

const exe = process.platform === 'win32' ? '.exe' : '';
fs.rmSync(WORK, { recursive: true, force: true }); /* (the last run's: this folder is the tests' own) */
fs.mkdirSync(WORK, { recursive: true });
execFileSync(path.join(ROOT, 'build', `test${exe}`), ['av_test_clips'], {
  cwd: ROOT, stdio: 'inherit', env: { ...process.env, HEMMA_TEST_CLIPS: WORK },
});
fs.writeFileSync(path.join(WORK, 'hemma.env'),
  `HEMMA_ADMIN_USER=${ADMIN.user}\nHEMMA_ADMIN_PASSWORD=${ADMIN.password}\nHEMMA_ADMIN_NAME=${ADMIN.name}\n`);

const hemma = spawn(path.join(ROOT, 'build', `hemma${exe}`),
  ['--host', '127.0.0.1', '--port', String(PORT), '--data', path.join(WORK, 'data'), '--env', path.join(WORK, 'hemma.env')],
  { cwd: ROOT, stdio: 'inherit' });
hemma.on('exit', (code) => process.exit(code ?? 1));
for (const sig of ['SIGINT', 'SIGTERM']) process.on(sig, () => hemma.kill());
