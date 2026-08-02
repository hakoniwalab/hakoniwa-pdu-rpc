# Hakoniwa Actionの状態モデル

> **Status: Draft**  
> 本文書はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 1. 目的

本書では、Hakoniwa Action Protocolにおける、accept済みGoalのRuntime状態とイベント処理規則を定義します。

状態名を先に増やすのではなく、次のマトリクスを中心に設計します。

```text
縦軸: 発生し得るイベント
横軸: Goal Runtime state
各セル: 判定、Action、次状態
```

この方法により、正常な状態遷移だけでなく、非同期処理によって生じる重複、競合、遅延イベントを明示的に検討します。

## 2. 適用範囲

本状態モデルは、acceptされた1つのGoalを`goal_id`単位で管理するServer Runtimeのモデルです。

以下は本状態モデルへ含めません。

- Action Type全体の状態
- Application内部のworker、thread、task、queueの状態
- Goal Requestのaccept/reject判定前の一時Context
- Client Runtimeのローカルな送受信待ち状態
- Transport接続そのものの状態
- Goalインスタンスを特定する前のRuntime dispatcher処理

同一Action Typeに複数Goalが存在する場合、それぞれが独立した状態を持ちます。

```text
Action Type A
  goal_id = X : DOING
  goal_id = Y : CANCELING
  goal_id = Z : FINISHING
```

## 3. Goalインスタンスの生成と破棄

Goal RequestがRuntimeおよびApplicationの検査を通過し、Applicationがacceptした時点で、Goalインスタンスを生成します。

```text
Goal Request
  -> Runtime validation
  -> Application decision
       -> reject: Goalインスタンスを生成しない
       -> accept: GoalインスタンスをDOINGで生成する
```

初期状態を表す実体stateは設けません。状態図上ではinitial pseudo-stateから`DOING`へ遷移します。

```text
[*] -> DOING
```

Goalの終端Result送信処理が完了し、Runtimeの保持責務が終了した時点でGoalインスタンスを破棄します。

```text
FINISHING -> [*]
```

インスタンス不存在は、`RELEASED`などの状態値では表現しません。

## 4. MUST状態

Runtimeはaccept済みGoalについて、少なくとも次の3状態を管理します。

```text
DOING
CANCELING
FINISHING
```

### 4.1 DOING

ApplicationがGoalを実行している状態です。

### 4.2 CANCELING

ApplicationがCancel Requestをacceptし、非同期の停止処理を行っている状態です。

Cancel Requestを受信しただけでは`CANCELING`へ遷移しません。ApplicationがCancelをacceptした時点で遷移します。

```text
DOING
  -> Cancel Request received
  -> Application notified
  -> Application accepts cancel
  -> CANCELING
```

ApplicationがCancelをrejectした場合は`DOING`を維持します。

### 4.3 FINISHING

Applicationが終端statusおよびResult bodyを確定し、RuntimeがResult送信とGoalインスタンス破棄を処理している状態です。

```text
DOING      -> FINISHING
CANCELING  -> FINISHING
```

`FINISHING`は、Applicationの完了通知とResult送信完了の間に到着するFeedback、Cancel、重複完了などを制御するために必要です。

## 5. 基本状態遷移

```text
normal:
  [*] -> DOING -> FINISHING -> [*]

cancel:
  [*] -> DOING -> CANCELING -> FINISHING -> [*]

cancel rejected:
  DOING -> DOING
```

## 6. イベント発生元

### 6.1 Server Application起因

- `PUBLISH_FEEDBACK(goal_id, feedback_body)`
- `COMPLETE_SUCCEEDED(goal_id, result_body)`
- `COMPLETE_CANCELED(goal_id, result_body)`
- `COMPLETE_ABORTED(goal_id, result_body)`
- `ACCEPT_CANCEL(goal_id)`
- `REJECT_CANCEL(goal_id, reason)`

### 6.2 Client / Protocol起因

