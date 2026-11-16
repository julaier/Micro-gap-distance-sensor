/*
 * app.c - 应用层实现
 *
 * 这里只负责两件事:
 *   1. app_init(): 按顺序初始化各模块
 *   2. app_poll(): 主循环, 处理 INTB 采样、按键、OLED 刷新、串口上报
 *
 * 显示布局在 display.c, 测量算法在 measure.c, 寄存器操作在 ldc1614.c。
 */

#include "app.h"
#include "board.h"
#include "uart.h"
#include "adc_temp.h"
#include "ldc1614.h"
#include "measure.h"
#include "protocol.h"
#include "display.h"
#include "button.h"
#include "console.h"
#include "hard_i2c.h"
#include "../test/Driver/test_board.h"

/* ---- 可调参数 ---- */
#define APP_BAUD          115200u   /* 上位机串口波特率, 接 CH340 */
#define APP_REPORT_MS     100u      /* 周期上报, 100ms */
#define APP_TEMP_MS       1000u     /* 温度每秒读一次 */
#define APP_POLL_MS       20u       /* INTB 没来就每 20ms 主动读一次 */
#define APP_DATA_TIMEOUT  500u      /* 超过这么久没读到数据就判故障 */
#define APP_DIAG_MS       1000u     /* 未就绪时只读诊断；不自动复位/改配置 */
#define APP_OLED_MS       250u      /* OLED 刷新周期, 别太快, 免得占满 I2C */
#define APP_I2C_DEBUG     1         /* 1 = LDC 初始化失败时打总线诊断, 调好后改 0 */
#define APP_I2C_AUTOCAL   0         /* V9 直接使用 test 固定半拍等待 */
#define APP_I2C_MATRIX    0         /* 旧矩阵代码保留，但 V9 禁止启用 */
#ifndef APP_I2C_SCAN
#define APP_I2C_SCAN      0         /* 0x2A 已确认，正常固件不再扫地址 */
#endif

#if APP_I2C_AUTOCAL || APP_I2C_MATRIX
#error V9 uses the original fixed test I2C source
#endif

#if APP_I2C_MATRIX && !HARD_I2C_DIAGNOSTIC_MATRIX
#error Enable HARD_I2C_DIAGNOSTIC_MATRIX before using the wiring matrix
#endif

static char s_out[192];
static uint32_t s_last_data_ms;/*上次成功读到数据的时间戳*/
static uint32_t s_last_poll_ms;/*上次主动轮询的时间戳*/
static uint32_t s_last_temp_ms;
static uint32_t s_last_oled_ms;
static float    s_temp_c;
static uint8_t  s_ldc_ready; /* No sampling until all configuration readbacks pass. */
static uint8_t  s_ldc_external;
static uint32_t s_last_diag_ms;

/*用于发送数据*/
static void send(const char *buf, int len)
{
    if (len > 0) {
        uart_write((const uint8_t *)buf, (uint16_t)len);
    }
}

/* 把字符串追加到 buf, 返回写了几个字节 */
static int append_str(char *p, const char *s)
{
    int n = 0;

    while (s[n] != '\0') {
        p[n] = s[n];
        n++;
    }
    return n;
}
//把无符号整数按十进制追加到 buf, 返回新的写指针；共用函数以节省 Flash
static __attribute__((noinline)) char *append_u32(char *p, uint32_t v)
{
    char tmp[10];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0u);
    while (n > 0) {
        *p++ = tmp[--n];
    }
    return p;
}
//带负号的版本
static char *append_i32(char *p, int32_t v)
{
    if (v < 0) {
        *p++ = '-';
        return append_u32(p, (uint32_t)(-v));
    }
    return append_u32(p, (uint32_t)v);
}

/* 温度明显异常时打一行原始值: $B,TEMP,cal=..,vref=..,ts=..,t10=.. */
static void report_temp_debug(int32_t t10)
{
    uint16_t cal = 0u;
    uint16_t vref = 0u;
    uint16_t ts = 0u;
    char *p = s_out;

    adc_temp_debug(&cal, &vref, &ts);

    p += append_str(p, "$B,TEMP,cal=");
    p = append_u32(p, cal);
    p += append_str(p, ",vref=");
    p = append_u32(p, vref);
    p += append_str(p, ",ts=");
    p = append_u32(p, ts);
    p += append_str(p, ",t10=");
    p = append_i32(p, t10);
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
}

