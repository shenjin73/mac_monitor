# mac_monitor

把 Mac 的系统状态和 CLI 会话状态实时显示到 Waveshare ESP32-S3-Touch-LCD-1.69 开发板上。
灵感与数据规则移植自 macOS 应用 [kimi_monitor](https://github.com/shenjin73/kimi_monitor)。

板子上 7 个屏幕，左右 swipe 切换（首尾循环），底部有点状页码指示：

| 屏 | 内容 |
|---|---|
| SESS | CLI 会话状态大灯（LED 灯罩质感位图）：红灯闪烁=等待用户介入，黄灯=工作中，绿灯=空闲，灰灯=无活跃会话；下方显示会话计数 |
| CPU | 蓝色圆环：使用率 % + 频率（需 sudo） |
| GPU | 紫色圆环：使用率 %（免 sudo，ioreg） + 频率（需 sudo） |
| MEM | 绿色圆环：使用率 % + 已用/总量 GB |
| FAN | 橙色圆环：风扇 RPM（免 sudo，SMC），下方 CPU 温度 / GPU 温度 / 系统功耗（>50W 红色） |
| KIMI | 套餐用量双环：外大环=每周配额，内小环=5 小时窗口；>70% 橙、>90% 红 |
| DEEPSEEK | 大号余额（≤¥5 橙、≤¥1 红）+ 今日 token 用量 |

## 架构

```
mac_stats.py (Mac) ──UDP 广播(udp/45678)──▶ ESP32 WiFi ──▶ parse_line ──▶ stats_t(互斥锁)
                └─USB 串口(可选)──────────▶ stdin 逐字节拼行 ──┘            │
                                        LVGL lv_timer 每 500ms 刷新 UI ◀──┘
```

- 数据线协议：一行空格分隔的 `key=val`（`-1` 表示不可用），UDP 一个数据包一行；USB 串口按行
- 固件 10 秒没收到数据 → 全部显示 `--`
- USB 串口通道保留作为备用。**烧录干扰只存在于串口模式**：`mac_stats.py` 走串口推送时，烧录前必须停掉它，否则会干扰 esptool；默认的 WiFi UDP 模式不经过 USB 线，烧录无需停推送

## 使用

```bash
# 固件（每个新终端先 source 环境）
source ~/esp/esp32s3-env.sh
cd ~/esp/projects/mac_monitor
idf.py build flash -p /dev/cu.usbmodem1101

# Mac 侧推送（WiFi UDP 广播，默认）
python3 host/mac_stats.py
# 或走 USB 串口
python3 host/mac_stats.py /dev/cu.usbmodem1101
```

Mac 和板子需在同一局域网。WiFi 凭据：复制 `main/wifi_config.h.example` 为 `main/wifi_config.h` 并填入自己的 SSID/密码（该文件已 gitignore，明文勿提交），改完重新烧录。
**ESP32-S3 只支持 2.4GHz**，且 SSID 区分大小写。

## 数据来源（host/mac_stats.py，纯 stdlib）

| 数据 | 来源 | 需要权限 |
|---|---|---|
| CPU % | `top -l 2` 第二次采样 | 否 |
| 内存 | `vm_stat`（active+wired+compressor） | 否 |
| GPU % | `ioreg` IOAccelerator `Device Utilization %` | 否 |
| 系统功耗 | `ioreg` AppleSmartBattery `SystemPowerIn` | 否 |
| CPU/GPU 温度、风扇 RPM | AppleSMC（IOKit + ctypes，CPU=`TCMb`，GPU=`Tg*` 组最热键） | 否 |
| CPU/GPU 频率 | `sudo -n powermetrics --samplers cpu_power,gpu_power` | **sudo** |
| 会话状态 | `~/.kimi-code/status`、`~/.claude/status`（150s 判死）；dsh 用 `session.lock` flock 判活 + 投影缓存 | 否 |
| Kimi 配额（60s） | `GET {apiBase}/usages`，复用 `~/.kimi-code/credentials/kimi-code.json`，自动刷新 token 并原子写回 | 否 |
| DeepSeek 余额（300s） | `api.deepseek.com/user/balance`，key 来自 `~/.dsh/.credentials.yaml` | 否 |
| DeepSeek 今日 token（120s） | 外部 `zstd` 解压回放 `~/.dsh/sessions/*/*/session*.jsonl.zstd` 按当天求和 | 需 `brew install zstd` |

sudo 只影响 CPU/GPU 频率。要免密：`sudo visudo` 加 `<用户名> ALL=(ALL) NOPASSWD: /usr/bin/powermetrics`，或直接 `sudo python3 host/mac_stats.py`。

注意：新版 macOS 已移除 powermetrics 的 `smc` sampler，温度/风扇不走 powermetrics。

## 文件结构

```
main/
  main.c           # 板级初始化：ST7789 + CST816 触摸 + LVGL（基于 Waveshare 02 例程）
  ui.c             # 7 屏 tileview、圆环仪表、会话灯、循环滑动、500ms 刷新
  serial_input.c   # stdin 逐字节拼行（VFS 非阻塞，fgets 会碎行）+ UDP 接收 + 解析
  wifi_sta.c       # WiFi station（CN 国家码 1-13 信道，自动重连）
  wifi_config.h    # WiFi 凭据（明文）
  lamp_imgs.c/h    # 会话灯位图（生成物，勿手改）
  lv_conf.h
host/
  mac_stats.py     # Mac 采样推送脚本
tools/
  gen_lamp.py      # 灯位图渲染器（径向渐变 + LED 点阵），预览输出到 tools/preview/
partitions.csv     # 3MB 应用分区（位图资产超出默认 1MB）
sdkconfig.defaults # esp32s3 / 16MB flash / 八线 PSRAM / LVGL 字体与色深
```

## 已知限制

- dsh 的审批弹窗（permission 弹窗）识别为"工作中"而非"等待用户"——kimi_monitor 的 zstd 日志尾部探测未移植；`ask_user_question` 等待走 hook，正常红灯
- 标签为英文：LVGL 自带 Montserrat 字体无中文字形
- 圆环是"暗轨道 + 实心圆头弧"：mac 版的 3D 管状描边靠 SwiftUI blur，MCU 上不可行
- 修改 `sdkconfig.defaults` 后需删除 `sdkconfig` 再编译才生效
