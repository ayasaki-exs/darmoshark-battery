# darmoshark-battery

一个独立、最小的 Linux 内核模块（DKMS），把 **Darmoshark（MOJI）** 鼠标电量
注册成标准 `power_supply` 设备。

Logitech 鼠标能在桌面电源部件里显示电量，是因为内核 `hid-logitech-hidpp`
驱动创建了 `hidpp_battery_N`，UPower 再通过 Solid 交给 KDE。Darmoshark 的
接收器没有这样的内核驱动，而 UPower 又不提供用户态注册设备的 API。本模块
在配置集合（usage page `0xff0a`）上发送厂商的 64 字节电量查询，并注册一个
外设 `power_supply`，于是电量会出现在同样的位置：

```text
鼠标 -> 厂商 HID 通道 -> darmoshark_battery -> power_supply -> UPower -> Solid -> KDE 电源和电池
```

## 支持的硬件

| USB ID | 设备 |
|---|---|
| `1915:073c` | Darmoshark M3S Pro（有线） |
| `1915:0723` | Darmoshark M3S Pro（2.4 GHz，1K NRF 接收器） |

## 依赖

- Linux，开启 `CONFIG_HIDRAW`（模块会保留配置接口的 hidraw 节点，供其他工具使用）。
- 当前内核的 headers 和 DKMS。
- Arch：`sudo pacman -S --needed linux-headers dkms bc pahole`。

模块许可证为 GPL-2.0-or-later。

## 安装（DKMS）

在本仓库目录（目录名就是 DKMS 包名 `darmoshark-battery`）执行：

```sh
make test          # 协议向量，仅主机
make               # 编译模块（需要 linux-headers）

sudo dkms add .
sudo dkms build darmoshark-battery/0.1.0
sudo dkms install darmoshark-battery/0.1.0
sudo modprobe darmoshark_battery
```

开机自动加载：

```sh
printf 'darmoshark_battery\n' | sudo tee /etc/modules-load.d/darmoshark-battery.conf
```

模块会在不重启的情况下从 `hid-generic` 接管接收器的配置接口，只重绑该接口；
同一 VID/PID 的鼠标、键盘、消费者接口保持可用。

### Arch Linux（AUR）

`packaging/aur/` 提供 `darmoshark-battery-dkms-git` 包：模块源码装到
`/usr/src/darmoshark-battery-<tag>/`，由 Arch `dkms` 的 pacman hook 为每个已装
内核自动构建，不需要 `.install` 脚本。包源为
<https://github.com/ayasaki-exs/darmoshark-battery>；构建与提交说明见
[packaging/aur/README.md](packaging/aur/README.md)。

## 验证

```sh
journalctl -k | tail -30
cat /sys/class/power_supply/darmoshark_battery_0/{capacity,status,scope,model_name}
upower -e | grep darmoshark
upower -d
```

`upower -d` 应出现 `model` 为 `Darmoshark M3S Pro ...`、`percentage` 与鼠标
一致的设备；KDE 托盘「电源和电池」部件会列出它。

目前只映射百分比：协议的充电状态字节尚未解码，所以 `status` 保持 `unknown`。

## 协议诊断

不装模块，直接从设备读取：

```sh
make probe
sudo ./build/darmoshark-probe
```

它会找到配置 hidraw 节点（`0xff0a` usage page），发送电量查询，打印百分比和
`dms`/`dms_v2` 布局。需要对节点有写权限（root，或给 `1915:0723`/`1915:073c`
加 hidraw udev 规则）。

## 卸载

```sh
sudo modprobe -r darmoshark_battery
sudo rm /etc/modules-load.d/darmoshark-battery.conf
sudo dkms remove darmoshark-battery/0.1.0 --all
```

## 限制

- 仅 Linux；这是通往 UPower 的桥，不是跨平台驱动。
- 只有电量百分比；充电状态未知。
- 模块绑定接收器的配置接口，并对同一 VID/PID 的其他接口按 `hid-generic`
  的方式处理（输入不受影响）。
- 厂商通道与其他工具共享（例如 OpenLogi 的 Darmoshark DPI 支持）。轮询为
  每分钟一次并过滤响应；偶发的交错由双方既有重试处理。
- DKMS 会随内核更新重建；安全启动环境需要像其他树外驱动一样签名。

## 协议参考

帧布局、checksum 以及 `dms`/`dms_v2` 响应偏移在 OpenLogi 的 Darmoshark 分支
中逆向并验证：

- `crates/openlogi-device/src/darmoshark.rs`
- `docs/DARMOSHARK.md`
- <https://github.com/ayasaki-exs/OpenLogi/tree/feat/darmoshark-driver>

`darmoshark_protocol.h` 是本仓库唯一的 C 实现；`make test` 锁定与 Rust 测试
相同的线上事实。协议变化时两边都要更新。

## 许可证

GPL-2.0-or-later，见 [LICENSE](LICENSE)。
