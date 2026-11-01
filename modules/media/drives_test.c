/* modules/media/drives.c: USB disks on a made-up machine (sysfs, udev's database, mountinfo) - what's
 * seen, what's offered, the requests for the disk helper and what its answers do to the library.
 * (Device numbers read "8-1" here: Windows can't have a ':' in a file name; the code takes them as text.) */
#include "../../tests/harness.h"
#include "../../core/plat.h"
#include "drives.h"
#include "library.h"
#include "media.h"

typedef struct {
    char root[600], sys[700], udev[700], minfo[700], ask[700], ans[700];
} Machine;

static void put(const char *dir, const char *name, const char *text) {
    char path[900];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    CHECK(file_write_atomic(path, text, strlen(text)));
}

/* /sys/block/<name> (dev, size in sectors) and its udev entry */
static void disk(Machine *m, const char *name, const char *dev, long long sectors, const char *udev) {
    char d[900], v[64];
    snprintf(d, sizeof d, "%s/%s", m->sys, name);
    CHECK(dir_make(d));
    put(d, "dev", dev);
    snprintf(v, sizeof v, "%lld\n", sectors);
    put(d, "size", v);
    snprintf(v, sizeof v, "b%s", dev);
    if (udev) put(m->udev, v, udev);
}

static void part(Machine *m, const char *disk_name, const char *name, const char *dev, const char *udev) {
    char d[900], v[64];
    snprintf(d, sizeof d, "%s/%s/%s", m->sys, disk_name, name);
    CHECK(dir_make(d));
    put(d, "dev", dev);
    put(d, "partition", "1\n");
    snprintf(v, sizeof v, "b%s", dev);
    if (udev) put(m->udev, v, udev);
}

static void mounts(Machine *m, const char *text) { CHECK(file_write_atomic(m->minfo, text, strlen(text))); }

#define USB "E:ID_BUS=usb\nE:ID_USB_DRIVER=usb-storage\n"
#define ROOT_MOUNTS                                                                                                 \
    "26 1 179-2 / / rw,noatime shared:1 - ext4 /dev/mmcblk0p2 rw\n"                                                 \
    "43 26 179-1 / /boot/firmware rw,relatime shared:80 - vfat /dev/mmcblk0p1 rw\n"                                 \
    "50 26 8-1 / /mnt/disk1 rw,noatime shared:86 - ext4 /dev/sda1 rw\n"

/* A Pi with: its card (the system), a USB disk in use at /mnt/disk1 (the household set it up by hand), a
   new exFAT disk, an empty one, a Mac's disk, a loop device and an empty card reader. */
static void machine(Machine *m) {
    str_copy(m->root, sizeof m->root, test_tmpdir());
    snprintf(m->sys, sizeof m->sys, "%s/sys", m->root);
    snprintf(m->udev, sizeof m->udev, "%s/udev", m->root);
    snprintf(m->minfo, sizeof m->minfo, "%s/mountinfo", m->root);
    snprintf(m->ask, sizeof m->ask, "%s/data-disks", m->root);
    snprintf(m->ans, sizeof m->ans, "%s/answers", m->root);
    CHECK(dir_make(m->sys));
    CHECK(dir_make(m->udev));
    disk(m, "mmcblk0", "179-0", 249737216, "E:ID_NAME=SD\n");
    part(m, "mmcblk0", "mmcblk0p1", "179-1", "E:ID_FS_TYPE=vfat\nE:ID_FS_UUID=1080-F633\nE:ID_FS_LABEL=bootfs\n");
    part(m, "mmcblk0", "mmcblk0p2", "179-2", "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=a368\nE:ID_FS_LABEL=rootfs\n");
    disk(m, "sda", "8-0", 3907029168LL,
         "E:ID_BUS=ata\nE:ID_MODEL=WDC_WD20EFPX\nE:ID_MODEL_ENC=WDC\\x20WD20EFPX\\x20\\x20\nE:ID_USB_DRIVER=usb-storage\n");
    part(m, "sda", "sda1", "8-1", "E:ID_FS_TYPE=ext4\nE:ID_FS_UUID=9adf\nE:ID_FS_LABEL=disk1\n");
    disk(m, "sdb", "8-16", 1953525168LL, USB "E:ID_MODEL=Expansion_HDD\n");
    part(m, "sdb", "sdb1", "8-17",
         "E:ID_FS_TYPE=exfat\nE:ID_FS_UUID=5E2A-1B3C\nE:ID_FS_LABEL_ENC=Film\\x20disk\nE:ID_FS_LABEL=Film_disk\n");
    disk(m, "sdc", "8-32", 976773168LL, USB "E:ID_MODEL_ENC=Blank\\x20One\nE:ID_PART_TABLE_TYPE=gpt\n");
    part(m, "sdc", "sdc1", "8-33", "E:ID_PART_ENTRY_NUMBER=1\n");
    disk(m, "sdd", "8-48", 500118192LL, USB "E:ID_MODEL=Mac_Disk\n");
    part(m, "sdd", "sdd1", "8-49", "E:ID_FS_TYPE=apfs\nE:ID_FS_UUID=aaaa-bbbb\n");
    disk(m, "loop0", "7-0", 1853440, "E:ID_FS_TYPE=swap\n");
    disk(m, "sde", "8-64", 0, USB); /* (a card reader with no card) */
    mounts(m, ROOT_MOUNTS);
}

