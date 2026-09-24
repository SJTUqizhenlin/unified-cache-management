# SPDK 开发指南：RDMA 重编、本地代码链接与测试样例

> 配套文档：`SPDK_BUILD_INSTALL.md`（首次构建、内网代理、noiommu vfio 等基础问题在彼处，本文不重复）。
> 环境回顾：openEuler 22.03 (aarch64) / SPDK 源码 `/home/qizhenlin/devSPDK/spdk` /
> 2 × 华为 ES3000 V6（83:00.0=系统盘，84:00.0=SPDK 裸盘，vfio-pci 接管）/ 4 × mlx5 RoCE 口。

---

## 一、RDMA 重编流程（`--with-rdma`）

首次构建时未开 RDMA（`CONFIG_RDMA=n`），导致 `libspdk_nvme.a` 不含
`nvme_rdma` 传输、`nvmf_tgt` 不链 ibverbs。重编步骤：

```bash
cd /home/qizhenlin/devSPDK/spdk

# 1. 确认构建依赖（见第二节坑 1）
python3 -c "import yaml, jinja2, tabulate" || pip3 install --user pyyaml jinja2 tabulate

# 2. 重新 configure（原构建为无参数默认，无需保留历史选项）
./configure --with-rdma

# 3. 验证配置生效
grep 'CONFIG_RDMA?=' mk/config.mk          # 应为 CONFIG_RDMA?=y

# 4. 全量重编（256 核约几分钟）
make -j$(nproc)

# 5. 验证产物
grep SPDK_CONFIG_RDMA build/include/spdk/config.h        # #define SPDK_CONFIG_RDMA 1
nm build/lib/libspdk_nvme.a | grep -c nvme_rdma          # >0
ldd build/bin/nvmf_tgt | grep -E 'ibverbs|rdmacm'        # 两个 so
```

**关键性质：现有测试代码零改动即可获得 RDMA**。传输层实现（`nvme_rdma.c`）
在 `libspdk_nvme.a` 里，重链即得；`-r` 参数本来就透传完整 trid；
`-libverbs -lrdmacm` 由 SPDK 的 mk 体系按 `CONFIG_RDMA` 自动注入 `SYS_LIBS`，
自有 Makefile 不需要改一行。

---

## 二、问题与解决方案汇总

### 构建期

**坑 1：python 缺 `pyyaml / jinja2 / tabulate`**

`make` 在生成 `include/spdk_internal/rpc_autogen.h` 时失败，报错信息
具有误导性（只说 "Run scripts/pkgdep.sh to install dependencies"，不指明
缺哪个包）。实际 `scripts/genrpc.py` 依次 import `yaml`、`jinja2`、`tabulate`，
缺一不可。

```bash
pip3 install --user pyyaml jinja2 tabulate
# 验证：直接跑生成器，报错会指明真实缺失模块
./scripts/genrpc.py --schema schema/schema.yaml --rpcs > /dev/null && echo OK
```

**坑 2：`mk/config.mk` 是 configure 的生成物**

`./configure` 会用 `CONFIG` 模板整体重写 `mk/config.mk`。从别处拷来的源码树
带着旧机器路径时（见旧文档坑 1），重跑 configure 即可修复；反之，手工改过
`mk/config.mk` 的话 configure 会冲掉。

### 运行期（SPDK 库使用）

**坑 3：qpair 请求池耗尽 → 提交静默失败 → 死循环**

`spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0)` 的默认请求池只有
512 个 `nvme_request` 对象、SQ 深度 256。两个放大因素会耗尽它：
(a) IO 尺寸超过 MDTS（本盘 128KB）时每个命令拆成父请求+N 个子请求；
(b) 高 QD。实测 144KB IO + QD 512 需要 512×3=1536 个对象 → 第 ~170 个
在途后 `spdk_nvme_ns_cmd_write` 返回负值，若不检查返回值，标记 in_flight
的槽位永不完成，程序死循环。

**解决**：按 QD × 拆分数显式扩容，并检查每次提交的返回值：

```c
struct spdk_nvme_io_qpair_opts qopts;
uint32_t max_xfer = spdk_nvme_ns_get_max_io_xfer_size(g_ns);
uint32_t split = (g_block_size + max_xfer - 1) / max_xfer;

spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &qopts, sizeof(qopts));
qopts.io_queue_size = g_qd * split + 1;
qopts.io_queue_requests = g_qd * (split + 1) + 256;
g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, &qopts, sizeof(qopts));
/* 每次 spdk_nvme_ns_cmd_read/write 的 rc < 0 必须处理 */
```

**坑 4：nvmf RDMA target 默认 `max_queue_depth=128`**

