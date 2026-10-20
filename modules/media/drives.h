/* modules/media/drives.h — movie disks plugged into the machine (issue #74; drives.c).
 *
 * Hemma looks (every few seconds, unprivileged: /sys/block, udev's /run/udev/data, /proc/self/mountinfo)
 * for USB disks, and offers what it finds on Library settings and the System page: "Use it for movies"
 * for one with a file system it can use, "Format for Hemma" for one with nothing on it at all. Doing it
 * needs root, so it goes - like updates (system/update.c) - as one line into <data>/disks/request for
 * the root helper deploy/hemma-disks, which checks every request again itself, mounts the disk by its
 * UUID (/etc/fstab, nofail), lets Hemma write to it (hemma.service.d/disks.conf) and answers in
 * /var/lib/hemma-disks: result ("<state> <unix> <uuid|-> <path|-> <words>") and disks (Hemma's disks,
 * "<uuid> <path> <fstype> <label>" a line). A disk that came in is added to the library folders. */
#ifndef HEMMA_MEDIA_DRIVES_H
#define HEMMA_MEDIA_DRIVES_H

#include "../../core/app.h"

#define DRIVES_MAX 16
#define DRIVE_VOLS 8

typedef struct {
    char kname[32];     /* "sda1" - or "sda": a file system on the whole disk */
    char fstype[24];    /* "ext4", "exfat", "ntfs" ... ("" = nothing Linux recognises) */
    char uuid[64], label[64];
    char mounted[300];  /* where it's mounted ("" = nowhere) */
} DriveVol;

typedef struct {
    char kname[32];     /* "sda" */
    char name[80];      /* what people would call it: its model, else "USB disk" */
    int64_t bytes;
    bool usb;           /* on USB (the only disks offered) */
    bool system;        /* holds /, /boot, swap ...: never offered, never touched */
    int nvol;
    DriveVol vol[DRIVE_VOLS];
} Drive;

/* The disks under sys (/sys/block), what udev knows of them (udev = /run/udev/data) and where they're
   mounted (mountinfo = the text of /proc/self/mountinfo). Loop, RAM, zram, md, dm, optical: left out.
   Portable file reading - tests hand it a made-up tree. */
int media_drives_scan(const char *sys, const char *udev, const char *mountinfo, Drive *out, int max);
bool media_drive_fs_ok(const char *fstype);  /* a file system Hemma can keep movies on */
void media_drive_size(int64_t bytes, char *out, size_t cap); /* "2.0 TB", as disks are sold */

void media_drives_start(void);
void media_drives_stop(void);
void media_drives_tick(int64_t now_ms);
void media_drives_card(Ctx *c, Buf *b);     /* Library settings (admins) */
void media_drives_dialog(Ctx *c, Buf *b);   /* ?format=<disk>: the typed confirmation */
bool media_drives_offer(Ctx *c, Buf *b);    /* Watch's first-run page: a new disk, one tap (true = shown) */
void media_drives_health(Health *h);
void media_drives_act_use(Ctx *c, Fields *f);
void media_drives_act_format(Ctx *c, Fields *f);
void media_drives_act_forget(Ctx *c, Fields *f);
void media_drives_act_seen(Ctx *c, Fields *f);

/* tests: look here instead (NULL = the real places), and the helper's two folders */
void media_test_drives(const char *sys, const char *udev, const char *mountinfo_file);
void media_test_drive_dirs(const char *ask_dir, const char *answers);

#endif
