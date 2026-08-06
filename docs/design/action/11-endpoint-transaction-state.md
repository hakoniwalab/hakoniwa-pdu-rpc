# Hakoniwa Action Endpoint Transaction契約

> **Status: Draft**  
> 本文書は、`04-state-model.md`、`05-client-state-model.md`、`06-protocol.md`、`10-cancel-result-race.md`に対する規範的な追補です。

## 1. 目的

Action Packet Endpoint内部で、Goal／Cancel判断、Feedback、terminal Result、slot ownershipとWire送信順序を安全に直列化する契約を定義します。

初版はTCPの同期送信を対象とし、不必要な並行性や配送中専用stateを導入しません。

```text
受信callback:
  queue_mutexでraw packetをFIFOへ追加

poll:
  FIFO先頭を取り出す
  state_mutexでbindingとslotを検証・更新

Application API:
  state_mutex内で検証
  同期send
  send結果に応じて状態を確定
```

## 2. 内部状態

### 2.1 `PacketBindingState`

```text
AWAITING_GOAL_DECISION
GOAL_ACCEPTED
CANCEL_ACCEPTED
RESULT_COMMITTED
```

- `AWAITING_GOAL_DECISION`: Goal RequestをApplicationへdispatch済みで、accept／reject判断待ち。
- `GOAL_ACCEPTED`: `GOAL_RESPONSE(ACCEPTED)`送信成功済み。
- `CANCEL_ACCEPTED`: `CANCEL_RESPONSE(ACCEPTED)`送信成功済み。
- `RESULT_COMMITTED`: terminal Resultを確定済み。送信成功または回復Policyを待つ。

付随Contextは次のとおりです。

```text
cancel_decision_pending: bool
next_feedback_sequence: uint32
slot_owners_: slot_index -> goal_id
packet_bindings_: goal_id -> ActionPacketBinding
```

Response送信中専用の`PendingResponse`や`*_SENDING`状態は持ちません。

## 3. 排他モデル

### 3.1 queue mutex

Transport callbackはraw packetを受信順にFIFOへ追加するだけです。Protocol状態、binding、slot ownership、Application callbackへ直接アクセスしてはなりません。

`poll()`は次の順序を守ります。

```text
queue_mutex lock
  FIFO先頭をmoveしてpop
queue_mutex unlock

decode / validate

state_mutex lock
  binding／slot処理
  必要なら同期send
state_mutex unlock
```

queue mutexを保持したままstate mutexを取得してはなりません。

### 3.2 state mutex

次の処理を同じstate mutexで直列化します。

- Goal／Cancel判断の検証
- Response packetの同期送信
- Feedback／Resultの同期送信
- `PacketBindingState`
- `cancel_decision_pending`
- Feedback sequence
- bindingとslot ownership
- Protocol違反Goalへの自動REJECT

これにより、送信中専用stateやRequest再キューを使わずWire順序を保証します。

### 3.3 実行主体の契約

```text
受信callback:
  複数threadから呼ばれてよい
  enqueueだけを行う

poll:
  同一Action Endpointにつきsingle-consumer
  同時に複数threadから呼ばない

Application API:
  複数threadから呼ばれてよい
  state_mutexで直列化される
```

`endpoint_->send()`から同期的に受信callbackへ再入しても、callbackはqueue mutexしか取得しないためstate mutexと循環しません。

## 4. API許可マトリクス

| Binding state | `accept_goal` | `reject_goal` | `accept_cancel` | `reject_cancel` | `send_feedback` | `complete(SUCCEEDED)` | `complete(CANCELED)` | `complete(ABORTED)` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `AWAITING_GOAL_DECISION` | ALLOW | ALLOW | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT |
| `GOAL_ACCEPTED` | REJECT | REJECT | pending Cancel時のみALLOW | pending Cancel時のみALLOW | ALLOW | ALLOW | REJECT | ALLOW |
| `CANCEL_ACCEPTED` | REJECT | REJECT | REJECT | REJECT | ALLOW | REJECT | ALLOW | ALLOW |
| `RESULT_COMMITTED` | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT | REJECT |

## 5. Response判断と送信失敗

### 5.1 Goal accept／reject

```text
state_mutex lock
  state = AWAITING_GOAL_DECISIONを検証
  GOAL_RESPONSEを同期送信

  accept成功:
    state = GOAL_ACCEPTED

  reject成功:
    bindingとslotを解放

  送信失敗:
    AWAITING_GOAL_DECISIONを維持
    同じ判断APIを再実行可能
state_mutex unlock
```

### 5.2 Cancel accept／reject