- `CANCEL_REQUEST_RECEIVED(goal_id)`
- `DUPLICATE_CANCEL_REQUEST_RECEIVED(goal_id)`
- `DUPLICATE_GOAL_REQUEST_RECEIVED(goal_id)`

### 6.3 Server Runtime / Transport起因

- `FEEDBACK_SEND_COMPLETED(goal_id)`
- `FEEDBACK_SEND_FAILED(goal_id)`
- `RESULT_SEND_COMPLETED(goal_id)`
- `RESULT_SEND_FAILED(goal_id)`
- `TRANSPORT_DISCONNECTED`
- `APPLICATION_RESPONSE_TIMEOUT(goal_id)`
- `SERVER_SHUTDOWN_REQUESTED`
- `RUNTIME_FORCED_TERMINATION(goal_id, reason)`

イベント名は説明用の抽象名です。公開APIの関数名やPDU種別をこの文書では確定しません。

## 7. Runtime dispatcherで扱うイベント

`UNKNOWN_GOAL_ID_REQUEST_RECEIVED(goal_id)`は、対応するGoalインスタンスが存在しない場合に発生するため、per-goal状態マトリクスには含めません。

```text
Runtime dispatcher
  -> goal_id lookup
       -> found: per-goal state matrixへ配送
       -> not found: unknown-goal handling
```

unknown-goalへの応答内容は、後続のProtocolおよびError規約で定義します。

## 8. セルの記述形式

各マトリクスセルでは、次の三要素を定義します。

```text
Decision:
  ALLOW
  PROTOCOL_REJECT
  APPLICATION_API_ERROR
  INVARIANT_VIOLATION
  IGNORE
  IDEMPOTENT
  DEFER

Action:
  RuntimeまたはApplicationが実行する処理

Next:
  DOING / CANCELING / FINISHING / RELEASE / SAME
```

- `ALLOW`: 現在状態で正規に受理する。
- `PROTOCOL_REJECT`: Client／Protocol起因の要求を受理せず、Protocol応答を返す。
- `APPLICATION_API_ERROR`: Server ApplicationからRuntimeへの不正操作としてApplicationへエラーを返す。
- `INVARIANT_VIOLATION`: Runtime内部で本来発生しないイベントを検出したことを表す。
- `IGNORE`: 副作用を起こさず破棄する。
- `IDEMPOTENT`: 以前と同じ応答または結果を返し、状態を変えない。
- `DEFER`: 判断または処理を別主体へ委譲し、現在状態を維持する。
- `SAME`: 状態を維持する。
- `RELEASE`: Goalインスタンスを破棄する。

## 9. Server Applicationイベント × 状態マトリクス

| Event | DOING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `PUBLISH_FEEDBACK` | `ALLOW`: Feedbackを採番・送信。`SAME` | **レビュー対象**: 原則`APPLICATION_API_ERROR`。停止進捗を許す場合は`ALLOW`。`SAME` | `APPLICATION_API_ERROR`: Result確定後のFeedback。`SAME` |
| `COMPLETE_SUCCEEDED` | `ALLOW`: terminal statusとResultを確定。`FINISHING` | **レビュー対象**: `APPLICATION_API_ERROR`または競合規約適用。`SAME` | `APPLICATION_API_ERROR`または冪等判定。`SAME` |
| `COMPLETE_CANCELED` | 原則`APPLICATION_API_ERROR`: Cancel未受理。`SAME` | `ALLOW`: Canceled Resultを確定。`FINISHING` | `APPLICATION_API_ERROR`または冪等判定。`SAME` |
| `COMPLETE_ABORTED` | `ALLOW`: Aborted Resultを確定。`FINISHING` | `ALLOW`: Cancel処理中のabortを許容。`FINISHING` | `APPLICATION_API_ERROR`または冪等判定。`SAME` |
| `ACCEPT_CANCEL` | `cancel_decision_pending=true`なら`ALLOW`し`CANCELING`へ。falseなら`APPLICATION_API_ERROR`で`SAME` | `IDEMPOTENT`または`APPLICATION_API_ERROR`: 重複accept。`SAME` | `APPLICATION_API_ERROR`: すでにResult確定済み。`SAME` |
| `REJECT_CANCEL` | `cancel_decision_pending=true`なら`ALLOW`しCancel Responseを返して`SAME`。falseなら`APPLICATION_API_ERROR` | `APPLICATION_API_ERROR`: すでにcancel accept済み。`SAME` | `APPLICATION_API_ERROR`: すでにResult確定済み。`SAME` |

