/*
 * A small read-only USB drive holding an offline copy of the keymap guide.
 *
 * Plug the left half into a computer and, alongside the keyboard, a ~100 KB
 * volume called KLOR GUIDE appears with KEYMAP.HTM on it. Open it in a browser
 * and you have the layout, with no network and no software to install. Anyone
 * who borrows the keyboard finds the documentation on the keyboard.
 *
 * THERE IS NO FILESYSTEM CODE HERE. src/usb_disk_image.h is a complete FAT12
 * image built by scripts/gen_usb_disk.py; this driver hands those bytes to
 * Zephyr's mass-storage class and the HOST does all the parsing. That is why
 * USB_MASS_STORAGE only needs DISK_ACCESS and no FAT support: the device is a
 * block device and nothing more. Building the image on a workstation also means
 * its layout is verified by fsck.fat and mtools before it is ever compiled in,
 * rather than debugged on a microcontroller that cannot be inspected.
 *
 * READ-ONLY IN THREE PLACES: status() reports DISK_STATUS_WR_PROTECT, write()
 * refuses outright, and the file's directory entry carries the read-only
 * attribute. The contents can only change by reflashing.
 *
 * THE HOST STILL MOUNTS IT READ-WRITE, and that cannot be fixed from here.
 * What tells an operating system a medium is write-protected is the WP bit --
 * the top bit of the device-specific parameter, byte 2 of the MODE SENSE(6)
 * parameter header. Zephyr's msc.c answers that command from a hardcoded
 * constant:
 *
 *     static bool modeSense6(void) {
 *         uint8_t sense6[] = { 0x03, 0x00, 0x00, 0x00 };
 *
 * Byte 2 is zero and nothing consults disk_access_status() to build it, so the
 * WP bit is never set no matter what this driver reports. Fixing it means
 * patching Zephyr, which this config deliberately does not do -- ZMK is pinned
 * by SHA precisely so it is not carrying local patches.
 *
 * The practical effect is cosmetic but confusing: a file manager will let you
 * appear to create or delete files. Those writes are refused by the device and
 * live only in the host's page cache, so they vanish on unmount. Verified on
 * hardware -- a file created on the mounted volume was gone after a remount and
 * KEYMAP.HTM was byte-identical to the repo copy. The data really is immutable;
 * the host is simply not warned in advance.
 *
 * LEFT HALF ONLY, and not by preference. ZMK_USB is declared
 * "depends on (!ZMK_SPLIT || ZMK_SPLIT_ROLE_CENTRAL)", so the peripheral cannot
 * present a USB device at all. Over Bluetooth there is no file transfer of any
 * kind, so this is inherently a plugged-in feature.
 *
 * INIT ORDER MATTERS. Zephyr's mass-storage class runs
 * SYS_INIT(mass_storage_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEVICE)
 * and calls disk_access_init() plus the sector-count and sector-size ioctls
 * right there. If this disk is not registered by then it logs "Storage init
 * ERROR !!!! - Aborting USB init" and gives up, so registration happens at
 * POST_KERNEL -- an entire init level earlier.
 *
 * The sector size must be 512. msc.c hardcodes BLOCK_SIZE 512 and aborts if the
 * disk reports anything else.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/disk.h>

#include "usb_disk_image.h"

LOG_MODULE_REGISTER(klor_usb_disk, LOG_LEVEL_INF);

BUILD_ASSERT(KLOR_DISK_SECTOR_SIZE == 512,
             "Zephyr's mass-storage class hardcodes a 512 byte block and "
             "refuses to start with anything else");
BUILD_ASSERT(KLOR_DISK_STORED_BYTES % KLOR_DISK_SECTOR_SIZE == 0,
             "the stored prefix must be a whole number of sectors");
BUILD_ASSERT(KLOR_DISK_STORED_BYTES <= KLOR_DISK_SECTOR_COUNT * KLOR_DISK_SECTOR_SIZE,
             "stored image is larger than the volume it claims to be");

static int klor_disk_init(struct disk_info *disk) { return 0; }

static int klor_disk_status(struct disk_info *disk) {
    /* Present and initialised, but write protected -- the host is told before
     * it tries, rather than discovering it through a failed write. */
    return DISK_STATUS_WR_PROTECT;
}

static int klor_disk_read(struct disk_info *disk, uint8_t *buf, uint32_t start_sector,
                          uint32_t num_sector) {
    if (start_sector + num_sector > KLOR_DISK_SECTOR_COUNT) {
        return -EIO;
    }

    size_t off = (size_t)start_sector * KLOR_DISK_SECTOR_SIZE;
    size_t len = (size_t)num_sector * KLOR_DISK_SECTOR_SIZE;

    /* Only the used prefix of the image is in flash. Everything past it is
     * blank space in the volume, so it is synthesised rather than stored --
     * that is worth about 60 KB of flash on a 100 KB volume. */
    size_t from_flash = 0;
    if (off < KLOR_DISK_STORED_BYTES) {
        from_flash = MIN(len, KLOR_DISK_STORED_BYTES - off);
        memcpy(buf, &klor_disk_image[off], from_flash);
    }
    if (from_flash < len) {
        memset(buf + from_flash, 0, len - from_flash);
    }
    return 0;
}

static int klor_disk_write(struct disk_info *disk, const uint8_t *buf, uint32_t start_sector,
                           uint32_t num_sector) {
    /* Unreachable in practice: status() advertises write protection, so the
     * class layer drops writes before they get here. */
    return -ENOTSUP;
}

static int klor_disk_ioctl(struct disk_info *disk, uint8_t cmd, void *buf) {
    switch (cmd) {
    case DISK_IOCTL_CTRL_INIT:
    case DISK_IOCTL_CTRL_DEINIT:
    case DISK_IOCTL_CTRL_SYNC:
        /* Nothing to flush -- the whole volume lives in flash as const data. */
        return 0;
    case DISK_IOCTL_GET_SECTOR_COUNT:
        *(uint32_t *)buf = KLOR_DISK_SECTOR_COUNT;
        return 0;
    case DISK_IOCTL_GET_SECTOR_SIZE:
        *(uint32_t *)buf = KLOR_DISK_SECTOR_SIZE;
        return 0;
    case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
        *(uint32_t *)buf = 1;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static const struct disk_operations klor_disk_ops = {
    .init = klor_disk_init,
    .status = klor_disk_status,
    .read = klor_disk_read,
    .write = klor_disk_write,
    .ioctl = klor_disk_ioctl,
};

/* The name is how msc.c finds us: it looks up CONFIG_MASS_STORAGE_DISK_NAME. */
static struct disk_info klor_disk = {
    .name = CONFIG_MASS_STORAGE_DISK_NAME,
    .ops = &klor_disk_ops,
};

static int klor_usb_disk_init(void) {
    int rc = disk_access_register(&klor_disk);
    if (rc) {
        LOG_ERR("failed to register the guide disk: %d", rc);
        return rc;
    }

    LOG_INF("guide disk ready: %u sectors of %u bytes, %u KB stored in flash",
            KLOR_DISK_SECTOR_COUNT, KLOR_DISK_SECTOR_SIZE, KLOR_DISK_STORED_BYTES / 1024);
    return 0;
}

/* POST_KERNEL, so this is registered before the mass-storage class goes looking
 * for it at APPLICATION. See the header comment. */
SYS_INIT(klor_usb_disk_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
