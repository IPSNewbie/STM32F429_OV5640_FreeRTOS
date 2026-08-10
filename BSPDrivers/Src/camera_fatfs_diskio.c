#include "camera_fatfs_diskio.h"  // 本工程映射给 FatFs 的物理盘号

#include "camera_sd_storage.h"    // takeover 会话中的 HAL SD polling 适配接口
#include "ff.h"                   // FatFs 基础类型和配置
#include "diskio.h"               // FatFs 要求实现的 disk_* ABI 与 RES/DSTATUS 类型

#include <stddef.h>                // 提供 NULL

//============================================================================
// @file    camera_fatfs_diskio.c
// @brief   FatFs 标准 disk_* 回调到 camera_sd_storage 的薄适配层
//
// FatFs 在 f_mount/f_stat/f_open/f_write 等操作中调用这些全局回调。本文件只校验
// physical drive、转交 LBA/count，并把项目错误码映射成 RES_*；它不直接切 GPIO、
// 不屏蔽 0x3018、不初始化 SDIO，也不负责 cleanup。所有硬件生命周期由显式的
// camera_sd_storage takeover 会话管理，读写采用 HAL SD polling。
//============================================================================

// 把唯一物理盘 0 的活动 SD 会话状态映射为 FatFs 的 0 或 STA_NOINIT。
// 这是 FatFs 内部回调，可能查询 HAL 卡状态，不是 CLI 的纯缓存 SD STATUS。
DSTATUS disk_status(BYTE physical_drive)
{
    if (physical_drive != CAMERA_FATFS_PHYSICAL_DRIVE)
    {
        return STA_NOINIT;
    }

    return (Camera_SDStorage_FatFsDiskStatus() == CAMERA_SD_OK) ?
        0U : STA_NOINIT;
}

// 校验既有 takeover 会话并等待卡进入 TRANSFER；不会重新执行 HAL_SD_Init。
DSTATUS disk_initialize(BYTE physical_drive)
{
    if (physical_drive != CAMERA_FATFS_PHYSICAL_DRIVE)
    {
        return STA_NOINIT;
    }

    return (Camera_SDStorage_FatFsDiskInitialize() == CAMERA_SD_OK) ?
        0U : STA_NOINIT;
}

// 将 FatFs LBA_t 起始扇区和连续 count 转交给下层 polling 读。
// 下层负责活动会话、容量范围、卡状态和 timeout；这里仅映射 OK/NOT_READY/PARAM/ERROR。
DRESULT disk_read(
    BYTE physical_drive,
    BYTE *buffer,
    LBA_t sector,
    UINT count)
{
    uint32_t result;  // camera_sd_storage 返回的精确错误码

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

// 将 FatFs 连续扇区写转交给受 write_allowed guard 保护的 polling 适配层。
// 因此只读 mount 检查或非保存会话不能通过该回调意外修改介质。
DRESULT disk_write(
    BYTE physical_drive,
    const BYTE *buffer,
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

    result = Camera_SDStorage_FatFsDiskWrite(
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

// 转交 CTRL_SYNC、扇区数、扇区大小和擦除块大小查询，并映射为 FatFs DRESULT。
// CTRL_SYNC 会等待卡回到 TRANSFER，保证前一项 polling 写操作真正结束。
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
