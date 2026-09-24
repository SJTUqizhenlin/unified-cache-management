# SPDK 编译安装文档（本机环境专用）

> 本文档基于实际编译过程整理，针对当前机器环境：openEuler 22.03 LTS-SP4 / aarch64 / 内网（需走企业代理）。

## 一、环境概述

| 项目 | 值 |
| --- | --- |
| OS | openEuler 22.03 LTS-SP4 (aarch64, ARM) |
| SPDK 源码路径 | `/home/qizhenlin/devSPDK/spdk` |
| 包管理器 | `yum` / `dnf` |
| Python venv（SPDK 构建用） | `/var/spdk/dependencies/pip` |
| 网络 | 内网，需走企业代理（见 `$https_proxy`） |
| NVMe 设备 | 2 块华为 ES3000 V6 (19e5:3754)：`nvme0n1`(0000:83:00.0)=系统盘，不可动；`nvme1n1`(0000:84:00.0)=3.5T 裸盘，可经 noiommu vfio 接管 |
| IOMMU/SMMU | 系统未给 NVMe 开 IOMMU（无 iommu_group），vfio 须用 noiommu 模式 |
| Hugepages | 已分配 32GB（16384 × 2MB，每 NUMA node 2048 页 × 8 节点），全部空闲 |

## 二、踩坑总结

### 坑 1：`mk/config.mk` 硬编码了别的机器的路径
从别处拷贝过来的 SPDK 源码树里 `mk/config.mk` 仍带有旧机器的绝对路径（如 `/workspace-genet/spdk/...`），导致 `make` 找不到 `lib/env_dpdk/env.mk`。

**解决**：重新跑 `./configure`，用本机路径重新生成 `mk/config.mk`。验证 `CONFIG_ENV` / `CONFIG_DPDK_DIR` 指向 `/home/qizhenlin/devSPDK/spdk/...`。

### 坑 2：`scripts/pkgdep.sh` 中途失败 + `set -e` 提前退出
`pkgdep.sh` 在 `pkgdep_setup_python_venv` 这一步执行 `pip-compile` 编译 `grpcio-tools` 的 wheel 时失败；脚本头部 `set -e` 使其立即退出，**导致后面 `openeuler.sh` 第 144 行的 `yum install autoconf automake libtool ...` 根本没执行**。

**解决**：不依赖 `pkgdep.sh`，手动 `yum install` 核心编译依赖（见第三节）。`grpcio-tools` 是 SMA/gRPC 绑定用的，对 C 核心构建非必需，可跳过。

### 坑 3（最关键）：内网代理 + `sudo` 默认清空环境变量
本机在内网，shell 里设置了 `https_proxy` 等代理环境变量，`curl` 能用。但 `sudo yum ...`（没加 `-E`）时，sudo 默认 `env_reset` 会清掉这些变量 → yum 直连镜像 → 端口 443 超时下载失败。

**解决**：所有需要联网的 `sudo` 命令一律加 `-E`，即 `sudo -E yum ...` / `sudo -E pip ...`，让代理变量透传。

### 坑 4：缺 `meson` / `ninja`
DPDK 用 meson + ninja 构建，系统未预装，`make` 时报 `meson: command not found`。

**解决**：`sudo -E yum install -y meson ninja-build`。

### 坑 5：Python venv 不完整
`pkgdep.sh` 在 pip 步骤中断，导致 `/var/spdk/dependencies/pip` 这个 venv 只建了壳，缺一堆构建脚本要用的 Python 包：
- `pyelftools`（DPDK meson 检测需要）
- `pyyaml`（`scripts/genrpc.py` 生成 `rpc_autogen.h` 需要）
- `ijson` / `jinja2` / `python-magic` / `tabulate` / `pandas` 等（SPDK 构建脚本依赖）

**解决**：用 venv 的 pip 手动补装（注意带 `-E` 走代理）：
```sh
sudo -E /var/spdk/dependencies/pip/bin/pip install pyelftools pyyaml ijson jinja2 python-magic tabulate pandas
```

### 坑 6：构建脚本用的 python 不是 venv 的 python
`scripts/genrpc.py` 的 shebang 是 `#!/usr/bin/env python3`，会取 PATH 里第一个 `python3`。直接 `make` 时解析到系统 `/usr/bin/python3`（没装 yaml）→ 报 `No module named 'yaml'`。
> 注：我已生成 `/etc/opt/spdk-pkgdep/paths/export.sh`，但顶层 `make` 不会自动 source 它（只有 `scripts/` 下的脚本会），所以**手动激活 venv 才稳**。

**解决**：每次 `make` 前先激活 venv：
```sh
source /var/spdk/dependencies/pip/bin/activate && make
```

### 坑 7：`dpdk/build-tmp` 半成品残留导致 meson 报错
meson 配置中途失败后，`dpdk/build-tmp` 是不完整的 build 目录，重跑会报 `Current directory is not a meson build directory`。