initiator 请求更大的 IO 队列时建连被拒，dmesg/journal 里是
`Invalid SQSIZE 128 (min 1, max 127)`，客户端侧表现为
`nvme_transport_ctrlr_connect_io_qpair() failed`。

```bash
rpc.py nvmf_create_transport -t RDMA -u 4096 --max-queue-depth 1024
```

**坑 5：tmpfs 上没有 O_DIRECT**

`/tmp` 若为 tmpfs，`open(O_DIRECT)` 返回 EINVAL。测试目录要放真实文件系统。

**坑 6：bash 会话超时连带杀后台进程**

在脚本/工具会话里 `nvmf_tgt ... &` 起的后台进程会随会话超时被清理。
**解决**：`sudo systemd-run --unit=nvmf-loop /path/nvmf_tgt -m 0xF0 -s 8192`，
由 systemd 托管，`systemctl stop nvmf-loop` 随停。

**坑 7：nvmf target 因 keep-alive 超时断连 → 程序无报错死等**

SPDK initiator 默认协商 10s KATO（`MIN_KEEP_ALIVE_TIMEOUT_IN_MS`），而
keep-alive 只在应用调用 `spdk_nvme_ctrlr_process_admin_completions()` 时才会
发出（见 spdk/nvme.h 中该字段的注释）。测试程序若只轮询 IO qpair，一旦总时长
（预写 + 测试）超过 10s 无 admin 流量，target 会强制断连；此后在途 IO 永不
完成，程序无限轮询挂死（表现：`keep alive timeout` 断连日志在 target 侧，
initiator 侧无任何输出）。PCIe 直连不受影响（本盘不致命强制 KAT）。
**解决**：所有轮询点在 `spdk_nvme_qpair_process_completions()` 之外同时调用
`spdk_nvme_ctrlr_process_admin_completions(g_ctrlr)`（见两个测试工具的
`poll_all()`），并加"30s 无完成即断定连接丢失"的看门狗。
`nvme_cache_test.c` 已同步修复，并顺带加固两点：
① qpair SQ 显式扩容（默认 256 项 = QD128 × 2 个 MDTS 拆分正好占满，索引写入
零余量；现按 `nslots × split + 32 + 1` 计算并封顶 1024 = 回环 transport 的
max-queue-depth）；② run loop 每步开始前 poll 一次（否则某步全部 skip 时
在途 IO 无人收割，空转到 deadline）。
实测（GLM-5.3 默认模型，n=500/c=32/t=30s/mixed/M=50）：local 34.7K shard
IOPS / 4.76 GiB/s / 3627us vs RDMA 回环 34.2K / 4.70 GiB/s / 3675us
（-1.4%，144KB 大块传输下瓶颈在盘侧）。

---

## 三、本地 C/C++ 代码如何链接 SPDK

所有 mk 机制都在 `/home/qizhenlin/devSPDK/spdk/mk/` 下，Makefile 只需
设 `SPDK_ROOT_DIR` 并 include。两种模式（实测都可用）：

### 模式 A：单 APP（适合独立小工具）

```makefile
SPDK_ROOT_DIR := /home/qizhenlin/devSPDK/spdk
APP = my_tool
include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

# 可选：链接后清理中间产物
LINK_C += && rm -f $(OBJS) $(OBJS:.o=.d)
```

`nvme.libtest.mk` 做三件事：`C_SRCS := $(APP:%=%.c)`、链 `nvme` 库 +
sock 模块、提供完整的 `$(CFLAGS) / $(LDFLAGS) / $(LIBS) / $(SYS_LIBS)`。
注意 **一个目录只能有一个 APP**（`C_SRCS` 由 APP 名映射，多 APP 会把
所有 main 链进每个二进制）。构建：`make`，清理：`make clean`。

### 模式 B：多目标（APP + 自定义规则，本仓库 ucm_cache_simu 的布局）

```makefile
SPDK_ROOT_DIR := /home/qizhenlin/devSPDK/spdk
APP = nvme_iops_test                      # 主工具走 libtest.mk
include $(SPDK_ROOT_DIR)/mk/nvme.libtest.mk

# 附加二进制：自己写规则，链接命令必须带全四个变量
nvme_cache_test: nvme_cache_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBS) $(ENV_LDFLAGS) $(SYS_LIBS)

kernel_cache_test: kernel_cache_test.c   # 非 SPDK 工具也复用 CFLAGS
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lpthread -laio
```

要点：
- 自定义规则**不能只写 `$(LDFLAGS)`**——SPDK 的库列表在 `$(LIBS)`、
  DPDK 环境在 `$(ENV_LDFLAGS)`、系统库（含 RDMA 开启后的
  `-libverbs -lrdmacm`）在 `$(SYS_LIBS)`，缺一个就是成片的
  `undefined reference to spdk_*`