`ACCEPT_CANCEL`および`REJECT_CANCEL`は、Cancel判断待ちContextが存在する場合だけ有効です。Contextが存在しない呼び出しは、Protocol上の拒否ではなくApplication APIの誤用として扱います。

## 10. Client / Protocolイベント × 状態マトリクス

| Event | DOING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `CANCEL_REQUEST_RECEIVED` | `DEFER`: Applicationへ通知。判断までは`SAME` | **レビュー対象**: 冪等に既存Cancel Responseを返す、または重複として拒否。`SAME` | **レビュー対象**: completion committedとして拒否、または別の規約を適用。`SAME` |
| `DUPLICATE_CANCEL_REQUEST_RECEIVED` | Cancel判断中なら重複Policyを適用。`SAME` | 既存のCancel受理結果を再応答する候補。`SAME` | 完了処理中として拒否する候補。`SAME` |
| `DUPLICATE_GOAL_REQUEST_RECEIVED` | `PROTOCOL_REJECT`。既存Goalは`SAME` | `PROTOCOL_REJECT`。既存Goalは`SAME` | `PROTOCOL_REJECT`または再照会Policy。既存Goalは`SAME` |

## 11. Cancel判断待ちContext

`CANCEL_REQUEST_RECEIVED`からApplicationの`ACCEPT_CANCEL`または`REJECT_CANCEL`まで、Goalの主状態は`DOING`を維持します。

```text
cancel_decision_pending = true / false
```

これはGoalの実行状態ではなく、未完了のProtocol要求を相関するためのRuntime管理情報です。

## 12. Runtime / Transportイベント × 状態マトリクス

| Event | DOING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `FEEDBACK_SEND_COMPLETED` | `ALLOW`: 送信済み情報を更新。`SAME` | 送信開始済みFeedbackについて完了処理。`SAME` | 送信開始済みFeedbackについて完了処理。`SAME` |
| `FEEDBACK_SEND_FAILED` | Runtime Policyに従いdrop、retry、Application通知。`SAME` | 同左。`SAME` | 同左。ただしResult送信を妨げない。`SAME` |
| `RESULT_SEND_COMPLETED` | `INVARIANT_VIOLATION`: Result未確定 | `INVARIANT_VIOLATION`: Result未確定 | `ALLOW`: 保持責務完了後に`RELEASE` |
| `RESULT_SEND_FAILED` | `INVARIANT_VIOLATION`: Result未確定 | `INVARIANT_VIOLATION`: Result未確定 | retry、保持、Runtime error通知のPolicyを適用。`SAME`または`RELEASE`は後続規約で決定 |
| `TRANSPORT_DISCONNECTED` | Goal実行を継続、abort、保持のいずれかを設定・Protocolで決定 | 停止処理を継続するかを設定・Protocolで決定 | Result保持・再送・破棄Policyを決定 |
| `APPLICATION_RESPONSE_TIMEOUT` | Cancel判断timeoutとして扱う場合、RuntimeがCancel Responseを拒否する候補。`SAME` | 通常は対象外 | 通常は対象外 |
| `SERVER_SHUTDOWN_REQUESTED` | shutdown policyへ`DEFER`。`SAME`または`FINISHING`は後続規約で決定 | shutdown policyへ`DEFER`。`SAME`または`FINISHING`は後続規約で決定 | Result配送・保持を含むshutdown policyへ`DEFER` |
| `RUNTIME_FORCED_TERMINATION` | 可能ならterminal通知を試み、Runtime errorを記録。解放条件は後続規約で決定 | 同左 | Result配送不能を含むRuntime errorを記録し、解放条件は後続規約で決定 |

