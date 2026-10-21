// eslint over Hemma's browser code (issue #49): web/wire.js (the page's wire, player and upload client) and
// web/conv.js (the upload converter's Worker). The recommended rules only - plain scripts, no plugins.
// npm run lint here runs it from the repository's top (with --config, eslint's base path is where it runs).
import js from '@eslint/js';
import globals from 'globals';

export default [
  js.configs.recommended,
  {
    files: ['web/wire.js'],
    languageOptions: { ecmaVersion: 2022, sourceType: 'script', globals: { ...globals.browser } },
  },
  {
    files: ['web/conv.js'],
    languageOptions: { ecmaVersion: 2022, sourceType: 'script', globals: { ...globals.worker } },
  },
  {
    rules: {
      // `catch (_) {}`: a failure that's meant to be quiet (storage that's full or blocked, a JSON that isn't)
      'no-empty': ['error', { allowEmptyCatch: true }],
      'no-unused-vars': ['error', { args: 'none', caughtErrors: 'none' }],
    },
  },
];