**解决**：`rm -rf dpdk/build-tmp` 后再 `make`，让 meson 重新 setup。

### 坑 8：setup.sh 的"占用"判定会保护系统盘，也会误跳过有空 GPT 的可用盘
`setup.sh config` 扫描所有 SPDK 支持的 PCI 设备，用 `block_in_use`（`scripts/common.sh:380`）判定占用：有 holder（LVM/dm）/ 挂载 / 分区表或文件系统签名 → 判占用、跳过保护。
- `nvme0n1`(0000:83:00.0) 是系统盘（根/分区/dm/挂载）→ 被 `mount@`/`holder@` 跳过 —— 正确保护，**绝不能动**。
- `nvme1n1`(0000:84:00.0) 是 3.5T 裸盘，无分区、无文件系统、未挂载，但上面有个**空 GPT 分区表签名**，仍被 `block_in_use` 判成 `data@nvme1n1` 跳过。

**怎么发现的**：`setup.sh status` 会打印每个设备被跳过的原因，例如：
```
0000:84:00.0 (19e5 3754): Active devices: data@nvme1n1, so not binding PCI dev
```
看到 `data@` 就说明盘上有"数据签名"（这里是空 GPT），用 `sudo blkid /dev/nvme1n1` / `sudo wipefs -n /dev/nvme1n1` / `lsblk -f /dev/nvme1n1` 可确认到底有没有真实数据。

**解决**：确认盘上无真实数据后，抹掉签名让 setup.sh 不再保护：
```sh
sudo wipefs -n /dev/nvme1n1      # 先查签名（只读，不改盘）
sudo wipefs -a /dev/nvme1n1     # 抹掉所有签名（本例抹掉空 GPT）
```
> 另：`setup.sh config` 默认 `HUGEMEM=2048` 会把大页设成 2GB。本机已有 32GB，要保留就用 `HUGEMEM=32768`。

### 坑 9：本机 aarch64 未给 NVMe 开 IOMMU/SMMU，vfio-pci 正常绑不上
SPDK 用户态接管 PCI 走 `vfio-pci`（首选）或 `uio_pci_generic`（回退）。`vfio-pci` 正常模式要求设备在 `iommu_group` 里，但本机：
- `/sys/class/iommu/` 空、`/sys/kernel/iommu_groups/` 空；
- 两块 NVMe 都没有 `/sys/bus/pci/devices/<bdf>/iommu_group`；
- 内核 cmdline 只有 `smmu.bypassdev=...`（针对别的设备，不是 NVMe）。

直接 `setup.sh config` 时，`is_iommu_enabled`（`scripts/common.sh:125`）为假 → setup.sh 回退到 `uio_pci_generic`。要走 vfio，须开 noiommu 模式：
```sh
echo Y | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
```
开了之后 `is_iommu_enabled` 判为真 → setup.sh 自动选 `vfio-pci`，绑定时创建 noiommu 组（`/dev/vfio/noiommu-0`）。
> noiommu 是 **unsafe 模式**（无 DMA 隔离），单机测试够用，**不要**在多租户/生产环境用。
> 若要走最正规的路线，可改内核 cmdline 让 SMMU 翻译这块 NVMe 后重启，但测试场景没必要。

### 坑 10：noiommu 模式与设备绑定都是易失的
`enable_unsafe_noiommu_mode` 和把 NVMe 绑到 vfio-pci 都是运行时状态，**重启或 `rmmod vfio*` 后失效**。需重做：重新设 `Y` + 再跑一次 `setup.sh config`。

## 三、完整编译步骤（本机可用）

### 1. 安装系统编译依赖（手动，绕开 pkgdep.sh 的中断）
```sh
cd /home/qizhenlin/devSPDK/spdk

sudo -E yum install -y \
  gcc gcc-c++ make \
  CUnit-devel libaio-devel openssl-devel libuuid-devel ncurses-devel \
  json-c-devel libcmocka-devel clang clang-devel python3-pip unzip \
  keyutils keyutils-libs-devel fuse3-devel patchelf pkgconfig \
  libiscsi-devel \
  autoconf automake libtool help2man numactl-devel nasm systemtap-sdt-devel \
  python3-devel \
  meson ninja-build
```
> 一定要带 `-E`，否则代理被 sudo 清掉，yum 下载会超时。

### 2. 补全 SPDK 构建 venv 的 Python 依赖
```sh
sudo -E /var/spdk/dependencies/pip/bin/pip install \
  pyelftools pyyaml ijson jinja2 python-magic tabulate pandas
```
> 若 venv 不存在（全新机器），先跑一次 `sudo -E ./scripts/pkgdep.sh` 让它建好 venv（即便最后 pip 步骤失败也无妨，venv 壳已建好），再执行本步补包。

### 3. 重新生成配置
```sh
./configure
```
确认输出含 `Using default SPDK env in /home/qizhenlin/devSPDK/spdk/lib/env_dpdk` 等本机路径。