### 12.1 ShutdownとRuntime強制終了

`SERVER_SHUTDOWN_REQUESTED`は、graceful shutdown、猶予時間、強制終了などのPolicyへ委譲します。本状態モデルでは具体的なtimeoutや停止方式を固定しません。

`RUNTIME_FORCED_TERMINATION`は、Runtime自身の資源枯渇や内部障害によってGoal処理を継続できないケースを表します。この場合、terminal Resultを送信できるとは限りません。

## 13. イレギュラーケースの洗い出し

後続のエラー・競合規約で、少なくとも以下を確定します。

- Result確定とFeedback発行の競合
- 通常完了とCancel Requestの競合
- Cancel受理と通常成功の競合
- 重複完了
- `FINISHING`中のCancel
- Result送信失敗
- Transport切断
- Server shutdown中の既存Goal
- Runtime強制終了時の通知・解放

## 14. Feedback規則

Feedbackの発行契機および周期はServer Applicationが決定します。

Protocolは次のみを規定します。

- Feedbackは0回以上発行できる。
- Feedbackは非終端通知である。
- Feedbackは`goal_id`で相関する。
- `sequence_no`はGoalごとに単調増加する。
- Runtimeが`sequence_no`を採番することを推奨する。
- `FINISHING`へ遷移した後、新規Feedbackを受理しない。

`CANCELING`中のFeedbackを許可するかはレビュー対象です。

## 15. 現時点の設計判断

- GoalインスタンスはApplicationのaccept後、`DOING`で生成する。
- accept済みGoalは`DOING`、`CANCELING`、`FINISHING`の3状態をMUSTで持つ。
- `UNKNOWN_GOAL_ID_REQUEST_RECEIVED`はper-goal状態マトリクスではなくRuntime dispatcherで扱う。
- Cancel Request受信だけでは`CANCELING`へ遷移しない。
- `ACCEPT_CANCEL`および`REJECT_CANCEL`はCancel判断待ちContextが存在する場合だけ有効とする。
- Application APIの誤用、Protocol拒否、Runtime不変条件違反を区別する。
- terminal statusとResult確定時に`FINISHING`へ遷移する。
- `SERVER_SHUTDOWN_REQUESTED`と`RUNTIME_FORCED_TERMINATION`をRuntime起因イベントとして検討対象に含める。
- Result送信後のRuntime保持責務が完了した時点でGoalインスタンスを破棄する。

## 16. レビューで確認する事項

1. `CANCELING`中のFeedbackを許可するか。
2. `CANCELING`中の`COMPLETE_SUCCEEDED`を常にApplication API Errorとするか。
3. `DOING`中の`COMPLETE_CANCELED`を常にApplication API Errorとするか。
4. Cancel判断待ち中に通常完了した場合、未回答Cancelへ何を返すか。
5. `CANCELING`中の重複Cancel Requestを冪等応答にするか。
6. `FINISHING`中のCancel Requestへ何を返すか。
7. 重複`COMPLETE_*`を冪等またはエラーのどちらにするか。
8. Result送信失敗時の保持、再送、解放条件をどこで定義するか。
9. shutdown policyをProtocol、Runtime設定、Application Policyのどこへ置くか。
10. Runtime強制終了時にterminal通知を試行する条件をどう定義するか。

## 17. 対象外

- PDUのバイトレイアウト
- 公開APIの具体的な関数シグネチャ
- Transport固有のretry実装
- Application内部のworkerおよびqueue状態
- 状態・イベント処理の排他実装
- Resultおよび終了済み`goal_id`の具体的な保持時間