static int find(const Drive *d, int n, const char *k) {
    for (int i = 0; i < n; i++)
        if (str_eq(d[i].kname, k)) return i;
    return -1;
}

TEST(media_drives_scan_reads_sysfs_udev_and_mountinfo) {
    Machine m;
    machine(&m);
    Buf mi = {0};
    CHECK(file_read_all(m.minfo, &mi, 1 << 16));
    Drive d[DRIVES_MAX];
    int n = media_drives_scan(m.sys, m.udev, buf_cstr(&mi), d, DRIVES_MAX);
    buf_free(&mi);
    CHECK_INT(n, 5); /* (no loop0, no empty card reader) */
    CHECK_STR(d[0].kname, "sda"); /* in order */
    int a = find(d, n, "sda"), b = find(d, n, "sdb"), c = find(d, n, "sdc"), k = find(d, n, "mmcblk0");
    CHECK(a >= 0 && b >= 0 && c >= 0 && k >= 0);
    CHECK_STR(d[a].name, "WDC WD20EFPX"); /* (from ID_MODEL_ENC, trimmed) */
    CHECK(d[a].usb); /* (a USB-SATA bridge: ID_BUS=ata, but usb-storage drives it) */
    CHECK(!d[a].system);
    CHECK_INT(d[a].nvol, 1);
    CHECK_STR(d[a].vol[0].mounted, "/mnt/disk1");
    CHECK(d[k].system && !d[k].usb);
    CHECK_STR(d[b].vol[0].label, "Film disk");
    CHECK_STR(d[b].vol[0].fstype, "exfat");
    CHECK_STR(d[b].vol[0].uuid, "5E2A-1B3C");
    CHECK_STR(d[b].vol[0].mounted, "");
    CHECK_STR(d[b].name, "Expansion HDD");
    CHECK(d[b].bytes == 1953525168LL * 512);
    CHECK_INT(d[c].nvol, 1);
    CHECK_STR(d[c].vol[0].fstype, "");
    char size[24];
    media_drive_size(d[b].bytes, size, sizeof size);
    CHECK_STR(size, "1.0 TB");
    media_drive_size(2000398934016LL, size, sizeof size);
    CHECK_STR(size, "2.0 TB");
    CHECK(media_drive_fs_ok("ntfs") && media_drive_fs_ok("ext4") && !media_drive_fs_ok("apfs") && !media_drive_fs_ok(""));
}

static int admin(const char *name) {
    int id = th_person(name);
    Person p = *person_get(id);
    p.role = ROLE_ADMIN;
    tbl_put(&T_person, &p);
    return id;
}

static void request(const Machine *m, char *out, size_t cap) { /* the helper's line, without its nonce */
    char path[800];
    Buf b = {0};
    snprintf(path, sizeof path, "%s/request", m->ask);
    out[0] = '\0';
    if (file_read_all(path, &b, 300)) {
        const char *s = buf_cstr(&b), *sp = strrchr(s, ' ');
        if (sp && strlen(sp + 1) == 17) str_ncopy(out, cap, s, (size_t)(sp - s));
    }
    buf_free(&b);
}

static void helper(Machine *m) {
    CHECK(dir_make(m->ask));
    CHECK(dir_make(m->ans));
    media_test_drives(m->sys, m->udev, m->minfo);
    media_test_drive_dirs(m->ask, m->ans);
    media_drives_tick(now_ms());
}

