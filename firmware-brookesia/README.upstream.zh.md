# ESP32-P4-WIFI6-Touch-LCD-4.3 ESP-Brookesia 固件

[English](README.md)

这是 ESP32-P4-WIFI6-Touch-LCD-4.3 Rev3.x 对应日期版本出厂镜像的源码快照，
提供 Brookesia 手机桌面及配套应用，包括 ESP-Hosted Wi-Fi、摄像头、音频、音乐、
视频、绘图、频谱分析、设置、计算器和小智。

## 环境要求

- ESP-IDF v5.5.5
- ESP32-P4 Rev3.x

## 板级配置

| 项目 | 参数 |
| --- | --- |
| 显示屏 | 4.3 英寸 ST7701 MIPI-DSI 屏 |
| 分辨率 | 480 x 800 |
| 数据 lane | 2 |
| 单 lane 速率 | 500 Mbps |
| DPI 时钟 | 30 MHz |
| 触摸 | GT911，I2C 接口 |

本地板级组件保留产品 BSP 的 ST7701 初始化序列、屏幕时序、引脚和 lane 速率，
并按芯片代际选择 MIPI PHY 参考时钟：pre-v3 使用旧版
`MIPI_DSI_PHY_CLK_SRC_DEFAULT`，Rev3.x 使用
`MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT`。Rev3.x 构建配置将最低芯片版本设为 3.0，
并启用 250 MHz PSRAM。

GT911 复位引脚为 GPIO 23，INT 未连接。BSP 在创建触摸设备前依次探测 I2C 地址
`0x5D` 和 `0x14`。
该配置与此日期版本镜像保持一致。产品示例使用的更新版 Registry BSP 不指定
GT911 的 INT 和 RST；后续出厂镜像应重新编译并发布，不应静默改写本源码快照。

## 编译

导出 ESP-IDF v5.5.5 环境后，在本目录执行：

```bash
idf.py -B build-lcd-4-3-v5.5.5-rev3_x -D SDKCONFIG="$PWD/build-lcd-4-3-v5.5.5-rev3_x/sdkconfig" -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.rev3_x" build
```

应用二进制命名为 `esp32-p4-lcd-4-3-brookesia.bin`。

## 合并出厂固件

编译成功后生成 16 MiB 合并镜像：

```bash
(cd build-lcd-4-3-v5.5.5-rev3_x && python -m esptool --chip esp32p4 merge_bin -o ../../ESP32-P4-WIFI6-Touch-LCD-4.3-FactoryOnly-260820.bin @flash_args)
```

合并镜像从偏移 `0x0` 烧录。编译通过不能证明显示、触摸、音频、摄像头、Wi-Fi
及其他功能已在实际硬件上验证。
