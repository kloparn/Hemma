#include "../tests/test.h"
#include "plat.h"

#include <stdatomic.h>

TEST(ip_parse_and_format) {
    uint32_t ip = 0;
    CHECK(ip_parse("192.168.1.42", &ip) && ip == 0xC0A8012A);
    char s[16];
    ip_format(ip, s);
    CHECK_STR(s, "192.168.1.42");
    CHECK(!ip_parse("192.168.1", &ip));
    CHECK(!ip_parse("256.1.1.1", &ip));
    CHECK(!ip_parse("1.2.3.4.5", &ip));
    CHECK(!ip_parse("1.2.3.04x", &ip));
    CHECK(!ip_parse("", &ip));
}

/* --host as the listener reads it (issue #99: "localhost" and "*" were taken for names) */
TEST(listen_addr_reads_host_like_the_listener) {
    uint32_t ip = 1;
    CHECK(listen_addr(NULL, &ip) && ip == 0);
    CHECK(listen_addr("", &ip) && ip == 0);
    CHECK(listen_addr("*", &ip) && ip == 0);
    CHECK(listen_addr("0.0.0.0", &ip) && ip == 0);
    CHECK(listen_addr("localhost", &ip) && ip == 0x7F000001);
    CHECK(listen_addr("LocalHost", &ip) && ip == 0x7F000001);
    CHECK(listen_addr("127.0.0.1", &ip) && ip == 0x7F000001);
    CHECK(listen_addr("192.168.1.50", &ip) && ip == 0xC0A80132);
    CHECK(!listen_addr("hemma.local", &ip));
    CHECK(!listen_addr("::", &ip));
}

TEST(files_roundtrip_with_utf8_names) {
    Buf path = {0};
    buf_printf(&path, "%s/nested/dir/räksmörgås.txt", test_tmpdir());
    char dir[512];
    str_copy(dir, sizeof dir, buf_cstr(&path));
    *strrchr(dir, '/') = 0;
    CHECK(dir_make(dir));
    CHECK(path_is_dir(dir));
    CHECK(file_write_atomic(buf_cstr(&path), "hej\nhopp", 8));
    Buf got = {0};
    CHECK(file_read_all(buf_cstr(&path), &got, 1 << 20));
    CHECK_INT(got.len, 8);
    CHECK(memcmp(got.p, "hej\nhopp", 8) == 0); /* binary mode: no CRLF translation */
    int64_t size = 0, mtime = 0;
    bool is_dir = true;
    CHECK(path_stat(buf_cstr(&path), &size, &mtime, &is_dir));
    CHECK_INT(size, 8);
    CHECK(!is_dir);
    CHECK(mtime > 1700000000);
    /* too-small limit fails cleanly */
    Buf small = {0};
    CHECK(!file_read_all(buf_cstr(&path), &small, 4));
    CHECK(file_remove(buf_cstr(&path)));
    CHECK(!path_stat(buf_cstr(&path), NULL, NULL, NULL));
    buf_free(&path);
    buf_free(&got);
    buf_free(&small);
}

TEST(file_pread_offsets) {
    char p[600];
    snprintf(p, sizeof p, "%s/bytes.bin", test_tmpdir());
    CHECK(file_write_atomic(p, "0123456789", 10));
    FILE *f = file_open(p, "rb");
    CHECK(f != NULL);
    char b[4] = {0};
    CHECK(file_pread(f, 6, b, 3));
    CHECK(memcmp(b, "678", 3) == 0);
    CHECK(file_pread(f, 0, b, 2));
    CHECK(memcmp(b, "01", 2) == 0);
    CHECK(!file_pread(f, 9, b, 3)); /* short read */
    CHECK(!file_seek(f, -1));
    fclose(f);
}

TEST(dir_iteration_lists_entries) {
    const char *root = test_tmpdir();
    char p[600];
    snprintf(p, sizeof p, "%s/sub", root);
    CHECK(dir_make(p));
    snprintf(p, sizeof p, "%s/a.mkv", root);
    CHECK(file_write_atomic(p, "x", 1));
    DirIter *it = dir_open(root);
    CHECK(it != NULL);
    DirEntry e;
    int files = 0, dirs = 0;
    while (dir_next(it, &e)) {
        if (e.is_dir) {
            dirs++;
            CHECK_STR(e.name, "sub");
        } else {
            files++;
            CHECK_STR(e.name, "a.mkv");
            CHECK_INT(e.size, 1);
        }
    }
    dir_close(it);
    CHECK_INT(files, 1);
    CHECK_INT(dirs, 1);
    CHECK(dir_open("definitely/not/here") == NULL);
}

TEST(random_hex_is_unique) {
    char a[33], b[33];
    rand_hex(a, 16);
    rand_hex(b, 16);
    CHECK_INT(strlen(a), 32);
    CHECK(strcmp(a, b) != 0);
}

TEST(tcp_loopback_roundtrip) {
    char err[128];
    sock_t ls = tcp_listen("127.0.0.1", 0, err, sizeof err);
    CHECK(ls != SOCK_BAD);
    int port = tcp_local_port(ls);
    CHECK(port > 0);
    sock_t c = tcp_connect_nb(0x7F000001, port);
    CHECK(c != SOCK_BAD);
    sock_t s = SOCK_BAD;
    for (int i = 0; i < 200 && s == SOCK_BAD; i++) {
        uint32_t peer = 0;
        s = tcp_accept(ls, &peer);
        if (s == SOCK_BAD) plat_sleep_ms(5);
        else CHECK(peer == 0x7F000001);
    }
    CHECK(s != SOCK_BAD);
    pollfd_t pf = {.fd = c, .events = POLLOUT};
    CHECK(plat_poll(&pf, 1, 1000) == 1);
    CHECK(tcp_connect_done(c));
    CHECK(sock_send(c, "ping", 4) == 4);
    char buf[8] = {0};
    long n = -1;
    for (int i = 0; i < 200; i++) {
        n = sock_recv(s, buf, sizeof buf);
        if (n > 0 || !sock_again(sock_errno())) break;
        plat_sleep_ms(5);
    }
    CHECK_INT(n, 4);
    CHECK(memcmp(buf, "ping", 4) == 0);
    sock_close(c);
    sock_close(s);
    sock_close(ls);
}

