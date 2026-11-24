# MiniCAD 嵌入增强分支（`sa-embed`）

本分支基于上游 [CHMOSE023/MiniCAD](https://github.com/CHMOSE023/MiniCAD)（MIT 协议）维护，面向需要把 MiniCAD 嵌入桌面/网页应用、做图纸版本比对与自动化图框文字提取的场景。

## 分支策略与上游同步

- `main` 分支保持与上游 `CHMOSE023/MiniCAD` 的 `main` 完全一致，不在 `main` 上直接提交改动。
- 所有嵌入与对比增强均维护在 `sa-embed` 分支。
- 需要同步上游新版本时：
  ```bash
  git fetch upstream
  git checkout main && git merge --ff-only upstream/main && git push origin main
  git checkout sa-embed && git merge upstream/main
  ```

## 核心增强能力

1. **单引擎同相机叠图对比**
   - 主图与对比底图共用同一个 MiniCAD WASM 实例、主相机、主视口与帧缓冲，避免双实例缩放不同步与双倍内存开销。
   - GPU 缓冲与光栅图片纹理按槽位隔离（主图 `0..31`，对比图 `32..63`）；关闭对比图或切换主图时按槽位范围精准释放渲染缓冲、GPU 纹理、文档与堆内存（`ReleaseFreeMemory()`）。
   - `WebGPURenderer` 与 `GLES3Renderer` 统一提供双离屏底片三态差异合成（`CompositeCompareLayers`）：主/副图各自渲染到独立缓存的透明离屏纹理，逐像素按覆盖度判定“重合退灰（可调浓度 0%–100%，0% 即隐藏相同部分） / 仅主图翠绿 / 仅对比图珊瑚红”，深浅背景自适应，并配合 `SetClearColor`、`ScissorRect` 卷帘裁剪与 `PaintStyle` 透明度控制；同时修复字体网格多槽位串槽与 Uniform 对齐问题。
   - `Viewport` 在同一通道内支持 `current`、`compare`、`blend`（三态差异叠图）、`split`（卷帘分屏）、`align-pick-base`、`align-pick-main`（三态实时跟随，对齐处重合线即时变灰）六种模式，双精度平差矩阵消除大坐标抖动，并支持在对比底图上做对象捕捉拾点。
   - 完整支持图纸空间（Layout）叠图：主图切换布局时对比底图自动按同名优先、同序号兜底跟随切换，也支持宿主通过 `compareLayoutIndex` 手动指定对比图的任意布局，并向宿主广播主图布局变化与对比图布局列表。
2. **宿主嵌入与浏览/批注双模式**
   - `?chrome=viewer|canvas` 嵌入外观与布局裁剪；浏览/批注双模式命令白名单与撤销栈否决钩子。
   - 导出红色云线批注 DWG、主图与对比图外部参照列举及按目录重载、容器隐藏时启停渲染轮询、嵌入态非阻塞报错回传。
   - **选中图元摘要导出接口**：通过 `MiniCADEmbed_GetSelectionVersion()` 轻量轮询当前主图 / 活动布局 / 选中集合的稳定版本号（无主图返回 `0`），并通过 `MiniCADEmbed_GetSelection(outPath, maxItems)`（`MainFrame::EmbedGetSelectionJson` + `src/Document/SaSelectionExport.h`）将选中图元按 `ObjectID` 升序导出为结构化 UTF-8 JSON（含 `count`、`truncated`、`bounds`、`typeCounts` 与单实体 `objectId` / `handle`（大写无 `0x` 前缀十六进制，经 `LookupCadEntityHandle` 查询并与 `SaDwgTool` SQLite `entities.handle` 一致） / `type` / `layer` / `blockName` / `text` / `length` / `area` / `radius` / `bounds`）。
3. **无界面 DWG 文字提取工具（`SaDwgText`）**
   - Node 版 WASM 命令行工具（`apps/SaDwgText/main.cpp` + `src/Document/SaTextExport.h`），只读解析 DWG 中的 `TEXT` / `MTEXT` / `ATTRIB`（含嵌套块路径、世界坐标、字高、图层），输出标准 TSV。
4. **通用修复**
   - 修复 `SnapEngine` 节点捕捉 `y1 = x1` 笔误。

## 编译构建

本地编译（Linux + `emsdk 5.0.7`）：

```bash
emcmake cmake -S . -B out/web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/web --target MiniCADWeb SaDwgText
```

推送到 `sa-embed` 分支会自动触发 `.github/workflows/build-embed.yml` 编译打包；推送 `v<上游版本>-sa.<序号>` 标签（例如 `v0.8.4-sa.4`）会自动发布包含 `web/`、`node/` 与 `SHA256SUMS` 的 GitHub Release。