/* 初始化失败时发一个 "$E,<code>" 报文, 方便不看代码就知道卡在哪 */
static void report_boot_error(int code)
{
    int p = 0;
    char tmp[12];
    int n = 0;
    int v = -code;

    s_out[p++] = '$';
    s_out[p++] = 'E';
    s_out[p++] = ',';
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0);
    while (n > 0) {
        s_out[p++] = tmp[--n];
    }
    s_out[p++] = '\r';
    s_out[p++] = '\n';
    send(s_out, p);
}

/* I2C 初始化失败时的总线诊断:
 *   $I,<pd>,<pu>     bit0 = SCL(PB7), bit1 = SDA(PB6)
 *                    pd: 内部下拉时读到的电平, 正常是 3 (外部上拉把线拉高)
 *                    pu: 内部上拉时读到的电平, 读到 0 说明线被短路到地
 *   $P,<addr>,<ack>  逐个地址探测, 1 = 成功, 0 = 失败（NACK 或总线异常）
 */
#if APP_I2C_DEBUG
static char hex_char(uint8_t v)
{
    return (char)((v < 10u) ? ('0' + v) : ('A' + v - 10));
}

/* 多处诊断复用一份格式化代码，避免展开后挤占最后一页 Flash 参数区。 */
static __attribute__((noinline)) char *append_hex(char *p, uint32_t value, unsigned digits)
{
    while (digits != 0u) {
        digits--;
        *p++ = hex_char((uint8_t)((value >> (digits * 4u)) & 0xFu));
    }
    return p;
}

static const char *i2c_stage_name(hard_i2c_stage_t stage)
{
    switch (stage) {
    case HARD_I2C_START:   return "START";
    case HARD_I2C_ADDR_W:  return "ADDR_W";
    case HARD_I2C_WRITE:   return "WRITE";
    case HARD_I2C_RESTART: return "RESTART";
    case HARD_I2C_ADDR_R:  return "ADDR_R";
    case HARD_I2C_READ:    return "READ";
    case HARD_I2C_STOP:    return "STOP";
    case HARD_I2C_DONE:    return "DONE";
    default:              return "IDLE";
    }
}

/* $X,<tag>,<首次错误码>,<阶段>,<字节下标>,<错误时电平> */
static void report_i2c_detail(const char *tag, const hard_i2c_diag_t *saved)
{
    hard_i2c_diag_t diag;
    char *p = s_out;

    if (saved != 0) diag = *saved;
    else hard_i2c_get_diag(&diag);
    p += append_str(p, "$X,");
    p += append_str(p, tag);
    *p++ = ',';
    p = append_i32(p, diag.rc);
    *p++ = ',';
    p += append_str(p, i2c_stage_name(diag.stage));
    *p++ = ',';
    p = append_u32(p, diag.byte_index);
    *p++ = ',';
    p = append_u32(p, diag.lines);
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
}

/* V9: $T,<tag>,<发送时钟数>,<本机估算Hz>,<半拍最小等待us>,<定时器Hz>。
 * 与独立 test 一样，仅统计最近发送字节，不宣称实测 100kHz。 */
static void report_i2c_timing(const char *tag, const hard_i2c_diag_t *saved)
{
    hard_i2c_diag_t diag;
    char *p = s_out;

    if (saved != 0) diag = *saved;
    else hard_i2c_get_diag(&diag);
    p += append_str(p, "$T,");
    p += append_str(p, tag);
    *p++ = ',';
    p = append_u32(p, diag.clocks);
    *p++ = ',';
    p = append_u32(p, diag.effective_hz);
    *p++ = ',';
    p = append_u32(p, diag.half_ticks);
    p += append_str(p, ",1000000");
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
}

/* V9: $W,<tag>,<tx_hex>,<ack_level>,<SDA_MODE>,<SDA_OD>,<stop_rc>。
 * ACK 为实际单次采样；原 test 不采集 MODE/OD，二者打印 255。 */
static void report_i2c_wire(const char *tag, const hard_i2c_diag_t *saved)
{
    hard_i2c_diag_t diag;
    char *p = s_out;

    if (saved != 0) diag = *saved;
    else hard_i2c_get_diag(&diag);
    p += append_str(p, "$W,");
    p += append_str(p, tag);
    *p++ = ',';
    p = append_hex(p, diag.tx_byte, 2u);
    *p++ = ',';
    p = append_u32(p, diag.ack_level);
    *p++ = ',';
    p = append_u32(p, diag.ack_sda_mode);
    *p++ = ',';
    p = append_u32(p, diag.ack_sda_od);
    *p++ = ',';
    p = append_i32(p, diag.stop_rc);
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
}

