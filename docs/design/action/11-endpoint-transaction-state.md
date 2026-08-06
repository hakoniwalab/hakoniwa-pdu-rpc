# Hakoniwa Action Endpoint Transaction State契約

> **Status: Draft**  
> 本文書は、`04-state-model.md`、`05-client-state-model.md`、`06-protocol.md`、`10-cancel-result-race.md`に対する規範的な追補です。
>
> 本文書は公開Action Protocol状態ではなく、Endpoint内部でProtocol判断とWire配送順序を結ぶTransaction状態を定義します。

## 1. 目的

Actionの公開Goal状態だけでは、判断のcommitとProtocol packetの同期送信の間に発生する競合を表現できません。

例として、Cancel acceptは次の複数段階を持ちます。

```text
ApplicationがCancel acceptを選択
  -> Cancel Response送信開始
  -> Cancel Response送信成功
  -> CANCELINGを公開
```

Response送信中にterminal Resultがcommitされると、`RESULT(CANCELED)`が`CANCEL_RESPONSE(ACCEPTED)`を追い越す可能性があります。本書は、このようなWire順序競合をEndpoint内部状態で防止するための契約を定義します。

## 2. 状態の二層構造

### 2.1 公開Goal状態

公開Goal状態は上位Goal Transaction層が所有します。

```text
EXECUTING
CANCELING
FINISHING
```

### 2.2 Endpoint Transaction状態

Endpoint Transaction状態はAction Packet Endpointが所有します。

```text
AWAITING_GOAL_DECISION
GOAL_ACCEPTED
CANCEL_ACCEPT_RESPONSE_SENDING
CANCEL_REJECT_RESPONSE_SENDING
CANCEL_ACCEPTED
RESULT_COMMITTED
```

`cancel_decision_pending`は`GOAL_ACCEPTED`に付随する直交Contextです。これはApplicationへCancel Requestをdispatch済みで、accept／reject判断を待っていることを表します。

Endpoint Transaction状態を公開Goal状態としてApplicationへ露出してはなりません。

## 3. 状態の意味

### 3.1 `AWAITING_GOAL_DECISION`

Goal Requestを受信し、Applicationのaccept／reject判断を待っています。

### 3.2 `GOAL_ACCEPTED`

Goal Responseの送信が成功し、Goal laneが実行中です。

`cancel_decision_pending=false`では通常実行中、`true`ではCancel RequestをApplicationへdispatch済みです。

### 3.3 `CANCEL_ACCEPT_RESPONSE_SENDING`

Cancel accept判断を一時commitし、`CANCEL_RESPONSE(ACCEPTED)`を同期送信しています。

この状態では、同一Goalのterminal Resultをcommitしてはなりません。送信成功後に`CANCEL_ACCEPTED`へ進み、送信失敗時は`GOAL_ACCEPTED + cancel_decision_pending=true`へ戻ります。

### 3.4 `CANCEL_REJECT_RESPONSE_SENDING`

Cancel reject判断を一時commitし、`CANCEL_RESPONSE(REJECTED)`を同期送信しています。

この状態では、同一Goalのterminal Resultをcommitしてはなりません。送信成功後は`GOAL_ACCEPTED + cancel_decision_pending=false`へ戻り、送信失敗時は`GOAL_ACCEPTED + cancel_decision_pending=true`へ戻ります。

### 3.5 `CANCEL_ACCEPTED`

`CANCEL_RESPONSE(ACCEPTED)`の送信が成功し、公開Goal状態を`CANCELING`として扱える状態です。

### 3.6 `RESULT_COMMITTED`

terminal statusとResult bodyをcommitし、Resultの配送責任が残っています。

新規Feedback、Cancel判断、二重completeを拒否します。Result送信成功後にGoal Contextとslot ownershipを解放します。送信失敗時は誤再利用防止のためこの状態を維持します。

## 4. Endpoint状態 × API許可マトリクス

