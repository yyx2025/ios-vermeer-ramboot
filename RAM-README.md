# Vermeer RAM 临时启动移植

来源：<https://github.com/xyyyds0106/ios-vermeer-lab>，源码版本 `d1dcb06ff1d8fa57acc54051ae87d31c97e771db`。
原始源码和说明保留；`UPSTREAM.json` 记录原文件哈希。原项目说明中的 UFS 根卷和存储授权不适用于本项目。

## 已实现的本地改动

- `tools/prepare_ram_gui.py` 复用作者 GUI/显示缓冲生命周期/触摸/USB 调试补丁，生成 71 个 RAM GUI 内核补丁。
- 不加入 Sweet UFS、Vermeer UFS、GPT/分区读写、持久 console 驱动。根卷使用 XNU 的 `md0`，根卷内文件修改只发生在 RAM。
- 新根卷准备检查要求 `rd=md0 vermeer_ram_only=1`、真实已初始化内存盘和已挂载的 HFS 根文件系统。保留原作者的 20H392 迁移版本检查。
- 原作者的 RAM 日志环和可选 CDC ACM USB 调试保留。
- 本地 GUI 及 RAM 配置检查通过。`vermeer-ios-ram-gui` 已在 macOS 完整编译成功，并通过最终内核 RAM-only 服务/禁止标记检查。首次构建发现根卷 hook 的一处 tab 上下文错误，修正后其余补丁在真实 XNU 源码上全部应用通过。
- Windows 可交叉编译 Android v4 独立启动 shim。它使用嵌入的 XNU payload，避免依赖 ABL 接收 boot ramdisk；不经过 EDK2/DarwinBoot 菜单。

## 临时启动验证镜像

工作目录 `outputs/vermeer-lab-ram-checkpoint.img`，15,941,632 字节。
SHA-256：`e5c4f8ebd9f62f5ced9e6f73d6c4822ddb1d4f21337cfa113dc0e50eb36555fe`。

内核取自作者 Actions `39428900143` 的 `xnu-kernel` 产物；内核与 PID1 均通过作者清单验证。
内核哈希 `99238192beebadd3bdf72c277d139dcba605967d2cb4cf35c846f31985ad7d7f`；PID1 哈希 `ec8b463e1aac6665ca49d97f13feace9b4f33c85f7298b59ce7f3224af078617`。

这是源代码 PID1 验证程序，**不包含 Apple launchd/SpringBoard，也不是完整 iOS GUI**。本机已编译打包，尚未在用户 K70 上验证。其 RAM 窗口为 `0x8C0000000–0x940000000`（2 GiB）；复制前要求实机 FDT 范围和保留区检查通过。

只执行 `fastboot boot`，不执行 flash、erase、set_active 或 GPT/分区修改。出现 XNU 日志后可能需要等待 120–150 秒；目标输出是 `BSD root: md0`、PID1 和持续 heartbeat。

## 官方固件

本地未发现作者 root002 或配套 20H392 IPSW，因此已按用户要求下载官方 iPhone10,3/iPhone10,6 iOS 16.7.16（20H392）IPSW。完整文件位于 `D:\ios hack\work\ipsw-cache\20H392`，6,171,035,648 字节，已通过官方 SHA-256：`ece1194f7e233e9109c5790a8d23a4564a10a254e15ec5b09978a343265568fb`。三个根卷容器已从本地 ZIP 提取并校验 CRC，保存到工作目录 `work/firmware-20H392`。

已从在线 ZIP 读取并校验各小成员 CRC，解析 BuildManifest 并解压 D22AP 的 LZFSE DeviceTree。通用 product ADT 已提取到工作目录 `work/firmware-20H392/d22ap-product.adt`；没有复制设备私有校准或 syscfg 占位数据。

系统组成：OS `087-86698-044.dmg`（4,730,814,044 字节），SystemOS `087-86593-047.dmg`（2,743,074,816 字节），AppOS `087-86715-047.dmg`（10,485,760 字节）。这些是镜像容器大小，不是展开后的 HFS 文件占用。

