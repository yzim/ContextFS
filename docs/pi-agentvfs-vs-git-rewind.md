# Pi AgentVFS Rewind 与 Git-Based Rewind 对比

本文比较当前 `extensions/pi` 中基于 AgentVFS 的 rewind 实现，与一种基于 Git 的 rewind 方案。

结论先行：如果 rewind 是 agent 运行时能力的一部分，需要完整恢复工作区副作用，并且不能污染用户的 Git 状态，AgentVFS 更适合作为主后端。如果目标是低门槛、零 daemon/FUSE 依赖、用户可以直接用 Git 工具检查和恢复，那么 Git-based rewind 更适合作为轻量 fallback。

## Pi `/tree` 机制背景

Pi 的 `/tree` 是会话树导航能力。一次 agent 对话不是单纯的线性日志，而是可以从某个历史节点 fork 出新的分支。用户在 `/tree` 中选择过去的节点后，Pi 会把 conversation leaf 切换到那个节点，后续 prompt 和 tool call 会从该节点继续展开。

这带来一个关键问题：conversation state 可以回到过去，但普通文件系统不会自动回到过去。如果用户在第 10 轮让 agent 修改了文件，然后通过 `/tree` 回到第 4 轮继续对话，Pi 的消息上下文已经回到第 4 轮，但 workspace 里的文件可能仍然停留在第 10 轮之后。这会产生 split-brain：

- agent 看到的对话历史说“这些改动还没发生”；
- 文件系统却已经包含后续分支的改动；
- 后续 tool call 会基于错误的文件状态继续修改；
- 不同 conversation branch 的文件副作用会互相污染。

因此，Pi 的 rewind 不能只恢复聊天记录，还必须在 tree navigation、fork、manual rewind 时恢复对应的 workspace 状态。理想语义是：

- mutating tool 或 mutating turn 之后创建文件系统 checkpoint；
- checkpoint 与 Pi session entry 或 turn timestamp 建立映射；
- 用户选择 `/tree` 节点时，先把 workspace rollback 到该节点对应的 checkpoint；
- rollback 失败时取消 tree 切换，避免 conversation 和 filesystem 状态不一致；
- rollback 前再创建 safety checkpoint，使 rewind 本身可以 undo。

当前 `extensions/pi` 的 AgentVFS 实现就是围绕这个语义设计的：它把 AgentVFS checkpoint 写入 Pi session metadata，`/tree` 前按 explicit checkpoint entry 或时间戳查找最近 checkpoint，然后调用 AgentVFS rollback。Git-based rewind 如果要达到同样效果，也必须实现同样的“Pi tree node 到文件系统快照”的映射，只是快照后端换成 Git object、hidden ref 或 stash。

## 两种方案

### 当前 AgentVFS 方案

Pi 扩展本身不实现文件版本管理，而是作为 AgentVFS 的编排层：

- Pi 必须运行在 AgentVFS FUSE mount 内。
- 扩展发现 mount 和 control socket。
- 通过 `agentvfs-ctl checkpoint` 创建 checkpoint。
- 通过 `agentvfs-ctl rollback` 回滚。
- 尽可能把 checkpoint metadata 写入 Pi conversation tree。
- 在 Pi `/tree` 和 fork 切换会话节点之前，先恢复对应的文件系统状态。

实际版本化数据由 AgentVFS 管理，包括 in-memory working tree、content-addressed object store、commit object、ref 和 rollback tree rebuild。

### Git-Based Rewind 方案

Git 方案会直接运行在普通项目目录中，并用 Git 仓库作为快照存储层。典型实现可能包括：

- `git add -A && git commit` 到隐藏 commit；
- `git stash push --include-untracked`；
- 隐藏 ref，例如 `refs/pi-rewind/...`；
- rewind 时执行 `git reset --hard`、`git checkout`、`git restore`、`git clean`；
- 额外保存和恢复用户原本的 branch、index、stash、未提交修改等状态。

不同 Git 实现细节会不同，但核心共同点是：Pi rewind 会开始管理用户项目的 `.git` 状态。

## 功能语义对比