凡例:

```text
ALLOW       操作を受理できる
CONDITIONAL 付随Contextまたはstatus条件を満たす場合だけ受理できる
REJECT      Application API Errorとして拒否する
IGNORE      inbound packetをApplicationへ通知せず破棄する
```

| Endpoint Transaction状態 | `accept_goal` | `reject_goal` | `accept_cancel` | `reject_cancel` | `send_feedback` | `complete(SUCCEEDED)` | `complete(CANCELED)` | `complete(ABORTED)` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `AWAITING_GOAL_DECISION` | ALLOW | ALLOW | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT |
| `GOAL_ACCEPTED` | REJECT | REJECT | CONDITIONAL: `cancel_decision_pending=true` | CONDITIONAL: `cancel_decision_pending=true` | ALLOW | ALLOW | REJECT | ALLOW |
| `CANCEL_ACCEPT_RESPONSE_SENDING` | REJECT | REJECT | REJECT | REJECT | ALLOW | REJECT | REJECT | REJECT |
| `CANCEL_REJECT_RESPONSE_SENDING` | REJECT | REJECT | REJECT | REJECT | ALLOW | REJECT | REJECT | REJECT |
| `CANCEL_ACCEPTED` | REJECT | REJECT | REJECT | REJECT | ALLOW | REJECT | ALLOW | ALLOW |
| `RESULT_COMMITTED` | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT |

Cancel Response配送中もFeedbackを許可します。Feedbackは非終端通知であり、Client RuntimeもCancel Response待ち中および`CANCELING`中にFeedbackを受理します。

## 5. inbound packet許可マトリクス

| Endpoint Transaction状態 | Goal Request | Cancel Request |
| --- | --- | --- |
| `AWAITING_GOAL_DECISION` | 同一`goal_id`の重複はIGNOREまたはProtocol reject。新規Goalは別slotのみ | IGNORE |
| `GOAL_ACCEPTED` | 同一`goal_id`は重複としてIGNORE。別`goal_id`が同一slotならREJECT response | `cancel_decision_pending=false`ならApplicationへdispatchしtrueへ。trueならIGNORE |
| `CANCEL_ACCEPT_RESPONSE_SENDING` | IGNORE | IGNORE |
| `CANCEL_REJECT_RESPONSE_SENDING` | IGNORE | IGNORE |
| `CANCEL_ACCEPTED` | IGNORE | IGNORE |
| `RESULT_COMMITTED` | IGNORE | IGNORE |

未知`goal_id`、異なるslotへ到着したCancel Request、終了済みGoalへの遅延Cancel Requestは、Application ContextやCancel Responseを生成せずIGNOREします。

## 6. Wire順序不変条件

実装は次の不変条件を常に満たさなければなりません。

### 6.1 Goal開始

```text
GOAL_RESPONSE(ACCEPTED) < FEEDBACK*
GOAL_RESPONSE(ACCEPTED) < CANCEL_RESPONSE
GOAL_RESPONSE(ACCEPTED) < RESULT
```

### 6.2 Cancel accept

```text
CANCEL_RESPONSE(ACCEPTED) < RESULT(CANCELED)
CANCEL_RESPONSE(ACCEPTED) < RESULT(ABORTED from CANCELING)
```

`CANCEL_ACCEPT_RESPONSE_SENDING`中はterminal `complete()`を拒否することで、この順序を保証します。

### 6.3 Cancel reject

```text
CANCEL_RESPONSE(REJECTED) < subsequent RESULT(SUCCEEDED / ABORTED)
```

`CANCEL_REJECT_RESPONSE_SENDING`中はterminal `complete()`を拒否し、reject Response送信成功後にだけGoalを通常実行状態へ戻します。

### 6.4 terminal一意性

```text
terminal RESULTはGoalごとに最大1回commitされる
RESULT_COMMITTED後はFeedback禁止
RESULT_COMMITTED後はCancel判断禁止
Result送信成功後はContextとslotを解放する
```

