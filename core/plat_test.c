#include "../tests/test.h"
#include "plat.h"

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