/* 在软复位和外部参考时钟配置之前只读 ID，避免混淆总线与转换故障。 */
static int report_i2c_id(const char *tag)
{
    uint16_t mid = 0u, did = 0u;
    int mid_rc, did_rc;
    (void)ldc1614_probe(LDC1614_I2C_ADDR); /* Same address-only probe as test main. */
    mid_rc = ldc1614_read_reg(LDC1614_REG_MANUFACTURER_ID, &mid);
    did_rc = ldc1614_read_reg(LDC1614_REG_DEVICE_ID, &did);
    char *p = s_out;

    p += append_str(p, "$D,");
    p += append_str(p, tag);
    *p++ = ',';
    p = append_i32(p, mid_rc);
    *p++ = ',';
    p = append_hex(p, mid, 4u);
    *p++ = ',';
    p = append_i32(p, did_rc);
    *p++ = ',';
    p = append_hex(p, did, 4u);
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
    if (mid_rc != 0 || did_rc != 0) {
        report_i2c_detail(tag, 0);
        report_i2c_timing(tag, 0);
        report_i2c_wire(tag, 0);
    }
    return mid_rc == 0 && did_rc == 0 && mid == 0x5449u && did == 0x3055u;
}

/* 只在初始化失败后运行：同一套驱动，两种映射 x 两种接收方向 x 两地址。
 * 只读只读寄存器的 ID，不改 LDC 配置，不因 ACK 就自动切换接线定义。 */
#if APP_I2C_MATRIX
static void report_i2c_matrix(void)
{
    uint8_t swapped, input_rx, address;

    for (swapped = 0u; swapped < 2u; swapped++) {
        for (input_rx = 0u; input_rx < 2u; input_rx++) {
            hard_i2c_debug_config(swapped, input_rx);
            for (address = 0x2Au; address <= 0x2Bu; address++) {
                uint8_t regs[2] = {0x7Eu, 0x7Fu};
                uint16_t ids[2] = {0u, 0u};
                int id_rc[2];
                int probe_rc;
                uint8_t index;
                char tag[7];
                char *p;

                tag[0] = swapped != 0u ? '6' : '7';
                tag[1] = swapped != 0u ? '7' : '6';
                tag[2] = input_rx != 0u ? 'I' : 'O';
                tag[3] = input_rx != 0u ? 'N' : 'D';
                tag[4] = hex_char((uint8_t)(address >> 4));
                tag[5] = hex_char((uint8_t)(address & 0xFu));
                tag[6] = '\0';
                probe_rc = hard_i2c_write(address, 0, 0u);
                report_i2c_wire(tag, 0); /* ID 读取不能覆盖地址探测现场 */
                report_i2c_timing(tag, 0);
                for (index = 0u; index < 2u; index++) {
                    uint8_t bytes[2] = {0u, 0u};
                    id_rc[index] = hard_i2c_write_read(address, &regs[index], 1u,
                                                     bytes, 2u);
                    if (id_rc[index] == 0) {
                        ids[index] = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
                    }
                }
                p = s_out;
                p += append_str(p, "$M,");
                p += append_str(p, tag);
                *p++ = ',';
                p = append_i32(p, probe_rc);
                for (index = 0u; index < 2u; index++) {
                    *p++ = ',';
                    p = append_i32(p, id_rc[index]);
                    *p++ = ',';
                    p = append_hex(p, ids[index], 4u);
                }
                *p++ = '\r';
                *p++ = '\n';
                send(s_out, (int)(p - s_out));
            }
        }
    }
    hard_i2c_debug_config(0u, 0u);
    uart_puts("$B,I2C_RESTORED,PB7_SCL,PB6_SDA,OD\r\n");
}
#endif

/* 仅在初始化失败后扫描普通 7 位地址，不发送寄存器或数据。
 * 0x00~0x07、0x78~0x7F 是保留地址，不作为普通从机探测。
 * V8: $S,<addr7>,<expected_write_byte>,<rc>,<stage>,<last_tx>,<ack_level>,<lines>
 * 十六进制：addr7/write_byte/last_tx；其余数字为十进制。
 * START 失败时 expected_write_byte 只是计划值，不表示已经发到线上。 */
