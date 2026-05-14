# I2C CLK 测试修改记录

## 1. 原始需求

在没有连接从设备的情况下，让 **nRF54LM20A** 的 I2C 时钟线（SCL）上能够持续输出时钟脉冲，以便用逻辑分析仪/示波器验证 I2C 时钟频率是否准确。

目标板：`nrf54lm20dk_nrf54lm20a_cpuapp`
I2C 引脚：SCL = P1.13, SDA = P1.14（通过设备树 `&i2c21` 配置）

---

## 2. 核心限制（根本原因）

**标准 I2C 协议规定**：主机发送从机地址后，从机必须在第 9 个时钟周期内回复 ACK（拉低 SDA）。如果没有从设备，SDA 保持高电平，主机收到 NACK 后会立即发送 STOP 并终止传输。

nRF 的 TWIM 硬件严格遵循此协议：
- 没有从设备 ACK → 传输在**地址字节结束后立即停止**
- **不可能**在一次传输中连续输出 4 字节的数据时钟
- 实际波形为：`START + 地址(8 bits) + NACK(1 bit) + STOP`

---

## 3. 修改内容

### 3.1 src/main.c

删除了原始的 BME280 传感器读取代码，改为在 `while(1)` 中循环发送 1 字节数据，忽略错误返回值。

```c
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>

#define I2C_NODE DT_NODELABEL(mysensor)

int main(void)
{
    static const struct i2c_dt_spec dev_i2c = I2C_DT_SPEC_GET(I2C_NODE);

    if (!device_is_ready(dev_i2c.bus)) {
        printk("I2C bus %s is not ready!\n", dev_i2c.bus->name);
        return -1;
    }

    printk("TWIM CLK test: sending 1 byte in loop...\n");

    uint8_t data = 0xAA;

    while (1) {
        /* Ignore return value; without a slave it will NACK and error out,
         * but the address byte (8 SCL clocks) must still be emitted.
         */
        (void)i2c_write_dt(&dev_i2c, &data, 1);
    }

    return 0;
}
```

### 3.2 boards/nrf54lm20dk_nrf54lm20a_cpuapp.overlay

启用了 `clock-frequency` 属性，设置 I2C 速率为 400 kbps（Fast mode）。

```dts
&i2c21 {
    status = "okay";
    pinctrl-0 = <&i2c21_default>;
    pinctrl-1 = <&i2c21_sleep>;
    pinctrl-names = "default", "sleep";
    clock-frequency = <I2C_BITRATE_FAST>; /* 400 kbps */
    mysensor: mysensor@77{
        compatible = "i2c-device";
        status = "okay";
        reg = < 0x77 >;
    };
};

&pinctrl {
    /omit-if-no-ref/ i2c21_default: i2c21_default {
        group1 {
            psels = <NRF_PSEL(TWIM_SCL, 1, 13)>,
                    <NRF_PSEL(TWIM_SDA, 1, 14)>;
        };
        nordic,drive-mode = <NRF_DRIVE_H0D1>;
        bias-pull-up;
    };
    /omit-if-no-ref/ i2c21_sleep: i2c21_sleep {
        group1 {
            psels = <NRF_PSEL(TWIM_SCL, 1, 13)>,
                    <NRF_PSEL(TWIM_SDA, 1, 14)>;
        };
        low-power-enable;
    };
};
```

### 3.3 prj.conf

```
CONFIG_I2C=y
CONFIG_CBPRINTF_FP_SUPPORT=y

# 缩短 I2C 传输超时（默认 500 ms → 10 ms）
# 这样没有从设备时，波形重试间隔更密集
CONFIG_I2C_NRFX_TRANSFER_TIMEOUT=10

# 禁用设备 Runtime PM，防止 TWIM 在 init 后进入 SUSPEND 状态
CONFIG_PM_DEVICE_RUNTIME=n
```

---

## 4. 测试结果

### 4.1 串口 Log

```
Starting I2C CLK test: writing 1 byte in loop...
I2C write error (expected if no slave): -116
...
```

错误码 **-116 = ETIMEDOUT**，表示 TWIM 传输在规定时间内未完成。

### 4.2 逻辑分析仪观察

**SCL 信号一直为高电平，没有产生 8 个时钟脉冲。**

> 波形特征：SCL 始终维持高电平，SDA 也为高电平，没有 Start 条件或时钟脉冲产生。

---

## 5. 问题分析

### 5.1 波形间隔问题（已部分解决）

