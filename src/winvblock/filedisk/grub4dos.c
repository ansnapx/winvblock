/**
 * Copyright (C) 2009-2011, Shao Miller <shao.miller@yrdsb.edu.on.ca>.
 *
 * This file is part of WinVBlock, derived from WinAoE.
 *
 * WinVBlock is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * WinVBlock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with WinVBlock.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * GRUB4DOS file-backed disk specifics.
 */

#include <ntddk.h>
#include <initguid.h>
#include <ntddstor.h>

#include "portable.h"
#include "winvblock.h"
#include "irp.h"
#include "wv_stdlib.h"
#include "wv_string.h"
#include "driver.h"
#include "bus.h"
#include "device.h"
#include "disk.h"
#include "libthread.h"
#include "filedisk.h"
#include "debug.h"
#include "probe.h"
#include "grub4dos.h"
#include "byte.h"
#include "msvhd.h"

/* From bus.c */
extern WVL_S_BUS_T WvBus;

/** Private. */
static WVL_FP_DISK_IO WvFilediskG4dPrevIo_;
static WVL_F_DISK_IO WvFilediskG4dIo_;

/**
 * Check if a disk might be the matching backing disk for
 * a GRUB4DOS sector-mapped disk.
 *
 * @v file              HANDLE to an open disk.
 * @v filedisk_ptr      Points to the filedisk to match against.
 */
static NTSTATUS STDCALL check_disk_match(
    IN HANDLE file,
    IN WV_SP_FILEDISK_T filedisk_ptr
  ) {
    WV_SP_MSVHD_FOOTER buf;
    NTSTATUS status;
    IO_STATUS_BLOCK io_status;
    LARGE_INTEGER end_part;

    /* Allocate a buffer for testing for a MS .VHD footer. */
    buf = wv_malloc(sizeof *buf);
    if (buf == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto err_alloc;
      }
    /*
     * Read in the buffer.  Note that we adjust for the .VHD footer (plus
     * prefixed padding for an .ISO) that we truncated from the reported disk
     * size earlier when the disk mapping was found.
     */
    end_part.QuadPart =
      filedisk_ptr->offset.QuadPart +
      (filedisk_ptr->disk->LBADiskSize * filedisk_ptr->disk->SectorSize) +
      filedisk_ptr->disk->SectorSize -
      sizeof *buf;
    status = ZwReadFile(
        file,
        NULL,
        NULL,
        NULL,
        &io_status,
        buf,
        sizeof *buf,
        &end_part,
        NULL
      );
    if (!NT_SUCCESS(status))
      goto err_read;
    /* Adjust the footer's byte ordering. */
    msvhd__footer_swap_endian(buf);
    /* Examine .VHD fields for validity. */
    if (!wv_memcmpeq(&buf->cookie, "conectix", sizeof buf->cookie))
      status = STATUS_UNSUCCESSFUL;
    if (buf->file_ver.val != 0x10000)
      status = STATUS_UNSUCCESSFUL;
    if (buf->data_offset.val != 0xffffffff)
      status = STATUS_UNSUCCESSFUL;
    if (buf->orig_size.val != buf->cur_size.val)
      status = STATUS_UNSUCCESSFUL;
    if (buf->type.val != 2)
      status = STATUS_UNSUCCESSFUL;
    /* Match against our expected disk size. */
    if (
        (filedisk_ptr->disk->LBADiskSize * filedisk_ptr->disk->SectorSize ) !=
        buf->cur_size.val
      )
      status = STATUS_UNSUCCESSFUL;

    err_read:

    wv_free(buf);
    err_alloc:

    return status;
  }

/**
 * Temporarily used by established disks in order to access the
 * backing disk late(r) during the boot process.
 */
