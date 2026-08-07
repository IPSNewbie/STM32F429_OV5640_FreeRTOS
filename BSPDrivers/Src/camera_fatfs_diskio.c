#include "camera_fatfs_diskio.h"

#include "camera_sd_storage.h"
#include "ff.h"
#include "diskio.h"

#include <stddef.h>

DSTATUS disk_status(BYTE physical_drive)
{
    if (physical_drive != CAMERA_FATFS_PHYSICAL_DRIVE)
    {
        return STA_NOINIT;
    }

    return (Camera_SDStorage_FatFsDiskStatus() == CAMERA_SD_OK) ?
        0U : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE physical_drive)
{
    if (physical_drive != CAMERA_FATFS_PHYSICAL_DRIVE)
    {
        return STA_NOINIT;
    }

    return (Camera_SDStorage_FatFsDiskInitialize() == CAMERA_SD_OK) ?
        0U : STA_NOINIT;
}

DRESULT disk_read(
    BYTE physical_drive,
    BYTE *buffer,
    LBA_t sector,
    UINT count)
{
    uint32_t result;

    if ((physical_drive != CAMERA_FATFS_PHYSICAL_DRIVE) ||
        (buffer == NULL) ||
        (count == 0U))
    {
        return RES_PARERR;
    }

    result = Camera_SDStorage_FatFsDiskRead(
        buffer,
        (uint32_t)sector,
        (uint32_t)count);
    if (result == CAMERA_SD_OK)
    {
        return RES_OK;
    }
    if (result == CAMERA_SD_ERR_FATFS_DISK_NOT_READY)
    {
        return RES_NOTRDY;
    }
    if (result == CAMERA_SD_ERR_INVALID_ARGUMENT)
    {
        return RES_PARERR;
    }

    return RES_ERROR;
}

DRESULT disk_ioctl(BYTE physical_drive, BYTE command, void *buffer)
{
    uint32_t result;

    if (physical_drive != CAMERA_FATFS_PHYSICAL_DRIVE)
    {
        return RES_PARERR;
    }

    result = Camera_SDStorage_FatFsDiskIoctl(command, buffer);
    if (result == CAMERA_SD_OK)
    {
        return RES_OK;
    }
    if (result == CAMERA_SD_ERR_FATFS_DISK_NOT_READY)
    {
        return RES_NOTRDY;
    }
    if (result == CAMERA_SD_ERR_INVALID_ARGUMENT)
    {
        return RES_PARERR;
    }

    return RES_ERROR;
}
