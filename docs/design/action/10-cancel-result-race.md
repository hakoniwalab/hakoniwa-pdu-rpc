# Hakoniwa Action Cancel／Result競合契約

> **Status: Draft**  
> 本文書は、`04-state-model.md`および`06-protocol.md`に対する規範的な追補です。
>
> Action Runtime実装およびContract Testでは、本書の競合規則を適用します。

## 1. 目的

Client起因CancelとServer Application起因の終端完了は、非同期に競合します。

```text
Client Runtime
  -> CANCEL_REQUEST

Server Application
  -> COMPLETE_SUCCEEDED / COMPLETE_ABORTED
```

本書では、競合時にCancelとResultのどちらが有効になるか、および敗者側のイベントをどのように処理するかを定義します。

## 2. 基本原則

```text
最初にRuntimeへcommitされた判断が勝つ。
```

競合結果は次の二つだけです。

```text
Cancel wins:
  Cancel acceptが先にcommitされる
  -> CANCEL_RESPONSE(ACCEPTED)
  -> RESULT(CANCELED または ABORTED)

Result wins:
  terminal Resultが先にcommitされる
  -> RESULT(SUCCEEDED または ABORTED)
  -> Cancel処理は破棄
  -> CANCEL_RESPONSEは送信しない
```

一度commitされた結果を、後着イベントが変更してはなりません。

## 3. Cancelが勝つ場合

### 3.1 条件

Server Runtimeが、Goalを`EXECUTING`から`CANCELING`へ遷移させるCancel acceptを先にcommitした場合です。

```text
EXECUTING
  -> CANCEL_REQUEST_RECEIVED
  -> cancel_decision_pending = true
  -> ACCEPT_CANCEL
  -> CANCELING
```

### 3.2 Protocol出力

Client起因Cancelでは、RuntimeはCancel受理を通知します。

```text
CANCEL_RESPONSE(ACCEPTED)
```

その後、Applicationが停止処理を完了するとterminal Resultを送ります。

```text
COMPLETE_CANCELED
  -> RESULT(CANCELED)
```

停止処理中にGoalを継続できない別の意味論的失敗が発生した場合は、既存状態モデルに従って`RESULT(ABORTED)`を許容します。

### 3.3 後着する通常成功

`CANCELING`へ遷移した後の`COMPLETE_SUCCEEDED`はApplication API Errorです。

```text
CANCELING + COMPLETE_SUCCEEDED
  -> APPLICATION_API_ERROR
  -> 状態変更なし
  -> 追加Resultなし
```

Cancel acceptによって確定した`CANCELING`を、後着する成功完了が覆してはなりません。

## 4. Resultが勝つ場合

### 4.1 条件

Server Applicationの終端完了をRuntimeが先にcommitし、Goalが`FINISHING`へ遷移した場合です。

```text
EXECUTING
  -> COMPLETE_SUCCEEDED / COMPLETE_ABORTED
  -> terminal statusとResult bodyをcommit
  -> FINISHING
```

この時点でGoalの意味論的な終端結果は確定しています。

### 4.2 pending Cancelの終了

Resultをcommitする際、Runtimeは同一Goalに存在する未回答のClient起因Cancel判断Contextを終了します。

```text
cancel_decision_pending = false
pending cancel event token = invalid
```

Result確定前にApplicationへCancel通知済みであっても、Applicationがまだ`ACCEPT_CANCEL`または`REJECT_CANCEL`を返していなければ、その判断権は失効します。

後着するApplication操作は成功してはなりません。

```text
FINISHING + ACCEPT_CANCEL
  -> APPLICATION_API_ERROR

FINISHING + REJECT_CANCEL
  -> APPLICATION_API_ERROR
```

### 4.3 遅延Cancel Request

Goalが`FINISHING`へ入った後に到着したCancel Requestは破棄します。

```text
FINISHING + CANCEL_REQUEST_RECEIVED
  -> IGNORE
  -> CANCEL_RESPONSEを送信しない
  -> terminal Resultを変更しない
  -> FINISHINGを維持
```

同様に、Result送信完了後に終了済み`goal_id`として観測された遅延Cancelも、Resultを変更せず破棄します。

### 4.4 Cancel Responseを送らない理由

Resultが勝った競合では、Clientへ送るProtocol出力はterminal Resultだけです。

```text
RESULT(SUCCEEDED / ABORTED)
```

次は送信しません。

```text
CANCEL_RESPONSE(ACCEPTED)
CANCEL_RESPONSE(REJECTED)
```

理由は次のとおりです。

- terminal ResultがGoal lifecycleの最終回答である。
- Result確定後にCancel判断を新たに成立させない。
- v1はCancel Request単位の独立した`request_id`を持たず、`goal_id`とpending Contextで相関する。
- 終端後の第二応答を発生させず、既存RPC ServiceのCancel／Response競合契約と対称にする。

## 5. Server状態マトリクスへの適用

`04-state-model.md`のClient／Protocolイベントについて、競合箇所は次を正とします。

| Event | EXECUTING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `CANCEL_REQUEST_RECEIVED` | `DEFER`: Applicationへ通知。判断までは`SAME` | 重複Cancel policyを適用。既存のCancel commitを変更しない。`SAME` | `IGNORE`: Result確定済み。Cancel Responseを送らず破棄。`SAME` |
| `DUPLICATE_CANCEL_REQUEST_RECEIVED` | 判断待ち中は重複policyを適用。`SAME` | 既存のCancel commitを変更しない。`SAME` | `IGNORE`: Cancel Responseを送らず破棄。`SAME` |

