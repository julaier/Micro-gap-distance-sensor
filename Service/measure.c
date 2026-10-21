/*
 * measure.c - 测量算法层实现
 */

#include <string.h>
#include "measure.h"
#include "flash_param.h"

/* 电脑不能发命令时, 调试参数直接在本文件修改。 */
#define MEAS_USE_FLASH_PARAMS   0u      /* 0 = 不读旧 Flash 参数, 只用下面代码值 */
#define MEAS_DEFAULT_ALARM_MM   0.800f  /* 报警阈值 */
#define MEAS_DEFAULT_WARN_MM    0.500f  /* 提醒阈值 */
#define MEAS_DEFAULT_HYST_MM    0.050f  /* 迟滞 */
#define MEAS_DEFAULT_TEMP_K     0.0f    /* 温补系数 */
#define MEAS_DEFAULT_TEMP_REF_C 25.0f   /* 温补参考温度 */
#define MEAS_DEFAULT_SWAP       0u      /* 0 = CH0/CH1, 1 = CH1/CH0 */

/*
 * 代码标定表:
 *   先把串口 $R 帧里的 RATIO 抄下来, 按间隙从小到大填到下面,
 *   再把 MEAS_CODE_CAL_POINTS 改成实际点数 (必须 >= 2)。
 *   点数不足 2 时, 测得距离会保持 0, 状态显示 UNCAL。
 */
#define MEAS_CODE_CAL_POINTS    4       /* 当前使用的代码标定点个数, 0~9 */
#if (MEAS_CODE_CAL_POINTS < 0) || (MEAS_CODE_CAL_POINTS > MEAS_MAX_POINTS)
#error "MEAS_CODE_CAL_POINTS must be between 0 and MEAS_MAX_POINTS"
#endif

#define MEAS_MAGIC         0x4C444331u   /* "LDC1",用于标识参数块 */
#define MEAS_MEDIAN_N      5             /* 中值滤波窗口大小 */
#define MEAS_AVG_N         8             /* 均值滤波窗口大小 */
#define MEAS_DEBOUNCE_MS   1500u         /* 连续超阈值超过这么久才动作 */

/* 存进 Flash 的参数块 (定长, 不含指针) */
typedef struct {
    uint32_t magic;                 /* 参数块标识 */
    float    alarm_mm;              /* 报警阈值, mm */
    float    warn_mm;               /* 提醒阈值, mm */
    float    hyst_mm;               /* 迟滞, mm */
    float    temp_k;                /* 温度补偿系数 */
    float    temp_ref_c;            /* 参考温度, 摄氏度 */
    uint8_t  n_points;              /* 标定点个数 */
    uint8_t  swap;                  /* 通道交换标志 */
    uint8_t  reserved[2];           /* 保留 */
    float    pt_gap[MEAS_MAX_POINTS]; /* 标定点间隙 */
    float    pt_ratio[MEAS_MAX_POINTS]; /* 标定点比值 */
} measure_params_t;
/*9个标定点的参数更改在这里改*/
static const float s_code_cal_gap[MEAS_MAX_POINTS] = {
    0.0f,0.50f, 0.80f, 1.00f, 2.00f,
    0.0f, 0.0f, 0.0f, 0.0f
};
static const float s_code_cal_ratio[MEAS_MAX_POINTS] = {
    0.0f,1.01027f, 1.00940f, 1.00517f, 0.99091f,
    0.0f, 0.0f, 0.0f, 0.0f
};

static void params_sort(measure_params_t *p);

static struct {
    measure_params_t p;

    float med_buf[MEAS_MEDIAN_N];/* 中值滤波缓冲区 */
    int   med_idx;/* 中值滤波索引 */
    int   med_n;/* 中值滤波有效数据个数 */

    float avg_buf[MEAS_AVG_N];/* 均值滤波缓冲区 */
    int   avg_idx;/* 均值滤波索引 */
    int   avg_n; /* 均值滤波有效数据个数 */

    float   filt_ratio;/* 滤波后的比值 */
    uint8_t have_ratio;/* 是否有有效比值 */