- 自定义规则只依赖 `.c` 文件，**SPDK 库重编后要 `touch` 源文件强制重链**
- `--with-rdma` 后 `$(SYS_LIBS)` 自动追加 verbs 库，无需手工加

### 依赖二进制验证

```bash
ldd ./my_tool | grep -E 'ibverbs|rdmacm'   # RDMA 版本应出现两行
nm build/lib/libspdk_nvme.a | grep -c nvme_rdma
```

---

## 四、最小测试代码样例

单文件、约 60 行，覆盖 probe → attach → qpair → 同步读 → 清理。
保存为 `hello_spdk.c`，用第三节的 Makefile（`APP = hello_spdk`）编译。

```c
/* hello_spdk.c — probe 一块 NVMe 盘，读第一个 4KB，打印前 16 字节 */
#include "spdk/config.h"
#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"

static struct spdk_nvme_ctrlr *g_ctrlr;
static struct spdk_nvme_ns    *g_ns;

static bool
probe_cb(void *ctx, const struct spdk_nvme_transport_id *trid,
         struct spdk_nvme_ctrlr_opts *opts)
{
    return true;                    /* 接受所有设备 */
}

static void
attach_cb(void *ctx, const struct spdk_nvme_transport_id *trid,
          struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
    const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);
    int nsid;

    g_ctrlr = ctrlr;
    printf("Attached: %s  %.40s FW %.8s\n", trid->traddr, cdata->mn, cdata->fr);
    for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
         nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
        g_ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
        if (spdk_nvme_ns_is_active(g_ns)) break;
        g_ns = NULL;
    }
}

struct my_ctx { int done; uint32_t bytes; };

static void
read_done(void *arg, const struct spdk_nvme_cpl *cpl)
{
    struct my_ctx *c = arg;
    c->bytes = cpl->cdw0 & 0xffffffff;   /* 仅为示意，长度用返回值判断 */
    c->done = 1;
    if (spdk_nvme_cpl_is_error(cpl)) {
        printf("read FAILED\n");
    }
}

int
main(int argc, char **argv)
{
    struct spdk_env_opts opts;
    struct spdk_nvme_transport_id trid;
    struct spdk_nvme_qpair *qpair;
    char buf[4096] __attribute__((aligned(4096)));
    struct my_ctx ctx = {0};

    /* 1. 环境初始化（大页内存、VFIO） */
    opts.opts_size = sizeof(opts);
    spdk_env_opts_init(&opts);
    opts.name = "hello_spdk";
    if (spdk_env_init(&opts) < 0) { fprintf(stderr, "env_init failed\n"); return 1; }

    /* 2. 目标：本例直连 PCIe 84:00.0；RDMA 时换 trid 即可，其余代码不变 */
    spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
    snprintf(trid.traddr, sizeof(trid.traddr), "0000:84:00.0");

    if (spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL) != 0
        || g_ns == NULL) {
        fprintf(stderr, "no controller/namespace\n"); return 1;
    }
    printf("NS: %u sectors x %uB = %.2f GiB, MDTS=%uB\n",
           (unsigned)spdk_nvme_ns_get_num_sectors(g_ns),
           spdk_nvme_ns_get_sector_size(g_ns),
           (double)spdk_nvme_ns_get_size(g_ns) / (1024.0 * 1024 * 1024),
           spdk_nvme_ns_get_max_io_xfer_size(g_ns));

    /* 3. IO qpair（生产代码务必按第二节坑 3 扩容请求池） */
    qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
    if (!qpair) { fprintf(stderr, "alloc_io_qpair failed\n"); return 1; }

    /* 4. 异步读 LBA 0，轮询收割 */
    if (spdk_nvme_ns_cmd_read(g_ns, qpair, buf, 0, 8, read_done, &ctx, 0) < 0) {
        fprintf(stderr, "read submit failed\n"); return 1;
    }
    while (!ctx.done) {
        spdk_nvme_qpair_process_completions(qpair, 0);
    }
    printf("first 16 bytes: ");
    for (int i = 0; i < 16; i++) printf("%02x ", (uint8_t)buf[i]);
    printf("\n");

    /* 5. 清理 */
    spdk_nvme_ctrlr_free_io_qpair(qpair);
    {
        struct spdk_nvme_detach_ctx *dctx = NULL;
        spdk_nvme_detach_async(g_ctrlr, &dctx);
        if (dctx) spdk_nvme_detach_poll(dctx);
    }
    spdk_env_fini();
    return 0;
}
```

运行：`sudo ./hello_spdk`（需 root + 大页）。
改一行 trid 即变远端：

