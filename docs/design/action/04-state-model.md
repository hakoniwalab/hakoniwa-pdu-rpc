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

この状態では、次の操作が発生し得ます。

- Feedback発行
- 通常完了
- 異常終了
- Cancel Request受信
- Cancelのaccept/reject判断

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

### 5.1 通常完了

```text
[*]
  -> DOING
  -> FINISHING
  -> [*]
```

### 5.2 Cancel完了

```text
[*]
  -> DOING
  -> CANCELING
  -> FINISHING
  -> [*]
```

### 5.3 Cancel拒否

```text
DOING
  -> Cancel Request
  -> Application rejects cancel
  -> DOING
```

## 6. イベント発生元

イベントは、発生元ごとに整理します。

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
- `UNKNOWN_GOAL_ID_REQUEST_RECEIVED(goal_id)`

### 6.3 Server Runtime / Transport起因

- `FEEDBACK_SEND_COMPLETED(goal_id)`
- `FEEDBACK_SEND_FAILED(goal_id)`
- `RESULT_SEND_COMPLETED(goal_id)`
- `RESULT_SEND_FAILED(goal_id)`
- `TRANSPORT_DISCONNECTED`
- `APPLICATION_RESPONSE_TIMEOUT(goal_id)`

イベント名は説明用の抽象名です。公開APIの関数名やPDU種別をこの文書では確定しません。

## 7. セルの記述形式

各マトリクスセルでは、次の三要素を定義します。

```text
Decision:
  ALLOW / REJECT / IGNORE / IDEMPOTENT / DEFER

Action:
  RuntimeまたはApplicationが実行する処理

Next:
  DOING / CANCELING / FINISHING / RELEASE / SAME
```

意味は以下です。

- `ALLOW`: 現在状態で正規に受理する。
- `REJECT`: 不正または受理不能としてエラーを返す。
- `IGNORE`: 副作用を起こさず破棄する。
- `IDEMPOTENT`: 以前と同じ応答または結果を返し、状態を変えない。
- `DEFER`: 判断または処理を別主体へ委譲し、現在状態を維持する。
- `SAME`: 状態を維持する。
- `RELEASE`: Goalインスタンスを破棄する。

## 8. Server Applicationイベント × 状態マトリクス

| Event | DOING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `PUBLISH_FEEDBACK` | `ALLOW`: Feedbackを採番・送信。`SAME` | **レビュー対象**: 原則`REJECT`。停止進捗を許す場合は`ALLOW`。`SAME` | `REJECT`: Result確定後のFeedback。`SAME` |
| `COMPLETE_SUCCEEDED` | `ALLOW`: terminal statusとResultを確定。`FINISHING` | `REJECT`または競合規約適用。`SAME` | `REJECT`または冪等判定。`SAME` |
| `COMPLETE_CANCELED` | 原則`REJECT`: Cancel未受理。`SAME` | `ALLOW`: Canceled Resultを確定。`FINISHING` | `REJECT`または冪等判定。`SAME` |
| `COMPLETE_ABORTED` | `ALLOW`: Aborted Resultを確定。`FINISHING` | `ALLOW`: Cancel処理中のabortを許容。`FINISHING` | `REJECT`または冪等判定。`SAME` |
| `ACCEPT_CANCEL` | Cancel判断待ちContextが存在する場合に`ALLOW`。Cancel Responseを確定し、`CANCELING` | `IDEMPOTENT`または`REJECT`: 重複accept。`SAME` | `REJECT`: すでにResult確定済み。`SAME` |
| `REJECT_CANCEL` | Cancel判断待ちContextが存在する場合に`ALLOW`。Cancel Responseを返し、`SAME` | `REJECT`: すでにcancel accept済み。`SAME` | `REJECT`: すでにResult確定済み。`SAME` |

### 8.1 完了イベントの共通Action

`COMPLETE_*`を受理する場合、Runtimeは少なくとも以下を一つの論理操作として行います。

1. terminal statusを確定する。
2. Result bodyを確定する。
3. 状態を`FINISHING`へ変更する。
4. 以降のFeedbackおよび新規Cancelを抑止する。
5. `RESPONSE_KIND_RESULT`を送信する。

状態を`FINISHING`へ変更してからResultを送信することで、完了処理中に到着するイベントとの競合を閉じます。

## 9. Client / Protocolイベント × 状態マトリクス