| 维度 | AgentVFS rewind | Git-based rewind |
|---|---|---|
| 工作区覆盖范围 | 只要写入经过 mount，就能覆盖 tracked、untracked、ignored、generated 文件。 | 默认只覆盖 Git 明确 stage/stash 的内容；ignored 文件需要额外策略，容易遗漏。 |
| 对用户 Git 状态的影响 | 不触碰 branch、index、stash、reflog、commit history。 | 需要操作 Git 状态或创建隐藏 refs/stashes，必须避免污染用户工作流。 |
| 非 Git 项目 | 支持。 | 不天然支持，除非自动初始化 Git repo 或引入 fallback。 |
| 与 Pi `/tree` 对齐 | 很自然：checkpoint 可以作为 Pi session entry，tree 切换前按 entry 或时间戳恢复。 | 可以实现，但每个 Pi 节点都要映射到隐藏 Git object，并处理 Git 状态保存/恢复。 |
| rewind undo | 当前扩展会在 rollback 前创建 safety checkpoint，并维护 redo stack。 | 可以通过额外 commit/stash 实现，但会进一步增加 Git 状态复杂度。 |
| 与原始 source tree 的隔离 | 强。只要 Pi 在 mount 内工作，rollback 不会直接改写原始 source tree。 | 弱。rewind 本质上会改写用户真实 working tree。 |
| 可移植性 | AgentVFS daemon 的 checkpoint/rollback 是跨平台方向，但当前 Pi 扩展的 mount discovery 仍偏 Linux。 | Git 跨平台成熟，部署门槛低。 |
| 运行依赖 | 需要 daemon、FUSE mount、`agentvfs-ctl` 和 live socket。 | 主要依赖 Git。 |
| 故障恢复 | 需要 AgentVFS 自己的 socket/mount/daemon 恢复路径。 | 用户可以用熟悉的 Git status、reflog、refs、stash 手工排查。 |

## 性能对比

当前仓库已有 `benchmarks/agent-sim`，它比较 AgentVFS、agentfs、branchfs，并记录 checkpoint latency、rollback latency、branch creation latency 和 storage size。这个 benchmark 目前还没有 Git rewind driver，因此下面的性能判断是基于机制分析，不是实测数字。

未来如果要做严格性能结论，应补充 Git driver，并用同一套 workload 输出同样的 CSV 指标。

### 稳态文件 I/O

AgentVFS 把 FUSE 放在普通读写路径上。Pi 的文件工具和 shell 命令只要访问 workspace，就会经过 FUSE、working tree、write buffer 和对象哈希路径。因此，即使不创建 checkpoint，AgentVFS 也有持续的文件系统层开销。

Git-based rewind 在普通文件系统上工作，日常 edit/build/test 不经过额外 FUSE 层。对于大量小文件 stat/read/write、构建系统扫描目录、测试生成临时文件等场景，Git 方案通常会有更低的 baseline overhead。

性能含义：

- Git 在普通工作负载的稳态 I/O 上更轻。
- AgentVFS 用持续运行时开销换取更完整、更低耦合的 checkpoint/rollback 语义。
- 未来 AgentVFS 可以继续优化 FUSE hot path、write buffer、metadata cache 和 mount startup，使长期运行的 agent session 更接近原生文件系统体验。

### Checkpoint 创建

AgentVFS checkpoint 会 flush open write buffers，序列化当前 working tree，写 commit object，fsync 新增且可达的对象，然后推进 ref。因为 AgentVFS 在文件写入发生时已经观察并记录了变化，checkpoint 时不需要重新从普通文件系统中全量发现变更。

Git checkpoint 通常需要在 checkpoint 时扫描 working tree、更新 index、写 blob/tree、创建 commit 或 stash，并且还要保存用户原有 index 状态。如果需要覆盖 untracked 或 ignored 文件，扫描和策略成本都会上升。

预期性能形态：

- 高频、小步 agent checkpoint 场景下，AgentVFS 有结构性优势，因为变更跟踪已经在文件系统层发生。
- 小型 Git repo、变更文件少、只关心 tracked 文件时，Git checkpoint 可能很快。
- 大型 repo 中如果 Pi 工具层能精确报告所有变更 path，Git 可以通过 pathspec 缩小扫描范围；否则容易退化为较重的全树扫描。
- ignored/generated 文件越重要，AgentVFS 越简单；Git 要么跳过它们，要么支付额外发现和存储成本。

未来改进方向：

- 为 `benchmarks/agent-sim` 增加 Git driver，量化 `git-tracked` 与 `git-all` 两种模式。
- 为 AgentVFS checkpoint metadata 增加更丰富的 agent context，例如 turn id、tool call id、prompt summary，减少 Pi 扩展本地 metadata 的压力。
- 增加 checkpoint retention 策略，避免长 session 中 checkpoint 历史无限增长。