TEST(listen_rejects_bad_host) {
    char err[128] = {0};
    CHECK(tcp_listen("not-an-ip", 0, err, sizeof err) == SOCK_BAD);
    CHECK_HAS(err, "bad listen address");
}

/* Issue #27: the disks a library might be on, from Linux's mount list (a real Pi's, with a USB disk and a
   mergerfs pool, plus the kind of lines to leave out). */
TEST(plat_parse_mounts_keeps_disks_under_mnt_and_media) {
    const char *text = "/dev/mmcblk0p2 / ext4 rw,noatime 0 0\n"
                       "proc /proc proc rw,nosuid 0 0\n"
                       "/dev/mmcblk0p1 /boot/firmware vfat rw,relatime 0 0\n"
                       "/dev/sda1 /mnt/disk1 ext4 rw,noatime 0 0\n"
                       "disk1 /mnt/media fuse.mergerfs rw,relatime,user_id=0 0 0\n"
                       "tmpfs /mnt/ram tmpfs rw 0 0\n"
                       "/dev/sdb1 /media/anna/My\\040Films exfat rw 0 0\n"
                       "/dev/sda1 /mnt/disk1 ext4 rw,noatime 0 0\n"
                       "broken line\n"
                       "/dev/sdc1 /mntx ext4 rw 0 0";
    char out[8][300];
    int n = plat_parse_mounts(text, out, 8);
    CHECK_INT(n, 3);
    CHECK_STR(out[0], "/mnt/disk1");
    CHECK_STR(out[1], "/mnt/media");
    CHECK_STR(out[2], "/media/anna/My Films");
    CHECK_INT(plat_parse_mounts(text, out, 1), 1);
    CHECK_INT(plat_parse_mounts("", out, 8), 0);
    char real[8][300];
    CHECK(plat_mounts(real, 8) >= 0); /* (whatever this machine has) */
}

/* Worker threads hand back what the event loop made (base.h free_on_loop; 1.5.1: musl kept every 2 KB upload job a
   worker freed - 100 MB on a Pi in an afternoon). The blocks wait, from any number of threads at once, until the
   event loop frees them - thread_spawn's own start records among them. */
static atomic_int g_handed;
static void hand_back(void *arg) {
    (void)arg;
    for (int i = 0; i < 100; i++) free_on_loop(xmalloc(1 + (size_t)i * 40));
    free_on_loop(NULL); /* (nothing: fine) */
    atomic_fetch_add(&g_handed, 1);
}

TEST(free_on_loop_takes_blocks_back_from_threads) {
    free_on_loop_now(); /* (whatever earlier tests' threads left) */
    atomic_store(&g_handed, 0);
    for (int t = 0; t < 4; t++) CHECK(thread_spawn(hand_back, NULL));
    for (int waited = 0; atomic_load(&g_handed) < 4 && waited < 5000; waited++) plat_sleep_ms(1);
    CHECK_INT(atomic_load(&g_handed), 4);
    CHECK(free_on_loop_now() >= 4 * 100 + 4); /* every block, and the four threads' start records */
    Buf b = {0};
    buf_puts(&b, "a job's bytes");
    buf_free_on_loop(&b);
    CHECK(!b.p && !b.len && !b.cap);
    CHECK(free_on_loop_now() >= 1);
}

TEST(plat_quotes_arguments_as_windows_reads_them) {
    Buf b = {0};
    const char *const plain[] = {"C:\\Windows\\System32\\curl.exe", "-sS", "--url", "https://x.org/a?b=1&c=2", NULL};
    plat_quote_args(plain, &b);
    CHECK_STR(b.p, "C:\\Windows\\System32\\curl.exe -sS --url https://x.org/a?b=1&c=2");
    /* spaces quoted; a quote escaped; backslashes doubled only before a quote (also the closing one) */
    const char *const spaced[] = {"a b", "", "Hemma/2.5 (+https://x)", "say \"hi\"", "C:\\dir with space\\", "a\\\\\"b", NULL};
    plat_quote_args(spaced, &b);
    CHECK_STR(b.p, "\"a b\" \"\" \"Hemma/2.5 (+https://x)\" \"say \\\"hi\\\"\" \"C:\\dir with space\\\\\" \"a\\\\\\\\\\\"b\"");
    buf_free(&b);
}

TEST(plat_runs_a_program_and_waits_for_it) {
    /* the computer's own shell: cmd.exe on Windows, /bin/sh elsewhere */
    const char *root = plat_getenv("SystemRoot");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "%s\\System32\\cmd.exe", root ? root : "");
    bool win = root && path_stat(cmd, NULL, NULL, NULL);
    const char *const exit3[] = {win ? cmd : "/bin/sh", win ? "/c" : "-c", "exit 3", NULL};
    CHECK_INT(plat_run(exit3, 10000), 3);
    const char *const none[] = {"/no/such/program/here", NULL};
    CHECK_INT(plat_run(none, 2000), -1);
    if (!win) { /* (stopped when it runs too long) */
        const char *const slow[] = {"/bin/sh", "-c", "sleep 5", NULL};
        int64_t t0 = now_ms();
        CHECK_INT(plat_run(slow, 200), -1);
        CHECK(now_ms() - t0 < 3000);
    }
}