nRF TWIM 的 I2C 波形极短。以 400 kbps 计算：
- `START + 地址(8bits) + NACK + STOP` ≈ **25 µs**
- 两次传输间隔 ≈ **10 ms**（超时等待）
- 占空比仅 **0.25%**

已通过在 `prj.conf` 中设置 `CONFIG_I2C_NRFX_TRANSFER_TIMEOUT=10` 缩短间隔。

### 5.2 SCL 完全无脉冲（核心问题）

即使波形极短，理论上逻辑分析仪在 **SCL 下降沿触发**模式下应能捕获到脉冲。实际观察到 SCL 完全保持高电平，说明** TWIM 硬件根本没有启动总线传输**。

可能原因：

1. **Runtime PM 导致设备未激活**：nRF TWIM 驱动在 `i2c_nrfx_twim_common_init()` 中通过 `pm_device_driver_init()` 初始化。若启用了 `CONFIG_PM_DEVICE_RUNTIME`，设备默认进入 `SUSPENDED` 状态，第一次 `i2c_write_dt()` 时才 `RESUME`。若 resume 流程异常，可能导致 TWIM 未真正使能。
   - **已尝试解决**：在 `prj.conf` 中添加 `CONFIG_PM_DEVICE_RUNTIME=n`。

2. **nrfx TWIM HAL 在 nRF54L 上的兼容性问题**：nRF54L 系列的 TWIM 寄存器布局与 nRF52/53 不同。nrfx v3.3.0 中对 nRF54LM20A 的 TWIM 支持可能不完整，导致 `nrfx_twim_xfer()` 调用后寄存器状态异常，总线未激活。

3. **设备树缺少 `clocks` 属性**：对比发现 `nrf54lm20_a_b.dtsi` 中 `i2c21` 节点缺少 `clocks` 属性（nRF52840 的 `i2c0` 有 `clocks = <&hfclk>`）。不过 TWIM 驱动代码中并未使用 `clocks` phandle，而是直接操作 `FREQUENCY` 寄存器，因此理论上不影响。

4. **pinctrl 配置未正确应用**：`i2c21_default` 使用了 `/omit-if-no-ref/` 标签，且板级 dtsi 中未定义 `i2c21_default`/`i2c21_sleep`（用户 overlay 中自定义）。若 pinctrl 解析异常，SCL/SDA 引脚可能未被正确配置为 TWIM 功能模式。

---

## 6. 后续建议方案

### 方案 A：验证 TWIM 是否真的被使能

在 `main()` 中添加 GPIO 测试，确认 P1.13 引脚是否受程序控制：

```c
#include <zephyr/drivers/gpio.h>
#define SCL_PORT DEVICE_DT_GET(DT_NODELABEL(gpio1))
#define SCL_PIN  13

int main(void) {
    gpio_pin_configure(SCL_PORT, SCL_PIN, GPIO_OUTPUT_INACTIVE);
    while (1) {
        gpio_pin_set(SCL_PORT, SCL_PIN, 1);
        k_busy_wait(5);
        gpio_pin_set(SCL_PORT, SCL_PIN, 0);
        k_busy_wait(5);
    }
}
```

若此代码能产生 100 kHz 方波，说明：
- 硬件连接正确
- P1.13 物理位置正确
- 逻辑分析仪工作正常

### 方案 B：使用 GPIO Bitbang I2C

Zephyr 提供 `gpio-i2c` 软件 I2C 驱动，通过 GPIO 手动翻转 SCL/SDA。软件 I2C 不受 TWIM 硬件限制，可以在没有从设备时**强制输出完整的 8 位数据时钟**（忽略 ACK）。

步骤：
1. 在 `prj.conf` 中启用 `CONFIG_I2C_GPIO=y`
2. 在设备树中定义 `gpio_i2c` 节点，映射到 P1.13/P1.14
3. 修改代码使用 `gpio-i2c` 设备

> **注意**：需要确认 `i2c_bitbang.c` 中 `i2c_write_byte()` 在无 ACK 时会 `goto finish`，即仍会在地址字节后停止。若要输出完整数据时钟，需要修改 `i2c_bitbang.c` 或自行编写 GPIO 翻转代码。

### 方案 C：直接编写 GPIO 翻转代码（最可靠）

不依赖任何 I2C 驱动，直接用 GPIO 模拟完整的 I2C Start + 8 位数据 + Stop 序列：