    uint8_t  level;        /* 已确认的档位 */
    uint8_t  pend_level;   /* 待确认的档位 */
    uint32_t pend_ms;      /* 待确认档位从什么时候开始 */

    measure_result_t last;/* 最近一次测量结果 */
} s;

/* ------------------------------------------------------------------ */
/* 参数默认值与 Flash 存取                                              */
/* ------------------------------------------------------------------ */
/*
*设置参数默认值
 */
static void params_default(measure_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->magic      = MEAS_MAGIC;/* 标识参数块 */
    p->alarm_mm   = MEAS_DEFAULT_ALARM_MM;/* 报警阈值, mm */
    p->warn_mm    = MEAS_DEFAULT_WARN_MM;/* 提醒阈值, mm */
    p->hyst_mm    = MEAS_DEFAULT_HYST_MM;/* 迟滞, mm */
    p->temp_k     = MEAS_DEFAULT_TEMP_K;/* 默认不做温度修正 */
    p->temp_ref_c = MEAS_DEFAULT_TEMP_REF_C;/* 参考温度, 摄氏度 */
    p->n_points   = 0u;/* 标定点个数 */
    p->swap       = MEAS_DEFAULT_SWAP;/* 通道交换标志 ,0的时候为ch0/ch1*/
}
//s为静态结构体，存储测量参数和滤波状态
void measure_init(void)
{
    memset(&s, 0, sizeof(s));
    params_default(&s.p);
    s.last.status = MEAS_ST_UNCAL;//未标定, 距离无效,初始化时先设置为未标定
    s.last.valid  = 0u;//距离无效,初始化时先设置为无效
}
//把电脑不能发命令时手工填写的代码标定点装入参数表
static void code_cal_apply(void)
{
    int n = (int)MEAS_CODE_CAL_POINTS;
    int i;

    if (n < 2 || n > MEAS_MAX_POINTS) {
        return;
    }

    for (i = 0; i < n; i++) {
        s.p.pt_gap[i]   = s_code_cal_gap[i];
        s.p.pt_ratio[i] = s_code_cal_ratio[i];
    }
    s.p.n_points = (uint8_t)n;
    params_sort(&s.p);
}

//读取参数: 可选从 Flash 读取, 代码标定点优先级最高
void measure_load(void)
{
#if MEAS_USE_FLASH_PARAMS
    const measure_params_t *fp = (const measure_params_t *)flash_param_ptr();

    if (fp->magic == MEAS_MAGIC && fp->n_points <= MEAS_MAX_POINTS) {
        memcpy(&s.p, fp, sizeof(s.p));
    }
#endif

    /* 代码标定点优先级最高, 电脑不能发命令时直接烧录调试。 */
    code_cal_apply();
}
//将当前参数块写入Flash，如果写入失败则不做处理
void measure_save(void)
{
    s.p.magic = MEAS_MAGIC;
    (void)flash_param_write(&s.p, (uint32_t)sizeof(s.p));
}