#if APP_I2C_SCAN
static void report_i2c_scan(void)
{
    uint16_t address;
    uint16_t ack_count = 0u, nack_count = 0u, error_count = 0u;
    char tag[3];
    char *p;

    uart_puts("$B,SCAN_BEGIN,08,77,W\r\n");
    for (address = 0x08u; address <= 0x77u; address++) {
        int rc = ldc1614_probe((uint8_t)address);
        hard_i2c_diag_t diag;

        hard_i2c_get_diag(&diag); /* 先保存本地址现场，串口不在位时序中打印。 */
        if (rc == 0) ack_count++;
        else if (rc == -2) nack_count++;
        else error_count++;

        p = s_out;
        p += append_str(p, "$S,");
        p = append_hex(p, address, 2u);
        *p++ = ',';
        p = append_hex(p, (uint32_t)address << 1, 2u);
        *p++ = ',';
        p = append_i32(p, rc);
        *p++ = ',';
        p += append_str(p, i2c_stage_name(diag.stage));
        *p++ = ',';
        p = append_hex(p, diag.tx_byte, 2u);
        *p++ = ',';
        p = append_u32(p, diag.ack_level);
        *p++ = ',';
        p = append_u32(p, diag.lines);
        *p++ = '\r';
        *p++ = '\n';
        send(s_out, (int)(p - s_out));

        /* 保留原来 2A~2D 的输出格式，方便与旧日志直接比较。 */
        if (address >= 0x2Au && address <= 0x2Du) {
            p = s_out;
            p += append_str(p, "$P,");
            p = append_hex(p, address, 2u);
            *p++ = ',';
            *p++ = rc == 0 ? '1' : '0';
            *p++ = '\r';
            *p++ = '\n';
            send(s_out, (int)(p - s_out));
            tag[0] = hex_char((uint8_t)(address >> 4));
            tag[1] = hex_char((uint8_t)(address & 0xFu));
            tag[2] = '\0';
            report_i2c_detail(tag, 0);
            report_i2c_timing(tag, 0);
        }
    }
    p = s_out;
    p += append_str(p, "$B,SCAN_DONE,112,");
    p = append_u32(p, ack_count);
    *p++ = ',';
    p = append_u32(p, nack_count);
    *p++ = ',';
    p = append_u32(p, error_count);
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
}

#endif /* APP_I2C_SCAN */

static void report_ldc_init_step(const ldc1614_init_step_t *step)
{
    char *p = s_out;
    p += append_str(p, "$C,");
    p = append_hex(p, step->reg, 2u);
    *p++ = ',';
    p = append_hex(p, step->expected, 4u);
    *p++ = ',';
    p = append_i32(p, step->write_rc);
    *p++ = ',';
    p = append_i32(p, step->read_rc);
    *p++ = ',';
    p = append_hex(p, step->actual, 4u);
    *p++ = ',';
    p += append_str(p, step->result != 0 ? "FAIL" :
                       step->nack_verified ? "NACK_VERIFIED" : "OK");
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
}

static void report_i2c_debug(void)
{
    hard_i2c_diag_t diag;
    /* Readback must not hide the first write NACK or timeout. */
    ldc1614_get_init_bus_diag(&diag);
    report_i2c_detail("INIT", &diag);
    report_i2c_timing("INIT", &diag);
    report_i2c_wire("INIT", &diag);
#if APP_I2C_SCAN
    report_i2c_scan();
#endif
#if APP_I2C_MATRIX
    report_i2c_matrix();
#endif
}

/* rc=0 passed; DATA=1 means no two valid fresh pairs, not an I2C failure.
 * rc=-4 means this stage was not attempted. Never label missing data NO_COIL:
 * without instruments it cannot distinguish sensor, reference and other faults. */
static __attribute__((noinline)) void report_ldc_check(const char *tag,
                                                      const char *stage, int rc)
{
    char *p = s_out;
    p += append_str(p, "$K,");
    p += append_str(p, tag);
    *p++ = ',';
    p += append_str(p, stage);
    *p++ = ',';
    p = append_i32(p, rc);
    *p++ = '\r';
    *p++ = '\n';
    send(s_out, (int)(p - s_out));
}
#else
#define report_ldc_check(tag, stage, rc) ((void)0)
#endif