```c
while (1) {
    i2c_start();          // SDA↓, SCL↓
    for (int i = 7; i >= 0; i--) {
        i2c_write_bit(data & (1 << i));  // SDA=bit, SCL↑, delay, SCL↓, delay
    }
    i2c_read_ack();       // SDA↑, SCL↑, delay, SCL↓ (ignore result)
    i2c_stop();           // SDA↓, SCL↑, SDA↑
}
```

这是**最可靠**的方案，完全绕过 TWIM 硬件的所有限制。

### 方案 D：物理伪造 ACK

将 **SDA (P1.14) 用 4.7kΩ 电阻短接到 GND**。这样 TWIM 硬件会认为从设备回复了 ACK，会继续发送后续数据字节。

效果：
- 地址字节 8 bits + ACK + 数据字节 8 bits + ACK + STOP
- 逻辑分析仪上能看到完整的 18 个 SCL 周期
- 可以准确测量时钟频率

---

## 7. 关键知识点

| 项目 | 说明 |
|------|------|
| 板型 | nrf54lm20dk_nrf54lm20a_cpuapp |
| I2C 实例 | i2c21 (TWIM21)，基地址 0xC7000 |
| SCL 引脚 | P1.13（扩展头 Pin 5） |
| SDA 引脚 | P1.14（扩展头 Pin 6 为 P3.0，注意区分！P1.14 不在扩展头标准位置，需确认物理焊盘） |
| 默认超时 | `CONFIG_I2C_NRFX_TRANSFER_TIMEOUT=500` ms |
| 错误码 -116 | `ETIMEDOUT`，传输超时 |
| I2C 速率选项 | `I2C_BITRATE_STANDARD`(100k), `I2C_BITRATE_FAST`(400k), `I2C_BITRATE_FAST_PLUS`(1M) |
| 波形时长 @400k | 约 25 µs（Start + Addr + NACK + Stop） |
| 逻辑分析仪触发 | **必须**设为 `SCL Falling Edge` + `Single/One-shot` 模式 |

---

---

## 9. 后续更新：SDA 拉低后出现波形，但频率只有 100k

### 9.1 现象

将 SDA (P1.14) 用 4.7kΩ 电阻短接到 GND 后，逻辑分析仪可以正常捕获 I2C 波形。但即使 overlay 中设置了 `clock-frequency = <I2C_BITRATE_FAST>;`（400 kbps），逻辑分析仪实测 SCL 频率只有 **103.95 kbps**（≈ 100k）。

### 9.2 根因分析

Zephyr nRF TWIM 驱动在 `i2c_nrfx_twim_common.h` 中使用 `I2C_NRFX_TWIM_FREQUENCY_CALCULATE_CORRECTED` 宏动态计算 `FREQUENCY` 寄存器值：

```c
#define I2C_NRFX_TWIM_FREQUENCY_CALCULATE_CORRECTED(bitrate, f_pclk, tolerance_percent)
    I2C_NRFX_TWIM_FREQUENCY_CALCULATE(bitrate,
        DIV_ROUND_CLOSEST(f_pclk * (100 + tolerance_percent), 100))
```

nRF54L 的 SoC dtsi 中给 TWIM 定义了 `clock-tolerance-percent = <6>;`，因此驱动一定会走 `CALCULATE_CORRECTED` 路径。

以 400k @ 16 MHz PCLK 计算：
- `f_pclk_corrected = 16_000_000 * 106 / 100 = 16_960_000`
- `divider = 16_960_000 / 400_000 = 42`
- `FREQUENCY = (1_048_576 / 42) << 12 = 0x06180000`

而 Nordic 预定义的标准值：
- `NRF_TWIM_FREQ_100K = 0x01980000`
- `NRF_TWIM_FREQ_400K = 0x06400000`

计算出的 `0x06180000` 不等于标准值 `0x06400000`。nRF54L 的 TWIM 硬件对非标准 `FREQUENCY` 值的解析行为不确定，实测结果 fallback 到了 **100k**。

### 9.3 解决方案

在 `main.c` 中显式调用 `i2c_configure()`，让驱动走 `i2c_nrfx_twim_configure()` 中的标准枚举路径，直接写入 `NRF_TWIM_FREQ_400K = 0x06400000`：

```c
ret = i2c_configure(dev_i2c.bus,
                    I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST));
```

修改后的 `main.c`：

