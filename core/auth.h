/* core/auth.h — accounts, passwords, sign-in.
 *
 * Passwords are stored as   pbkdf2-sha256$<iterations>$<salt hex>$<hash hex>
 * where hash = PBKDF2-HMAC-SHA256(HMAC-SHA256(server salt, password), per-user salt).
 * The server salt (HEMMA_AUTH_SALT — a secret "pepper") lives only in the
 * environment / .env, never in the database, so a copied hemma.db alone can't
 * be brute-forced. Changing it invalidates every password.
 *
 * Environment:
 *   HEMMA_AUTH_SALT        required to sign in (generate: hemma --gen-salt)
 *   HEMMA_ADMIN_USER       the first admin's username
 *   HEMMA_ADMIN_PASSWORD   the first admin's password: used once, on the first start that has no
 *                          admin yet (setting "auth.bootstrapped"); remove it afterwards
 *   HEMMA_ADMIN_NAME       admin's display name (optional)
 *   HEMMA_ADMIN_RESET=1    recovery: (re)create HEMMA_ADMIN_USER as an admin with HEMMA_ADMIN_PASSWORD
 *                          and sign it out everywhere. Applied once per password value.
 *   HEMMA_KDF_ITERATIONS   PBKDF2 work factor (default 120000; stored per hash, upgraded on sign-in)
 *
 * Deleting or renaming the bootstrap admin later never brings it back on restart.
 *
 * First run without HEMMA_ADMIN_*: every signed-out visit shows the welcome page, where
 * the first visitor creates the admin (POST /setup). It is offered only while the household
 * has never had an admin ("auth.bootstrapped" unset) and none can sign in; the POST checks
 * again, so racing submissions can't both win. The environment bootstrap closes it too.
 *
 * Recovery codes: 8 one-time codes per person who can sign in (not display accounts), each
 * 12 characters from a 32-letter alphabet without 0 O 1 I (60 bits), shown as 7KQ4-M2XP-9RTA.
 * Stored only as HMAC-SHA256(server salt, person id + code). "Forgot your password?" (/forgot)
 * takes username + code + a new password; failures count against the sign-in throttle.
 *
 * Last resort on the server: hemma --reset-password USER (auth_reset_cli) leaves a request in
 * the database (table auth_reset, raw SQL) with a temporary password's hash. A running Hemma
 * notices it within a tick (PRAGMA data_version), otherwise the next start applies it:
 * password replaced, every session ended, and the person is asked to pick their own.
 *
 * Guessing is throttled per (address, username) and per account (a higher limit, so
 * many addresses can't share the work); addresses the account signed in from before
 * are exempt from the account-wide lock. Each address also has a budget across all
 * usernames (10 wrong passwords/codes, the current-password checks included): every
 * guess costs a password hash on the event loop, so a flood of made-up usernames must
 * not freeze it. All of these are checked before anything is hashed. After a limit:
 * 1, 2, 4, 8, then 15 minutes. Failures are forgotten after a quiet half hour, never
 * earlier - a full table doesn't evict recent ones (a new address waits instead while
 * the address table is full of fresh failures). */
#ifndef HEMMA_AUTH_H
#define HEMMA_AUTH_H

#include "app.h"

#define AUTH_CODES 8     /* recovery codes per person */
#define AUTH_CODE_LEN 15 /* "7KQ4-M2XP-9RTA" + NUL */

bool auth_ready(void);                /* a server salt is configured */
void auth_configure(const char *server_salt, int iterations); /* tests; app_boot reads the env */
/* A new install makes its own secret: without HEMMA_AUTH_SALT and before anyone has a
   password, a fresh random one goes into the settings file (created if missing, readable
   by its owner only) and the environment. 1 = made one, 0 = there is one already,
   -1 = not made (passwords exist - a new secret would lock everyone out - or the file
   can't be written); `why` says what happened. */
int auth_ensure_secret(const char *env_file, char *why, size_t cap);
bool auth_hash(const char *password, char *out, size_t cap);
bool auth_check(const char *password, const char *stored);
bool auth_needs_rehash(const char *stored);
const char *auth_password_problem(const char *password); /* NULL = fine, else a friendly reason */
bool auth_username_ok(const char *u);                     /* 2-32 of [a-z0-9._-] */
Person *person_by_username(const char *username);         /* case-insensitive */
void auth_bootstrap_admin(void);                          /* create/attach/reset the env admin */

