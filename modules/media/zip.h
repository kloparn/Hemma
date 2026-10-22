/* modules/media/zip.h — ZIP archives written as they're sent: a whole season in one download.
 *
 * Nothing is compressed (STORED: video doesn't shrink), so the archive's exact size is known before a
 * byte goes out (zip_size) - a browser then shows progress and the time left. Each entry's CRC-32 is
 * known only once its bytes have gone, so it follows them in a data descriptor (general purpose bit 3);
 * the central directory at the end has everything again. Names are UTF-8 (bit 11), times DOS times.
 * ZIP64 only where a number needs it - an entry of 4 GB or more (a zip64 field in its local header, so
 * its data descriptor has 8-byte sizes), an entry starting past 4 GB, a central directory past 4 GB or
 * 65,535 entries or more (the zip64 end records) - so a small archive stays a plain one.
 *
 * Pure: no globals, files or sockets. zip_write is driven by two callbacks (read the entries' bytes,
 * send the archive's), so a test can check every byte and the stream sends a season from disk. */
#ifndef HEMMA_MEDIA_ZIP_H
#define HEMMA_MEDIA_ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;            /* in the archive: UTF-8, '/' between folders, never "" */
    uint64_t size;               /* exactly this many bytes follow */
    uint16_t dos_time, dos_date; /* zip_dos_time */
    uint32_t crc;                /* CRC-32 of its bytes: zip_write fills it in as they go */
} ZipEntry;

#define ZIP_NAME_MAX 1024 /* (the format takes 65,535 bytes; nothing here needs more) */
#define ZIP_HEAD_MAX (46 + ZIP_NAME_MAX + 32) /* the largest header these functions write */
#define ZIP_END_MAX (56 + 20 + 22)

/* A local time as a DOS date and time (2-second steps; clamped to 1980-2107). */
void zip_dos_time(int year, int mon, int day, int hour, int min, int sec, uint16_t *time, uint16_t *date);

/* CRC-32 (ISO 3309, as zip and gzip use it): crc = 0 to start, then the running value. */
uint32_t zip_crc32(uint32_t crc, const void *p, size_t n);

/* The archive's size in bytes, exactly what zip_write writes for these entries (their crc aside). */
uint64_t zip_size(const ZipEntry *e, int n);

/* The pieces, each written into out (at most ZIP_HEAD_MAX / ZIP_END_MAX bytes), returning its length:
   an entry's local header (offset: where it starts in the archive), its data descriptor (after its
   bytes, with e->crc), its central directory header, and the end records for n entries whose central
   directory starts at cd_offset and is cd_size long. 0 = a name that's empty or too long. */
size_t zip_local_header(const ZipEntry *e, uint64_t offset, uint8_t *out);
size_t zip_data_descriptor(const ZipEntry *e, uint8_t *out);
size_t zip_central_header(const ZipEntry *e, uint64_t offset, uint8_t *out);
size_t zip_end_records(uint64_t n, uint64_t cd_offset, uint64_t cd_size, uint8_t *out);

typedef struct {
    /* entry i's next bytes, in order, into buf (at most cap): how many - 0 = it ended early or can't be
       read. zip_write asks for entry i only after entry i-1 was read to its end, and never past its size. */
    size_t (*read)(void *ctx, int i, void *buf, size_t cap);
    /* the archive's next bytes: false = stop (the other end went away) */
    bool (*write)(void *ctx, const void *p, size_t n);
    void *ctx;
} ZipIo;

/* The whole archive: exactly zip_size(e, n) bytes went to io->write when it returns true, and every
   e[i].crc is filled in. false = a read came up short or a write failed (the archive is unfinished).
   buf/cap: room for the entries' bytes on their way (a few hundred KB). */
bool zip_write(ZipEntry *e, int n, const ZipIo *io, void *buf, size_t cap);

#endif
