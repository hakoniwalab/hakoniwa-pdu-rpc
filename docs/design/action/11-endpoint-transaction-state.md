# Hakoniwa Action Endpoint Transaction契約

> **Status: Draft**  
> 本文書は、`04-state-model.md`、`05-client-state-model.md`、`06-protocol.md`、`10-cancel-result-race.md`に対する規範的な追補です。
>
> 本文書は、公開Action Protocol状態ではなく、Action Packet Endpoint内部で意味状態とResponse配送を直列化するための契約を定義します。

## 1. 目的

Action Endpointでは、Applicationの判断とProtocol Responseの同期送信の間に競合が発生します。

```text
Application判断
  -> Response送信開始
  -> Response送信成功または失敗
  -> 意味状態の確定またはrollback
```

Response送信中にFeedback、Cancel、Result、別の判断APIが割り込むと、Wire順序違反やContext不整合が発生します。

本書では、この問題を意味状態の増加ではなく、次の二軸で扱います。

```text
PacketBindingState   Goal laneの意味状態
pending_response     一時的なResponse配送トランザクション
```

## 2. 内部モデル

### 2.1 意味状態 `PacketBindingState`

```text
AWAITING_GOAL_DECISION
GOAL_ACCEPTED
CANCEL_ACCEPTED
RESULT_COMMITTED
```

#### `AWAITING_GOAL_DECISION`

Goal Requestを受信し、Applicationのaccept／reject判断を待っています。

#### `GOAL_ACCEPTED`

`GOAL_RESPONSE(ACCEPTED)`の送信が成功し、Goal laneが実行中です。

`cancel_decision_pending=false`では通常実行中、`true`ではCancel RequestをApplicationへdispatch済みです。

#### `CANCEL_ACCEPTED`

`CANCEL_RESPONSE(ACCEPTED)`の送信が成功し、Goal laneはCancel処理中です。

#### `RESULT_COMMITTED`

terminal statusとResult bodyをcommitし、Resultの配送責任が残っています。

Result送信成功後にGoal Contextとslot ownershipを解放します。送信失敗時は誤再利用防止のためこの状態を維持します。

### 2.2 Response配送 `PendingResponse`

```text
NONE
GOAL_ACCEPT
GOAL_REJECT
CANCEL_ACCEPT
CANCEL_REJECT
```

`pending_response != NONE` は、同一GoalのResponse同期送信中であることを表します。

これはGoalの意味状態ではなく、一時的な配送トランザクションです。公開Goal状態としてApplicationへ露出してはなりません。

### 2.3 付随Context

```text
cancel_decision_pending: bool
next_feedback_sequence: uint32
```

`cancel_decision_pending=true`は、Cancel RequestをApplicationへdispatch済みで、accept／reject判断を待っていることを表します。

## 3. Response配送中の共通規則

`pending_response != NONE` の間、同一Goalに対して次を適用します。

```text
accept_goal       REJECT
reject_goal       REJECT
accept_cancel     REJECT
reject_cancel     REJECT
complete          REJECT
```

Feedbackは次のとおり扱います。

```text
GOAL_ACCEPT / GOAL_REJECT 配送中:
  REJECT
  Goal Responseより先にFeedbackを送らない

CANCEL_ACCEPT / CANCEL_REJECT 配送中:
  ALLOW
  ClientはCancel Response待ち中もFeedbackを受理できる
```

同一slotへ届いた後続Requestは、Response配送中であることだけを理由に破棄してはなりません。

```text
pending_response != NONE:
  inbound RequestをDEFER
  Response送信と後処理の完了後に再評価する
```

## 4. API許可マトリクス

以下の表は `pending_response == NONE` の場合に適用します。`pending_response != NONE` の場合は、前節の共通規則を優先します。

| Meaning state | `accept_goal` | `reject_goal` | `accept_cancel` | `reject_cancel` | `send_feedback` | `complete(SUCCEEDED)` | `complete(CANCELED)` | `complete(ABORTED)` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `AWAITING_GOAL_DECISION` | ALLOW | ALLOW | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT |
| `GOAL_ACCEPTED` | REJECT | REJECT | `cancel_decision_pending=true`ならALLOW | `cancel_decision_pending=true`ならALLOW | ALLOW | ALLOW | REJECT | ALLOW |
| `CANCEL_ACCEPTED` | REJECT | REJECT | REJECT | REJECT | ALLOW | REJECT | ALLOW | ALLOW |
| `RESULT_COMMITTED` | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT |

## 5. Response種別ごとの処理

### 5.1 Goal accept

```text
事前条件:
  state = AWAITING_GOAL_DECISION
  pending_response = NONE

開始:
  pending_response = GOAL_ACCEPT

送信成功:
  state = GOAL_ACCEPTED
  pending_response = NONE

送信失敗:
  state = AWAITING_GOAL_DECISION
  pending_response = NONE
  accept_goalを再実行可能
```

### 5.2 Goal reject

```text
事前条件:
  state = AWAITING_GOAL_DECISION
  pending_response = NONE

開始:
  pending_response = GOAL_REJECT

送信成功:
  Goal Contextとslot ownershipを解放

送信失敗:
  state = AWAITING_GOAL_DECISION
  pending_response = NONE
  reject_goalを再実行可能
```

### 5.3 Cancel accept

```text
事前条件:
  state = GOAL_ACCEPTED
  cancel_decision_pending = true
  pending_response = NONE

開始:
  pending_response = CANCEL_ACCEPT

送信成功:
  state = CANCEL_ACCEPTED
  cancel_decision_pending = false
  pending_response = NONE

送信失敗:
  state = GOAL_ACCEPTED
  cancel_decision_pending = true
  pending_response = NONE
  accept_cancelを再実行可能
```

### 5.4 Cancel reject

```text
事前条件:
  state = GOAL_ACCEPTED
  cancel_decision_pending = true
  pending_response = NONE

開始:
  pending_response = CANCEL_REJECT

送信成功:
  state = GOAL_ACCEPTED
  cancel_decision_pending = false
  pending_response = NONE

送信失敗:
  state = GOAL_ACCEPTED
  cancel_decision_pending = true
  pending_response = NONE
  reject_cancelを再実行可能
```

## 6. inbound Request契約

| Meaning state | Goal Request | Cancel Request |
| --- | --- | --- |
| `AWAITING_GOAL_DECISION` | 同一`goal_id`の重複はIGNOREまたはProtocol reject。新規Goalは別slotのみ | IGNORE |
| `GOAL_ACCEPTED` | 同一`goal_id`は重複としてIGNORE。別`goal_id`が同一slotならREJECT response | `cancel_decision_pending=false`ならApplicationへdispatchしtrueへ。trueならIGNORE |
| `CANCEL_ACCEPTED` | IGNORE | IGNORE |
| `RESULT_COMMITTED` | IGNORE | IGNORE |

ただし、`pending_response != NONE` の間に同一slotへ届いたRequestはDROPせずDEFERし、Response後処理完了後に再評価します。

未知`goal_id`、異なるslotへ到着したCancel Request、終了済みGoalへの遅延Cancel Requestは、Application ContextやCancel Responseを生成せずIGNOREします。

## 7. Wire順序不変条件

### 7.1 Goal開始

```text
GOAL_RESPONSE(ACCEPTED) < FEEDBACK*
GOAL_RESPONSE(ACCEPTED) < CANCEL_RESPONSE
GOAL_RESPONSE(ACCEPTED) < RESULT
```

### 7.2 Cancel accept

```text
CANCEL_RESPONSE(ACCEPTED) < RESULT(CANCELED)
CANCEL_RESPONSE(ACCEPTED) < RESULT(ABORTED from CANCELING)
```

### 7.3 Cancel reject

```text
CANCEL_RESPONSE(REJECTED) < subsequent RESULT(SUCCEEDED / ABORTED)
```

### 7.4 terminal一意性

```text
terminal RESULTはGoalごとに最大1回commitされる
RESULT_COMMITTED後はFeedback禁止
RESULT_COMMITTED後はCancel判断禁止
Result送信成功後はContextとslotを解放する
```

## 8. Result commit契約

Result commitは `pending_response == NONE` の場合だけ許可します。