/* ------------------------------------------------------------------ */
/* 对外参数接口                                                         */
/* ------------------------------------------------------------------ */
//更新报警阈值，提醒阈值和迟滞值，如果传入的值大于0，则更新对应的参数，否则保持原值不变
void measure_set_thresholds(float alarm_mm, float warn_mm, float hyst_mm)
{
    if (alarm_mm > 0.0f) {
        s.p.alarm_mm = alarm_mm;
    }
    if (warn_mm > 0.0f) {
        s.p.warn_mm = warn_mm;
    }
    if (hyst_mm > 0.0f) {
        s.p.hyst_mm = hyst_mm;
    }
}
//获取当前的报警阈值，提醒阈值和迟滞值，如果传入的指针不为NULL，则将对应的参数值写入指针所指向的变量中
void measure_get_thresholds(float *alarm_mm, float *warn_mm, float *hyst_mm)
{
    if (alarm_mm != 0) {
        *alarm_mm = s.p.alarm_mm;
    }
    if (warn_mm != 0) {
        *warn_mm = s.p.warn_mm;
    }
    if (hyst_mm != 0) {
        *hyst_mm = s.p.hyst_mm;
    }
}
//设置温补系数和参考温度，ratio_per_c为比值/摄氏度，ref_c为参考温度，如果传入的值大于0，则更新对应的参数，否则保持原值不变
void measure_set_temp_comp(float ratio_per_c, float ref_c)
{
    s.p.temp_k     = ratio_per_c;
    s.p.temp_ref_c = ref_c;
}
//获取当前的温补系数和参考温度，如果传入的指针不为NULL，则将对应的参数值写入指针所指向的变量中
void measure_get_temp_comp(float *ratio_per_c, float *ref_c)
{
    if (ratio_per_c != 0) {
        *ratio_per_c = s.p.temp_k;
    }
    if (ref_c != 0) {
        *ref_c = s.p.temp_ref_c;
    }
}
//设置通道交换状态，如果swap不为0，则将s.p.swap设置为1，否则设置为0
void measure_set_swap(uint8_t swap)
{
    s.p.swap = (swap != 0u) ? 1u : 0u;
}
//获取当前的通道交换状态，返回s.p.swap的值，如果为1，则表示通道已交换，否则为0
uint8_t measure_get_swap(void)
{
    return s.p.swap;
}

/* 标定点按间隙从小到大排序, 插值时好查,插入排序,后面可以考虑二分查找 */
static void params_sort(measure_params_t *p)
{
    int i;

    for (i = 1; i < (int)p->n_points; i++) {
        float g = p->pt_gap[i];
        float r = p->pt_ratio[i];
        int j = i - 1;

        while (j >= 0 && p->pt_gap[j] > g) {
            p->pt_gap[j + 1]   = p->pt_gap[j];
            p->pt_ratio[j + 1] = p->pt_ratio[j];
            j--;
        }
        p->pt_gap[j + 1]   = g;
        p->pt_ratio[j + 1] = r;
    }
}
/*
*设置标定点，如果index小于1或大于MEAS_MAX_POINTS，则返回-1；
*如果index大于当前标定点个数加1，则返回-2；
*如果没有有效比值，则返回-3；
*否则将gap_mm和当前滤波后的比值存入对应的标定点，并更新标定点个数，然后对标定点按间隙从小到大排序，
*最后返回0
*/
int measure_cal_set(int index, float gap_mm)
{
    if (index < 1 || index > MEAS_MAX_POINTS) {
        return -1;
    }
    if (index > (int)s.p.n_points + 1) {
        return -2;          /* 必须顺序录入 */
    }
    if (!s.have_ratio) {
        return -3;          /* 还没有有效比值 */
    }

    s.p.pt_gap[index - 1]   = gap_mm;
    s.p.pt_ratio[index - 1] = s.filt_ratio;
    s.p.n_points = (uint8_t)index;

    params_sort(&s.p);
    return 0;
}
//获取当前标定点个数，返回s.p.n_points的值
int measure_cal_count(void)
{
    return (int)s.p.n_points;
}
/*获取标定点信息*/
int measure_cal_get(int index, float *gap_mm, float *ratio)
{
    if (index < 0 || index >= (int)s.p.n_points) {
        return -1;
    }
    if (gap_mm != 0) {
        *gap_mm = s.p.pt_gap[index];
    }
    if (ratio != 0) {
        *ratio = s.p.pt_ratio[index];
    }
    return 0;
}
//清除所有标定点，将s.p.n_points设置为0
void measure_cal_clear(void)
{
    s.p.n_points = 0u;
}