/* Verify stable configuration independently of sensor data. With report=1,
 * also capture STATUS and both raw channels, even when UNREADCONV is clear.
 * Raw diagnostic reads are NOT freshness/measurement proof. Do not compare
 * DRIVE_CURRENT after wake: automatic drive calibration may change its fields.
 * No writes or recovery clocks here; capture before any fallback soft reset. */
static int check_ldc_config(const char *tag, int external, int report)
{
    static const uint8_t regs[] = {
        0x1Au, 0x1Bu, 0x08u, 0x09u, 0x10u, 0x11u, 0x19u, 0x14u, 0x15u,
        0x18u, 0x00u, 0x01u, 0x02u, 0x03u
    };
    static const uint16_t expected[] = {
        0x0801u, 0x820Cu, 0xFFFFu, 0xFFFFu, 0x0180u, 0x0180u, 0x0001u,
        0x1001u, 0x1001u
    };
    const unsigned config_count = sizeof(expected) / sizeof(expected[0]);
    unsigned count = report ? sizeof(regs) : config_count;
    unsigned i;
    int result = 0;
    for (i = 0u; i < count; ++i) {
        uint16_t value = 0u;
        int rc = ldc1614_read_reg(regs[i], &value);
        if (i < config_count) {
            uint16_t target = expected[i];
            if (i == 0u && external) target |= 0x0200u;
            if (rc == 0 && value != target) rc = LDC1614_ERR_CONFIG_HOLD;
            if (result == 0 && rc != 0) result = rc;
        }
#if APP_I2C_DEBUG
        /* Keep $Q read_rc as the actual read result, not the comparison code. */
        if (report || rc != 0) {
            char *p = s_out;
            p += append_str(p, "$Q,");
            p += append_str(p, tag);
            *p++ = ',';
            p = append_hex(p, regs[i], 2u);
            *p++ = ',';
            p = append_i32(p, rc == LDC1614_ERR_CONFIG_HOLD ? 0 : rc);
            *p++ = ',';
            p = append_hex(p, value, 4u);
            *p++ = '\r';
            *p++ = '\n';
            send(s_out, (int)(p - s_out));
        }
#else
        (void)tag;
#endif
    }
    return result;
}

/* A matching CONFIG is not proof of an oscillator or sensor working.
 * Require two newly completed CH0+CH1 pairs; constant sensor values are valid.
 * No INTB dependency: check UNREADCONV bits every 20ms, with bounded waiting. */
static int verify_ldc_conversions(const char *tag)
{
    uint32_t ch0 = 0u, ch1 = 0u;
    uint16_t status = 0u, err = 0u;
    unsigned poll, pairs = 0u;
    int rc = 1;
    for (poll = 0u; poll <= 25u; ++poll) {
        rc = ldc1614_read_fresh_raw(&ch0, &ch1, &err, &status);
        if (rc == 0) {
            if (++pairs == 2u) break;
        } else if (rc < 0) {
            pairs = 0u;
            /* A bus failure is not a missing sensor; preserve its transaction. */
            if (rc != LDC1614_ERR_NO_DATA) break;
        }
        if (poll < 25u) board_delay_ms(APP_POLL_MS);
    }
#if APP_I2C_DEBUG
    {
        char *p = s_out;
        p += append_str(p, "$V,");
        p += append_str(p, tag);
        *p++ = ',';
        p = append_u32(p, pairs);
        *p++ = ',';
        p = append_i32(p, rc);
        *p++ = ',';
        p = append_hex(p, status, 4u);
        *p++ = ',';
        p = append_u32(p, ch0);
        *p++ = ',';
        p = append_u32(p, ch1);
        *p++ = ',';
        p = append_hex(p, err, 4u);
        *p++ = '\r';
        *p++ = '\n';
        send(s_out, (int)(p - s_out));
    }
#else
    (void)tag;
#endif
    if (rc < 0 && rc != LDC1614_ERR_NO_DATA) return rc;
    return pairs == 2u ? 0 : LDC1614_ERR_NO_DATA;
}