static NTSTATUS STDCALL WvFilediskG4dIo_(
    IN WVL_SP_DISK_T disk,
    IN WVL_E_DISK_IO_MODE mode,
    IN LONGLONG start_sector,
    IN UINT32 sector_count,
    IN PUCHAR buffer,
    IN PIRP irp
  ) {
    WV_SP_FILEDISK_T filedisk_ptr;
    NTSTATUS status;
    GUID disk_guid = GUID_DEVINTERFACE_DISK;
    PWSTR sym_links;
    PWCHAR pos;
    HANDLE file;

    /* Establish pointer to the filedisk. */
    filedisk_ptr = CONTAINING_RECORD(disk, WV_S_FILEDISK_T, disk);
    /*
     * Find the backing disk and use it.  We walk a list
     * of unicode disk device names and check each one.
     */
    status = IoGetDeviceInterfaces(&disk_guid, NULL, 0, &sym_links);
    if (!NT_SUCCESS(status))
      goto dud;
    pos = sym_links;
    while (*pos != UNICODE_NULL) {
        UNICODE_STRING path;
        OBJECT_ATTRIBUTES obj_attrs;
        IO_STATUS_BLOCK io_status;

        RtlInitUnicodeString(&path, pos);
        InitializeObjectAttributes(
            &obj_attrs,
            &path,
            OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
            NULL,
            NULL
          );
        /* Try this disk. */
        file = 0;
        status = ZwCreateFile(
            &file,
            GENERIC_READ | GENERIC_WRITE,
            &obj_attrs,
            &io_status,
            NULL,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE,
            FILE_OPEN,
            FILE_NON_DIRECTORY_FILE |
              FILE_RANDOM_ACCESS |
              FILE_SYNCHRONOUS_IO_NONALERT,
            NULL,
            0
          );
        /* If we could open it, check it out. */
        if (NT_SUCCESS(status))
          status = check_disk_match(file, filedisk_ptr);
        /* If we liked this disk as our backing disk, stop looking. */
        if (NT_SUCCESS(status))
          break;
        /* We could not open this disk or didn't like it.  Try the next one. */
        if (file)
          ZwClose(file);
        while (*pos != UNICODE_NULL)
          pos++;
        pos++;
      } /* while */
    wv_free(sym_links);
    /* If we did not find the backing disk, we are a dud. */
    if (!NT_SUCCESS(status))
      goto dud;
    /* Use the backing disk and restore the original read/write routine. */
    filedisk_ptr->file = file;
    filedisk_ptr->disk->disk_ops.Io = WvFilediskG4dPrevIo_;
    /* Call the original read/write routine. */
    return WvFilediskG4dPrevIo_(
        filedisk_ptr->disk,
        mode,
        start_sector,
        sector_count,
        buffer,
        irp
      );

    dud:
    return WvlIrpComplete(irp, 0, STATUS_DEVICE_NOT_READY);
  }

typedef struct WV_FILEDISK_GRUB4DOS_DRIVE_FILE_SET {
    UCHAR int13_drive_num;
    char *filepath;
  }
  WV_S_FILEDISK_GRUB4DOS_DRIVE_FILE_SET,
  * WV_SP_FILEDISK_GRUB4DOS_DRIVE_FILE_SET;

static VOID STDCALL process_param_block(
    const char * param_block,
    WV_SP_FILEDISK_GRUB4DOS_DRIVE_FILE_SET sets
  ) {
    const char * end = param_block + 2047;
    const char sig[] = "#!GRUB4DOS";
    const char ver[] = "v=1";
    int i = 0;

    /* Check signature. */
    if (!wv_memcmpeq(param_block, sig, sizeof sig)) {
        DBG("RAM disk is not a parameter block.  Skipping.\n");
        return;
      }
    param_block += sizeof sig;
    /*
     * Looks like a parameter block someone passed from GRUB4DOS.
     * Check the version.
     */
    if (!wv_memcmpeq(param_block, ver, sizeof ver)) {
        DBG("Parameter block version unsupported.  Skipping.\n");
        return;
      }
    param_block += sizeof ver;
    /*
     * We are interested in {filepath, NUL, X} sets,
     * where X is INT13h drive num.
     */
    RtlZeroMemory(sets, sizeof *sets * 8);
    while (param_block < end && i != 8) {
        const char *walker = param_block;

        /* Walk to ASCII NUL terminator. */
        while (walker != end && *walker)
          walker++;
        if (walker == param_block || walker == end)
          /* End of filenames or run-away sequence. */
          break;
        walker++;
        /* Make a note of the filename.  Skip initial '/' or '\'. */
        if (*param_block == '/' || *param_block == '\\')
          param_block++;
        sets[i].filepath = wv_malloc(walker - param_block);
        if (sets[i].filepath == NULL) {
            DBG("Could not store filename\n");
            /* Skip drive num. */
            walker++;
            param_block = walker;
            continue;
          } /* if */
        RtlCopyMemory(sets[i].filepath, param_block, walker - param_block);
        /* Replace '/' with '\'. */
        {   char * rep = sets[i].filepath;

            while (*rep) {
                if (*rep == '/')
                  *rep = '\\';
                rep++;
              }
          } /* rep scope. */
        /* The next byte is expected to be the INT 13h drive num. */
        sets[i].int13_drive_num = *walker;
        walker++;
        i++;
        param_block = walker;
      } /* while */
  }