| Event | DOING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `CANCEL_REQUEST_RECEIVED` | `DEFER`: Applicationへ通知。判断までは`SAME` | **レビュー対象**: 冪等に既存Cancel Responseを返す、または重複として拒否。`SAME` | `REJECT`またはterminal結果を案内。`SAME` |
| `DUPLICATE_CANCEL_REQUEST_RECEIVED` | Cancel判断中なら重複Policyを適用。`SAME` | 既存のCancel受理結果を再応答する候補。`SAME` | 完了処理中として拒否する候補。`SAME` |
| `DUPLICATE_GOAL_REQUEST_RECEIVED` | Runtime rejection。既存Goalは`SAME` | Runtime rejection。既存Goalは`SAME` | Runtime rejectionまたは再照会Policy。既存Goalは`SAME` |
| `UNKNOWN_GOAL_ID_REQUEST_RECEIVED` | 対象インスタンス外のため、現在Goalへ影響なし | 対象インスタンス外のため、現在Goalへ影響なし | 対象インスタンス外のため、現在Goalへ影響なし |

### 9.1 Cancel判断待ち

`CANCEL_REQUEST_RECEIVED`からApplicationの`ACCEPT_CANCEL`または`REJECT_CANCEL`まで、Goalの主状態は`DOING`を維持します。

ただしRuntimeは、同じCancel RequestをApplicationへ重複通知しないため、次のような補助情報を保持する可能性があります。

```text
cancel_decision_pending = true / false
```

これはGoalの実行状態ではなく、未完了のProtocol要求を相関するためのRuntime管理情報です。

## 10. Runtime / Transportイベント × 状態マトリクス

| Event | DOING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `FEEDBACK_SEND_COMPLETED` | `ALLOW`: 送信済み情報を更新。`SAME` | 送信開始済みFeedbackについて完了処理。`SAME` | 送信開始済みFeedbackについて完了処理。`SAME` |
| `FEEDBACK_SEND_FAILED` | Runtime Policyに従いdrop、retry、Application通知。`SAME` | 同左。`SAME` | 同左。ただしResult送信を妨げない。`SAME` |
| `RESULT_SEND_COMPLETED` | `REJECT`: Result未確定 | `REJECT`: Result未確定 | `ALLOW`: 保持責務完了後に`RELEASE` |
| `RESULT_SEND_FAILED` | `REJECT`: Result未確定 | `REJECT`: Result未確定 | retry、保持、Runtime error通知のPolicyを適用。`SAME`または`RELEASE`は後続規約で決定 |
| `TRANSPORT_DISCONNECTED` | Goal実行を継続、abort、保持のいずれかを設定・Protocolで決定 | 停止処理を継続するかを設定・Protocolで決定 | Result保持・再送・破棄Policyを決定 |
| `APPLICATION_RESPONSE_TIMEOUT` | Goal accept後のCancel判断timeoutとして扱う場合、Cancel ResponseをRuntimeが拒否する候補。`SAME` | 通常は対象外 | 通常は対象外 |

## 11. イレギュラーケースの洗い出し

以下は、イベントマトリクスから後続のエラー・競合規約へ送る主要論点です。

### 11.1 Result確定とFeedback発行の競合

```text
Application thread A: COMPLETE_SUCCEEDED
Application thread B: PUBLISH_FEEDBACK
```

Runtimeが先に受理したイベントによって結果を決めます。

- Feedbackが先に受理された場合、そのFeedback送信後に`FINISHING`へ遷移できる。
- Completeが先に受理された場合、`FINISHING`へ遷移し、後続Feedbackを拒否する。

具体的な排他・atomicity要件は後続文書で定義します。

### 11.2 通常完了とCancel Requestの競合

```text
Server Application: COMPLETE_SUCCEEDED
Client: Cancel Request
```

- Completeが先に`FINISHING`へ遷移させた場合、Cancel Requestは受理しない。
- Cancel Requestが先にApplicationへ通知されても、Applicationがacceptする前に通常完了が確定する可能性がある。

Cancel判断待ちと通常完了の優先規則は、後続の競合規約で確定します。

### 11.3 Cancel受理と通常成功の競合

`CANCELING`へ遷移した後の`COMPLETE_SUCCEEDED`は、原則として不正とします。

ただし、停止要求を受けた時点ですでに目的を達成していた場合などを`SUCCEEDED`として許容するかはApplication semanticsに関係するため、レビュー対象です。

### 11.4 重複完了

