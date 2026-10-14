/*
 * mb_pointtable.h — 表驱动 Modbus 点表框架
 *
 * 设计：点表是"单一事实来源"。一个点由 mb_point_t 描述，
 * 通过 mb_pointtable_ops() 适配到 mb_core 的 mb_map_ops 回调。
 *
 * 点的取值/赋值有两种绑定方式（hook 优先于 reg）：
 *   1) reg  : 直接绑定变量地址
 *              BIT        -> rt_uint8_t *
 *              U16 / S16  -> rt_uint16_t * / rt_int16_t *
 *              U32        -> rt_uint32_t *（占连续 2 个寄存器，高字在前）
 *              STR        -> rt_uint8_t *（长度由调用方约定）
 *   2) hook : 复杂点（外设访问、需要副作用）用回调实现
 *              int hook(rt_uint8_t area, rt_uint16_t addr, void *val, rt_bool_t write)
 *              write=0 读出填 *val；write=1 用 *val 写入
 *              返回 0 成功 / -RT_ERROR 非法地址 / -RT_EINVAL 非法值
 *              area 用于区分不同区中相同地址的点（如 HREG/IREG 同址）
 *
 * 用 MB_PT(...) 宏声明点，便于 tools/gen_points_json.py 解析导出 JSON。
 */
#ifndef MB_POINTTABLE_H
#define MB_POINTTABLE_H

#include "mb_core.h"

enum mb_pt_area {
    MB_AREA_COIL = 1,   /* 0x  线圈       */
    MB_AREA_DIN,        /* 1x  离散输入   */
    MB_AREA_IREG,       /* 3x  输入寄存器 */
    MB_AREA_HREG,       /* 4x  保持寄存器 */
};

enum mb_pt_type {
    MB_T_BIT = 1,
    MB_T_U16,
    MB_T_S16,
    MB_T_U32,
    MB_T_STR,
};

enum mb_pt_attr {
    MB_A_RO = 0,        /* 只读 */
    MB_A_RW,            /* 读写 */
    MB_A_WO,            /* 只写 */
};

typedef int (*mb_pt_hook_t)(rt_uint8_t area, rt_uint16_t addr,
                            void *val, rt_bool_t write);

typedef struct {
    const char  *name;  /* 点名，上位机据此生成界面 */
    rt_uint16_t  addr;  /* 区内地址 */
    rt_uint8_t   area;  /* enum mb_pt_area */
    rt_uint8_t   type;  /* enum mb_pt_type */
    rt_uint8_t   attr;  /* enum mb_pt_attr */
    void        *reg;   /* 变量绑定，可为 RT_NULL */
    mb_pt_hook_t hook;  /* 回调，可为 RT_NULL */
} mb_point_t;

/* 安装点表（pts 需为静态常量数组，框架只保存指针） */
void mb_pointtable_init(const mb_point_t *pts, rt_size_t n);

/* 取得适配后的 mb_map_ops，用于 mb_tcp_start() */
const struct mb_map_ops *mb_pointtable_ops(void);

/* 点表声明宏（gen_points_json.py 依赖此固定格式，改动需同步脚本） */
#define MB_PT(area, addr, type, attr, reg, hook, name) \
    { name, addr, area, type, attr, (void *)(reg), hook }

#endif /* MB_POINTTABLE_H */
