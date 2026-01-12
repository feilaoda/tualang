# 词法与源代码形式规范

Status: Frozen

本文件冻结 CAI 源代码的“表面形态”规则：编码、换行/分号、注释、标识符、关键字与字面量形式。

> 可解析语法（lexer+parser）的唯一来源是 `cai/parser.g4`；本文件提供规范化文字描述，用于实现与测试对齐。

---

## 1. 源文件
- 源文件编码必须为 UTF-8。
- 行结束支持 `\n` 与 `\r\n`，语义上等价（都产生换行）。

---

## 2. 空白与语句分隔
- 空白字符：`' ' '\t' '\f' '\v' '\r'` 在大多数位置等价于分隔符（不产生 token）。
- 语句分隔符（`SEMI`）来源：
  - 显式分号 `;`
  - 换行（一个或多个 `\r`/`\n` 组合）

因此：同一行中可用 `;` 写多句；也可一行一句依赖换行。

---

## 3. 注释
- 单行注释：`//` 到行尾，忽略。
- 多行注释：`/* ... */`，忽略；不支持嵌套。

---

## 4. 标识符与关键字

### 4.1 标识符
- `Identifier ::= [A-Za-z_][A-Za-z0-9_]*`

### 4.2 关键字保留
- 关键字不能作为标识符使用（例如不能定义变量名为 `if`）。
- CAI 不提供裸指针类型（例如 `ptr`/`void*`）；指针仅以 `Ptr<T>` 这种受限类型出现（见 `cai/docs/spec/ptr.md`）。

---

## 5. 字面量

### 5.1 整数
- 十进制：
  - `INT_LIT ::= Digit+`
  - `LONG_LIT ::= Digit+ ('l'|'L')`

数值范围与溢出规则见 `cai/docs/spec/numeric.md`。

### 5.2 浮点
- `FLOAT_LIT ::= Digit+ '.' Digit+ ExponentPart? | Digit+ ExponentPart`
- `ExponentPart ::= ('e'|'E') ('+'|'-')? Digit+`

### 5.3 字符串
- `STRING_LIT ::= '"' ( EscapeSeq | ~["\\] )* '"'`
- `EscapeSeq ::= '\\' .`

字符串语义（Unicode/UTF-8/不可变）见 `cai/docs/spec/string.md`。

---

## 6. 解析歧义约束
- `{ ... }` 在表达式位置表示 map 字面量；在语句位置表示 block（由 parser 上下文决定）。
- `TypeName{ ... }` 表示 struct 字面量初始化（见 `cai/docs/spec/struct.md`）。