Server Applicationイベントについては、次を正とします。

| Event | EXECUTING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `ACCEPT_CANCEL` | pending Contextが有効なら`ALLOW`して`CANCELING`へ | `APPLICATION_API_ERROR` | `APPLICATION_API_ERROR`: Result確定済み |
| `REJECT_CANCEL` | pending Contextが有効なら`ALLOW`して`EXECUTING`を維持 | `APPLICATION_API_ERROR` | `APPLICATION_API_ERROR`: Result確定済み |

Result commitとCancel acceptは、同一Goal Contextに対する排他的な状態更新として実装します。

## 6. Protocolシーケンス

### 6.1 Cancel wins

```text
Client Runtime                    Server Runtime
      |                                 |
      |--- CANCEL_REQUEST ------------->|
      |                                 |--- ApplicationへCancel通知
      |                                 |<-- ACCEPT_CANCEL
      |<-- CANCEL_RESPONSE(ACCEPTED) -----|
      |                                 |<-- COMPLETE_CANCELED
      |<-- RESULT(CANCELED) --------------|
```

### 6.2 Result wins: Cancel Request受信前

```text
Client Runtime                    Server Runtime
      |                                 |
      |                                 |<-- COMPLETE_SUCCEEDED
      |<-- RESULT(SUCCEEDED) -------------|
      |--- CANCEL_REQUEST ------------->|
      |                                 |--- drop
      |                                 |    no CANCEL_RESPONSE
```

### 6.3 Result wins: Cancel判断待ち中

```text
Client Runtime                    Server Runtime          Server Application
      |                                 |                         |
      |--- CANCEL_REQUEST ------------->|                         |
      |                                 |--- Cancel通知 ---------->|
      |                                 |<-- COMPLETE_SUCCEEDED ----|
      |                                 |--- close pending cancel   |
      |<-- RESULT(SUCCEEDED) -------------|                         |
      |                                 |<-- ACCEPT/REJECT_CANCEL --| late
      |                                 |--- API error              |
      |                                 |    no CANCEL_RESPONSE     |
```

## 7. Client Runtime契約

Client Runtimeはterminal Resultを受信した時点でGoalを終端として扱います。

```text
RESULT received
  -> Goal terminal
  -> pending local cancel operationを終了
  -> 同一goal_idの後続非終端イベントを成功イベントとして公開しない
```

Server RuntimeはResult勝利時にCancel Responseを生成しないため、Client RuntimeはResult後のCancel Responseを正常シーケンスとして期待しません。

Transportや旧実装から不正な遅延Cancel Responseを受信した場合も、terminal Resultを覆してはなりません。

## 8. 最小Contract Test

### 8.1 Cancel wins

```text
Given:
  Goal is EXECUTING
  Cancel Request is delivered

When:
  Server Application accepts cancel before terminal completion

Then:
  Client receives CANCEL_RESPONSE(ACCEPTED)
  Goal becomes CANCELING
  Server can complete CANCELED
  Client receives RESULT(CANCELED)
  COMPLETE_SUCCEEDED is rejected after cancel accept
```

### 8.2 Result wins before Cancel dispatch

```text
Given:
  Goal is EXECUTING

When:
  Server commits RESULT(SUCCEEDED)
  A Cancel Request arrives afterward

Then:
  Client receives RESULT(SUCCEEDED)
  Server emits no CANCEL_RESPONSE
  Cancel is not dispatched to the Application
  Goal terminal status remains SUCCEEDED
```

### 8.3 Result wins during Cancel decision

```text
Given:
  Goal is EXECUTING
  Cancel Request has been dispatched to the Application
  cancel_decision_pending is true

When:
  Server commits RESULT(SUCCEEDED) before ACCEPT_CANCEL / REJECT_CANCEL

Then:
  pending Cancel Context is closed
  Client receives RESULT(SUCCEEDED)
  Server emits no CANCEL_RESPONSE
  late ACCEPT_CANCEL and REJECT_CANCEL return an Application API Error
  no second terminal or cancel response is emitted
```

### 8.4 Atomic race

```text
Given:
  COMPLETE_SUCCEEDED and ACCEPT_CANCEL execute concurrently

Then:
  exactly one operation commits

If COMPLETE_SUCCEEDED commits:
  state = FINISHING
  output = RESULT(SUCCEEDED)
  no CANCEL_RESPONSE

If ACCEPT_CANCEL commits:
  state = CANCELING
  output includes CANCEL_RESPONSE(ACCEPTED)
  later terminal result cannot be SUCCEEDED
```

## 9. 設計判断

- Cancel acceptとterminal Result commitは、同一Goal Context上で排他的に確定する。
- Cancel acceptが先ならCancelが勝ち、Client起因CancelではCancel Responseを返す。
- terminal Result commitが先ならResultが勝つ。
- Result勝利時はpending Cancel Contextを閉じる。
- `FINISHING`中および終了後の遅延Cancel Requestは副作用なく破棄する。
- Result勝利時はCancel Responseを生成・送信しない。
- 後着するCancel判断はApplication API Errorとする。
- 後着イベントは、先にcommitされた状態とterminal statusを変更しない。
- この規則をC++ Runtime、C API、Python/CFFI、およびContract Testで共通適用する。
