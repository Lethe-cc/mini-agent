# mini-agent 技术方案 020：状态流转集中描述（state machines）

## 文档状态

- 状态：**草案 / 待人工评审**（2026-11-20）。把散落在 007/008/010/013/017/018、
  architecture.md、ADR 001/006 与 `tui.hpp`/`tui_bridge.hpp`/`loop.cpp` 的状态流转
  集中为单一权威描述；不改变任何行为。图均用 PlantUML。
- 核对基线：main `3e3cbff`。代码缝：`ConnState`/`AuthProbe`/`connection_state`
  （tui.hpp）、`ApprovalWait`/`ApprovalKey`/`TurnBridge`（tui_bridge.hpp）、
  `run_turn`（core/loop.cpp）、各 worker（tui_workers.cpp）。

## 1. 连接/认证状态（ConnState + AuthProbe）

`connection_state(config, probe, listing)` **纯派生、不触网**；提示符 glyph+颜色双编码。
`AuthProbe` 证据来自 `/models` 列表与聊天回合（`source=chat`），凭据一变即重置。

```plantuml
@startuml conn-state
[*] --> Unconfigured : base_url/api_key 缺失
Unconfigured --> Unverified : 配置齐备（未探测）
Unverified --> Checking : 发起 /models 或 chat 探测
Checking --> Verified : 2xx 可用响应
Checking --> Rejected : 401/403
Checking --> Unreachable : 无响应（网络错误）
Checking --> Unverified : 非_auth 状态码
Verified --> Unverified : 凭据变更（重置）
Rejected --> Unverified : 凭据变更
Unreachable --> Unverified : 凭据变更
Verified --> Checking : 重新探测
@enduml
```

verdict 存于 per-profile `auth_probes` map（v6/v7），`auth` 镜像活动档案；
`auth_log_token` ∈ {untried, verified, rejected, unreachable, refused}。

## 2. 回合生命周期（run_turn + join-then-read）

worker 只写 bridge/result；主线程 join 后才读并提交历史（失败/中断不回滚）。

```plantuml
@startuml turn-lifecycle
[*] --> Idle
Idle --> Streaming : start_turn（快照 config/chat，worker 启动）
Streaming --> Streaming : on_delta 追加 bridge（主线程帧抽干）
Streaming --> Commit : run_turn 成功（仅此时写 chat）
Streaming --> Aborted : abort_requested（Ctrl-C/Esc）
Streaming --> Failed : ProviderError（失败轮回滚，历史不变）
Commit --> Idle : join + 提交 + 保存 session + 抽干 pending 队列
Aborted --> Idle : join（不落盘）
Failed --> Idle : join（不落盘）
Idle --> [*] : Ctrl-C 二次 / Ctrl-D / /exit
@enduml
```

max-rounds：末轮无工具摘要；提示折进首位 System（008 D7），无首位退化为末尾 User
（提交按下标跳过，永不落盘）。

## 3. 审批流（TurnBridge 固定槽位+序号）

```plantuml
@startuml approval
[*] --> Published : worker publish_approval(call, preview)（arm 竞态守卫）
Published --> Waiting : 主线程渲染审批块（AWAITING APPROVAL）
Waiting --> Answer : y/n/a/?（ApprovalKey Allow/Deny/AllowSession）
Waiting --> Timeout : 120s 自动拒（90s 起显示倒计时）
Waiting --> Aborted : abort_requested
Answer --> Allow : y / a（a 记入 session_allowed）
Answer --> Deny : n / Esc / Ctrl-C（disarm 预览守卫）
Timeout --> Deny
Aborted --> Deny
Allow --> Executed : worker 重读一致才执行
Deny --> [*]
Executed --> [*]
@enduml
```

策略 `tool_approval=ask|allowlist|off`；off/allowlist 命中或 session 已允许→直接 Allow。
审批期间键盘被审批独占（y/n/a/?/Esc/Ctrl-C），其余字符吞掉。

## 4. 后台 worker（models/probe/compact/sidebar）

统一 **running/done/stale + join-then-read**：扫描在飞时不 join 主线程（置 stale，
完成后重发）；screen/logger 仅主线程。

```plantuml
@startuml worker
[*] --> Idle
Idle --> Running : start_*（快照入线程）
Running --> Done : 写 result + done=true
Done --> Idle : 主线程 join 后读 result、渲染/折叠
Running --> Stale : 期间再次请求（不 join）
Stale --> Running : 在飞完成后重发
@enduml
```

## 5. 预览 pane 模态（018）

```plantuml
@startuml preview
[*] --> Closed
Closed --> Open : /preview 或侧栏 p（空闲门控）
Open --> Open : 滚轮/方向键滚动（resize 重渲染）
Open --> Closed : Esc / Ctrl-C / Enter(关闭后发送) / 菜单接管
Closed --> Closed : streaming/approval/compact 期间拒绝打开
@enduml
```

## 6. session 生命周期（ADR 001）

temp（不落盘）/ normal（原子写+锁）；`/session new|<id>` 进程内切换；
退出时 normal 保存、temp 丢弃；`.lock` 防同 id 并发。

## 7. 详细流程图（activity / sequence，代码核对）