```text
GOAL_ACCEPTED:
  SUCCEEDED / ABORTED

CANCEL_ACCEPTED:
  CANCELED / ABORTED
```

Result packetの形式・容量検証はcommit前に行います。検証失敗時は意味状態を変更せず、修正したResultで再実行可能にします。

送信開始前に次を実行します。

```text
state = RESULT_COMMITTED
cancel_decision_pending = false
```

Result送信失敗時は `RESULT_COMMITTED` とslot ownershipを維持し、terminal statusへ自動変換しません。

## 9. Client相関契約

Client Runtimeは次の状態でResultを受理します。

| Client内部状態 | 受理するResult |
| --- | --- |
| `ACCEPTED` | `SUCCEEDED`, `ABORTED` |
| `AWAITING_CANCEL_RESPONSE` | `SUCCEEDED`, `ABORTED`。Result勝利としてContextを解放し、後着Cancel ResponseをIGNORE |
| `CANCELING` | `CANCELED`, `ABORTED` |

Client Runtimeは `AWAITING_CANCEL_RESPONSE` 中の `CANCELED` Resultを受理しません。Server側のWire順序契約により、正常系では必ずCancel Responseが先着します。

ResultをApplicationイベントへ移した後、Client RuntimeはGoal Contextとslot ownershipを解放します。後着するFeedback、Cancel Response、重複Resultは相関ContextがないためIGNOREします。

## 10. 排他契約

同一Goalについて、次を同じmutexまたは同等の直列化機構で排他的に更新します。

```text
PacketBindingState
PendingResponse
cancel_decision_pending
slot ownership
```

Response送信自体をmutex外で実行する場合、送信前に `pending_response` を設定し、送信後の状態更新まで同一Goalの競合操作を抑止します。

上位Services層は、Endpoint API実行中に同一Endpointの `reset_contexts()` または破棄を並行実行しないようライフサイクルを直列化します。

## 11. 必須Contract Test

### 11.1 Goal accept順序

```text
GOAL_RESPONSE(ACCEPTED)送信をblockする
並行するsend_feedbackとcompleteを拒否する
送信成功後にsend_feedback／completeを許可する
Wire上でGoal Responseが先である
```

### 11.2 Goal Response再試行

```text
GOAL_RESPONSE(ACCEPTED)初回送信失敗
  -> accept_goal=false
  -> 同じaccept_goalを再実行可能

GOAL_RESPONSE(REJECTED)初回送信失敗
  -> reject_goal=false
  -> 同じreject_goalを再実行可能
  -> 成功後にslot解放
```

### 11.3 Cancel Response順序

```text
CANCEL_RESPONSE(ACCEPTED)送信をblockする
並行するcomplete(CANCELED)を拒否する
送信成功後にcomplete(CANCELED)を許可する
Wire上でCancel Responseが先である
```

### 11.4 Cancel Response再試行

```text
Cancel accept／reject Response初回送信失敗
  -> 判断を確定しない
  -> cancel_decision_pendingを維持
  -> 同じ判断APIを再実行可能
```

### 11.5 inbound RequestのDEFER

```text
Response送信をblockする
同一slotへ後続Requestを到着させる
RequestをDROPしない
Response後処理完了後に再評価する
```

### 11.6 Result勝利とterminal禁止操作

```text
Cancel判断待ち中にResultを先にcommit
  -> Resultだけを送信
  -> pending Cancel判断を失効
  -> 後着accept_cancel／reject_cancelを拒否

RESULT_COMMITTED後:
  二重completeを拒否
  Feedbackを拒否
  Cancel判断を拒否
  slotを再利用しない
```

## 12. 実装レビュー手順

1. 本文書の意味状態、`pending_response`、成功／失敗後処理を更新する。
2. `04-state-model.md`、`05-client-state-model.md`、`06-protocol.md`、`10-cancel-result-race.md`との矛盾を確認する。
3. Server／Client実装の各条件分岐を本書へ対応付ける。
4. 競合箇所はblocking Endpointまたはfailure injection Endpointを使ってContract Test化する。
5. Endpoint lifecycle統合後は、同じContractをTCP実通信でも確認する。