```c
/* RDMA 到本机 nvmf target（回环），其余全不变 */
spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_RDMA);
snprintf(trid.traddr, sizeof(trid.traddr), "192.168.1.12");
trid.trsvcid = 4420;
```

---

## 五、NVMe-oF 回环配方（单机测 RDMA，已验证）

不需要第二台机器；target 与 initiator 同机，走完整 RDMA 协议栈，
仅无网线时延。**挂载期间 84:00.0 被 target 独占（直连测试暂停），
停止后立即恢复，盘上数据不受影响。**

> 以下流程已封装为脚本 `nvmf_loop.sh`：`sudo ./nvmf_loop.sh start [traddr] [trsvcid]`
> / `sudo ./nvmf_loop.sh stop`，失败自动回滚。手动步骤如下：

```bash
R=/home/qizhenlin/devSPDK/spdk/scripts/rpc.py

# 1. 起 target（systemd 托管防会话误杀，见坑 6）
sudo systemd-run --unit=nvmf-loop \
    /home/qizhenlin/devSPDK/spdk/build/bin/nvmf_tgt -m 0xF0 -s 8192

# 2. 建 RDMA transport（--max-queue-depth 必须，见坑 4）
sudo $R nvmf_create_transport -t RDMA -u 4096 --max-queue-depth 1024

# 3. 后端挂真实 SSD
sudo $R bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:84:00.0

# 4. 建 subsystem + NS + listener（192.168.1.12 = mlx5_0 的 IP）
sudo $R nvmf_create_subsystem nqn.2016-06.io.spdk:c1 -a -s SPDK0001
sudo $R nvmf_subsystem_add_ns nqn.2016-06.io.spdk:c1 Nvme0n1
sudo $R nvmf_subsystem_add_listener nqn.2016-06.io.spdk:c1 \
       -t RDMA -a 192.168.1.12 -s 4420

# 5. 现有工具直接连（discovery NQN 自动发现）
sudo ./nvme_iops_test -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.1.12 trsvcid:4420' \
     -o 144 -S 8 -t 5 -w randread

# 6. 拆除，直连恢复
sudo systemctl stop nvmf-loop
```

回环实测（mlx5_0 RoCE，对比 PCIe 直连；2026-09-21，nvme_iops_test 修复坑 7 后，
两侧同参数：`-S 8` 预写工作集、`-t 10`）：
- 4K randread QD1：11.4K vs 12.1K IOPS（**-5.6%**），时延 87.5 vs 82.6us（单跳 +5us）；
- 4K randread QD128：624K vs 555K IOPS（**+12%**，单核 initiator 已 CPU 饱和，
  target 侧 4 个 poller 核分担驱动盘的工作）；
- 144KB randread QD128：5.72 vs 5.86 GiB/s（-2.4%），时延 +74us；
- 4K randwrite QD128：两侧均 1.26 GiB/s（写瓶颈在盘侧，与传输无关）。
注：RDMA 侧 target 占用 NUMA0/1 大页后测试进程内存回落到远端节点，
此为本配置固有代价。

---

## 六、本仓库工具索引（本目录 `testSPDK/`，已迁入 UCM 仓库；SPDK 源码仍在 `/home/qizhenlin/devSPDK/spdk`）

| 工具 | 目录 | 说明 |
|---|---|---|
| `nvme_iops_test` | nvme_iops_test/ | SPDK 裸盘 IOPS 基准（-o/-q/-S/-w/-r，支持 PCIe/RDMA/TCP）|
| `kernel_iops_test` | nvme_iops_test/ | libaio 版 IOPS 基准（文件系统上）|
| `nvme_cache_test` | ucm_cache_simu/ | UCM cache 模拟（SPDK 裸 LBA + hash 索引 + 索引持久化）|
| `kernel_cache_test` | ucm_cache_simu/ | UCM cache 模拟（POSIX 文件 + .tmp/rename 语义，-e psync/aio）|
| `cpu_sample.sh` | ucm_cache_simu/ | 测量期 CPU 采样（自动跳过 init-dump）|
| `ssd_pressure_model.py` | ucm_cache_simu/ | 服务压力模型（N/TTFT/TPOT → RPS → 盘带宽需求）|
| `nvmf_loop.sh` | testSPDK/ | NVMe-oF RDMA 回环 target 一键启停（start/stop，见第五节）|

cache 模拟的关键参数：`-n` 块池、`-c` load step、`-M` 写比例、`-W` layerwise、
`-F` ID 替换率、`-S rand|seq` 定靶、`-e psync|aio`（仅 kernel 侧）、
`-r` trid（仅 SPDK 侧）。
