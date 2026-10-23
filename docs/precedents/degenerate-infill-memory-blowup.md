# 病态 infill 内存爆炸（Faceted_Bowl_2）—— 引擎无护栏，云端须外加资源上限

**状态：** 未修复（引擎侧无内存护栏，属部署层缓解项）；2026-10-23 定性；
同日 QA 回执3 确认 >25GiB 定量画像归档，缓解三件套建议已转达平台侧
**Fixture：** QA 用例库 `fixtures_archive/Faceted_Bowl_2.3mf`（本地副本
`~/Downloads/source/单色模型/Faceted_Bowl_2.3mf`，0.2MB）
**判例编号候选：** P-34

## 现象（QA 回执2，AWS 服务器）

跑批 NO_RESULT 后直切复验：引擎进程被 kernel OOM-kill（shell 137=SIGKILL），
仅产出 fb2.log 无 json。桌面 2.4.1 同件失败但形态不同（exit 2 无产物）。

## 本机复现画像（2026-10-23，15GiB 机器）

- 切片阶段 16s 正常完成（SupportSpotsGenerator 材质告警为已知噪音）；
  内存爆炸在 **G-code 生成阶段**
- RSS ~**19MB/层**单调增长：层 435/约1000（模型高 100mm × 0.1 层高）时
  **11.35GiB** 仍在涨（为保护机器手动终止）；全程外推 **>25GiB**
- 终止时部分 gcode 已 **757MB / 2690 万行**

## 根因层

- 不是网格规模：**13,916 三角面片 / 6,960 顶点**，闭合拓扑一致
  （E=3F/2=V+F−2 恰好成立），引擎 7 项几何检查零告警
- 是 **sparse infill 退化为海量微米级段**：435 层即 2590 万条挤出移动、
  ΣE=69.2m 耗材（对照壁 55 万条 / 9.5m——15% 密度名义下 infill 耗材
  7 倍于壁、条数 47 倍于壁，平均段长微米级）
- 两侧一致病态：桌面 2.4.1 exit 2 无产物 → **非引擎回归、非下发问题**；
  引擎失败形态（无界内存而非优雅报错）是引擎侧唯一缺口

## 部署缓解（必须，引擎内无对应机制）

1. 云端对引擎进程外加资源上限：cgroup / `ulimit -v` / 容器 memory limit，
   超限按失败用例处理（会得到非 0 rc 而非 137 也可接受，但 json 仍可能
   不落盘——与 [[explicit-json-path-unconditional-write]] 的失败出口行为
   相关，超时/OOM 类杀死目前不保证 json）
2. 独立看门狗：`-t` 只在**盘入口**检查，单盘病态几何可无限打穿（本例
   与 8 模型批量报告 ddb OOM / dfbf Voronoi 14h 同族结论）
3. 磁盘：被杀后 `plate_N.gcode.tmp` 孤儿残留（本例 757MB），需清理策略
   （与 [[disk-space-slicing-ops]] 合并考虑）

## 排查方法备忘

- 分阶段 RSS 采样（/proc/PID/status VmRSS 每 5s）+ `/usr/bin/time -v`
  峰值；`-t` 不覆盖导出段，须外部限时
- gcode 按 `;TYPE:` 分区统计 E 移动数与 ΣE 即可定位退化 feature
  （awk `-F';TYPE:'` + ` E\.?[0-9.]` 匹配，注意 Orca 相对 E 模式与
  `E.002` 无前导零写法）