static int start_ldc(int external)
{
    const char *tag = external ? "EXT40" : "INTERNAL";
    int rc, data_rc = -4, hold_rc;
    s_ldc_external = external ? 1u : 0u;
    uart_puts(external ? "$B,LDC_START,EXT40\r\n" : "$B,LDC_START,INTERNAL\r\n");
    rc = external ? ldc1614_init(0, 0) : ldc1614_init_clock(0, 0, 0);
    report_ldc_check(tag, "INIT", rc);
    if (rc == 0) {
        data_rc = verify_ldc_conversions(tag);
        rc = data_rc;
    }
    if (rc != 0) {
#if APP_I2C_DEBUG
        char *p = s_out;
        p += append_str(p, "$B,LDC_ATTEMPT,");
        p += append_str(p, tag);
        *p++ = ',';
        p = append_i32(p, rc);
        *p++ = '\r';
        *p++ = '\n';
        send(s_out, (int)(p - s_out));
        if (rc != LDC1614_ERR_NO_DATA)
            report_i2c_debug(); /* Before snapshot reads replace the bus diag. */
#endif
    }
    /* Even an exact CONFIG readback does not prove that the other settings
     * survived wake. Recheck after the conversion wait, also without coils. */
    hold_rc = check_ldc_config(tag, external, rc != 0);
    report_ldc_check(tag, "HOLD", hold_rc);
    report_ldc_check(tag, "DATA", data_rc == LDC1614_ERR_NO_DATA ? 1 : data_rc);
    if ((rc == 0 || rc == LDC1614_ERR_NO_DATA) && hold_rc != 0)
        return LDC1614_ERR_CONFIG_HOLD;
    return rc;
}

void app_init(void)
{
    int rc;

    unsigned attempt;

    s_ldc_ready = 0u;
    board_init(); /* Exact test_board.c clock/GPIO/TIM14/USART2 setup. */
    uart_puts(board_clock_is_hse8() ? "$B,BOOT,HSE8M\r\n" : "$B,BOOT,CLOCK_UNEXPECTED\r\n");
    uart_puts("$B,FW,C011_SW_I2C_V12_STAGE_DIAG\r\n");    uart_puts("$B,I2C_SOURCE,ORIGINAL_TEST,ADDR,2A\r\n");
    test_delay_ms(100u); /* SysTick is deliberately still OFF here. */
    ldc1614_init_driver();

#if APP_I2C_DEBUG
    /* No formal UART reinit, button, ADC, EXTI or SysTick before this check.
     * Like the standalone test, allow the module more than one ID attempt. */
    for (attempt = 0u; attempt < 5u; ++attempt) {
        if (report_i2c_id("REF_ID")) break;
        if (attempt + 1u < 5u) test_delay_ms(1000u);
    }
#else
    (void)attempt;
#endif

    board_start_app_services();
#if APP_I2C_DEBUG
    (void)report_i2c_id("IRQ_ID");
#endif
    /* USART2 is already initialized by the unchanged test_board.c. */
    button_init(); /* Keep the unused switch code. */
    console_init();

    /* 新板不使用 OLED；保留初始化函数，仅注释调用。
     * uart_puts((display_init() == 0) ? "$B,OLED OK\r\n" : "$B,OLED FAIL\r\n");
     */

    adc_temp_init();

    measure_init();
    measure_load();

    protocol_init();/*初始化串口协议层,设置上报间隔为APP_REPORT_MS毫秒*/
    protocol_set_rate_ms(APP_REPORT_MS);/*设置上报间隔为APP_REPORT_MS毫秒，上面默认就是100这里可以不用*/

#if APP_I2C_DEBUG
    report_ldc_check("BOOT", "COMM", report_i2c_id("PRE_ID") ? 0 : -11);
#endif
#if APP_I2C_DEBUG
    ldc1614_set_init_trace(report_ldc_init_step);
#else
    ldc1614_set_init_trace(0);
#endif
    rc = start_ldc(1);
    /* Only a CONFIG/start-conversion failure warrants this one-shot comparison.
     * ID, wiring, divider or other earlier failures are not a clock diagnosis. */
    if (rc == LDC1614_ERR_CONFIG || rc == LDC1614_ERR_CONFIG_HOLD ||
        rc == LDC1614_ERR_NO_DATA) {
        uart_puts("$B,LDC_FALLBACK,INTERNAL,RECHECK_CAL\r\n");
        rc = start_ldc(0); /* Full reset + all settings, not just clearing bit9. */
        if (rc == 0) uart_puts("$B,LDC_CLOCK,INTERNAL,RECHECK_CAL\r\n");
    } else if (rc == 0) {
        uart_puts("$B,LDC_CLOCK,EXT40\r\n");
    }
    if (rc != 0) {
        measure_fault();
        report_boot_error(rc);/*汇报错误*/
        uart_puts("$B,LDC_DIAG_ONLY\r\n");
    }

    s_temp_c = (float)adc_temp_read_x10() / 10.0f;/*读取温度传感器的温度值，单位为摄氏度*/
    {
        int32_t t10 = (int32_t)(s_temp_c * 10.0f);/*温度 x10, 用来判断是不是异常*/

        if (t10 < -400 || t10 > 1250) {
            report_temp_debug(t10);/*温度明显不对, 把原始值打出来方便查*/
        }
    }
    s_last_temp_ms = board_millis();/*记录上次读取温度的时间戳*/
    s_last_data_ms = s_last_temp_ms;/*记录上次读到数据的时间戳*/
    s_last_poll_ms = s_last_temp_ms;/*记录上次轮询的时间戳*/
    s_last_diag_ms = s_last_temp_ms;

    if (rc == 0) {
        s_ldc_ready = 1u;
        uart_puts("$B,LDC1614 ready\r\n");
#if APP_I2C_DEBUG
        report_i2c_timing("READY", 0);
#endif
    }

    s_last_oled_ms = board_millis();
    /* display_update(); */ /* 新板不使用 OLED */
}