### 7.1 run_turn 工具循环（core/loop.cpp）

```plantuml
@startuml run-turn-activity
start
:staging = copy(chat) + user;
:round = 0; done = false;
repeat
  :last_round = (round == max);
  :tools_for_round = (tools_enabled && !last_round) ? tools_json : "";
  if (tools_enabled && last_round?) then (yes)
    :append_system_note 折进首位 System;
    if (无首位 System?) then (yes)
      :staging.add(末尾 User 提示);
      :limit_note = idx;
    endif
  endif
  :result = ask_provider(staging, tools_for_round);
  :累加 usage / reasoning（prompt 取末轮）;
  if (!tools_enabled || !has_tool_calls || last_round?) then (yes)
    :final_text = result.text;
    :done = true;
  else (no)
    :逐个 tool_call 顺序执行
    (审批 Allow 才 execute; Deny→denied;
    abort→throw Cancelled;
    staging.add(tool message));
  endif
  :++round;
repeat while (round <= max && !done?) is (yes) not (no)
:commit：chat.add(user) + staging[prefix+1..]
跳过 limit_note 与 System（仅成功时）;
:usage_out / reasoning_out 回传; return final_text;
stop
@enduml
```

失败/中断抛 ProviderError → 不 commit，历史停在上一轮边界（007 §5、008 §4）。

### 7.2 中断/Esc 优先级（tui.cpp 输入分支）

```plantuml
@startuml esc-precedence
start
:收到 Esc（非 CSI 序列）;
if (preview_pane.open?) then (yes)
  :close_preview_pane;
elseif (sidebar_focus?) then (yes)
  :sidebar_focus = false;
elseif (streaming || compact_running?) then (yes)
  :abort_requested = true;
elseif (reasoning_menu_open?) then (yes)
  :dismiss reasoning menu;
elseif (model_menu_open?) then (yes)
  :dismiss model menu;
else (idle)
  :无动作;
endif
stop
@enduml
```

Ctrl-C 升级链：preview→关 pane；streaming/compact→首次 abort、已 abort 再按→退出；
idle→2s 内二次→退出，否则清 composer+提示。Ctrl-D 直接退出。审批期间键盘被审批独占。

### 7.3 审批 worker↔主线程时序（tui_bridge.hpp）

```plantuml
@startuml approval-sequence
participant Worker as W
participant TurnBridge as B
participant Main as M
W -> B : publish_approval(call, preview)（arm 守卫）seq
M -> B : 渲染审批块（extra_rows）
M -> B : answer_approval(y/n/a/?) / 超时 / abort
W -> B : wait_approval(seq,120s) → Answer/Timeout/Aborted
alt Allow
  W -> W : 重读一致才 execute_one
else Deny/Timeout/Aborted
  W -> B : disarm + push_tool_event(未执行)
end
W -> B : finish_running_tool(round)
M -> B : drain_tool_events → 历史 [tool] 行
@enduml
```

### 7.4 /compact 流程（tui_workers.cpp，015/017）

```plantuml
@startuml compact-activity
start
:backup = save_backup(session);
:cut = find_retained_tail(msgs, 20000);
:tail = msgs[cut..]; region = msgs[..cut];
:previous = 旧 marked summary; prune_tool_outputs(region,4);
:summary = provider.complete(build_compact_request);
if (无模板 && !abort?) then (yes)
  :NUDGE 重试; summary = complete(nudge);
  if (仍无模板?) then (yes)
    :error;
  endif
endif
if (abort || error?) then (yes)
  :历史不动;
  stop
endif
:主线程 join 后：chat = system + marked summary + tail;
:save; 报告 pruned/nudged;
stop
@enduml
```

### 7.5 帧渲染管线（tui_frame.hpp，007 §5.6）

```plantuml
@startuml frame-activity
start
:poll(stdin, frame_ms);
if (有输入?) then (yes)
  :键位分发（7.2）;
endif
:drain_tool_events / 完成 handler（models/probe/compact/sidebar/turn）;
:tail = streaming ? deltas_snapshot : "";
:extra_rows = 审批块 > turn panel > 预览 pane;
:composer_display（反显光标）/ prompt=connection_state / status / sysinfo / sidebar;
:screen.render(行级 diff + CSI 2026);
stop
@enduml
```

## 8. 状态转移表（节选）

| 机器 | 当前态 | 事件 | 下一态 | 副作用 |
| --- | --- | --- | --- | --- |
| conn | Unverified | probe 2xx | Verified | 提示符 `>` 绿 |
| conn | Checking | 401/403 | Rejected | `!` 红 |
| conn | * | 凭据变更 | Unverified | 重置 AuthProbe |
| turn | Streaming | run_turn ok | Commit | 写 chat+save |
| turn | Streaming | abort | Aborted | 不落盘 |
| approval | Waiting | 120s | Timeout→Deny | disarm |
| worker | Running | 再请求 | Stale | 不 join，完成后重发 |
| pane | Open | Esc | Closed | 恢复鼠标 |

## 9. 引用

007 §5.6（帧/中断）、008 §5.4（审批）、010 §13（probe）、013（profile/auth）、
017（compact）、018（pane）、ADR 001（session）、ADR 006（审批策略）。