## 仍需完成

1. 制作并验证完整 journaled HFSX 根卷：需要 APFS 解压并保留 POSIX 所有者、模式和硬链接；现有 17.7.11 文件不与 16.7.16 内核混用。电脑上使用独立、无窗口的 Linux 文件系统构建环境，仅接入新建本地构建盘及只读固件文件，不接入任何物理磁盘；手机运行目标仍是实体 K70。
3. 当前 2 GiB RAM 窗口容不下完整根卷。小 shim + USB 分段传输、64 位总长度/偏移、最终 RAM 原位接收和更大内存映射尚未实现，不能把大镜像直接交给 `fastboot boot`。
4. 在这台 12 GB K70 上验证真实高位 RAM、完整卷的内存占用、XNU/launchd/backboardd/SpringBoard 与触摸日志。作者记录的 Hello milestone 不等于本次 RAM 移植已成功。

本项目没有执行手机刷写，也没有将固件或根卷上传到 GitHub。

## 成功构建和下一次实机诊断

用户授权创建的私有仓库：<https://github.com/yyx2025/ios-vermeer-ramboot>。
成功运行：<https://github.com/yyx2025/ios-vermeer-ramboot/actions/runs/41186716430>。
内核源码提交 `cbebe4b5aa8577777dbc9397af65dc5e762222ec`；下载后与构建 SHA256SUMS 再次核对通过。
最终内核 17,167,928 字节，SHA-256 `41e9004a7202b0718561c7a56c3e25c804b0b1b0bd634428ef9b561d3e34c8c9`，工作目录输出 `outputs/vermeer-ram-gui-kernel`。它只是一份内核，还不是可启动完整 iOS 的镜像。

`outputs/vermeer-ram-map.img` 是独立内存布局诊断：15,941,632 字节，SHA-256 `8b788c9a76cd04ce8ef640909816966eef90490d54ab708caefddc097fcf0cff`。
它在内存中临时运行 shim，只读取 ABL 的有界 FDT、显示真实 BANK/BASE/SIZE 和固定保留区检查，不跳转 XNU，90 秒后请求复位。`CHECK4G/6G/8G/10G=0` 只表示静态 FDT 检查通过，不能代替实机 RAM 访问和动态保留区验证。
内存解析测试覆盖银行列表、保留区冲突、地址溢出和每一种截断长度；本地编译通过。
现有 `vermeer-lab-ram-checkpoint.img` 保留原哈希，可另行验证 XNU/md0/PID1。两份镜像都不是完整 iOS GUI。

### KSU 授权后的只读采集

2026-10-31，用户为 KSU Shell（UID 2000）开启 root 后，已通过 ADB 只读导出实机 Android FDT。它显示 `0x8C0000000..0x980000000`（3 GiB）与 `0x980000000..0xB00000000`（6 GiB）相邻，固定保留区未与该 9 GiB 段重叠。动态保留池和实机大范围访问仍需另外验证。
第一版 guard 对 4/6/8 GiB 返回 `0x12`，是由于要求单个 reg 条目完整覆盖。新 guard 接受相邻/重叠条目的连续覆盖，仍拒绝 1 字节空洞、溢出、固定保留区冲突和畸形 FDT。更新后的单元测试和实际 Android FDT 检查通过（4/6/8 为 0，10 为 18）。
独立 `outputs/vermeer-ram-map-v2.img` 只读取 live FDT，不跳转 XNU或访问大候选窗口；原 checkpoint 和实际 XNU 的 2 GiB 映射不变。Android FDT 为 8 条，而此前 ABL 屏幕为 9 条，不能省略启动时的 live FDT 检查。
`tools/collect_k70_memory.py --root --output OUTPUT_DIR` 仅使用用户已授予的 Shell root 来读取，普通模式仍无需 root。源码不包含任何实机 FDT/固件数据。