void app_poll(void)
{
    uint32_t now = board_millis();/*现在的时间戳*/

    /* ---- 0. 串口命令 ---- */
    console_poll();

    /* ---- 0. 按键 ---- */
    button_scan(now);

#if APP_I2C_DEBUG
    if (!s_ldc_ready && (uint32_t)(now - s_last_diag_ms) >= APP_DIAG_MS) {
        const char *tag = s_ldc_external ? "EXT40" : "INTERNAL";
        int hold_rc;
        s_last_diag_ms = now;
        report_ldc_check(tag, "COMM", report_i2c_id("DIAG_ID") ? 0 : -11);
        hold_rc = check_ldc_config(tag, s_ldc_external, 1);
        report_ldc_check(tag, "HOLD", hold_rc);
        /* No resets, automatic reconfiguration, or zero/stale data promotion.
         * Reboot after installing sensors to run full measurement validation. */
    }
#endif

    if (button_short_press(BTN_KEY1)) {
        /* display_toggle_page(); */
        /* display_update(); */
    }
    if (button_short_press(BTN_KEY2)) {
        /* display_toggle_hold(); */
        /* display_update(); */
    }

    /* ---- 1. 采样 ----
     * INTB 触发优先; 如果 INTB 没接或没来, 每 20ms 主动读一次兜底。 */
    if (s_ldc_ready && (board_intb_take() ||
        (uint32_t)(now - s_last_poll_ms) >= APP_POLL_MS)) {
        s_last_poll_ms = now;
        uint32_t ch0 = 0u;
        uint32_t ch1 = 0u;
        uint16_t err = 0u;

        int sample_rc = ldc1614_read_fresh_raw(&ch0, &ch1, &err, 0);
        if (sample_rc == 0) {
            s_last_data_ms = now; /* Only successful data refreshes this timestamp. */
            //如果距离上次读温度的时间大于APP_TEMP_MS，就再读一次温度，单位为摄氏度
            if ((uint32_t)(now - s_last_temp_ms) >= APP_TEMP_MS) {
                s_last_temp_ms = now;
                s_temp_c = (float)adc_temp_read_x10() / 10.0f;
            }
            measure_update(ch0, ch1, s_temp_c, now);
        } else if (sample_rc < 0) {
            measure_fault();
        }
    }

    /* ---- 2. 长时间读不到数据 -> 故障 ---- */
    if ((uint32_t)(now - s_last_data_ms) > APP_DATA_TIMEOUT) {
        measure_fault();
    }

    /* ---- 3. OLED 刷新: 新板不使用，保留原调用代码 ---- */
    if ((uint32_t)(now - s_last_oled_ms) >= APP_OLED_MS) {
        s_last_oled_ms = now;
        /* display_update(); */
    }

    /* ---- 4. 周期上报 ---- */
    send(s_out, protocol_tick(s_out, (int)sizeof(s_out), measure_latest(), now));

    board_feed_watchdog();
}