/* ------------------------------------------------------------------ */
/* 滤波                                                                 */
/* ------------------------------------------------------------------ */
//中值滤波: 先排序, 再取中间值,用的还是插入排序
static float median_of(float *t, int n)
{
    int i;

    for (i = 1; i < n; i++) {
        float v = t[i];
        int j = i - 1;
        while (j >= 0 && t[j] > v) {
            t[j + 1] = t[j];
            j--;
        }
        t[j + 1] = v;
    }
    return t[n / 2];
}
//均值滤波: 求平均值
static float average_of(const float *in, int n)
{
    float sum = 0.0f;
    int i;

    for (i = 0; i < n; i++) {
        sum += in[i];
    }
    return sum / (float)n;
}
//滤波处理: 先中值滤波, 再均值滤波, 返回滤波后的比值
static float filter_push(float ratio)
{
    float tmp[MEAS_MEDIAN_N];
    float med;
    int i;

    s.med_buf[s.med_idx] = ratio;
    s.med_idx = (s.med_idx + 1) % MEAS_MEDIAN_N;
    //如果中值滤波缓冲区未满，则增加有效数据个数
    if (s.med_n < MEAS_MEDIAN_N) {
        s.med_n++;
    }
    //将中值滤波缓冲区的有效数据复制到临时数组tmp中
    for (i = 0; i < s.med_n; i++) {
        tmp[i] = s.med_buf[i];
    }
    med = median_of(tmp, s.med_n);
    //将中值滤波结果存入均值滤波缓冲区
    s.avg_buf[s.avg_idx] = med;
    s.avg_idx = (s.avg_idx + 1) % MEAS_AVG_N;
    if (s.avg_n < MEAS_AVG_N) {
        s.avg_n++;
    }
    return average_of(s.avg_buf, s.avg_n);
}

/* ------------------------------------------------------------------ */
/* 标定插值                                                             */
/* ------------------------------------------------------------------ */

/*
 * 用相邻两个标定点做线性换算：
 * 返回 1 = 斜率有效，0 = 标定点方向不合法。
 * 得出的间隙值
 */
static int segment_gap(float ratio,
                       float r0, float r1,
                       float g0, float g1,
                       float *gap_mm)
{
    float dr = r0 - r1;

    if (dr <= 0.0f) {
        return 0;
    }

    *gap_mm = g0 + ((r0 - ratio) / dr) * (g1 - g0);
    return 1;
}

/*
 * 返回 1 = 实际比值落在标定区间内；
 * 返回 0 = 超出标定区间，此时按最近两个点线性外推。
 * 间隙变大 -> 比值变小；若方向相反，用 SWAP 命令调换 CH0/CH1。
 */
static int interpolate(const measure_params_t *p, float ratio, float *gap_mm)
{
    int n = (int)p->n_points;
    int i;

    if (n < 2) {
        return 0;
    }

    /* 小于最小标定间隙：用最前面两点外推。 */
    if (ratio >= p->pt_ratio[0]) {
        if (segment_gap(ratio, p->pt_ratio[0], p->pt_ratio[1],
                        p->pt_gap[0], p->pt_gap[1], gap_mm) == 0) {
            *gap_mm = p->pt_gap[0];
        }
        return 0;
    }

    /* 大于最大标定间隙：用最后面两点外推。 */
    if (ratio <= p->pt_ratio[n - 1]) {
        if (segment_gap(ratio, p->pt_ratio[n - 2], p->pt_ratio[n - 1],
                        p->pt_gap[n - 2], p->pt_gap[n - 1], gap_mm) == 0) {
            *gap_mm = p->pt_gap[n - 1];
        }
        return 0;
    }

    /* 标定区间内：分段线性插值。 */
    for (i = 0; i < n - 1; i++) {
        float r0 = p->pt_ratio[i];
        float r1 = p->pt_ratio[i + 1];

        if (ratio <= r0 && ratio >= r1) {
            if (segment_gap(ratio, r0, r1,
                            p->pt_gap[i], p->pt_gap[i + 1], gap_mm) != 0) {
                return 1;
            }
        }
    }

    /* 标定点顺序或方向异常时，退回最后一点，避免输出未定义值。 */
    *gap_mm = p->pt_gap[n - 1];
    return 0;
}

/* ------------------------------------------------------------------ */
/* 迟滞 + 去抖                                                          */
/* ------------------------------------------------------------------ */

