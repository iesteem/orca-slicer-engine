# Base-bump 漂移治理 Checklist

> **触发条件**：OrcaSlicer 上游 base 同步合入本仓库（历实例：2026-10-10 合入
> 上游 b626b4be62，Orca 2.4.1 sync → 2026-10-12 c4da825 对齐）。
>
> **原则出处**：TECH-DESIGN-TEST-SYSTEM-2026-10-12 §一 T5（漂移先钉死、再谈
> 对错）、§三（三类漂移实证）、§5.2（本 checklist 是 M0 落地物）。
>
> **一句话**：base 同步后的红用例不是"坏了"，是"现实变了"——每个红必须先
> CLI 复现确认，再分类处置，钉死或修复，最后把新现实记入基线。

---

## 第 0 步：同步前基线（合入上游**之前**做）

- [ ] 跑一次全量并归档（`scripts/nightly_tests.sh` 或手动等价命令），
  得到"哪些红是新的"的对照面。没有 before 快照，后面全部分类都是猜。
- [ ] 记录当轮全量用例总数（`<testcase>` 计数口径，见
  junit-report-truncation-trap：Catch2 junit `tests` 属性是断言数不是用例数）。

## 第 1 步：构建

- [ ] 按 P-14（`docs/precedents/build-environment-constraints.md`）：
  所有构建 `-j 2`；Orca 侧变更后 StaticConfig 需 regen、静态归档全量重建，
  防 ODR/ABI 混链 SIGSEGV。
- [ ] 确认构建零警告新增（尤其 StaticConfig 相关）。

## 第 2 步：全量跑 + 收集红清单

- [ ] `python3 tests/run_tests.py build-static`（先 `cmake --build` 再跑；
  run_tests.py 不构建）。
- [ ] **从 junit 报告收集红用例名**，不靠 console 回滚。
- [ ] 采信守卫：`grep -c '<testcase'` 比对总数（当前基线 212 = unit 74 +
  integration 138；显著偏低 = 残缺跑，见 junit 截断陷阱）。

## 第 3 步：逐个红用例 CLI 复现

- [ ] 用真实 CLI 单 fixture 复现（资源根 `-r <repo>/package/resources`，
  记住 C API 自拼 `/profiles`）。排除测试环境噪音（路径 / 资源根 /
  输出目录未预清）之后，才进入分类。

## 第 4 步：分类（每个红用例必须落入且仅落入一类）

| 类 | 定义 | 实例（2.4.1 对齐） |
|---|---|---|
| **A 配置面漂移** | 上游新增/改默认的配置键族；旧项目不携带 → 代码默认生效 vs 官方预设显式值 | `top/bottom_color_penetration_layers`（代码默认 4 vs 官方 5） |
| **B 行为反转** | fixture 锚定的失败/成功条件在新算法下不再复现 | Case 9 擦塔：上游重做塔分区后足迹缩回床内，越界条件消失 |
| **C 定罪组成变化** | 多盘混合场景中某盘的 issue 组成变化，核心语义不变 | Case 12.5：盘 1 的 TOOLPATH_OUTSIDE 消失，仅剩盘 3 预处理越界 |
| **R 真回归** | 断言语义仍应成立，是引擎/上游引入的缺陷 | ——（2.4.1 对齐未出现） |
| **E 测试环境问题** | 路径、资源根、产物目录互踩等非被测物问题 | —— |

## 第 5 步：按类处置

- **A → 键族豁免**：豁免**按族不按键**（如 `kCodeDefaultFilledKeys` 集合），
  保持"项目携带键的真实分叉仍会红"的严格性。注释必须留：根因链（项目缺键 →
  替换契约不加键 → 代码默认填充）+ 桌面 parity 论证（旧项目不重选预设，
  桌面也是代码默认生效）。
- **B → 翻转钉死 + 检测口转移**：断言翻转为新现实（含 CLI 复现证据）；
  **显式声明哪个用例承接原检测能力**（实例：TOOLPATH_OUTSIDE 检测口从
  Case 9 转移至 Case 11 toolpath_outside_mixed_7855），不留孤悬检测口。
- **C → 收缩到不变量**：保留真正不变的核心断言（逐盘定罪 / 干净兄弟盘
  隔离 / 退出码类别聚合），删除随 base 合法变动的组成性断言。
- **R → 修 bug，红测先行**（T6）：`test(red):` 提交在前，修复提交转绿。
- **E → 修测试环境**：输出目录预清（093c015 纪律）、路径契约。

## 第 6 步：注释与提交纪律

- [ ] 每处漂移处置的断言旁注释：上游 commit 哈希 + 根因 + 日期
  （实例格式见 test_preset_substitution.cpp 的
  `kCodeDefaultFilledKeys` 注释块、test_slice_e2e.cpp Case 9 头注释）。
- [ ] 提交信息记录基线快照："Full suite: ALL GREEN N cases / 0 failures
  (日期)"（实例：c4da825 记 212 / 0）。
- [ ] 行为契约变化的，同步更新/新增判例（T2：判例高于测试，测试不可
  悄悄偏离判例；判例只能被人类明确批准后推翻）。

## 第 7 步：归档新基线

- [ ] 再跑一次全量归档（after 快照），before/after 两份归档即本次
  base-bump 的对齐报告存档。

---

## 附：快速命令

```bash
# 全量 + 归档（也可直接跑夜间脚本）
scripts/nightly_tests.sh
# 红跑后读报告（不重跑 24 分钟套件）
python3 tests/run_tests.py --report-only build-static
# 用例总数守卫（采信报告前）
grep -c '<testcase' build-static/tests/engine-integration.junit.xml  # ≥138
grep -c '<testcase' build-static/tests/engine-tests.junit.xml        # ≥74
```