### Rollback 延迟

AgentVFS rollback 会解析目标 commit，按 tree object rebuild in-memory working tree，invalidate open file handles，然后推进 branch ref。由于 mount 展示的是 daemon 内部 working tree，rollback 不需要像普通 checkout 那样逐个改写真实工作目录文件。

Git rollback 会直接更新真实 working directory。`reset --hard` 加 `clean` 或 `restore` 需要删除、重写或恢复目标状态与当前状态不同的文件。成本取决于变更文件数量、文件大小、untracked/ignored 策略，以及工作树里是否有 submodule、nested repo、sparse checkout 等复杂情况。

预期性能形态：

- 大范围 rewind、多文件变化、生成物较多时，AgentVFS 更可能稳定，因为它主要切换 daemon 内部可见树。
- 小 diff rewind 时，Git 可能非常快。
- Git 要完整处理 untracked/ignored 文件时，rollback 流程会更重，也更容易触碰用户不希望改动的文件。

未来改进方向：

- AgentVFS 需要继续强化 open file handle invalidation、page cache 一致性和跨平台 mount 行为。
- Pi 扩展应在 rollback 前后展示更清晰的状态，例如目标 checkpoint、当前 mount、失败原因和 undo checkpoint。
- Git fallback 如果实现，应默认限制为 tracked 文件，并把 `include-untracked` / ignored 文件恢复作为显式 opt-in。

### 存储增长

AgentVFS 使用 BLAKE3 content-addressed objects，checkpoint 和 branch 之间可以共享相同内容。它不会污染 `.git`，但会引入独立 store 目录。当前 roadmap 已经指出，GC、retention、history compaction 仍是后续重点；长期 session、频繁 rollback、branch delete 或 checkpoint pruning 后，未引用对象可能持续累积。

Git 也使用 content-addressed object store，并且有成熟的 pack、delta compression 和 GC。对于文本历史，Git pack 后可能非常省空间。但如果 Git rewind 把 ignored build output、大 binary、generated artifacts 都纳入 commit/stash，`.git` 会快速膨胀，而且这个膨胀发生在用户项目仓库内部。

预期性能形态：

- AgentVFS 的优势是隔离：agent snapshot 存在 `.git` 之外。
- Git 的优势是成熟：pack/delta/gc 已经被大量项目验证。
- AgentVFS 的未来工作应重点补齐 GC、retention policy 和 checkpoint pruning，让长期 agent session 的存储增长可控。

### Branch 与 Fork

AgentVFS branch creation 是共享 CAS 对象上的 ref 操作，存储成本接近零，语义上也贴近 Pi tree/fork。当前仍有后续规模化工作：roadmap 中提到 lazy branch instantiation 和 bounded memory，否则大量 branch 可能带来过多 resident working tree。

Git branch creation 本身也很便宜，但一个普通 checkout 同一时间只能展示一个 branch。多个并发 agent 分支需要 worktree 或 clone，而每个 worktree 都有自己的 checkout 文件和工作目录成本。

预期性能形态：

- 串行 tree navigation 中，两者都可以低成本切换 ref，但 Git 必须更新真实 checkout。
- 多 agent 并发分支中，AgentVFS 架构更贴近目标，但仍需要实现 lazy branch / bounded memory。
- Git worktree 成熟可靠，但每个并发分支都有独立工作目录开销。

未来改进方向：

- AgentVFS 应优先推进 lazy branch instantiation，避免 100 个分支变成 100 份完整 resident working tree。
- Pi 侧可以进一步把 conversation branch 与 AgentVFS branch 显式绑定，减少时间戳匹配 fallback。
- 跨平台 branch routing 需要从 Linux-only cgroup 路由演进到显式 agent token。

### 启动与退出

AgentVFS 需要 mount startup、socket discovery 和 teardown。当前 Pi 扩展要求 Pi 已经从 mount 内启动；如果 Pi 在原始 source directory 中运行，扩展会禁用 rewind。已有 autostart/cwd handoff 设计可以缓解这个问题，但还不是当前实现。

Git 几乎没有启动成本：找到 repo，创建隐藏 refs 或 stash namespace 即可。因此轻量使用场景下，Git 的启动体验明显更简单。

未来改进方向：