同じGoalへ複数の`COMPLETE_*`が発行された場合、最初に受理された終端結果だけを有効とします。

後続の完了要求は、次のいずれかとします。

- 同一terminal statusおよび同一Resultなら冪等に成功扱い
- statusやResultに関係なく重複完了エラー

この選択は後続のAPIおよび競合規約で決定します。

### 11.5 FINISHING中のCancel

Resultがすでに確定しているため、新しいCancelはGoalの終端結果を変更しません。

Cancel Responseとして何を返すかは、次の候補があります。

- `REJECTED`: completion already committed
- terminal Resultを再通知する
- unknown/finished Goalとして扱う

### 11.6 Result送信失敗

Result送信に失敗しても、Applicationの処理結果はすでに確定しています。

したがって、`FINISHING`から`DOING`または`CANCELING`へ戻してはなりません。

```text
FINISHING
  -> Result send failed
  -> FINISHINGを維持、またはRuntime Contextをエラー終了
```

再送および保持期間は、Transport非依存のProtocol規約とRuntime設定の境界を後続文書で整理します。

## 12. Feedback規則

Feedbackの発行契機および周期はServer Applicationが決定します。

Protocolは次のみを規定します。

- Feedbackは0回以上発行できる。
- Feedbackは非終端通知である。
- Feedbackは`goal_id`で相関する。
- `sequence_no`はGoalごとに単調増加する。
- Runtimeが`sequence_no`を採番することを推奨する。
- `FINISHING`へ遷移した後、新規Feedbackを受理しない。

`CANCELING`中のFeedbackを許可するかはレビュー対象です。

## 13. 状態をProtocol公開するか

`DOING`、`CANCELING`、`FINISHING`は、まずServer Runtimeがイベント受理可否を判断するための正規状態です。

これらをClientへ明示的に通知するPDUを追加するかは、本状態モデルでは決定しません。

現在のPDU契約では、Clientは次のメッセージからlifecycleを観測します。

```text
Goal Response(ACCEPTED)
Feedback 0..N
Cancel Response(ACCEPTED / REJECTED)
Result(SUCCEEDED / CANCELED / ABORTED)
```

Client側が観測する状態とServer Runtimeの内部状態は同一である必要はありません。

## 14. 現時点の設計判断

- Action Type全体の状態はProtocolに持たない。
- worker状態はApplication実装の責務であり、本状態モデルから除外する。
- Goal Requestがrejectされた場合、Goalインスタンスを生成しない。
- GoalインスタンスはApplicationのaccept後、`DOING`で生成する。
- 初期状態は実体stateにせず、initial pseudo-stateで表現する。
- accept済みGoalは`DOING`、`CANCELING`、`FINISHING`の3状態をMUSTで持つ。
- Cancel Request受信だけでは`CANCELING`へ遷移しない。
- ApplicationがCancelをacceptした時点で`CANCELING`へ遷移する。
- terminal statusとResult確定時に`FINISHING`へ遷移する。
- `FINISHING`中は新規Feedback、Cancel、別の完了要求によって終端結果を変更しない。
- Result送信後のRuntime保持責務が完了した時点でGoalインスタンスを破棄する。
- イベント処理規則は、イベント×状態マトリクスで定義する。

## 15. レビューで確認する事項

1. `CANCELING`中のFeedbackを許可するか。
2. `CANCELING`中の`COMPLETE_SUCCEEDED`を常に拒否するか。
3. `DOING`中の`COMPLETE_CANCELED`を常に拒否するか。
4. Cancel判断待ち中に通常完了した場合、未回答Cancelへ何を返すか。
5. `CANCELING`中の重複Cancel Requestを冪等応答にするか。
6. `FINISHING`中のCancel Requestへ何を返すか。
7. 重複`COMPLETE_*`を冪等またはエラーのどちらにするか。
8. Result送信失敗時の保持、再送、解放条件をどこで定義するか。
9. Transport切断時にApplication実行を継続するかをProtocolが規定するか。
10. 状態をClientへ明示公開する必要があるか。

## 16. 対象外

- PDUのバイトレイアウト
- 公開APIの具体的な関数シグネチャ
- Transport固有のretry実装
- Application内部のworkerおよびqueue状態
- 状態・イベント処理の排他実装
- Resultおよび終了済み`goal_id`の具体的な保持時間

これらは、Protocol、エラー・競合規約、API設計、設定モデルで順に具体化します。
