/*
 * fal_cfg.h - FAL 分区表（放 bsp/gd32/arm/gd32h759i-eval/board/ 下）
 *
 * GD25X512ME 分区规划（3 字节地址模式，低 16MB）：
 *   param      1MB  参数/标定数据掉电保存（裸 fal 读写）
 *   app        7MB  预留：固件备份/OTA 暂存坑位
 *   filesystem 8MB  littlefs：日志、CSV、配置文件，挂载到 /flash
 */

#ifndef _FAL_CFG_H_
#define _FAL_CFG_H_

#include <rtconfig.h>
#include <board.h>

extern const struct fal_flash_dev gd25x512_flash;

#define FAL_FLASH_DEV_TABLE  { &gd25x512_flash }

#define FAL_PART_TABLE \
{ \
    { FAL_PART_MAGIC_WORD, "param",      "gd25x512", 0,                    1 * 1024 * 1024, 0 }, \
    { FAL_PART_MAGIC_WORD, "app",        "gd25x512", 1 * 1024 * 1024,      7 * 1024 * 1024, 0 }, \
    { FAL_PART_MAGIC_WORD, "filesystem", "gd25x512", 8 * 1024 * 1024,      8 * 1024 * 1024, 0 }, \
}

#endif /* _FAL_CFG_H_ */