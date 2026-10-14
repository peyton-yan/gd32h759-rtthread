/*
 * mb_pointtable.c — 表驱动点表框架实现
 *
 * addr 查找规则：先精确命中点基址；U32 点额外命中基址+1（低字）。
 * 读出：基址 -> 高字，基址+1 -> 低字。
 * 写入：U32 采用读-改-写，高低字分别更新（FC 0x10 一次写两字最安全）。
 */
#include "mb_pointtable.h"

static const mb_point_t *pt_table;
static rt_size_t         pt_count;

static const mb_point_t *pt_find(rt_uint8_t area, rt_uint16_t addr,
                                 rt_uint8_t *word_idx)
{
    rt_size_t i;

    for (i = 0; i < pt_count; i++) {
        const mb_point_t *p = &pt_table[i];
        if (p->area != area) {
            continue;
        }
        if (p->addr == addr) {
            if (word_idx) {
                *word_idx = 0;
            }
            return p;
        }
        if (p->type == MB_T_U32 && (rt_uint16_t)(p->addr + 1) == addr) {
            if (word_idx) {
                *word_idx = 1;
            }
            return p;
        }
    }
    return RT_NULL;
}

static int pt_read_bit(rt_uint8_t area, rt_uint16_t addr, rt_uint8_t *val)
{
    const mb_point_t *p = pt_find(area, addr, RT_NULL);

    if (p == RT_NULL || p->attr == MB_A_WO) {
        return -RT_ERROR;
    }
    if (p->hook) {
        rt_uint8_t v = 0;
        int r = p->hook(p->area, addr, &v, RT_FALSE);
        if (r < 0) {
            return r;
        }
        *val = v ? 1 : 0;
        return 0;
    }
    if (p->reg == RT_NULL) {
        return -RT_ERROR;
    }
    *val = (*(rt_uint8_t *)p->reg) ? 1 : 0;
    return 0;
}

static int pt_write_bit(rt_uint8_t area, rt_uint16_t addr, rt_uint8_t val)
{
    const mb_point_t *p = pt_find(area, addr, RT_NULL);

    if (p == RT_NULL || p->attr == MB_A_RO) {
        return -RT_ERROR;
    }
    if (p->hook) {
        rt_uint8_t v = val ? 1 : 0;
        return p->hook(p->area, addr, &v, RT_TRUE);
    }
    if (p->reg == RT_NULL) {
        return -RT_ERROR;
    }
    *(rt_uint8_t *)p->reg = val ? 1 : 0;
    return 0;
}

static int pt_read_reg(rt_uint8_t area, rt_uint16_t addr, rt_uint16_t *val)
{
    rt_uint8_t word = 0;
    const mb_point_t *p = pt_find(area, addr, &word);

    if (p == RT_NULL || p->attr == MB_A_WO) {
        return -RT_ERROR;
    }
    if (p->hook) {
        return p->hook(p->area, addr, val, RT_FALSE);
    }
    if (p->reg == RT_NULL) {
        return -RT_ERROR;
    }
    switch (p->type) {
    case MB_T_BIT:
        *val = (*(rt_uint8_t *)p->reg) ? 1 : 0;
        break;
    case MB_T_U16:
        *val = *(rt_uint16_t *)p->reg;
        break;
    case MB_T_S16:
        *val = (rt_uint16_t)(*(rt_int16_t *)p->reg);
        break;
    case MB_T_U32: {
        rt_uint32_t u = *(rt_uint32_t *)p->reg;
        *val = word ? (rt_uint16_t)(u & 0xFFFFu) : (rt_uint16_t)(u >> 16);
        break;
    }
    default:
        return -RT_ERROR;
    }
    return 0;
}

static int pt_write_reg(rt_uint8_t area, rt_uint16_t addr, rt_uint16_t val)
{
    rt_uint8_t word = 0;
    const mb_point_t *p = pt_find(area, addr, &word);

    if (p == RT_NULL || p->attr == MB_A_RO) {
        return -RT_ERROR;
    }
    if (p->hook) {
        return p->hook(p->area, addr, &val, RT_TRUE);
    }
    if (p->reg == RT_NULL) {
        return -RT_ERROR;
    }
    switch (p->type) {
    case MB_T_BIT:
        *(rt_uint8_t *)p->reg = val ? 1 : 0;
        break;
    case MB_T_U16:
        *(rt_uint16_t *)p->reg = val;
        break;
    case MB_T_S16:
        *(rt_int16_t *)p->reg = (rt_int16_t)val;
        break;
    case MB_T_U32: {
        rt_uint32_t u = *(rt_uint32_t *)p->reg;
        u = word ? ((u & 0xFFFF0000u) | val)
                 : ((u & 0x0000FFFFu) | ((rt_uint32_t)val << 16));
        *(rt_uint32_t *)p->reg = u;
        break;
    }
    default:
        return -RT_ERROR;
    }
    return 0;
}

static int op_read_coil(rt_uint16_t addr, rt_uint8_t *val)
{
    return pt_read_bit(MB_AREA_COIL, addr, val);
}

static int op_write_coil(rt_uint16_t addr, rt_uint8_t val)
{
    return pt_write_bit(MB_AREA_COIL, addr, val);
}

static int op_read_din(rt_uint16_t addr, rt_uint8_t *val)
{
    return pt_read_bit(MB_AREA_DIN, addr, val);
}

static int op_read_ireg(rt_uint16_t addr, rt_uint16_t *val)
{
    return pt_read_reg(MB_AREA_IREG, addr, val);
}

static int op_read_hreg(rt_uint16_t addr, rt_uint16_t *val)
{
    return pt_read_reg(MB_AREA_HREG, addr, val);
}

static int op_write_hreg(rt_uint16_t addr, rt_uint16_t val)
{
    return pt_write_reg(MB_AREA_HREG, addr, val);
}

static const struct mb_map_ops pt_ops = {
    .read_coil  = op_read_coil,
    .write_coil = op_write_coil,
    .read_din   = op_read_din,
    .read_ireg  = op_read_ireg,
    .read_hreg  = op_read_hreg,
    .write_hreg = op_write_hreg,
};

void mb_pointtable_init(const mb_point_t *pts, rt_size_t n)
{
    pt_table = pts;
    pt_count = n;
}

const struct mb_map_ops *mb_pointtable_ops(void)
{
    return &pt_ops;
}