TEST(media_drives_offers_new_disks_and_asks_the_helper) {
    th_boot();
    int adam = admin("Adam"), eva = th_person("Eva");
    Machine m;
    machine(&m);
    helper(&m);
    const char *h = th_page(adam, "/media/settings/folders");
    CHECK_HAS(h, "New disk found: Film disk");
    CHECK_HAS(h, "exFAT · 1.0 TB · Expansion HDD");
    CHECK_HAS(h, "data-a=\"media.drive_use\"");
    CHECK_HAS(h, "name=\"uuid\" value=\"5E2A-1B3C\"");
    CHECK_HAS(h, "Blank One");
    CHECK_HAS(h, "href=\"/media/settings/folders?format=sdc\"");
    CHECK_HAS(h, "APFS (a Mac"); /* (escaped: Mac&#39;s) */
    CHECK(!strstr(h, "WDC WD20EFPX")); /* (in use, set up by hand: none of Hemma's business) */
    CHECK(!strstr(h, "mmcblk0"));
    CHECK(!strstr(th_page(eva, "/media/settings/folders"), "media.drive_use"));
    CHECK_HAS(th_page(adam, "/system"), "A USB disk is plugged in: Film disk (exFAT, 1.0 TB).");
    /* the first-run page offers it with one tap */
    CHECK_HAS(th_page(adam, "/media"), "id=\"media-drive-offer\"");

    CHECK(!th_act(eva, "media.drive_use", "uuid=5E2A-1B3C").ok);
    ThResult r = th_act(adam, "media.drive_use", "uuid=9adf"); /* in use */
    CHECK(!r.ok);
    r = th_act(adam, "media.drive_use", "uuid=aaaa-bbbb"); /* apfs */
    CHECK(!r.ok);
    CHECK(th_act(adam, "media.drive_use", "uuid=5E2A-1B3C").ok);
    char req[120];
    request(&m, req, sizeof req);
    CHECK_STR(req, "use 5E2A-1B3C");
    CHECK_HAS(th_page(adam, "/media/settings/folders"), "Setting up the disk");
    r = th_act(adam, "media.drive_use", "uuid=5E2A-1B3C");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "busy");

    /* the helper did it: mounted, listed, answered - the folder joins the library */
    char mnt[700], line[1200];
    snprintf(mnt, sizeof mnt, "%s/mnt/film-disk", m.root);
    CHECK(dir_make(mnt));
    snprintf(line, sizeof line, "5E2A-1B3C %s exfat Film disk\n", mnt);
    put(m.ans, "disks", line);
    snprintf(line, sizeof line, ROOT_MOUNTS "60 26 8-17 / %s rw - exfat /dev/sdb1 rw\n", mnt);
    mounts(&m, line);
    snprintf(line, sizeof line, "used %lld 5E2A-1B3C %s\n", (long long)now_sec() + 1, mnt);
    put(m.ans, "result", line);
    media_test_drives(m.sys, m.udev, m.minfo); /* (look again now) */
    media_drives_tick(now_ms());
    char roots[MEDIA_MAX_ROOTS][300];
    int nroots = media_roots(roots, MEDIA_MAX_ROOTS);
    CHECK_INT(nroots, 1);
    CHECK_STR(roots[0], mnt);
    h = th_page(adam, "/media/settings/folders");
    CHECK_HAS(h, "<b>Film disk is ready.</b>");
    CHECK_HAS(h, "id=\"media-mine-5E2A-1B3C\"");
    CHECK(!strstr(h, "New disk found: Film disk"));
    CHECK_HAS(th_page(adam, "/"), "added the movie disk"); /* (Latest) */
    CHECK(th_act(adam, "media.drive_seen", "").ok);
    CHECK(!strstr(th_page(adam, "/media/settings/folders"), "is ready.</b>"));
    /* the answer again (a restart reads it once more): nothing twice */
    media_test_drive_dirs(m.ask, m.ans);
    media_drives_tick(now_ms());
    CHECK_INT(media_roots(roots, MEDIA_MAX_ROOTS), 1);

    /* unplugged: missing */
    char d[800];
    snprintf(d, sizeof d, "%s/sdb/sdb1/dev", m.sys);
    CHECK(file_remove(d));
    snprintf(d, sizeof d, "%s/sdb/dev", m.sys);
    CHECK(file_remove(d));
    mounts(&m, ROOT_MOUNTS);
    media_test_drives(m.sys, m.udev, m.minfo);
    media_drives_tick(now_ms());
    CHECK_HAS(th_page(adam, "/system"), "The movie disk &quot;Film disk&quot;");
    CHECK_HAS(th_page(adam, "/media/settings/folders"), "missing: plug it back in");
    /* plugged in again, not mounted (a Pi doesn't by itself after its start): Hemma asks */
    snprintf(d, sizeof d, "%s/sdb", m.sys);
    put(d, "dev", "8-16");
    snprintf(d, sizeof d, "%s/sdb/sdb1", m.sys);
    put(d, "dev", "8-17");
    put(m.ans, "result", "refused 1 - - (an old answer)\n"); /* (answered: the helper isn't busy) */
    media_test_drive_dirs(m.ask, m.ans);
    media_test_drives(m.sys, m.udev, m.minfo);
    media_drives_tick(now_ms());
    request(&m, req, sizeof req);
    CHECK_STR(req, "use 5E2A-1B3C");

    /* forgotten: out of the library */
    snprintf(line, sizeof line, "refused %lld - - fine\n", (long long)now_sec() + 2);
    put(m.ans, "result", line);
    media_drives_tick(now_ms());
    CHECK(th_act(adam, "media.drive_forget", "uuid=5E2A-1B3C").ok);
    request(&m, req, sizeof req);
    CHECK_STR(req, "forget 5E2A-1B3C");
    put(m.ans, "disks", "");
    snprintf(line, sizeof line, "forgotten %lld 5E2A-1B3C %s\n", (long long)now_sec() + 3, mnt);
    put(m.ans, "result", line);
    media_drives_tick(now_ms());
    CHECK_INT(media_roots(roots, MEDIA_MAX_ROOTS), 0);
    th_halt();
}