```text
state_mutex lock
  state = GOAL_ACCEPTEDを検証
  cancel_decision_pending = trueを検証
  CANCEL_RESPONSEを同期送信

  accept成功:
    state = CANCEL_ACCEPTED
    cancel_decision_pending = false

  reject成功:
    state = GOAL_ACCEPTED
    cancel_decision_pending = false

  送信失敗:
    state = GOAL_ACCEPTEDを維持
    cancel_decision_pending = trueを維持
    同じ判断APIを再実行可能
state_mutex unlock
```

通信失敗をterminal statusへ自動変換してはなりません。

## 6. Result commit

Result packetの形式・容量検証はcommit前に行います。検証失敗時は状態を変えず、修正したpacketで再実行できます。

```text
state_mutex lock
  packetとterminal statusを検証
  state = RESULT_COMMITTED
  cancel_decision_pending = false
  Resultを同期送信

  送信成功:
    bindingとslotを解放

  送信失敗:
    RESULT_COMMITTEDとslot ownershipを維持
state_mutex unlock
```

Resultはterminal commitであるため、Goal／Cancel Responseとは異なり、送信失敗後に元状態へ戻しません。再送・保持・破棄は後続Policyで定義します。

## 7. inbound Request

### 7.1 Goal Request

- 新規`goal_id`かつ空きslot: Applicationへdispatch。
- 重複`goal_id`: Applicationへdispatchせず、受信slotへ`GOAL_RESPONSE(REJECTED)`をbest-effort送信。
- 使用中slotへの別Goal: Applicationへdispatchせず、受信slotへ`GOAL_RESPONSE(REJECTED)`をbest-effort送信。
- 自動REJECT生成・送信失敗: ERRORログを残し、追加の回復状態は作らない。

### 7.2 Cancel Request

次の場合だけApplicationへdispatchします。

```text
対応bindingが存在する
受信slotとbinding slotが一致する
state = GOAL_ACCEPTED
cancel_decision_pending = false
```

unknown、cross-slot、accept前、Cancel受理後、Result commit後、重複判断待ちのCancelは無応答で破棄します。ただし、action名、slot、goal ID、理由と「Cancel Responseを送らない」ことをWARNINGログへ記録します。

不正packet／headerは相関情報を信頼できないため、ERRORログを残して無応答で破棄します。

## 8. Wire順序不変条件

```text
GOAL_RESPONSE(ACCEPTED) < FEEDBACK*
GOAL_RESPONSE(ACCEPTED) < CANCEL_RESPONSE
GOAL_RESPONSE(ACCEPTED) < RESULT

CANCEL_RESPONSE(ACCEPTED) < RESULT(CANCELED / ABORTED)
CANCEL_RESPONSE(REJECTED) < subsequent RESULT(SUCCEEDED / ABORTED)
```

state mutex内でResponse送信と状態確定を完結するため、競合APIは送信完了まで待ち、その後の確定状態を評価します。

送信中に到着したRequestはcallback FIFOへ追加されます。packetを一度取り出してqueue末尾へ戻すDEFER処理は禁止します。

## 9. Client相関

| Client内部状態 | 受理するResult |
| --- | --- |
| `ACCEPTED` | `SUCCEEDED`, `ABORTED` |
| `AWAITING_CANCEL_RESPONSE` | `SUCCEEDED`, `ABORTED`。Result勝利としてContextを解放 |
| `CANCELING` | `CANCELED`, `ABORTED` |

Clientは`AWAITING_CANCEL_RESPONSE`中の`CANCELED` Resultを受理しません。ServerのWire順序契約により、Cancel accept時は必ずCancel Responseが先に送信されます。

## 10. 必須Contract Test

- Goal Responseをblockし、並行ResultよりWire上で先になること。
- Response送信中に到着したCancel／次GoalがFIFOへ残り、送信後に処理されること。
- Goal accept／reject Response失敗後に同じ判断を再実行できること。
- Cancel accept／reject Response失敗後にpending判断を維持すること。
- Cancel ResponseよりCANCELED Resultが先に送信されないこと。
- Result送信失敗時に`RESULT_COMMITTED`とslot ownershipを保持すること。
- duplicate Goal／slot collisionへREJECTを返すこと。
- 自動REJECT送信失敗をERRORログへ記録すること。
- 無応答Cancelの理由をWARNINGログへ記録すること。

blocking Endpointを使うテストは、待機assertが失敗した場合も必ずblockを解除しthreadをjoinしなければなりません。

## 11. 将来の拡張

実測された性能要求によって複数slotの同時送信が必要になった場合に限り、global state mutexからslot単位mutexへの分割を検討します。

分割時も、本文書のWire順序、FIFO、binding、slot ownership、送信失敗契約を維持しなければなりません。