```c
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/printk.h>
#include <nrfx_twim.h>
#include <hal/nrf_twim.h>

#define I2C_NODE DT_NODELABEL(mysensor)

int main(void)
{
    static const struct i2c_dt_spec dev_i2c = I2C_DT_SPEC_GET(I2C_NODE);

    if (!device_is_ready(dev_i2c.bus)) {
        printk("I2C bus %s is not ready!\n", dev_i2c.bus->name);
        return -1;
    }

    printk("TWIM CLK test: sending 1 byte in loop...\n");

    /* Read current FREQUENCY register before configure */
    NRF_TWIM_Type *twim = NRF_TWIM21;
    printk("Before i2c_configure, FREQUENCY = 0x%08X\n", (unsigned int)twim->FREQUENCY);

    /* Explicitly set I2C speed to 400k */
    int ret = i2c_configure(dev_i2c.bus,
                            I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST));
    if (ret != 0) {
        printk("i2c_configure failed: %d\n", ret);
        return -1;
    }

    printk("After i2c_configure, FREQUENCY = 0x%08X\n", (unsigned int)twim->FREQUENCY);
    printk("Expected for 400k: 0x06400000, for 100k: 0x01980000\n");

    uint8_t data = 0xAA;

    while (1) {
        (void)i2c_write_dt(&dev_i2c, &data, 1);
    }

    return 0;
}
```

### 9.4 关键结论

| 设置方式 | 预期写入 FREQUENCY 寄存器 | 实测频率 |
|----------|--------------------------|----------|
| 设备树 `clock-frequency = <400000>` + `clock-tolerance-percent = <6>` | `0x06180000` (动态计算) | ~100k |
| `i2c_configure(I2C_SPEED_FAST)` | `0x06400000` (标准枚举) | **仍然 ~100k** |

`i2c_configure()` 调用后实测仍为 100k，说明问题**不在驱动的频率计算宏**，而是 TWIM 硬件本身或时钟源的问题。

### 9.5 新的排查方向：直接读取 FREQUENCY 寄存器

已在 `main.c` 中添加寄存器读取代码：

```c
NRF_TWIM_Type *twim = NRF_TWIM21;
printk("Before i2c_configure, FREQUENCY = 0x%08X\n", (unsigned int)twim->FREQUENCY);
...
printk("After i2c_configure, FREQUENCY = 0x%08X\n", (unsigned int)twim->FREQUENCY);
```

根据串口 Log 中打印的寄存器值，可以判断：

| Log 输出 | 含义 |
|----------|------|
| `Before = 0x06180000, After = 0x06400000` | 驱动写入了正确的 400k 值，但硬件仍然输出 100k → **TWIM 时钟源不是 16 MHz，可能是 4 MHz** |
| `Before = 0x01980000, After = 0x01980000` | `i2c_configure()` 根本没有写入新值 → **TWIM 未处于 ACTIVE 状态或 configure API 未绑定** |
| `Before = 0x00000000, After = 0x06400000` | 写入成功，但硬件输出 100k → **nRF54LM20A 的 TWIM 在 400k 设置下有硬件限制/Errata** |

### 9.6 旧结论（已证伪）：nRF54LM20A TWIM21 PCLK = 4 MHz

> 以下结论是此前基于波形现象做出的推断，现已被后续源码和 build 产物验证推翻。
> 当前不要再把 `TWIM21 PCLK = 4 MHz` 作为根因继续使用。

根据 Log：
```
Before i2c_configure, FREQUENCY = 0x06186000
After i2c_configure, FREQUENCY  = 0x06400000
```

驱动正确写入了标准 400k 值 `0x06400000`，但逻辑分析仪实测仍为 ~100k。

验证计算：
- `0x06400000 >> 12 = 25_600`
- 分频比 `= 2^20 / 25_600 = 40.96`
- 若 `PCLK = 16 MHz`：`16M / 40.96 = 390_625` ≈ 400k ✓
- 若 `PCLK = 4 MHz`：`4M / 40.96 = 97_656` ≈ **100k** ✓（与实测 103.95k 吻合）

**结论：nRF54LM20A 的 TWIM21 时钟源是 4 MHz，而非 16 MHz。**

### 9.7 旧解决方案（不再采用）

在 `main.c` 中直接覆盖 `FREQUENCY` 寄存器，写入 4 倍标准值：

```c
NRF_TWIM_Type *twim = NRF_TWIM21;
/* nRF54LM20A TWIM21 runs on 4 MHz PCLK, not 16 MHz.
 * Standard 400k value (0x06400000) yields ~100k on 4 MHz.
 * Write 4x the standard value to get actual 400k.
 */
twim->FREQUENCY = 0x19000000;
```

