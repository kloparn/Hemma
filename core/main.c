/* core/main.c — command line entry point.
 *
 * Settings come from (strongest first): command line flags, the real
 * environment, a .env file, built-in defaults. See .env.example. */
#include "app.h"
#include "auth.h"
#include "env.h"
#include "plat.h"
#include "release.h"
#include "store.h"

static void usage(void) {
    fprintf(stderr,
            "Hemma %s - a household hub for your home network\n\n"
            "usage: hemma [options]\n"
            "  --host ADDR    listen address (default 0.0.0.0 = the whole LAN)      [HEMMA_HOST]\n"
            "  --port N       port (default 8080; 80 needs CAP_NET_BIND_SERVICE)    [HEMMA_PORT]\n"
            "  --data DIR     where hemma.db lives (default ./data)                 [HEMMA_DATA]\n"
            "  --env FILE     settings file to load (default ./.env, optional)\n"
            "  --web DIR      serve web assets from DIR instead of the embedded copy\n"
            "                 (dev mode: CSS/JS edits reload open screens automatically)\n"
            "  --demo         try Hemma on a made-up household, in a throwaway folder of its own (the\n"
            "                 login is in this window and on the sign-in page; your data isn't touched)\n"
            "  --gen-salt     print a fresh random value for HEMMA_AUTH_SALT and exit\n"
            "  --gen-setup-code\n"
            "                 print a fresh one-time code for HEMMA_SETUP_CODE (the welcome page's) and exit\n"
            "  --reset-password USER\n"
            "                 last resort for a forgotten password: print a temporary one for USER and\n"
            "                 exit. Give it the server's --data and --env and run it as the user Hemma\n"
            "                 runs as; a running Hemma applies it within seconds (docs/accounts.md)\n"
            "  --restore FILE put a backup back - a .db from the backup folder, or a .tar downloaded from\n"
            "                 the System page. It's checked, then waits in the data folder: Hemma puts it\n"
            "                 in place at its next start (what was there is kept). Give it the server's\n"
            "                 --data and --env and run it as the user Hemma runs as (docs/raspberry-pi.md)\n"
            "  --setup-note   write the welcome page's setup code (or, once the admin exists, that it's used\n"
            "                 up) to HEMMA-SETUP.txt on a Raspberry Pi's memory card and its console's login\n"
            "                 screen, then exit. Run by hemma-setup.service as root (docs/raspberry-pi.md);\n"
            "                 --boot DIR and --issue FILE write elsewhere instead\n"
            "  --verify-release DIR [--partial]\n"
            "                 is DIR a release download as the Hemma project signed it? Exit 0 = yes.\n"
            "                 hemma-upgrade asks the installed Hemma before installing (docs/raspberry-pi.md)\n"
            "  --gen-signing-key FILE | --sign FILE\n"
            "                 for the project's maintainer: a new release signing key (the secret into FILE,\n"
            "                 the public half printed), or a signature of FILE with $HEMMA_SIGNING_KEY\n"
            "  --verbose | --quiet | --version\n\n"
            "While it runs, Hemma logs here and to hemma.log in its data folder. Admins find checks\n"
            "(\"is everything working?\") and the live log on its System page. Scripts: GET /healthz.\n",
            HEMMA_VERSION_FULL);
}

