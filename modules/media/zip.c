/* modules/media/zip.c — ZIP archives written as they're sent (see zip.h). After PKWARE's APPNOTE.TXT 6.3:
 * 4.3.7 local header, 4.3.9 data descriptor, 4.3.12 central directory, 4.3.14-16 the end records,
 * 4.5.3 the zip64 extended information field. */
#include "zip.h"

#include <string.h>

#define M32 0xFFFFFFFFu
#define M16 0xFFFFu
#define FLAGS 0x0808u            /* bit 3: CRC and sizes follow the data; bit 11: the name is UTF-8 */
#define MADE_BY ((3u << 8) | 45) /* Unix (so the mode below counts), APPNOTE 4.5 */
#define MODE (0100644u << 16)    /* a plain file, rw-r--r-- */

static const uint32_t CRC_TABLE[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

uint32_t zip_crc32(uint32_t crc, const void *p, size_t n) {
    const uint8_t *b = p;
    crc = ~crc;
    while (n--) crc = CRC_TABLE[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void zip_dos_time(int year, int mon, int day, int hour, int min, int sec, uint16_t *time, uint16_t *date) {
    if (year < 1980) year = 1980, mon = 1, day = 1, hour = 0, min = 0, sec = 0;
    if (year > 2107) year = 2107, mon = 12, day = 31, hour = 23, min = 59, sec = 58;
    *time = (uint16_t)((hour << 11) | (min << 5) | (sec / 2));
    *date = (uint16_t)(((year - 1980) << 9) | (mon << 5) | day);
}

/* ------------------------------------------------------------ the pieces */

static uint8_t *put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
    return p + 2;
}
static uint8_t *put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
    return p + 4;
}
static uint8_t *put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
    return p + 8;
}

static bool big(const ZipEntry *e) { return e->size >= M32; }    /* its sizes need 8 bytes */
static bool far_off(uint64_t offset) { return offset >= M32; }  /* where it starts needs 8 bytes */
static size_t name_len(const ZipEntry *e) {
    size_t n = e->name ? strlen(e->name) : 0;
    return n <= ZIP_NAME_MAX ? n : 0;
}
static uint32_t needs(const ZipEntry *e, uint64_t offset) { return big(e) || far_off(offset) ? 45 : 20; }

static uint64_t local_len(const ZipEntry *e) { return 30 + name_len(e) + (big(e) ? 20 : 0); }
static uint64_t desc_len(const ZipEntry *e) { return big(e) ? 24 : 16; }
static uint64_t extra64_len(const ZipEntry *e, uint64_t offset) { /* the central header's zip64 field */
    uint64_t n = (big(e) ? 16 : 0) + (far_off(offset) ? 8 : 0);
    return n ? 4 + n : 0;
}
static uint64_t central_len(const ZipEntry *e, uint64_t offset) { return 46 + name_len(e) + extra64_len(e, offset); }
static bool end64(uint64_t n, uint64_t cd_offset, uint64_t cd_size) {
    return n >= M16 || cd_offset >= M32 || cd_size >= M32;
}

size_t zip_local_header(const ZipEntry *e, uint64_t offset, uint8_t *out) {
    size_t nl = name_len(e);
    if (!nl) return 0;
    uint8_t *p = put32(out, 0x04034b50);
    p = put16(p, needs(e, offset));
    p = put16(p, FLAGS);
    p = put16(p, 0); /* stored */
    p = put16(p, e->dos_time);
    p = put16(p, e->dos_date);
    p = put32(p, 0); /* CRC and sizes: in the data descriptor (bit 3) */
    p = put32(p, big(e) ? M32 : 0);
    p = put32(p, big(e) ? M32 : 0);
    p = put16(p, (uint32_t)nl);
    p = put16(p, big(e) ? 20 : 0);
    memcpy(p, e->name, nl);
    p += nl;
    if (big(e)) { /* says the data descriptor's sizes are 8 bytes; both sizes there, 0 like the 4-byte ones */
        p = put16(p, 1);
        p = put16(p, 16);
        p = put64(p, 0);
        p = put64(p, 0);
    }
    return (size_t)(p - out);
}

size_t zip_data_descriptor(const ZipEntry *e, uint8_t *out) {
    uint8_t *p = put32(out, 0x08074b50);
    p = put32(p, e->crc);
    if (big(e)) {
        p = put64(p, e->size);
        p = put64(p, e->size);
    } else {
        p = put32(p, (uint32_t)e->size);
        p = put32(p, (uint32_t)e->size);
    }
    return (size_t)(p - out);
}

