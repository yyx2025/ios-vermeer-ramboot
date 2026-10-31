# Vermeer 完整根卷 RAM 临时启动

完整官方 iOS 16.7.16（20H392）根卷已经制作、核对、检查文件系统并压缩完成。新 RAM 接入代码已完成本地编译和协议测试；完整桌面仍待实体 Redmi K70（12 GB）验证。

## 已验证的阶段

用户已报告 9 GiB 范围抽样只读 `SPARSE READ PASS`，以及较低 8 GiB 抽样写入、别名检查、恢复 `SPARSE WRITE ALIAS PASS`、`RESTORED=1`。旧 checkpoint 照片确认 `BSD root: md0` 和源码 PID 1 持续运行，但它没有 Apple 用户空间。

USB 当前被映射身份检查拒绝。新内核会打印具体 SMMU 值，检查继续保留。`vermeer-usb-map.img` 未实机测试；用户要求后续集中测试。

## 完整根卷和预算

| 项目 | 实际大小 |
|---|---:|
| 完整官方文件，按源 inode 去重 | 11,304,857,126 字节（10.53 GiB） |
| 完整 HFSX 逻辑卷 | 13 GiB |
| 压缩后驻留 RAM 的数据 | 5,884,403,712 字节（5.48 GiB） |
| 有上限的临时写入空间 | 512 MiB |
| XNU 物理分配器 | 2 GiB |
| 压缩卷及写入覆盖层的外部区域 | 6 GiB |

144097 个唯一文件哈希、321210 个条目及链接、所有者、权限完成核对，HFSX 检查通过，已经从电脑构建环境卸载。Linux HFS 的 symlink mode 固定显示 0777，该字段明确排除；目标及所有者已核对。没有应用 activation/Setup 跳过修改。

HFSX SHA256：`11592101bbfe8dd17f6c1e5edd5ffbde7de9abaac2dd98ca4d83dffb5c72d21a`。

压缩卷 SHA256：`e610f21c8816169ae2e1e5d3190bb0d6913902e47116162da165530e87890f83`。

压缩卷不是 boot 镜像。电脑先临时启动小镜像，再通过 USB 分段装入压缩卷。`md0` 逻辑容量仍为 13 GiB；读取按需解压，修改进入 16 KiB 粒度的 RAM 覆盖层，满时返回空间不足。

XNU 保持 `0x8C0000000..0x940000000`（2 GiB）。根卷及覆盖层使用 `0x940000000..0xAC0000000`（6 GiB），上方 1 GiB 留空。shim 在复制前检查 live FDT 的较低 8 GiB，外部区域以普通可缓存 RAM 属性映射。抽样通过不代表逐字节验证或完整运行时占用已通过。

## 新代码和验证

RAM GUI profile 为 73 个补丁，保留作者 GUI、触摸、AKS、watchdog 和 RAM/USB 日志，排除 UFS/GPT/持久日志代码。在挂根前接收完整压缩卷，核对完整 SHA256、索引 SHA256、块 CRC32 和 journaled HFS header，再注册压缩 `md0`。大小和偏移使用 64 位处理。

测试覆盖压缩读取、跨页临时写入、容量用尽、数据损坏、超过 4 GiB 的偏移，以及 Python 帧到实际 C 接收器的分片、错误确认和截断。八个新/改 C 模块使用上次成功构建的真实 XNU 头文件和配置在本机编译通过；完整链接构建的状态独立记录。

`tools/build_ram_boot.py --ram-stream` 的根卷位置参数使用 packed-root JSON；kernel 和官方 product ADT 为本地输入。它只嵌入 kernel+ADT，不嵌入根卷或 RAMDisk 占位物。实际新内核必须通过 `--gui --ram-only --ram-stream` 标记检查，旧内核不可用于此模式。

`tools/send_ram_root.py` 在电脑核对完整压缩卷 SHA256，等待准确的 K70DBG004 USB 身份，逐段发送并核对 ACK。错误即停止；最终哈希符合预期才提交。完成后保存启动日志，并在 120/300 秒发出只读进程、显示帧和输入查询。

## 构建与集中测试

私有源码仓库：<https://github.com/yyx2025/ios-vermeer-ramboot>。上游：<https://github.com/xyyyds0106/ios-vermeer-lab>，基线 `d1dcb06ff1d8fa57acc54051ae87d31c97e771db`。

旧版已实机启动内核保留为 `outputs/vermeer-ram-gui-kernel`。新内核另存 `outputs/vermeer-ram-stream-kernel`，新小镜像目标为 `outputs/vermeer-complete-ram-stream.img`；是否已产生及校验通过，以相邻 JSON 记录为准。

macOS 14 构建服务进入停机/退役期，改用 macOS 15，复用经哈希核对的 17 组旧版 IIG 源码输出。RAM profile 不改变这些接口定义；没有分发旧编译器二进制。原缓存产物于 2026-11-07 过期，长期重建需保存已审核的生成源码。

`outputs/Start-Vermeer-RAM.ps1` 默认只检查文件。镜像完成后，用户进入 fastboot 并执行 `-Boot`，可在一轮中完成临时启动、RAM 接收和日志收集。没有完整镜像及匹配记录时入口会停止。

本次没有操作手机分区，也没有上传固件、根卷、设备 FDT 或凭据。USB、完整根卷挂载、Apple launchd/backboardd/SpringBoard 和触摸仍待集中实机验证。
