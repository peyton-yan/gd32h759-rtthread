/*
 * SD 卡挂载管理（DFS 挂载表 + 事件驱动自动挂载）
 *
 * 背景：mmcsd 的卡片识别在 mmcsd_detect 线程里异步完成，而 DFS 的
 * mount_table 在文件系统初始化阶段同步执行——此刻 sd0 块设备尚未
 * 注册，启动即挂载必然失败（[E/DFS] mount fs[elm] on / failed）。
 * 这不是 bug，是时序错位。因此挂载表留空，挂载/卸载交给事件驱动
 * 的守护线程：框架在识别完成/移除时通过 mmcsd_wait_cd_changed()
 * 发事件，插入则挂载，移除则 umount（避免文件句柄悬空）。
 *
 * menuconfig 开关：
 *   BSP_SDIO_AUTO_MOUNT  自动挂载守护线程（本文件唯一功能主体）
 *
 * 放入 applications/ 即可编译（SConscript 默认收集该目录全部 .c）。
 */

#include <rtthread.h>
#include <dfs.h>
#include <dfs_fs.h>
/* RT_USING_DFS_MNTTABLE 在本工程被必选，本符号必须始终提供。
 * 故意留空：sd0 上电时还不存在；eMMC/SPI Flash 等上电即在的
 * 块设备可在此登记 */
const struct dfs_mount_tbl mount_table[] =
{
    {0}
};

#if defined(BSP_SDIO_AUTO_MOUNT)


#include <drivers/dev_mmcsd_core.h>     /* mmcsd_wait_cd_changed / MMCSD_HOST_PLUGED */

#define DBG_TAG             "app.sdmnt"
#define DBG_LVL             DBG_INFO
#include <rtdbg.h>

#define SD_DEVICE_NAME      "sd0"
#define SD_MOUNT_POINT      "/"
#define SD_FS_TYPE          "elm"

static void sdcard_mount(void)
{
    if (dfs_mount(SD_DEVICE_NAME, SD_MOUNT_POINT, SD_FS_TYPE, 0, 0) == 0)
        LOG_I("mount %s on %s ok", SD_DEVICE_NAME, SD_MOUNT_POINT);
    else
        LOG_E("mount %s on %s failed", SD_DEVICE_NAME, SD_MOUNT_POINT);
}

static void sdcard_mnt_thread(void *param)
{
    /* 线程启动与首次卡片识别存在竞争：若 sd0 已注册就直接挂，
     * 否则进入事件等待（识别完成的 PLUGED 事件随后必到）。
     * 直接挂载后必须把邮箱里那封"首次插入"事件排掉——它对应的
     * 就是刚才这次挂载，不排则进入循环后对同一挂载点重复挂载 */
    if (rt_device_find(SD_DEVICE_NAME) != RT_NULL)
    {
        sdcard_mount();
        mmcsd_wait_cd_changed(RT_WAITING_NO);
    }

    while (1)
    {
        rt_int32_t st = mmcsd_wait_cd_changed(RT_WAITING_FOREVER);

        if (st == MMCSD_HOST_PLUGED)
        {
            sdcard_mount();
        }
        else if (st == MMCSD_HOST_UNPLUGED)
        {
            if (dfs_unmount(SD_MOUNT_POINT) == 0)
                LOG_I("unmount %s ok", SD_MOUNT_POINT);
            else
                LOG_E("unmount %s failed", SD_MOUNT_POINT);
        }
    }
}

static int sdcard_mnt_init(void)
{
    rt_thread_t tid = rt_thread_create("sd_mnt", sdcard_mnt_thread, RT_NULL,
                                       2048, 21, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
    return 0;
}
INIT_APP_EXPORT(sdcard_mnt_init);

#endif /* BSP_SDIO_AUTO_MOUNT */