size_t zip_central_header(const ZipEntry *e, uint64_t offset, uint8_t *out) {
    size_t nl = name_len(e);
    if (!nl) return 0;
    uint8_t *p = put32(out, 0x02014b50);
    p = put16(p, MADE_BY);
    p = put16(p, needs(e, offset));
    p = put16(p, FLAGS);
    p = put16(p, 0);
    p = put16(p, e->dos_time);
    p = put16(p, e->dos_date);
    p = put32(p, e->crc);
    p = put32(p, big(e) ? M32 : (uint32_t)e->size);
    p = put32(p, big(e) ? M32 : (uint32_t)e->size);
    p = put16(p, (uint32_t)nl);
    p = put16(p, (uint32_t)extra64_len(e, offset));
    p = put16(p, 0); /* comment */
    p = put16(p, 0); /* disk */
    p = put16(p, 0); /* internal attributes */
    p = put32(p, MODE);
    p = put32(p, far_off(offset) ? M32 : (uint32_t)offset);
    memcpy(p, e->name, nl);
    p += nl;
    if (extra64_len(e, offset)) { /* only the fields set to 0xFFFFFFFF above, in this order (4.5.3) */
        p = put16(p, 1);
        p = put16(p, (uint32_t)extra64_len(e, offset) - 4);
        if (big(e)) {
            p = put64(p, e->size); /* uncompressed */
            p = put64(p, e->size); /* compressed */
        }
        if (far_off(offset)) p = put64(p, offset);
    }
    return (size_t)(p - out);
}

size_t zip_end_records(uint64_t n, uint64_t cd_offset, uint64_t cd_size, uint8_t *out) {
    uint8_t *p = out;
    if (end64(n, cd_offset, cd_size)) {
        p = put32(p, 0x06064b50); /* the zip64 end of central directory record */
        p = put64(p, 44);         /* (its size after these 12 bytes) */
        p = put16(p, MADE_BY);
        p = put16(p, 45);
        p = put32(p, 0); /* this disk */
        p = put32(p, 0); /* the disk where the central directory starts */
        p = put64(p, n);
        p = put64(p, n);
        p = put64(p, cd_size);
        p = put64(p, cd_offset);
        p = put32(p, 0x07064b50); /* its locator */
        p = put32(p, 0);
        p = put64(p, cd_offset + cd_size);
        p = put32(p, 1); /* disks in all */
    }
    p = put32(p, 0x06054b50); /* fields too small for their number say 0xFFFF... and the zip64 record has it */
    p = put16(p, 0);
    p = put16(p, 0);
    p = put16(p, n >= M16 ? M16 : (uint32_t)n);
    p = put16(p, n >= M16 ? M16 : (uint32_t)n);
    p = put32(p, cd_size >= M32 ? M32 : (uint32_t)cd_size);
    p = put32(p, cd_offset >= M32 ? M32 : (uint32_t)cd_offset);
    p = put16(p, 0); /* comment */
    return (size_t)(p - out);
}

/* ---------------------------------------------------------- the archive */

uint64_t zip_size(const ZipEntry *e, int n) {
    uint64_t at = 0, cd = 0;
    for (int i = 0; i < n; i++) {
        cd += central_len(&e[i], at);
        at += local_len(&e[i]) + e[i].size + desc_len(&e[i]);
    }
    uint8_t end[ZIP_END_MAX];
    return at + cd + zip_end_records((uint64_t)(n > 0 ? n : 0), at, cd, end);
}

bool zip_write(ZipEntry *e, int n, const ZipIo *io, void *buf, size_t cap) {
    uint8_t h[ZIP_HEAD_MAX];
    uint64_t at = 0;
    if (!cap) return false;
    for (int i = 0; i < n; i++) {
        size_t k = zip_local_header(&e[i], at, h);
        if (!k || !io->write(io->ctx, h, k)) return false;
        uint32_t crc = 0;
        for (uint64_t left = e[i].size; left;) {
            size_t want = left < cap ? (size_t)left : cap;
            size_t got = io->read(io->ctx, i, buf, want);
            if (!got || got > want) return false;
            crc = zip_crc32(crc, buf, got);
            if (!io->write(io->ctx, buf, got)) return false;
            left -= got;
        }
        e[i].crc = crc;
        size_t d = zip_data_descriptor(&e[i], h);
        if (!io->write(io->ctx, h, d)) return false;
        at += k + e[i].size + d;
    }
    uint64_t cd_at = at, cd = 0, off = 0;
    for (int i = 0; i < n; i++) {
        size_t k = zip_central_header(&e[i], off, h);
        if (!k || !io->write(io->ctx, h, k)) return false;
        cd += k;
        off += local_len(&e[i]) + e[i].size + desc_len(&e[i]);
    }
    uint8_t end[ZIP_END_MAX];
    size_t k = zip_end_records((uint64_t)(n > 0 ? n : 0), cd_at, cd, end);
    return io->write(io->ctx, end, k);
}