### 4. 编译（务必先激活 venv）
```sh
source /var/spdk/dependencies/pip/bin/activate && make
```
若中途改了配置或 meson 失败，先 `rm -rf dpdk/build-tmp` 再重编。

## 四、验证

```sh
# 产物清单
ls -l build/bin/

# 二进制可执行性
file build/bin/spdk_tgt
build/bin/spdk_lspci -h
```

预期：`build/bin/` 下有 `spdk_tgt`、`nvmf_tgt`、`iscsi_tgt`、`vhost`、`spdk_nvme_perf`、`spdk_top` 等可执行文件，`spdk_lspci -h` 能打印 usage。

## 五、接管 NVMe 设备用于测试（noiommu vfio 流程）

针对本机：保留 32GB 大页、系统盘 `nvme0`(83:00.0) 自动跳过、只接管 `nvme1`(84:00.0)。

### 1. 抹掉 nvme1 上的空 GPT 签名（仅首次，或签名被重建时）
```sh
sudo wipefs -n /dev/nvme1n1      # 先查签名（只读）
sudo wipefs -a /dev/nvme1n1      # 抹掉空 GPT
```

### 2. 开启 vfio noiommu 模式（每次重启后都要）
```sh
echo Y | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
```

### 3. 接管设备（保留 32GB 大页）
```sh
sudo -E HUGEMEM=32768 scripts/setup.sh config
```
预期输出（核心几行）：
```
0000:83:00.0 (19e5 3754): Active devices: holder@.../mount@..., so not binding PCI dev
0000:84:00.0 (19e5 3754): nvme -> vfio-pci
INFO: Requested 16384 hugepages but 16384 already allocated
```
> 第一行：系统盘被保护跳过；第二行：目标盘从内核 `nvme` 解绑、绑到 `vfio-pci`；第三行：大页保持 32GB。

### 4. 验证接管 & SPDK 能识别
```sh
# 设备状态：84:00.0 的 Driver 应为 vfio-pci
scripts/setup.sh status | tail -3

# noiommu 字符设备应存在
ls -l /dev/vfio/noiommu-0

# SPDK 下发 admin 命令，打印 Identify Controller（型号/序列号/FW 等）
sudo -E build/bin/spdk_nvme_identify -r 'trtype:PCIe traddr:0000:84:00.0'
```

### 5. 跑 I/O 性能测试样例
```sh
# 4K 随机读，队列深度 128，跑 10 秒
sudo -E build/bin/spdk_nvme_perf -r 'trtype:PCIe traddr:0000:84:00.0' -q 128 -o 4096 -t 10 -w randread

# 4K 随机写
sudo -E build/bin/spdk_nvme_perf -r 'trtype:PCIe traddr:0000:84:00.0' -q 128 -o 4096 -t 10 -w randwrite

# 1M 顺序读（看带宽）
sudo -E build/bin/spdk_nvme_perf -r 'trtype:PCIe traddr:0000:84:00.0' -q 1 -o 1048576 -t 10 -w read
```
> `-w` 支持：`read` / `write` / `randread` / `randwrite` / `rw` / `randrw` 等。`-q`=队列深度，`-o`=块大小(字节)，`-t`=时长(秒)。完整参数 `build/bin/spdk_nvme_perf -h`。
> **所有 SPDK 运行程序都要 `sudo -E`**：需要 hugepages + `/dev/vfio/noiommu-0` 访问权限。

### 6. 还回内核（测完恢复）
```sh
sudo -E scripts/setup.sh reset
```
系统盘 `nvme0` 始终未被 SPDK 接管，`reset` 只把 `nvme1` 绑回内核 `nvme` 驱动。

## 六、备注

- **每次重编都要先激活 venv**：`source /var/spdk/dependencies/pip/bin/activate && make`。否则 `genrpc.py` 等脚本会用到无依赖的系统 python 而报错。
- **所有联网的 sudo 命令带 `-E`**：代理变量靠它透传，否则 yum/pip 直连超时。
- **所有 SPDK 运行程序也带 `sudo -E`**：需要 hugepages + `/dev/vfio/*` 访问权限，普通用户跑不起来。
- **noiommu 接管是易失的**：重启或 `rmmod vfio*` 后，需重新 `echo Y > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode` + 再跑一次 `setup.sh config`。`wipefs` 抹掉的 GPT 签名则是持久的，不用重做。
- **系统盘 nvme0(83:00.0) 永远不要动**：`setup.sh` 会自动跳过在用设备保护它，但别用 `DRIVER_OVERRIDE` 之类强制绑定它，否则可能丢系统。
- **可选组件**（如 RDMA、文档、io_uring 等）可用 `./configure --with-rdma` 等开启，或补装对应 `pkgdep.sh --help` 列出的依赖；不在本机最小构建范围内。
- **SMA/gRPC Python 绑定**：`grpcio-tools` 在本机编译 wheel 失败（Python 3.9 / 构建工具链限制）。若需要 SMA 功能，可尝试用 Python 3.11+ 或预编译 wheel，本最小编译未包含。