VOID filedisk_grub4dos__find(void) {
    PHYSICAL_ADDRESS PhysicalAddress;
    PUCHAR PhysicalMemory;
    WV_SP_PROBE_INT_VECTOR InterruptVector;
    UINT32 Int13Hook;
    WV_SP_PROBE_SAFE_MBR_HOOK SafeMbrHookPtr;
    WV_SP_GRUB4DOS_DRIVE_MAPPING Grub4DosDriveMapSlotPtr;
    UINT32 i;
    BOOLEAN FoundGrub4DosMapping = FALSE;
    WV_SP_FILEDISK_T filedisk_ptr;
    const char sig[] = "GRUB4DOS";
    /* Matches disks to files. */
    WV_S_FILEDISK_GRUB4DOS_DRIVE_FILE_SET sets[8] = {0};

    /*
     * Find a GRUB4DOS sector-mapped disk.  Start by looking at the
     * real-mode IDT and following the "SafeMBRHook" INT 0x13 hook.
     */
    PhysicalAddress.QuadPart = 0LL;
    PhysicalMemory = MmMapIoSpace(PhysicalAddress, 0x100000, MmNonCached);
    if (!PhysicalMemory) {
        DBG("Could not map low memory\n");
        return;
      }
    InterruptVector = (WV_SP_PROBE_INT_VECTOR) (
        PhysicalMemory + 0x13 * sizeof *InterruptVector
      );
    /* Walk the "safe hook" chain of INT 13h hooks as far as possible. */
    while (SafeMbrHookPtr = WvProbeGetSafeHook(
        PhysicalMemory,
        InterruptVector
      )) {
        /* Check signature. */
        if (!wv_memcmpeq(SafeMbrHookPtr->VendorId, sig, sizeof sig - 1)) {
            DBG("Non-GRUB4DOS INT 0x13 Safe Hook\n");
            InterruptVector = &SafeMbrHookPtr->PrevHook;
            continue;
          }
        Grub4DosDriveMapSlotPtr = (WV_SP_GRUB4DOS_DRIVE_MAPPING) (
            PhysicalMemory +
            (((UINT32) InterruptVector->Segment) << 4)
            + 0x20
          );
        /*
         * Search for parameter blocks, which are disguised as
         * GRUB4DOS RAM disk mappings for 2048-byte memory regions.
         */
        i = 8;
        while (i--) {
            PHYSICAL_ADDRESS param_block_addr;
            char * param_block;

            if (Grub4DosDriveMapSlotPtr[i].DestDrive != 0xff)
              continue;
            param_block_addr.QuadPart =
              Grub4DosDriveMapSlotPtr[i].SectorStart * 512;
            param_block = MmMapIoSpace(param_block_addr, 2048, MmNonCached);
            if (param_block == NULL) {
                DBG("Could not map potential G4D parameter block\n");
                continue;
              }
            /*
             * Could be a parameter block.  Process it.  There can be only one.
             */
            process_param_block(param_block, sets);
            MmUnmapIoSpace(param_block, 2048);
            if (sets[0].filepath != NULL)
              break;
          } /* search for parameter blocks. */
      /* Search for sector-mapped (typically file-backed) disks. */
      i = 8;
      while (i--) {
          WVL_E_DISK_MEDIA_TYPE media_type;
          UINT32 sector_size;

          if (
              (Grub4DosDriveMapSlotPtr[i].SectorCount == 0) ||
              (Grub4DosDriveMapSlotPtr[i].DestDrive == 0xff)
            ) {
              DBG("Skipping non-sector-mapped GRUB4DOS disk\n");
              continue;
            }
          DBG(
              "GRUB4DOS SourceDrive: 0x%02x\n",
              Grub4DosDriveMapSlotPtr[i].SourceDrive
            );
          DBG(
              "GRUB4DOS DestDrive: 0x%02x\n",
              Grub4DosDriveMapSlotPtr[i].DestDrive
            );
          DBG("GRUB4DOS MaxHead: %d\n", Grub4DosDriveMapSlotPtr[i].MaxHead);
          DBG(
              "GRUB4DOS MaxSector: %d\n",
              Grub4DosDriveMapSlotPtr[i].MaxSector
            );
          DBG(
              "GRUB4DOS DestMaxCylinder: %d\n",
              Grub4DosDriveMapSlotPtr[i].DestMaxCylinder
            );
          DBG(
              "GRUB4DOS DestMaxHead: %d\n",
              Grub4DosDriveMapSlotPtr[i].DestMaxHead
            );
          DBG(
              "GRUB4DOS DestMaxSector: %d\n",
              Grub4DosDriveMapSlotPtr[i].DestMaxSector
            );
          DBG(
              "GRUB4DOS SectorStart: 0x%08x\n",
              Grub4DosDriveMapSlotPtr[i].SectorStart
            );
          DBG(
              "GRUB4DOS SectorCount: %d\n",
              Grub4DosDriveMapSlotPtr[i].SectorCount
            );
          /* Possible precision loss. */
          if (Grub4DosDriveMapSlotPtr[i].SourceODD) {
              media_type = WvlDiskMediaTypeOptical;
              sector_size = 2048;
            } else {
              media_type =
                (Grub4DosDriveMapSlotPtr[i].SourceDrive & 0x80) ?
                WvlDiskMediaTypeHard :
                WvlDiskMediaTypeFloppy;
              sector_size = 512;
            }
          /*
           * Create the threaded, file-backed disk.  Hook the
           * read/write routine so we can accessing the backing disk
           * late(r) during the boot process.
           */
          filedisk_ptr = WvFilediskCreatePdo(media_type);
          if (filedisk_ptr == NULL) {
              DBG("Could not create GRUB4DOS disk!\n");
              return;
            }
          /* Record the usual filedisk I/O function for later. */
          WvFilediskG4dPrevIo_ = filedisk_ptr->disk->disk_ops.Io;
          /* Hook the first I/O request. */
          filedisk_ptr->disk->disk_ops.Io = WvFilediskG4dIo_;
          /* Other parameters we know. */
          filedisk_ptr->disk->Media = media_type;
          filedisk_ptr->disk->SectorSize = sector_size;
          /* Find an associated filename, if one exists. */
          {   int j = 8;

              while (j--) {
                  if (
                      (sets[j].int13_drive_num ==
                        Grub4DosDriveMapSlotPtr[i].SourceDrive) &&
                      (sets[j].filepath != NULL)
                    )
                    WvFilediskHotSwap(filedisk_ptr, sets[j].filepath);
                }
            } /* j scope. */
          DBG("Sector-mapped disk is type: %d\n", media_type);
          /* Note the offset of the disk image from the backing disk's start. */
          filedisk_ptr->offset.QuadPart =
            Grub4DosDriveMapSlotPtr[i].SectorStart *
            (Grub4DosDriveMapSlotPtr[i].DestODD ? 2048 : 512);
          /*
           * Size and geometry.  Please note that since we require a .VHD
           * footer, we exclude this from the LBA disk size by truncating
           * a 512-byte sector for HDD images, or a 2048-byte sector for .ISO.
           */
          {   ULONGLONG total_size =
                Grub4DosDriveMapSlotPtr[i].SectorCount *
                (Grub4DosDriveMapSlotPtr[i].DestODD ? 2048 : 512);

              filedisk_ptr->disk->LBADiskSize =
                (total_size - filedisk_ptr->disk->SectorSize) /
                filedisk_ptr->disk->SectorSize;
            } /* total_size scope. */
          filedisk_ptr->disk->Heads = Grub4DosDriveMapSlotPtr[i].MaxHead + 1;
          filedisk_ptr->disk->Sectors =
            Grub4DosDriveMapSlotPtr[i].DestMaxSector;
          filedisk_ptr->disk->Cylinders =
            filedisk_ptr->disk->LBADiskSize /
            (filedisk_ptr->disk->Heads * filedisk_ptr->disk->Sectors);
          /*
           * Set a filedisk "hash" and mark the drive as a boot-drive.
           * The "hash" is 'G4DX', where X is the GRUB4DOS INT 13h
           * drive number.  Note that mutiple G4D INT 13h chains will
           * cause a "hash" collision!  Too bad for now.
           */
          filedisk_ptr->hash = 'G4DX';
          ((PUCHAR) &filedisk_ptr->hash)[0] =
            Grub4DosDriveMapSlotPtr[i].SourceDrive;
          FoundGrub4DosMapping = TRUE;
          filedisk_ptr->Dev->Boot = TRUE;
          /* Add the filedisk to the bus. */
          filedisk_ptr->disk->ParentBus = WvBus.Fdo;
          if (!WvBusAddDev(filedisk_ptr->Dev))
            WvDevFree(filedisk_ptr->Dev);
        } /* search for sector-mapped disks. */
      InterruptVector = &SafeMbrHookPtr->PrevHook;
    } /* walk the safe hook chain. */

    MmUnmapIoSpace(PhysicalMemory, 0x100000);
    if (!FoundGrub4DosMapping) {
        DBG("No GRUB4DOS sector-mapped disk mappings found\n");
      }
  }
