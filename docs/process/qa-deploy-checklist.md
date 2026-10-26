# QA 服务器部署 Checklist

> **触发条件**：向 QA/云端服务器交付引擎二进制（scp/rsync）。
>
> **出处**：回执 4（2026-10-24）备注——10-24 07:32 部署 scp 丢失执行位
> （QA 手动 chmod 恢复）；同轮 3664 复测因构建源码时点无交接记录，
> 排查成本翻倍（须靠 git 推送序反推）。两条都是本次新增。

## 交付前

- [ ] 记录**源码时点**：交付二进制对应的 commit 哈希（含未推送 commit 时
  写明"本地领先 origin 于 <哈希>"）。
- [ ] 记录**产物指纹**：`md5sum <binary>`；package/ 与 build-static/ 的
  二进制必须同一指纹（本仓静态构建可复现，指纹不同 = 交付了陈旧构建）。
- [ ] 构建按 P-14：`-j 2`；Orca 侧变更后先 regen StaticConfig 并全量重建
  静态归档（防 ODR/ABI 混链）。

## 传输

- [ ] **保留执行位**：`rsync -a` 或 `scp -p`；裸 `scp` 会丢权限位。
  兜底：传输后 `chmod +x <dst>`。
- [ ] 资源目录若同轮更新：只同步 `package/resources` 精选子集，
  **禁止整树 rsync**（resources-version-json-contract 判例：版本串只读
  Snapmaker.json，内容漂移不会被版本号暴露）。

## 交接记录（随二进制发 QA）

- [ ] 交付单三行：commit 哈希 / binary md5 / 资源版本串（Snapmaker.json
  version）。QA 复测报告引用交付单，双向可追溯（本轮 3664"构建过旧"
  假设需 git 推送序反推才能排除，即因无此记录）。
- [ ] 若交付构建**早于 origin 最新推送**（如本地 build-static 陈旧），
  在交付单显式标注，防止旧构建被当作新修复的验证载体。

## 交付后

- [ ] QA 侧首跑冒烟：任意 fixture 切一次，`engine_version` +
  `resources_version` 与交付单一致。