static uint8_t level_with_hysteresis(const measure_params_t *p, uint8_t cur, float d)
{
    float h = p->hyst_mm;

    switch (cur) {
    case MEAS_ST_NORMAL:
        if (d >= p->alarm_mm) {
            return MEAS_ST_ALARM;
        }
        if (d >= p->warn_mm) {
            return MEAS_ST_WARN;
        }
        return MEAS_ST_NORMAL;

    case MEAS_ST_WARN:
        if (d >= p->alarm_mm) {
            return MEAS_ST_ALARM;
        }
        if (d < (p->warn_mm - h)) {
            return MEAS_ST_NORMAL;
        }
        return MEAS_ST_WARN;

    default:  /* ALARM */
        if (d < (p->warn_mm - h)) {
            return MEAS_ST_NORMAL;
        }
        if (d < (p->alarm_mm - h)) {
            return MEAS_ST_WARN;
        }
        return MEAS_ST_ALARM;
    }
}

/* 新档位要连续保持 MEAS_DEBOUNCE_MS 才生效, 避免振动/干扰误报 */
static uint8_t debounce(uint8_t want, uint32_t now_ms)
{
    if (want == s.level) {
        s.pend_level = want;
        s.pend_ms = now_ms;
        return s.level;
    }
    if (want != s.pend_level) {
        s.pend_level = want;
        s.pend_ms = now_ms;
        return s.level;
    }
    if ((uint32_t)(now_ms - s.pend_ms) >= MEAS_DEBOUNCE_MS) {
        s.level = want;
    }
    return s.level;
}

/* ------------------------------------------------------------------ */
/* 主流程                                                               */
/* ------------------------------------------------------------------ */
//进入故障状态，距离无效，设置s.last.status为MEAS_ST_FAULT，s.last.valid为0,就是设置为故障状态
void measure_fault(void)
{
    s.last.status = MEAS_ST_FAULT;
    s.last.valid  = 0u;
}
//更新测量结果，ch0和ch1为原始值，temp_c为温度，now_ms为当前时间戳
void measure_update(uint32_t ch0, uint32_t ch1, float temp_c, uint32_t now_ms)
{
    float ratio_raw;
    float ratio;
    float gap = 0.0f;
    int   inside;
    uint8_t want;
//如果ch0或ch1为0，则进入故障状态，返回
    if (ch0 == 0u || ch1 == 0u) {
        measure_fault();
        return;
    }
//计算比值，目前是用ch0/ch1，如果s.p.swap不为0，则用ch1/ch0
    ratio_raw = (s.p.swap != 0u) ? ((float)ch1 / (float)ch0)
                                 : ((float)ch0 / (float)ch1);
//将比值传入滤波函数，得到滤波后的比值，并设置s.have_ratio为1，表示有有效比值
    s.have_ratio = 1u;
    s.filt_ratio = filter_push(ratio_raw);

    /* 温度二阶修正 */
    ratio = s.filt_ratio - s.p.temp_k * (temp_c - s.p.temp_ref_c);

    s.last.ch0    = ch0;
    s.last.ch1    = ch1;
    s.last.ratio  = ratio;
    s.last.temp_c = temp_c;
/*标定点少于 2 个，就无法建立“比值和距离之间的关系”
*距离 = 0
*有效标志 = 0
*状态 = 4，未标定
*同时把状态机重置回正常，等待后面重新标定。
*/
    if (s.p.n_points < 2u) {
        s.last.dist_mm = 0.0f;
        s.last.valid   = 0u;/*距离无效*/
        s.last.status  = MEAS_ST_UNCAL;
        s.level        = MEAS_ST_NORMAL;
        s.pend_level   = MEAS_ST_NORMAL;
        s.pend_ms      = now_ms;
        return;
    }

    inside = interpolate(&s.p, ratio, &gap);
    s.last.dist_mm = gap;
    s.last.valid   = (uint8_t)(inside ? 1u : 0u);
/*状态判断*/
    want = level_with_hysteresis(&s.p, s.level, gap);
    s.last.status = debounce(want, now_ms);
}
//返回最近一次测量结果的指针
const measure_result_t *measure_latest(void)
{
    return &s.last;
}
