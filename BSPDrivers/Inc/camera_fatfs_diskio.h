#ifndef ISP_OV5640_CAMERA_FATFS_DISKIO_H
#define ISP_OV5640_CAMERA_FATFS_DISKIO_H

/**
 * @file camera_fatfs_diskio.h
 * @brief 本工程 SD 块设备到 FatFs physical drive 的映射
 *
 * 标准 disk_status/disk_read 等原型由第三方 diskio.h 定义，本头文件不重复声明。
 */

/** @brief 本工程唯一 SD 块设备映射为 FatFs drive 0。 */
#define CAMERA_FATFS_PHYSICAL_DRIVE 0U

#endif /* ISP_OV5640_CAMERA_FATFS_DISKIO_H */
