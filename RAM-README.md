# Vermeer RAM 临时启动移植

来源：<https://github.com/xyyyds0106/ios-vermeer-lab>，源码版本 `d1dcb06ff1d8fa57acc54051ae87d31c97e771db`。
原始源码和说明保留；`UPSTREAM.json` 记录原文件哈希。原项目说明中的 UFS 根卷和存储授权不适用于本项目。

## 已实现的本地改动

- `tools/prepare_ram_gui.py` 复用作者 GUI/显示缓冲生命周期/触摸/USB 调试补丁，生成 71 个 RAM GUI 内核补丁。
- 不加入 Sweet UFS、Vermeer UFS、GPT/分区读写、持久 console 驱动。根卷使用 XNU 的 `md0`，根卷内文件修改只发生在 RAM。
- 新根卷准备检查要求 `rd=md0 vermeer_ram_only=1`、真实已初始化内存盘和已挂载的 HFS 根文件系统。保留原作者的 20H392 迁移版本检查。
- 原作者的 RAM 日志环和可选 CDC ACM USB 调试保留。
- 本地 GUI 及 RAM 配置检查通过。新增 GitHub 构建选项 `vermeer-ios-ram-gui`；完整 XNU 尚未编译，补丁尚未在完整 XNU 源码树中全部应用验证。
- Windows 可交叉编译 Android v4 独立启动 shim。它使用嵌入的 XNU payload，避免依赖 ABL 接收 boot ramdisk；不经过 EDK2/DarwinBoot 菜单。

## 临时启动验证镜像

工作目录 `outputs/vermeer-lab-ram-checkpoint.img`，15,941,632 字节。
SHA-256：`e5c4f8ebd9f62f5ced9e6f73d6c4822ddb1d4f21337cfa113dc0e50eb36555fe`。

内核取自作者 Actions `39428900143` 的 `xnu-kernel` 产物；内核与 PID1 均通过作者清单验证。
内核哈希 `99238192beebadd3bdf72c277d139dcba605967d2cb4cf35c846f31985ad7d7f`；PID1 哈希 `ec8b463e1aac6665ca49d97f13feace9b4f33c85f7298b59ce7f3224af078617`。

这是源代码 PID1 验证程序，**不包含 Apple launchd/SpringBoard，也不是完整 iOS GUI**。本机已编译打包，尚未在用户 K70 上验证。其 RAM 窗口为 `0x8C0000000–0x940000000`（2 GiB）；复制前要求实机 FDT 范围和保留区检查通过。

只执行 `fastboot boot`，不执行 flash、erase、set_active 或 GPT/分区修改。出现 XNU 日志后可能需要等待 120–150 秒；目标输出是 `BSD root: md0`、PID1 和持续 heartbeat。

## 官方固件

本地未发现作者 root002 或配套 20H392 IPSW，因此已按用户要求下载官方 iPhone10,3/iPhone10,6 iOS 16.7.16（20H392）IPSW。完整文件下载位置为 `D:\ios hack\work\ipsw-cache\20H392`，下载完成后校验官方 SHA-256。

已从在线 ZIP 读取并校验各小成员 CRC，解析 BuildManifest 并解压 D22AP 的 LZFSE DeviceTree。通用 product ADT 已提取到工作目录 `work/firmware-20H392/d22ap-product.adt`；没有复制设备私有校准或 syscfg 占位数据。

系统组成：OS `087-86698-044.dmg`（4,730,814,044 字节），SystemOS `087-86593-047.dmg`（2,743,074,816 字节），AppOS `087-86715-047.dmg`（10,485,760 字节）。这些是镜像容器大小，不是展开后的 HFS 文件占用。

## 仍需完成

1. 在原工作流要求的 macOS/Xcode 环境编译 RAM GUI 内核，验证所有补丁和最终二进制没有物理存储路径。
2. 校验并解包完整官方 IPSW，使用支持 APFS 解压并保留 POSIX 所有者、模式和硬链接的环境制作 journaled HFSX 根卷；现有 17.7.11 文件不与 16.7.16 内核混用。
3. 当前 2 GiB RAM 窗口容不下完整根卷。小 shim + USB 分段传输、64 位总长度/偏移、最终 RAM 原位接收和更大内存映射尚未实现，不能把大镜像直接交给 `fastboot boot`。
4. 在这台 12 GB K70 上验证真实高位 RAM、完整卷的内存占用、XNU/launchd/backboardd/SpringBoard 与触摸日志。作者记录的 Hello milestone 不等于本次 RAM 移植已成功。

本项目没有执行手机刷写，也没有将固件或根卷上传到 GitHub。