/* hemma --restore FILE (issue #35): checked, then staged for the next start (store_stage_restore). */
static int restore_cli(const char *file, const char *data_dir, const char *env_file) {
    const char *base = file;
    for (const char *p = file; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    char secret[200], why[400];
    if (!store_stage_restore(data_dir, file, base, secret, sizeof secret, why, sizeof why)) {
        fprintf(stderr, "Not restored: %s\n", why);
        return 1;
    }
    printf("Ready: %s was checked and waits in %s - Hemma puts it in place at its next start\n"
           "(keeping what's there now as hemma.db.before-restore-...).\n",
           base, data_dir);
    const char *now = env("HEMMA_AUTH_SALT", "");
    if (secret[0] && !str_eq(secret, now))
        printf("\nIts passwords were made with another password secret. Before starting Hemma, replace the\n"
               "HEMMA_AUTH_SALT line in %s with this one (from hemma-secret.txt in the download):\n\n"
               "    HEMMA_AUTH_SALT=%s\n",
               env_file, secret);
    else if (!secret[0])
        printf("\nPasswords only work with the password secret they were made with: if this is a new install,\n"
               "put the old HEMMA_AUTH_SALT line back in %s first (docs/raspberry-pi.md, \"Backups\").\n",
               env_file);
    printf("\nThen (re)start Hemma - on a Pi: sudo systemctl restart hemma\n");
    return 0;
}

/* Signed releases (issue #73; core/release.h): no settings file, no data folder. */
static int release_cli(int argc, char **argv) {
    const char *verb = NULL, *arg = NULL;
    bool partial = false;
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--partial")) partial = true;
        else if (!verb && i + 1 < argc &&
                 (str_eq(argv[i], "--verify-release") || str_eq(argv[i], "--gen-signing-key") || str_eq(argv[i], "--sign")))
            verb = argv[i], arg = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    if (!verb) {
        usage();
        return 2;
    }
    char why[400];
    if (str_eq(verb, "--verify-release")) {
        Buf say = {0};
        bool ok = release_verify_dir(arg, partial, &say);
        fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "verified" : "NOT verified", buf_cstr(&say));
        buf_free(&say);
        return ok ? 0 : 1;
    }
    if (partial) {
        usage();
        return 2;
    }
    if (str_eq(verb, "--gen-signing-key")) {
        char pub[65];
        if (!release_key_new(arg, pub, why, sizeof why)) {
            fprintf(stderr, "%s\n", why);
            return 1;
        }
        printf("The secret key is in %s (only you can read it): keep it, and a copy somewhere safe.\n"
               "Put its contents into the GitHub secret HEMMA_SIGNING_KEY (Settings > Secrets and\n"
               "variables > Actions), and this public key into core/release.c (RELEASE_KEYS):\n\n%s\n",
               arg, pub);
        return 0;
    }
    char sig[129];
    if (!release_sign(plat_getenv("HEMMA_SIGNING_KEY"), arg, sig, why, sizeof why)) {
        fprintf(stderr, "not signed: %s\n", why);
        return 1;
    }
    printf("%s\n", sig);
    return 0;
}

/* hemma --demo (issue #31): a fresh folder under the temporary folder, with a settings file that
   makes the admin "anna" (and a random password) - app_demo_seed fills in the rest at the first boot.
   Nothing of the household's: no --data, no --env, no .env read. */
static bool make_demo(char *dir, size_t dcap, char *env_file, size_t ecap, char *password, size_t pcap) {
    const char *tmp = plat_getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = plat_getenv("TEMP");
    if (!tmp || !*tmp) tmp = plat_getenv("TMP");
    if (!tmp || !*tmp) tmp = "/tmp";
    char rnd[9], pw[9];
    rand_hex(rnd, 4);
    rand_hex(pw, 4);
    snprintf(dir, dcap, "%s/hemma-demo-%s", tmp, rnd);
    snprintf(env_file, ecap, "%s/demo.env", dir);
    snprintf(password, pcap, "demo-%s", pw);
    if (!dir_make(dir)) return false;
    FILE *f = file_open(env_file, "wb");
    if (!f) return false;
    fprintf(f, "# hemma --demo: a made-up household (this folder is a throwaway)\nHEMMA_ADMIN_USER=anna\n"
               "HEMMA_ADMIN_NAME=Anna\nHEMMA_ADMIN_PASSWORD=%s\n", password);
    fclose(f);
    return true;
}

