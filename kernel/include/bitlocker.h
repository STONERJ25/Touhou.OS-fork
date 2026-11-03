/*
 * BitLocker, read only: the metadata of a BitLocker volume, the unlocking of its volume master key with a CLEAR KEY protector, and the decrypting of its sectors.
 * Written from the libbde "BitLocker Drive Encryption (BDE) format" document, NIST SP 800-38C (CCM), IEEE 1619 / NIST SP 800-38E (XTS) and Ferguson's "AES-CBC +
 * Elephant diffuser" paper. docs/NTFS.md has the structures as implemented and says what was only checked against the test generator.
 *
 * A clear key is what Windows leaves on a volume while BitLocker is "suspended" (or encrypted but not yet activated): the key that wraps the volume master key
 * is stored on the disk in the open, so no password is needed. A volume with only other protectors (TPM, PIN, recovery password, password, startup key) is not
 * unlocked: bde_open() fails with -EACCES and the report names the protectors. Nothing here ever writes to the device, and the decrypted view is read only.
 *
 * The parser and the ciphers are plain C over a read callback (the same sources are built for the host in tests/host/ntfstool.c); only bde_attach() and
 * bde_detach(), which make the decrypted view a block device, are kernel code.
 */
#ifndef K_BITLOCKER_H
#define K_BITLOCKER_H

#ifndef NTFS_HOST
#include "types.h"
#endif

typedef struct BdeVol BdeVol;

typedef struct BdeIo {
    int (*read)(void *ctx, u64 off, void *buf, size_t len);      /* bytes of the BitLocker volume; 0 or -errno */
    void *ctx;
    u64 size;                                                    /* bytes */
    u32 sector;                                                  /* the device's sector size */
} BdeIo;

#define BDE_MAX_PROT 16

typedef struct BdeReport {
    int nprot;
    u16 prot[BDE_MAX_PROT];                                      /* protection types of the VMK entries: 0 clear key, 0x100 TPM, 0x200 startup key, ... */
    u16 method;                                                  /* encryption method of the volume (0x8000 .. 0x8005), 0 when unknown */
    bool unlocked;
    char text[480];                                              /* one line for the log: what was found, and why not when it failed */
} BdeReport;

bool bde_is_bitlocker(const u8 *sector0);                        /* "-FVE-FS-" at byte 3 */
int  bde_open(const BdeIo *io, BdeVol **out, BdeReport *rep);    /* parse the metadata, unlock with the clear key, check the result is NTFS; -EACCES without a clear key */
void bde_close(BdeVol *b);
int  bde_read(BdeVol *b, u64 off, void *buf, size_t len);        /* decrypted bytes; off and len must be multiples of the sector size */
u64  bde_size(const BdeVol *b);
u32  bde_sector(const BdeVol *b);
int  bde_dev_read(void *ctx, u64 off, void *buf, size_t len);    /* a read callback over a BdeVol that accepts any offset and length */

#ifndef NTFS_HOST
#include "blk.h"
/* Opens the BitLocker volume on `src` and registers the decrypted, read-only view as a block device (bde0, bde1, ...). On failure `why` says what was found. */
int  bde_attach(BlockDev *src, BdeVol **bde, BlockDev **view, char *why, size_t whyn);
void bde_detach(BdeVol *bde, BlockDev *view);
#endif

#endif
