# MiniCAD 嵌入增强分支（`sa-embed`）

本分支基于上游 [CHMOSE023/MiniCAD](https://github.com/CHMOSE023/MiniCAD)（MIT 协议）维护，面向需要把 MiniCAD 嵌入桌面/网页应用、做图纸版本比对与自动化图框文字提取的场景。

## 分支策略与上游同步

- `main` 分支保持与上游 `CHMOSE023/MiniCAD` 的 `main` 完全一致，不在 `main` 上直接提交改动。
- 所有嵌入与对比增强均维护在 `sa-embed` 分支。
- 需要同步上游新版本时（推送 `main` 前必须校验 `HEAD` 与 `upstream/main` 提交号完全一致，防止误推额外提交）：
  ```bash
  git fetch upstream
  git checkout main && git merge --ff-only upstream/main
  test "$(git rev-parse HEAD)" = "$(git rev-parse upstream/main)" && git push origin main
  git checkout sa-embed && git merge upstream/main
  ```

## 核心增强能力

1. **单引擎同相机叠图对比**
   - 主图与对比底图共用同一个 MiniCAD WASM 实例、主相机、主视口与帧缓冲，避免双实例缩放不同步与双倍内存开销。
   - GPU 缓冲与光栅图片纹理按槽位隔离（主图 `0..31`，对比图 `32..63`）；关闭对比图、切回 `Off` 或切换主图时按槽位范围精准释放渲染缓冲、GPU 纹理、双离屏对比底片（`Viewport::ReleaseCompareLayers()`）、文档与堆内存（`ReleaseFreeMemory()`）。
   - `WebGPURenderer` 与 `GLES3Renderer` 统一提供双离屏底片三态差异合成（`CompositeCompareLayers`）：主/副图各自以满覆盖模式（`PaintStyle::FullCoverage`，不因批注锁层淡显拉低 alpha 误判差异）渲染到独立缓存的透明离屏纹理，逐像素按覆盖度判定“重合退灰（可调浓度 0%–100%，0% 即隐藏相同部分） / 仅主图翠绿 / 仅对比图珊瑚红”，深浅背景自适应，并配合 `SetClearColor`、`ScissorRect` 卷帘裁剪与 `PaintStyle` 透明度控制；同时修复字体网格多槽位串槽与 Uniform 对齐问题。
   - `Viewport` 在同一通道内支持 `current`、`compare`、`blend`（三态差异叠图）、`split`（卷帘分屏）、`align-pick-base`、`align-pick-main`（三态实时跟随，对齐处重合线即时变灰）六种模式，双精度平差矩阵消除大坐标抖动，隐藏主图或卷帘副图侧时统一约束主图高亮/夹点与点击/框选拾取（同时保留全视口平移缩放），并支持在对比底图上做对象捕捉拾点。
   - 完整支持图纸空间（Layout）叠图：主图切换布局时对比底图自动按同名优先、同序号兜底跟随切换，也支持宿主通过 `compareLayoutIndex` 手动指定对比图的任意布局，并向宿主广播主图布局变化与对比图布局列表。
2. **宿主嵌入与浏览/批注双模式**
   - 支持的网址查询参数：`?webgl`（强制 WebGL2 后端）、`?lang=<code>`（界面语言）、`?open=<URL>`（可重复，拉取远程图纸，可配合 `window.MiniCADHost.fetch` / `window.MiniCADHost.findXrefs`；拉取期间若发生换图、新建或关闭全部图纸，将通过 `MainDocumentIntentVersion` 自动丢弃过期响应、中止后续拉取并清理 `/remote/<id>` VFS 文件）。
   - 浏览（`0`）/ 批注（`1`）双模式通过 `MiniCADEmbed_SetMode(mode)` 切换至固定嵌入布局（`layouts.saembed`），启用命令白名单、工具白名单、撤销栈否决钩子与撤销/重做门禁（浏览态拦截直接撤销/重做入口）；批注模式下新对象落入 `SA-问题批注` 层并保护全部原图既有实体（含原图既有 `SA-问题批注` 层实体）。
   - 导出红色云线批注 DWG（自动沿用当前模型/图纸空间布局属性，校验坐标与顶点数上限并经 RAII 清理临时实体）、主图与对比图外部参照列举及按目录重载（嵌入态由宿主接管外参，不弹桌面缺失外参框）、嵌入态非阻塞报错回传。
   - **选中图元摘要导出接口**：通过 `MiniCADEmbed_GetSelectionVersion()` 轻量轮询当前主图 / 活动布局 / 选中集合的稳定版本号（无主图返回 `0`），并通过 `MiniCADEmbed_GetSelection(outPath, maxItems)`（`MainFrame::EmbedGetSelectionJson` + `src/Document/SaSelectionExport.h`）将选中图元按 `ObjectID` 升序导出为严格合法 UTF-8 JSON（非法字节序列替换为 `U+FFFD`）：
     - 顶层字段：`count`（准确总选中数，不受 50,000 扫描上限截断）、`scannedCount`（实际扫描并计入统计的数量）、`truncated`（布尔，`items.size() < count`）、`statsTruncated`（布尔，`scannedCount < count`）、`bounds`、`typeCounts`、`items`。
     - 单实体字段：`objectId`、`handle`（大写无 `0x` 前缀十六进制，经 `LookupCadEntityHandle` 查询并与 `SaDwgTool` SQLite `entities.handle` 一致）、`type`、`layer`、`blockName`、`text`、`length`、`area`（带孔 `HATCH` 按边界环包含关系/奇偶规则计算真实净面积）、`radius`、`bounds`。
3. **WASM 就绪、回调与 VFS 文件寿命约定**
   - **初始化就绪与错误契约**：WebGPU 适配器/设备创建为异步流程，所有依赖 UI/平台的 `MiniCADEmbed_*` 导出入口均统一校验 `IsUiReady()`；宿主必须在轮询到 `MiniCADEmbed_IsReady() === 1` 后再调用业务接口。错误回传按接口区分：
     - **仅** `MiniCADEmbed_ExportMarkupDwg(rects, count, labels, outPath, errPath)` 接受 `errPath` 参数，成功写出后返回正整数批注快照令牌（`>= 1`，供落盘成功后传给 `MiniCADEmbed_MarkMarkupClean(token)` 确认基线），失败或未就绪时返回 `0` 并在 `errPath` 写入 UTF-8 原因；
     - 异步开图失败原因经 `MiniCADEmbed_GetMainError(outPath)` / `MiniCADEmbed_GetCompareError(outPath)` 查询（有错误时写入 `outPath` 并返回 `1`，无错误返回 `0`）；
     - 其余返回整型状态或计数的接口在未就绪或失败时统一返回 `0`（不接受 `errPath`），返回 `void` 的接口在未就绪时静默忽略。
   - **宿主 `Module` 回调**（在调用 `MiniCADEmbed_SetMode` / `MiniCADEmbed_StartPickPoint` 前挂载到 Emscripten `Module`）：
     - `Module.onAbort(message)`：图形设备初始化失败等致命错误通知。
     - `Module.onSaEmbedModeChanged(mode)`：用户在功能区切换浏览（`0`）/ 批注（`1`）选项卡后通知宿主。
     - `Module.onSaEmbedLayoutChanged(index, isPaper, name)`：主图活动布局变化后通知宿主。
     - `Module.onSaEmbedPointPicked(seq, worldX, worldY)` / `Module.onSaEmbedPickCancelled(seq)`：拾点完成或用户/宿主显式取消（Esc、右键、`MiniCADEmbed_CancelPickPoint()`）后的异步安全回调；新请求顶替旧请求或换图/关图时旧会话静默作废，不再派发过期取消回调。
   - **实际导出的 `MiniCADEmbed_*` 接口清单**（见 `apps/Web/src/Host/WebApp.cpp`）：
     - 就绪与模式：`MiniCADEmbed_IsReady`、`MiniCADEmbed_SetMode`
     - 图纸与对比图生命周期：`MiniCADEmbed_OpenDrawing`、`MiniCADEmbed_IsOpening`、`MiniCADEmbed_GetMainError`、`MiniCADEmbed_OpenCompareDrawing`、`MiniCADEmbed_IsOpeningCompare`、`MiniCADEmbed_GetCompareError`、`MiniCADEmbed_CloseCompareDrawing`、`MiniCADEmbed_HasCompareDocument`、`MiniCADEmbed_CloseOtherDocuments`、`MiniCADEmbed_CloseAllDocuments`
     - 叠图与布局控制：`MiniCADEmbed_SetCompareState`、`MiniCADEmbed_GetMainLayout`、`MiniCADEmbed_ListCompareLayouts`、`MiniCADEmbed_ZoomAll`、`MiniCADEmbed_ZoomCompareAll`、`MiniCADEmbed_ZoomToWorld`、`MiniCADEmbed_GetViewWorld`、`MiniCADEmbed_WindowToWorld`、`MiniCADEmbed_WorldToWindow`
     - 拾点与外参：`MiniCADEmbed_StartPickPoint`、`MiniCADEmbed_CancelPickPoint`、`MiniCADEmbed_ListXrefs`、`MiniCADEmbed_ReloadXrefs`、`MiniCADEmbed_ListCompareXrefs`、`MiniCADEmbed_ReloadCompareXrefs`
     - 批注脏态与基线：`MiniCADEmbed_HasMarkupChanges`（相对打开或上次 `MiniCADEmbed_MarkMarkupClean(token)` 基线是否存在批注修改，撤销回基线返回 `0`，有真实新增/删除/修改返回 `1`）、`MiniCADEmbed_MarkMarkupClean(token)`（凭 `MiniCADEmbed_ExportMarkupDwg` 签发的正整数快照令牌将导出瞬间的批注快照记为干净基线，校验令牌与当前主文档匹配后消费令牌并返回 `1`，无效/已消费/跨文档令牌返回 `0`；异步写盘期间新增的笔迹保持脏态）
     - 数据与批注导出：`MiniCADEmbed_ExportText`、`MiniCADEmbed_GetSelectionVersion`、`MiniCADEmbed_GetSelection`、`MiniCADEmbed_ExportMarkupDwg`（成功返回快照令牌 `>= 1`，失败返回 `0`）
   - **VFS 文件寿命约定**：
     - 宿主写入 Emscripten 内存文件系统（`FS.writeFile`）并传给 `MiniCADEmbed_OpenDrawing(fsPath)` / `MiniCADEmbed_OpenCompareDrawing(fsPath)` 的图纸文件，必须保持有效直至 `MiniCADEmbed_IsOpening()` / `MiniCADEmbed_IsOpeningCompare()` 返回 `0`（读取完成）；关闭全部或换图时在途异步加载会被代次机制自动作废。
     - 传给 `MiniCADEmbed_ReloadXrefs(searchRoot)` / `MiniCADEmbed_ReloadCompareXrefs(searchRoot)` 的外参目录文件需在该同步调用返回前保持可读，返回后宿主可自行回收。
     - 传给 `MiniCADEmbed_ExportText` / `MiniCADEmbed_GetSelection` / `MiniCADEmbed_ExportMarkupDwg` / `MiniCADEmbed_List*` / `MiniCADEmbed_Get*Error` 的 `outPath` / `errPath` 均由 WASM 完整写出并校验短写后返回，文件所有权与读后删除（`FS.unlink`）由宿主负责。
4. **无界面 DWG 文字提取工具（`SaDwgText`）**
   - Node 版 WASM 命令行工具（`apps/SaDwgText/main.cpp` + `src/Document/SaTextExport.h`），只读解析 DWG 中的 `TEXT` / `MTEXT` / `ATTRIB`（遍历含可导出文字的 `MINSERT` 行列阵列单元、对无字块做防环递归剪枝缓存以跳过无字大阵列展开、按变换矩阵换算真实世界字高、保留外层及嵌套 `ATTRIB` 不重复套本级矩阵语义），流式输出标准 TSV 到 `stdout`。
   - **三项硬预算与退出码契约**：默认单次导出施加三项硬预算——最多展开 `250,000` 个阵列单元（`MaxArrayCells`）、最多输出 `100,000` 行文字（`MaxRows`）、最多输出 `16 MiB` 字节（`MaxBytes`）。因采用流式写 `stdout`，当触发任一硬预算（退出码 `3`）、读图失败或写 `stdout` 失败（退出码 `1`，参数错误退出码 `2`）时，`stdout` 可能已包含已刷出的部分 TSV；调用方**必须校验进程退出码为 `0`** 后再消费完整 TSV 输出（错误原因见 `stderr`）。
5. **通用修复**
   - 修复 `SnapEngine` 节点捕捉 `y1 = x1` 笔误、块交点捕捉叶节点与最近曲线预算上限（64 条）以及斜切仿射正交性校验。
   - 修复网页嵌入版 `WebInput` 在跨源 iframe 拖出丢 `mouseup`、窗口失焦、页面隐藏或 `pointercancel` 时鼠标拖拽粘连的问题：补齐指针捕获、`mousemove` 按 W3C `e.buttons` 实时核对补发抬起、侧键过滤、`PointerCancel` 与 `Module.MiniGUIWebInput.cancelPointer()` 清理链路。

## 编译构建

本地编译（Linux + `emsdk 5.0.7`）：

```bash
emcmake cmake -S . -B out/web -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/web --target MiniCADWeb SaDwgText
```

推送到 `sa-embed` 分支会自动触发 `.github/workflows/build-embed.yml` 编译打包；推送 `v<上游版本>-sa.<序号>` 标签（例如 `v0.8.4-sa.8`）会自动发布包含构建产物与 `SHA256SUMS` 的 GitHub Release。