int main(int argc, char **argv) {
    if (!plat_init()) {
        fprintf(stderr, "platform init failed\n");
        return 1;
    }
    /* first pass: things that must happen before reading the environment */
    const char *env_file = ".env";
    bool demo = false, other_place = false;
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--demo")) demo = true;
        else if ((str_eq(argv[i], "--data") || str_eq(argv[i], "--env")) && i + 1 < argc) other_place = true;
        if (str_eq(argv[i], "--env") && i + 1 < argc) env_file = argv[++i];
        else if (str_eq(argv[i], "--gen-salt")) {
            char hex[65];
            rand_hex(hex, 32);
            printf("%s\n", hex);
            return 0;
        } else if (str_eq(argv[i], "--gen-setup-code")) {
            char code[16];
            auth_new_setup_code(code);
            printf("%s\n", code);
            return 0;
        } else if (str_eq(argv[i], "--version")) {
            printf("hemma %s\n", HEMMA_VERSION_FULL);
            return 0;
        } else if (str_eq(argv[i], "--verify-release") || str_eq(argv[i], "--gen-signing-key") ||
                   str_eq(argv[i], "--sign")) {
            return release_cli(argc, argv);
        }
    }
    static char demo_dir[400], demo_env[440], demo_pw[16], demo_data[420];
    if (demo) {
        if (other_place) {
            fprintf(stderr, "--demo keeps its own throwaway folder: leave out --data and --env\n");
            return 2;
        }
        if (!make_demo(demo_dir, sizeof demo_dir, demo_env, sizeof demo_env, demo_pw, sizeof demo_pw)) {
            fprintf(stderr, "couldn't make a folder for the demo in %s\n", demo_dir);
            return 1;
        }
        env_file = demo_env;
        snprintf(demo_data, sizeof demo_data, "%s/data", demo_dir);
    }
    int loaded = env_load(env_file);

    AppConfig cfg = {
        .host = env("HEMMA_HOST", "0.0.0.0"),
        .port = (int)env_int("HEMMA_PORT", 8080),
        .data_dir = env("HEMMA_DATA", "data"),
        .web_dir = NULL,
        .env_file = env_file,
        .demo_password = demo ? demo_pw : NULL,
    };
    if (demo) cfg.data_dir = demo_data;
    const char *reset_user = NULL, *restore_file = NULL, *note_boot = NULL, *note_issue = NULL;
    bool setup_note = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (str_eq(a, "--host") && next) cfg.host = argv[++i];
        else if (str_eq(a, "--port") && next) cfg.port = (int)str_int_or(argv[++i], -1);
        else if (str_eq(a, "--data") && next) cfg.data_dir = argv[++i];
        else if (str_eq(a, "--web") && next) cfg.web_dir = argv[++i];
        else if (str_eq(a, "--reset-password") && next) reset_user = argv[++i];
        else if (str_eq(a, "--restore") && next) restore_file = argv[++i];
        else if (str_eq(a, "--setup-note")) setup_note = true;
        else if (str_eq(a, "--boot") && next) note_boot = argv[++i];
        else if (str_eq(a, "--issue") && next) note_issue = argv[++i];
        else if (str_eq(a, "--env") && next) i++; /* handled above */
        else if (str_eq(a, "--demo")) continue;        /* (also above) */
        else if (str_eq(a, "--verbose")) g_log_level = LOG_DEBUG;
        else if (str_eq(a, "--quiet")) g_log_level = LOG_WARN;
        else {
            usage();
            return str_eq(a, "--help") || str_eq(a, "-h") ? 0 : 2;
        }
    }
    if (cfg.port < 0 || cfg.port > 65535) {
        fprintf(stderr, "bad port\n");
        return 2;
    }
    if (loaded >= 0) log_info("settings: %d from %s", loaded, env_file);
    if (reset_user) {
        auth_configure(env("HEMMA_AUTH_SALT", NULL), (int)env_int("HEMMA_KDF_ITERATIONS", 120000));
        Buf out = {0};
        int rc = auth_reset_cli(cfg.data_dir, reset_user, 8000, &out, NULL);
        fputs(buf_cstr(&out), rc ? stderr : stdout);
        buf_free(&out);
        return rc;
    }
    if (restore_file) return restore_cli(restore_file, cfg.data_dir, env_file);
    if (setup_note) { /* (issue #72) */
        Buf out = {0};
        int rc = auth_setup_note_cli(cfg.data_dir, note_boot, note_issue, &out);
        fputs(buf_cstr(&out), stdout);
        buf_free(&out);
        return rc;
    }
    return app_main(&cfg);
}