验证：`0x19000000 >> 12 = 102_400`，`2^20 / 102_400 = 10.24`，`4M / 10.24 = 390_625` ≈ **400k** ✓

---

## 10. 结论

- **无从设备时 TWIM 不输出时钟**：nRF TWIM 硬件严格遵循 I2C 协议，NACK 后自动停止。必须通过物理拉低 SDA 伪造 ACK，才能看到完整波形。
- **频率寄存器差异的真实原因**：`overlay` 初始化路径和 `i2c_configure()` 运行时路径使用了两套不同的频率求值逻辑。前者会受 `clock-tolerance-percent` 影响，后者直接写标准枚举值。

## 11. 修正结论：`0x0F0F0000` vs `0x0FF00000` 的根因

在给 SCL 增加 1 kOhm 外部上拉后，板上已经可以稳定看到 1 MHz 波形。这说明此前“TWIM21 实际 PCLK 只有 4 MHz”的推断不成立，问题不在外围时钟源，而在 **设备树初始化路径和运行时 API 路径对 `FREQUENCY` 寄存器的求值不同**。

### 11.1 为什么 `Before i2c_configure = 0x0F0F0000`

`Before i2c_configure` 读取到的是 **驱动初始化阶段**写入的值。

Zephyr 的 `i2c_nrfx_twim` 驱动在实例初始化时使用 `I2C_FREQUENCY(inst)` 宏：

```c
#define I2C_FREQUENCY(inst) \
    COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, clock_tolerance_percent), \
        (I2C_NRFX_TWIM_FREQUENCY_CALCULATE_CORRECTED(...)), \
        (I2C_NRFX_TWIM_FREQUENCY_ENUM_GET(...)))
```

而 nRF54LM20A 的 SoC dtsi 默认给 `i2c21` 带了：

- `clock-frequency = <1000000>`
- `clock-tolerance-percent = <6>`

因此初始化走的是 `I2C_NRFX_TWIM_FREQUENCY_CALCULATE_CORRECTED()`，不是标准枚举路径。

以 1 MHz 计算：

- `f_pclk = 16 MHz`
- `f_pclk_corrected = 16 MHz * 106 / 100 = 16.96 MHz`
- `divider = DIV_ROUND_CLOSEST(16.96, 1) = 17`
- `FREQUENCY = (2^20 / 17) << 12 = 0x0F0F0000`

所以：

- `Before i2c_configure = 0x0F0F0000`

这是 **设备树 + clock tolerance 修正** 的直接结果，不是硬件偷偷降频。

### 11.2 为什么 `After i2c_configure = 0x0FF00000`

`After i2c_configure` 读取到的是 **运行时 `i2c_configure()`** 写入的值。

`i2c_nrfx_twim_configure()` 不走 `CALCULATE_CORRECTED`，而是直接按 speed 枚举写标准寄存器值：

```c
case I2C_SPEED_FAST_PLUS:
    nrf_twim_frequency_set(..., NRF_TWIM_FREQ_1000K);
    break;
```

对应标准定义：

- `NRF_TWIM_FREQ_1000K = 0x0FF00000`

所以：

- `After i2c_configure = 0x0FF00000`

这是 **运行时 API 直接覆盖了初始化值**，因此和 `Before` 不同是预期行为。

### 11.3 为什么 overlay 和 `main()` 设置看起来“不一样”

不是因为它们一个生效、一个不生效，而是因为：

- `overlay` 通过设备树初始化时，默认会叠加 `clock-tolerance-percent = 6`，得到校正值 `0x0F0F0000`
- `main()` 里调用 `i2c_configure(I2C_SPEED_FAST_PLUS)` 时，直接写标准枚举 `0x0FF00000`

两者都生效，只是 **求值规则不同**。

### 11.4 这次实际修正

为了让 `overlay` 初始化路径和 `i2c_configure()` 的结果一致，已在 `&i2c21` overlay 中删除继承的 `clock-tolerance-percent`：

```dts
&i2c21 {
    /delete-property/ clock-tolerance-percent;
    clock-frequency = <I2C_BITRATE_FAST_PLUS>;
    ...
};
```

重编译后验证结果：

- `build/.../zephyr.dts` 中 `i2c21` 节点已不再包含 `clock-tolerance-percent`
- `clock-frequency` 仍为 `1000000`

因此后续如果仍打印寄存器值，`Before i2c_configure` 应与标准 1 MHz 枚举路径一致，不再出现 `0x0F0F0000` 这种由 tolerance 修正得到的值。