typedef enum { LOGIN_OK, LOGIN_WRONG, LOGIN_THROTTLED, LOGIN_UNAVAILABLE } LoginResult;
/* Checks credentials with throttling (see above). On LOGIN_OK *person is set;
 * on LOGIN_THROTTLED *wait_s says how long to wait. */
LoginResult auth_login(const char *username, const char *password, uint32_t ip, int64_t now, int *person, int *wait_s);
/* A signed-in person re-entering their password (e.g. before changing it), throttled per
   person and against the address budget of `ip` (the device asking). */
LoginResult auth_recheck(const Person *p, const char *password, uint32_t ip, int64_t now, int *wait_s);
void auth_forget_failures(const char *username); /* e.g. after an admin set a new password */
void auth_reset_throttle(void);

/* A new password for `person`: hashed and stored, earlier lockouts forgotten, the
   "temporary password" mark cleared, and every session but `keep` (may be NULL) ended.
   Returns the number of sessions ended, -1 if passwords can't be set (no server salt). */
int auth_set_password(int person, const char *password, const Session *keep);
/* Signed in with a temporary password from the server's command line: should pick their own. */
bool auth_temporary(int person);

/* ---- first run ---- */
bool auth_setup_open(void); /* the welcome page is offered (no admin yet, never had one) */
/* The one-time code the welcome page asks for ("7KQ4-M2XP"), made on first use: HEMMA_SETUP_CODE
   from the settings file when set (the Pi installer writes it), else Hemma's own, kept until the
   admin exists. "" once setup is over. Shown in the log at start; never on a page. */
const char *auth_setup_code(void);
void auth_new_setup_code(char out[16]); /* a fresh one (hemma --gen-setup-code) */

/* ---- recovery codes ---- */
int auth_codes_left(int person);
/* A fresh set for `person` (the old ones stop working). The plain codes stay in memory for
   the session `shown_to` until it confirms (auth_codes_seen) or 30 minutes pass - they are
   never stored. False for display accounts and people who can't sign in. */
bool auth_codes_new(int person, const Session *shown_to);
int auth_codes_fresh(const Session *s, char codes[AUTH_CODES][AUTH_CODE_LEN]); /* waiting to be saved: count or 0 */
void auth_codes_seen(const Session *s);
void auth_codes_drop(int person); /* sign-in removed, person deleted */
/* "Forgot your password?": checks a code with the sign-in throttle (shared budget). On LOGIN_OK
   the code is used up and *person set; the caller sets the new password. Display accounts and
   unknown usernames get LOGIN_WRONG, like a wrong code. */
LoginResult auth_recover(const char *username, const char *code, uint32_t ip, int64_t now, int *person, int *wait_s);

/* ---- server-side reset: hemma --reset-password USER ----
   Opens <data_dir>/hemma.db on its own connection (the server may be running), asks for a
   temporary password for USER and waits up to wait_ms for a running Hemma to apply it.
   Messages for the terminal go to `out`; the temporary password also to temp_out (may be
   NULL, >= 24 bytes). Returns the process exit code. Needs auth_configure() first. */
int auth_reset_cli(const char *data_dir, const char *username, int wait_ms, Buf *out, char *temp_out);

/* Where to go after signing in: `in` if it is a plain same-site path ("/book?w=2"), else "/". */
void auth_safe_next(const char *in, char *out, size_t cap);

/* Core glue: the sign-in, welcome and forgot-password pages for signed-out visitors, and
   POST /login, /logout, /setup, /forgot. */
void auth_page(Ctx *c, Buf *b);
bool auth_http(Ctx *c, Conn *conn, HttpReq *r);
void auth_init(void);  /* app_boot, before the store opens: registers the recovery code table */
void auth_start(void); /* after it opened: applies server-side resets left while Hemma was stopped */
void auth_tick(void);  /* on the loop: resets asked for while Hemma runs */

#endif