TEST(media_drives_formats_only_an_empty_disk_typed_erase) {
    th_boot();
    int adam = admin("Adam");
    Machine m;
    machine(&m);
    helper(&m);
    const char *h = th_page(adam, "/media/settings/folders?format=sdc");
    CHECK_HAS(h, "role=\"dialog\"");
    CHECK_HAS(h, "<b>Blank One</b> (500 GB)");
    CHECK_HAS(h, "Type ERASE to format it");
    CHECK(!strstr(th_page(adam, "/media/settings/folders?format=sdb"), "role=\"dialog\"")); /* (not empty) */
    CHECK(!strstr(th_page(adam, "/media/settings/folders?format=mmcblk0"), "role=\"dialog\""));
    char bytes[40];
    snprintf(bytes, sizeof bytes, "%lld", 976773168LL * 512);
    char form[200];
    snprintf(form, sizeof form, "disk=sdc&bytes=%s&confirm=yes", bytes);
    ThResult r = th_act(adam, "media.drive_format", form);
    CHECK(!r.ok);
    CHECK_STR(r.field, "confirm");
    snprintf(form, sizeof form, "disk=sdb&bytes=%lld&confirm=ERASE", 1953525168LL * 512);
    CHECK(!th_act(adam, "media.drive_format", form).ok); /* exFAT on it: never */
    snprintf(form, sizeof form, "disk=sdc&bytes=12345&confirm=ERASE");
    CHECK(!th_act(adam, "media.drive_format", form).ok); /* not the disk shown */
    snprintf(form, sizeof form, "disk=sdc&bytes=%s&confirm=erase", bytes);
    r = th_act(adam, "media.drive_format", form);
    CHECK(r.ok);
    CHECK_STR(r.redirect, "/media/settings/folders");
    char req[120], want[120];
    request(&m, req, sizeof req);
    snprintf(want, sizeof want, "format sdc %s", bytes);
    CHECK_STR(req, want);
    th_halt();
}

TEST(media_drives_without_the_helper_say_how) {
    th_boot();
    int adam = admin("Adam");
    Machine m;
    machine(&m);
    media_test_drives(m.sys, m.udev, m.minfo);
    media_test_drive_dirs(NULL, NULL);
    media_drives_tick(now_ms());
    const char *h = th_page(adam, "/media/settings/folders");
    CHECK_HAS(h, "New disk found: Film disk");
    CHECK_HAS(h, "needs its disk helper");
    CHECK(!strstr(h, "data-a=\"media.drive_use\""));
    CHECK(!strstr(h, "?format="));
    ThResult r = th_act(adam, "media.drive_use", "uuid=5E2A-1B3C");
    CHECK(!r.ok);
    CHECK_HAS(r.toast, "no disk helper");
    th_halt();
}

TEST(media_drives_leave_a_machine_without_usb_disks_alone) {
    th_boot();
    int adam = admin("Adam");
    Machine m;
    str_copy(m.root, sizeof m.root, test_tmpdir());
    snprintf(m.sys, sizeof m.sys, "%s/no-such-sys", m.root);
    snprintf(m.udev, sizeof m.udev, "%s/no-udev", m.root);
    snprintf(m.minfo, sizeof m.minfo, "%s/no-mountinfo", m.root);
    media_test_drives(m.sys, m.udev, m.minfo);
    media_test_drive_dirs(NULL, NULL);
    media_drives_tick(now_ms());
    CHECK(!strstr(th_page(adam, "/media/settings/folders"), "media-drive"));
    CHECK(!strstr(th_page(adam, "/system"), "New disk found"));
    th_halt();
}