### 6.5 送信失敗

```text
Cancel Response送信失敗:
  Cancel判断を確定しない
  cancel_decision_pendingを維持する
  同じaccept／reject判断を再実行可能にする

Result送信失敗:
  RESULT_COMMITTEDを維持する
  slotを再利用しない
  terminal statusへ自動変換しない
```

## 7. Client相関契約

Client Runtimeは次の状態でResultを受理します。

| Client内部状態 | 受理するResult |
| --- | --- |
| `ACCEPTED` | `SUCCEEDED`, `ABORTED` |
| `AWAITING_CANCEL_RESPONSE` | `SUCCEEDED`, `ABORTED`。Result勝利としてContextを解放し、後着Cancel ResponseをIGNORE |
| `CANCELING` | `CANCELED`, `ABORTED` |

Client Runtimeは`AWAITING_CANCEL_RESPONSE`中の`CANCELED` Resultを受理しません。Server側が`CANCEL_RESPONSE(ACCEPTED)`を先にWireへ送る契約によって、正常系ではこのpacket順序は発生しません。

ResultをApplicationイベントへ移した後、Client RuntimeはGoal Contextとslot ownershipを解放します。後着するFeedback、Cancel Response、重複Resultは相関ContextがないためIGNOREします。

## 8. 排他契約

同一Goalについて、次の判断は同じmutexまたは同等の直列化機構で排他的にcommitします。

```text
Cancel accept
Cancel reject
terminal complete
reset／shutdownによるContext破棄
```

Cancel ResponseのTransport送信自体をmutex外で行う場合、必ず配送中状態を先にcommitし、`complete()`がその状態を受理しないようにします。

上位Services層は、Endpoint API実行中に同一Endpointの`reset_contexts()`または破棄を並行実行しないようライフサイクルを直列化します。

## 9. 必須Contract Test

### 9.1 Cancel Response順序

```text
Cancel Response送信を意図的にblockする
並行してcomplete(CANCELED)を呼ぶ
completeは失敗する
Cancel Response送信成功後にcomplete(CANCELED)が成功する
Wire上でCancel ResponseがResultより先である
```

### 9.2 Cancel accept Response送信失敗

```text
1回目のCANCEL_RESPONSE(ACCEPTED)送信を失敗させる
accept_cancelはfalse
pending Cancel判断は維持される
同じaccept_cancelを再実行できる
2回目成功後にCANCEL_ACCEPTEDへ進む
```

### 9.3 Cancel reject Response送信失敗

```text
1回目のCANCEL_RESPONSE(REJECTED)送信を失敗させる
reject_cancelはfalse
pending Cancel判断は維持される
同じreject_cancelを再実行できる
2回目成功後に通常実行へ戻る
```

### 9.4 Result勝利

```text
Cancel判断待ち中にcomplete(SUCCEEDED / ABORTED)を先にcommitする
Resultだけを送信する
pending Cancel判断を失効させる
後着accept_cancel／reject_cancelは失敗する
Cancel Responseを送信しない
```

### 9.5 terminal後の禁止操作

```text
RESULT_COMMITTED後:
  二重completeを拒否
  Feedbackを拒否
  Cancel判断を拒否
  同一slotの別Goalを拒否または隔離
```

## 10. 実装レビュー手順

Action Endpoint実装を変更した場合、次の順序で確認します。

1. 本文書の状態・マトリクス・Wire順序不変条件を更新する。
2. `04-state-model.md`、`05-client-state-model.md`、`06-protocol.md`、`10-cancel-result-race.md`との矛盾を確認する。
3. Server／Client実装の各条件分岐をマトリクスへ対応付ける。
4. 競合箇所はblocking Endpointまたはfailure injection Endpointを使ってContract Test化する。
5. Endpoint lifecycle統合後は、同じContractをTCP実通信でも確認する。
