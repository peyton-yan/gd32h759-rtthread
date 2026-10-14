#ifndef MB_POINTS_H
#define MB_POINTS_H

#include <rtthread.h>

/* 初始化外设并使能点表。返回点数量 */
rt_size_t mb_points_register(void);

#endif /* MB_POINTS_H */
