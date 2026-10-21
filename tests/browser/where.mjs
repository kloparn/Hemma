// Where the browser tests' Hemma lives - shared by the launcher, the setup and the tests.
import os from 'node:os';
import path from 'node:path';

export const PORT = 18093;
export const BASE = `http://127.0.0.1:${PORT}`;
export const ROOT = path.resolve(import.meta.dirname, '../..');
export const WORK = path.join(os.tmpdir(), 'hemma-browser-tests'); /* data, settings file, clips: made anew each run */
export const LIBRARY = path.join(WORK, 'library');
export const ADMIN = { user: 'anna', password: 'browser-tests-2026', name: 'Anna' };
export const SIGNED_IN = path.join(import.meta.dirname, '.auth', 'anna.json');