- 实现 `pi-agentvfs` launcher，在 Pi 初始化前完成 workspace init/start、cwd 切换和 argv path normalization。
- 增加 `agentvfs.autoMount=off|auto|require`，让用户可以选择失败时 warn-and-disable 还是 fail-closed。
- 提升 macOS/Windows 的 workspace CLI 与 mount discovery 支持，降低 Pi 扩展对 Linux `/proc/mounts` 的依赖。

## 正确性与安全边界

AgentVFS 的核心正确性前提是：所有需要 rewind 的写入都必须经过 AgentVFS mount。如果 Pi 或用户直接编辑原始 source tree，AgentVFS 无法回滚那些改动。当前扩展已经选择 fail closed：不在 mount 内就禁用 rewind。

Git 的核心正确性挑战是：必须完整保存和恢复用户 Git 状态。这里容易出现边界情况：

- staged 与 unstaged 修改不同；
- untracked 与 ignored 文件需要明确策略；
- submodule、nested repo 需要单独处理；
- hook、attributes、line ending filter、sparse checkout 会影响行为；
- hidden commits/stashes 虽然不直接显示在普通 log 中，但仍然是用户 repo 状态。

未来改进方向：

- AgentVFS 侧应把“当前是否安全可 rewind”做成更强的状态检查，包括 mount、socket、daemon health、cwd、current commit。
- Pi 扩展需要更清楚地区分 unavailable、disabled、degraded、ready 几种状态。
- Git fallback 如果存在，应把安全边界写得很窄：默认 tracked-only，并在启用 untracked/ignored 恢复前明确提示风险。

## Pi 用户体验

AgentVFS 更贴近 Pi 的会话树语义：mutating turn 后 checkpoint，`/tree` 前 rollback，rewind 前创建 safety checkpoint，Git 状态保持干净。主要体验成本在启动和可见性：用户必须在 mount 内工作，并理解“原始 source tree”和“AgentVFS mount”之间的关系。

Git 更贴近用户已有心智模型：`git log`、`git diff`、`git reflog`、`.git` 都可检查。主要体验成本在副作用：Pi rewind 会和用户的源代码管理状态交织在一起，尤其是 index、stash、hidden refs、ignored artifacts。

未来改进方向：

- AgentVFS extension 应提供 `/agentvfs status` 作为明确的运行面板，展示 mount、socket、workspace、checkpoint 数、last restore、tree integration 状态。
- `/tree` integration 应优先使用 explicit checkpoint entry，时间戳匹配只作为 fallback。
- 文档和 CLI 输出应避免让用户误以为原始 source directory 会自动同步或自动回滚。

## 建议

建议继续把 AgentVFS 作为 Pi rewind 的主路径，原因是它更符合 agent runtime 的高保真需求：

- 能恢复 agent 文件系统副作用，而不只限于 Git tracked files；
- 不污染用户 Git history、index、stash；
- 支持 ignored、untracked、generated 文件；
- 能自然对齐 Pi `/tree` 和 fork；
- 后续可以和 per-agent branches、telemetry、policy、transactional tool rollback 合并。

Git-based rewind 更适合作为 fallback 或轻量模式：

- 用户不想安装或启动 daemon/FUSE；
- 当前平台的 AgentVFS mount/workspace 支持还不够完整；
- 项目只需要 tracked source file rewind；
- 用户希望所有状态都能用 Git 工具检查；
- session 较短，不要求完整恢复 generated/ignored artifacts。

合理的产品路线不是用 Git 替代 AgentVFS，而是：

- AgentVFS：默认高保真后端。
- Git fallback：明确受限的轻量后端，默认 tracked-only。
- Benchmark：补齐 Git driver，用实测数据决定是否值得投入更复杂的 Git all-files 模式。

## Benchmark 补齐计划

为了把性能比较从机制判断升级为实测结论，建议给 `benchmarks/agent-sim` 增加 Git driver，至少包含两个模式：

- `git-tracked`：只 stage/commit tracked source paths。
- `git-all`：包含 untracked 文件，并明确 ignored 文件策略。

需要输出与现有 harness 一致的指标：

- checkpoint latency；
- rollback latency；
- branch/fork creation latency；
- total wall time；
- peak storage bytes，包括 `.git`；
- untracked / ignored 文件场景下的成功率和语义差异。

在 Git driver 完成之前，本文的性能部分应视为基于架构机制的预期，而不是 benchmark 结论。
