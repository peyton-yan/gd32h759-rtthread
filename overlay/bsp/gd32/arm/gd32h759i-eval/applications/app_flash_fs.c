/*
 * app_flash_fs.c - GD25X512ME 文件系统应用（littlefs 挂载到 /flash）
 *
 * 流程：fal_init（读分区表）-> fal_blk_device_create("filesystem")
 *       -> dfs_mount littlefs；首次使用（或损坏）自动 dfs_mkfs。
 *
 * littlefs 是掉电安全 + 磨损均衡的 NOR 专用文件系统——比 FatFS 适合
 * 裸 Flash（FatFS 会把同一扇区磨死）。
 *
 * msh 验证：
 *   mount 成功后 echo hello >/flash/test.txt; cat /flash/test.txt
 */

#include <rtthread.h>
#include <board.h>

#if defined(BSP_USING_OSPI) && defined(RT_USING_FAL) && defined(RT_USING_DFS)

#include <fal.h>
#include <string.h>
#include <dfs_fs.h>
#include <dfs_file.h>

#define DBG_TAG             "app.flashfs"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define FS_PART_NAME    "filesystem"
#define FS_MOUNT_PATH   "/flash"

int flash_fs_init(void)
{
    rt_kprintf("[flash_fs] step1: fal_init\n");
    fal_init();

    rt_kprintf("[flash_fs] step2: blk_device_create\n");
    struct rt_device *dev = fal_mtd_nor_device_create("filesystem");
    if (!dev) { rt_kprintf("[flash_fs] blk create FAIL\n"); return -1; }

   rt_kprintf("[flash_fs] step3: dfs_mount at /\n");
    if (dfs_mount("filesystem", "/", "lfs", 0, 0) == 0) {
        rt_kprintf("[flash_fs] mounted at /\n");
        return 0;
    }

    rt_kprintf("[flash_fs] step4: mkfs (8MB erase, may take 2 min)\n");
    if (dfs_mkfs("lfs", "filesystem") != 0) {
        rt_kprintf("[flash_fs] mkfs FAIL\n");
        return -1;
    }
    rt_kprintf("[flash_fs] step5: remount\n");
    int ret = dfs_mount("filesystem", "/", "lfs", 0, 0);
    rt_kprintf("[flash_fs] remount ret=%d\n", ret);
    return ret;
}
INIT_APP_EXPORT(flash_fs_init);

/* 参数区裸读写演示：fal_partition_* 直接操作 "param" 分区，
 * 工控场景的标定参数掉电保存就这么用 */
#ifdef RT_USING_FINSH
#include <finsh.h>

static void param_demo(int argc, char **argv)
{
    const struct fal_partition *part = fal_partition_find("param");
    rt_uint32_t magic = 0x50415241;     /* 'PARA' */
    rt_uint32_t val = 0;
    rt_uint8_t buf[4];

    if (part == RT_NULL) {
        rt_kprintf("param partition not found\n");
        return;
    }

    if (argc > 1) {
        /* 写：param_demo 1234 */
        val = (rt_uint32_t)atoi(argv[1]);
        fal_partition_erase(part, 0, 4);
        memcpy(buf, &val, 4);
        fal_partition_write(part, 0, buf, 4);
        fal_partition_write(part, 4, (const rt_uint8_t *)&magic, 4);
        rt_kprintf("param: wrote %lu (+magic)\n", val);
    } else {
        /* 读：param_demo */
        fal_partition_read(part, 0, buf, 4);
        memcpy(&val, buf, 4);
        fal_partition_read(part, 4, buf, 4);
        memcpy(&magic, buf, 4);
        rt_kprintf("param: value=%lu magic=0x%08lX %s\n",
                   val, magic, magic == 0x50415241 ? "(valid)" : "(invalid!)");
    }
}
MSH_CMD_EXPORT(param_demo, param_demo [value] - param partition rw demo);
#endif

#endif /* BSP_USING_OSPI && RT_USING_FAL && RT_USING_DFS */