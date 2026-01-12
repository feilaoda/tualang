# CAI 开发 TODO（不属于规范）

约定：
- 优先级：`P0`（阻塞/基础）、`P1`（重要）、`P2`（可延后）
- 完成标记：完成后改为 `[x]`；未完成为 `[ ]`
- “完整实现”要求：任务的 DoD（Definition of Done）必须满足通用场景，禁止为单一 case 写死逻辑，避免将来返工

## 编译器前端（Parser/AST/诊断/模块）
- [ ] P0 解析器生成管线（ANTLR4 → 生成代码 → 可复现构建）
  - DoD：`caic`/构建脚本可在干净环境生成解析器；生成物不需要手工提交；增量构建稳定
- [ ] P0 语法树 AST（带 span/文件/行列信息）
  - DoD：所有语法节点携带源位置信息；错误时能定位到 token 级别；支持“尽量继续解析”收集多错误
- [ ] P0 统一诊断系统（error code + 消息模板 + span 高亮）
  - DoD：支持多错误汇总；错误码可稳定引用；输出格式跨平台一致
- [ ] P0 模块加载与依赖图（DAG）构建
  - DoD：按规范处理初始化顺序与循环依赖报错；支持 `import/from/as`；依赖图可用于后续增量编译
- [ ] P0 名称解析（作用域/遮蔽/qualified name）
  - DoD：支持局部/顶层/模块成员；在 AST 上输出解析结果（symbol id），禁止后端再猜名字
- [ ] P0 可见性（`private`）与 API 边界
  - DoD：跨模块访问在语义阶段阻止；错误信息包含“声明位置”和“访问位置”

## 类型系统与语义（基础类型优先）
- [ ] P0 基础类型与字面量：`int/long/.../bool/byte/string/null`
  - DoD：类型推导与显式标注一致；数值转换/溢出/比较规则可测试；`null` 规则可测试
- [ ] P0 `fn`：参数模式（`const/let/move`）、多返回、lambda、泛型（最小可用）
  - DoD：多返回与 ABI/调用约定一致；泛型采用可扩展的单态化/字典传递设计（选其一并定型），避免后续推翻
- [ ] P0 `struct`：字段/方法/`init`/`deinit`/嵌入字段
  - DoD：默认 `repr(C)` 数据布局；递归/循环嵌套按规范强制使用 `std.memory.Box<T>` 断开布局
- [ ] P0 `enum`：tag 联合体布局与 payload
  - DoD：布局与判别值规则可验证；`match` 的 exhaustiveness（穷尽性）检查可测试
- [ ] P0 `object`：命名空间与静态成员
  - DoD：可用作模块内组织单例 API；无隐式捕获外部状态（语义清晰可预测）
- [ ] P0 `trait/impl`：静态分发的一致性规则
  - DoD：方法签名匹配与冲突诊断清晰；impl 选择规则可预测；禁止依赖特定编译顺序的“偶然解析”

## 所有权/引用/受限指针（避免暴露裸指针）
- [ ] P0 所有权与 move/drop 语义（含 `defer`）
  - DoD：可用静态规则描述资源释放时机；drop 顺序与作用域规则可测试
- [ ] P0 `Ref<T>` / `&T`：借用与别名规则
  - DoD：只读/可写借用互斥；跨函数借用传递规则清晰；错误定位到借用冲突的两端
- [ ] P0 `Ptr<T>`：受限指针（显式能力边界）
  - DoD：无“隐式裸指针”逃逸；只有在 `unsafe` 或特定 API 下才能构造/解引用；与 FFI 映射规则清晰
- [ ] P0 `Slice<T>` ：编译期可区分读写视图
  - DoD：由类型系统保证读写权限；从 `bytes.slice()/slice(true)` 等构造点能被编译器识别并传播

## 基础标准库类型（先定义语义，再实现库与内建）
- [ ] P0 `string`（UTF-8）与基础操作
  - DoD：索引/切片的安全语义明确；与 `bytes` 互转规则明确；性能边界可预测
- [ ] P0 `bytes`（拥有型字节序列）
  - DoD：与 `Slice<byte>`零拷贝视图；边界检查规则可测试
- [ ] P0 `map`/`map<K,V>` 与字面量 `{k: v}`
  - DoD：字面量的 key 求值顺序固定；哈希/比较约束可诊断；迭代稳定性规则明确
- [ ] P0 `Option<T>`（与 `null` 区分）
  - DoD：模式匹配/解构/guard 支持；避免隐式 null；零成本表示（可选：niche 优化）不破坏语义

## 后端与产物（AOT，先跑通基础类型）
- [ ] P0 MIR（中间表示）与 lowering：AST → MIR
  - DoD：MIR 可用于 borrow/escape 分析与优化；MIR 形式不绑定特定后端
- [ ] P0 LLVM IR 生成：MIR → LLVM IR
  - DoD：基础类型/函数/struct/enum 可生成可执行文件；无 GC；调试信息最小可用
- [ ] P1 AOT 产物：`exe/staticlib/dylib`（按规范）
  - DoD：静态优先链接；符号冲突规则可验证；`extern` 导出与 `#[export_name]`（若存在）可工作

## 测试（按目录分类，保证可回归）
- [ ] P0 Grammar 测试：样例程序必须能被 `cai/parser.g4` 解析
  - DoD：覆盖 `fn/import/struct/object/trait/impl/enum/match/extern` 等语法路径；CI/本地一键运行
- [ ] P0 文档一致性测试：spec index、引用路径、禁止旧路径残留
  - DoD：`cai/docs/spec/README.md` 列出的文件全部存在；仓库内无旧 spec 路径引用
