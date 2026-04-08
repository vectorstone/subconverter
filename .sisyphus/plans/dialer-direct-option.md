# 为 Dialer 组添加 DIRECT 节点

## TL;DR

> **核心目标**：在 `📞 dialer 节点` proxy-group 中添加 `DIRECT` 选项，允许特定场景下走直连。
> 
> **交付物**：修改 `src/generator/config/subexport.cpp` 中 dialer 组的构建逻辑
> 
> **预估工作量**：快速（single-file change）

---

## Context

### 原始请求
用户要求在 proxy-groups 中的 "dialer 节点" group 里增加一个 direct 节点，以便个别场景下需要走直连。

### 代码分析结果
通过探索发现：
- **dialer 组定义位置**：`src/generator/config/subexport.cpp:822-833`
- **当前行为**：dialer 组仅包含标记为 dialer 的节点列表（`dialer_nodes`）
- **组名**：`📞 dialer 节点`（定义在第 32 行）
- **组类型**：`select`（选择性代理组）

### 参考模式
在其他 proxy-group 构建逻辑中（第 795-796 行），当 `filtered_nodelist` 为空时会添加 `DIRECT` 作为默认选项。

---

## 工作Objective

### 核心目标
修改 dialer 组的构建逻辑，在代理列表中显式添加 `DIRECT` 选项。

### 具体交付物
- 修改 `src/generator/config/subexport.cpp:827` 行附近
- 在 `dialer_group["proxies"]` 中添加 `DIRECT` 作为第一个或最后一个选项

### 完成定义
- [ ] 编译通过：`cmake --build build -j` 成功
- [ ] 运行时验证：启动服务后生成的 Clash 配置中 `📞 dialer 节点` 组包含 `DIRECT` 选项

### Must Have
- dialer 组的 `proxies` 数组包含 `DIRECT`
- 保持现有 dialer 节点列表不变
- 遵循现有代码风格（4 空格缩进，brace 风格一致）

### Must NOT Have（防护栏）
- 不修改 dialer 节点检测逻辑
- 不改变其他 proxy-group 的行为
- 不添加新的配置项或参数

---

## 验证策略

> **零人工干预** — 所有验证通过命令执行。

### 测试决策
- **基础设施存在**：NO（项目无正式单元测试）
- **自动化测试**：NO
- **验证方式**：Agent-Executed QA Scenarios（手动场景验证）

### QA 策略
每个任务必须包含 agent-executed QA 场景（详见 TODO 模板）。

---

## 执行策略

### 并行执行波

> 单一文件修改，顺序执行。

```
Wave 1（唯一步骤）:
└── Task 1: 修改 dialer 组构建逻辑 [quick]
```

### 依赖矩阵
- **Task 1**: —（可立即开始）

### Agent 调度摘要
- **Wave 1**: **1** — T1 → `quick`

---

## TODOs

- [ ] 1. 为 dialer 组添加 DIRECT 选项

  **做什么**：
  - 定位到 `src/generator/config/subexport.cpp:822-833`
  - 在构建 `dialer_group["proxies"]` 时，先添加 `"DIRECT"` 到代理列表
  - 然后追加 `dialer_nodes` 列表
  
  **参考代码模式**：
  ```cpp
  if(has_dialer_nodes)
  {
      YAML::Node dialer_group;
      dialer_group["name"] = dialer_group_name;
      dialer_group["type"] = "select";
      dialer_group["proxies"].push_back("DIRECT");  // 新增这行
      for(const auto &node : dialer_nodes)
          dialer_group["proxies"].push_back(node);
      // ... rest of the code
  }
  ```
  
  **禁止做**：
  - 不修改 dialer 节点检测逻辑（第 73-89 行）
  - 不改变其他 proxy-group 的构建逻辑
  
  **推荐 Agent 配置**：
  - **Category**: `quick`
    - 原因：单一文件局部修改，逻辑简单
  - **Skills**: `[]`
    - 无需特殊技能，标准 C++ 编辑
  
  **并行化**：
  - **可并行**: NO
  - **并行组**: 顺序执行
  - **阻塞**: 无
  - **被阻塞**: 无
  
  **参考**：
  - `src/generator/config/subexport.cpp:795-796` - 参考 DIRECT 添加模式
  - `src/generator/config/subexport.cpp:822-833` - 目标修改位置
  
  **验收标准**：
  - [ ] 代码编译通过
  - [ ] 生成的 YAML 中 dialer 组包含 DIRECT 选项
  
  **QA 场景**：
  
  ```
  场景：验证 dialer 组包含 DIRECT
    工具：Bash (curl + yq 或 grep)
    前置条件：服务已启动，监听 25500 端口
    步骤：
      1. 准备测试订阅（包含至少一个 dialer 节点）
      2. curl http://127.0.0.1:25500/sub?target=clash&url=<test-subscription>
      3. 检查输出中 "📞 dialer 节点" 组的 proxies 字段
    预期结果：proxies 数组包含 "DIRECT" 和 dialer 节点
    失败指标：grep 输出不包含 DIRECT 或格式错误
    证据：.sisyphus/evidence/task-1-dialer-group-verify.txt
  ```
  
  **证据捕获**：
  - [ ] 保存生成的 Clash 配置片段到证据文件
  
  **提交**: YES
  - 消息：`feat: add DIRECT option to dialer proxy-group`
  - 文件：`src/generator/config/subexport.cpp`
  - 预提交：`cmake --build build -j`

---

## 最终验证波

> 单一任务，无需并行验证。

- [ ] F1. **代码验证** — `quick`
  读取修改后的文件，确认：
  - DIRECT 在 proxies 数组开头
  - dialer_nodes 紧随其后
  - 语法正确，缩进一致
  输出：`Code [PASS/FAIL] | VERDICT`

---

## 提交策略

- **1**: `feat: add DIRECT option to dialer proxy-group` — subexport.cpp, cmake --build build -j

---

## 成功标准

### 验证命令
```bash
# 构建验证
cmake --build build -j  # 预期：无错误，生成 subconverter 二进制

# 运行时验证（人工或 agent 执行）
curl http://127.0.0.1:25500/sub?target=clash&url=<test-sub> | grep -A5 "📞 dialer 节点"
# 预期输出包含：proxies: [DIRECT, <dialer-node-1>, ...]
```

### 最终检查清单
- [ ] DIRECT 选项存在于 dialer 组
- [ ] 编译成功
- [ ] 代码风格一致