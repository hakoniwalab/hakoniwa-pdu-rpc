# Hakoniwa Action Cancel／Result競合契約

> **Status: Implemented contract**
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

### 3.3 Cancel Response配送と状態公開

Endpointは、Cancel判断の検証、Cancel Responseの同期送信、状態確定を同じstate mutex区間で実行します。Response配送中専用の状態は追加しません。

```text
accept:
  state_mutex lock
    state = GOAL_ACCEPTED
    cancel_decision_pending = true
    CANCEL_RESPONSE(ACCEPTED)を同期送信
    送信成功:
      state = CANCEL_ACCEPTED
      cancel_decision_pending = false
  state_mutex unlock

reject:
  state_mutex lock
    state = GOAL_ACCEPTED
    cancel_decision_pending = true
    CANCEL_RESPONSE(REJECTED)を同期送信
    送信成功:
      state = GOAL_ACCEPTED
      cancel_decision_pending = false
  state_mutex unlock
```

同じstate mutexを使う`complete()`はCancel Response送信完了まで待つため、Cancel accept時は必ず`CANCEL_RESPONSE(ACCEPTED)`が`RESULT(CANCELED / ABORTED)`より先にWireへ送信されます。

Cancel Response送信に失敗した場合は、`GOAL_ACCEPTED`と`cancel_decision_pending=true`を維持します。Applicationは同じaccept／reject判断を再実行できます。通信異常をGoalのterminal statusへ変換しません。TCPの非OK同期送信は完全なProtocol packetを配送できていないものとして扱います。

### 3.4 後着する通常成功

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
| `CANCEL_REQUEST_RECEIVED` | pendingでなければ`DEFER`: Applicationへ通知。判断までは`SAME`。pendingなら`IGNORE` | `IGNORE`: 既存Cancel commitを変更せず、追加Responseを送らない。`SAME` | `IGNORE`: Result確定済み。Cancel Responseを送らず破棄。`SAME` |
| `DUPLICATE_CANCEL_REQUEST_RECEIVED` | 判断待ち中は`IGNORE`し、追加dispatch／Responseを生成しない。`SAME` | `IGNORE`: 既存Cancel commitを変更しない。`SAME` | `IGNORE`: Cancel Responseを送らず破棄。`SAME` |

Server Applicationイベントについては、次を正とします。

| Event | EXECUTING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `ACCEPT_CANCEL` | pending Contextが有効なら`ALLOW`して`CANCELING`へ | `APPLICATION_API_ERROR` | `APPLICATION_API_ERROR`: Result確定済み |
| `REJECT_CANCEL` | pending Contextが有効なら`ALLOW`して`EXECUTING`を維持 | `APPLICATION_API_ERROR` | `APPLICATION_API_ERROR`: Result確定済み |

Result commitとCancel acceptは、同一Goal Contextに対する排他的な状態更新として実装します。Endpointでは、判断、同期送信、状態確定を同じstate mutex区間で直列化します。

v1はTCPを前提とし、Cancel Request単位の`request_id`を持ちません。このため同一Goalの判断待ち中またはCancel受理後に届く追加Cancel Requestは、再送か新規要求かを区別せず無応答で破棄します。Cancelを`REJECTED`と判断した後はGoalが`EXECUTING`へ戻るため、Clientは改めてCancel Requestを送信できます。

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

## 9. Action v1必須Contract Test集合

Action Runtimeは、以下のContract Testを通過しなければなりません。

各テストでは、次を明示的に検査します。

```text
Initial state
Input sequence
Expected Client events
Expected Server state
Expected Goal Handle／Context validity
Expected Context release
Forbidden output
```

### 9.1 Goal accepted、Feedback、正常完了

```text
GOAL_REQUEST
  -> GOAL_RESPONSE(ACCEPTED)
  -> FEEDBACK [0..n]
  -> RESULT(SUCCEEDED)
```

検査項目:

- `goal_id`が全イベントで一致する。
- Goal accept時に`action_name + Server Goal Handle`へ対応するGoal Contextが有効になる。
- Feedbackの`sequence_no`がGoal単位で単調増加する。
- Result commit後はGoalが`FINISHING`へ進む。
- Result配送完了後にGoal Contextが解放され、同じServer Goal Handleで継続操作できなくなる。

禁止出力:

```text
CANCEL_RESPONSE
二重RESULT
terminal後のFEEDBACK
```

### 9.2 Goal rejected

```text
GOAL_REQUEST
  -> GOAL_RESPONSE(REJECTED)
```

検査項目:

- accept済みGoal Contextを生成しない。
- Goal Contextを生成しない。
- 同じGoal Requestへのaccept／reject再実行を許可しない。

禁止出力:

```text
FEEDBACK
RESULT
CANCEL_RESPONSE
```

### 9.3 Cancel accepted

```text
accepted Goal
  -> CANCEL_REQUEST
  -> CANCEL_RESPONSE(ACCEPTED)
  -> RESULT(CANCELED)
```

検査項目:

- Cancel accept時に`EXECUTING`から`CANCELING`へ遷移する。
- Cancel判断は一度だけ確定できる。
- `action_name + Server Goal Handle`はterminal Resultまで継続操作に使用できる。
- `COMPLETE_CANCELED`後にResultを一度だけ送る。

禁止出力:

```text
RESULT(SUCCEEDED)
二重CANCEL_RESPONSE
二重RESULT
```

### 9.4 Cancel rejected

```text
accepted Goal
  -> CANCEL_REQUEST
  -> CANCEL_RESPONSE(REJECTED)
  -> Goal continues
  -> RESULT(SUCCEEDED)
```

検査項目:

- Cancel reject後も状態は`EXECUTING`である。
- Goal ContextとServer Goal Handleは有効なままである。
- 通常Feedbackと通常完了を継続できる。

禁止出力:

```text
RESULT(CANCELED)
Goal Contextの早期解放
```

### 9.5 Result wins Cancel race

以下を個別に検査します。

```text
Result wins before Cancel dispatch
Result wins while Cancel decision is pending
COMPLETE_SUCCEEDED and ACCEPT_CANCEL atomic race
```

共通契約:

```text
terminal Resultのみを送る
pending Cancel Contextを閉じる
late Cancelをdropする
CANCEL_RESPONSEは送信しない
terminal statusを後着イベントで変更しない
```

### 9.6 Cancel wins Result race

```text
ACCEPT_CANCEL commits first
  -> CANCEL_RESPONSE(ACCEPTED)
  -> state = CANCELING
  -> COMPLETE_SUCCEEDED fails
  -> RESULT(CANCELED or ABORTED)
```

検査項目:

- Cancel acceptと通常完了の両方が成功しない。
- Cancel勝利後は`SUCCEEDED`を送らない。
- Resultは一度だけ送る。

### 9.7 Multiple concurrent Goals

```text
Goal A accepted
Goal B accepted
Feedback B
Feedback A
Result A
Result B
```

検査項目:

- 一つのClient handleで複数Goalを保持できる。
- 異なる`goal_id`のServer Goal HandleとGoal Contextが混線しない。
- イベント配送順がGoal間で入れ替わっても正しくdispatchされる。
- 一方のGoal終端が他方のContextを解放しない。
- GoalごとのFeedback sequenceが独立する。

### 9.8 Invalid、reused、terminal後の操作

少なくとも次を成功させてはなりません。

```text
unknown `action_name + Server Goal Handle`でaccept / reject
判断済みGoal／Cancelへのaccept / reject再実行
unknown `action_name + Server Goal Handle`でfeedback / complete
terminal後のfeedback
terminal後のcomplete再実行
Goal reject後のcancel / feedback / complete
Cancel判断の二重accept / reject
```

具体的なエラーコードはC API仕様に従いますが、少なくとも次を保証します。

```text
状態を変更しない
Protocol packetを生成しない
Contextを復活させない
二重terminalを発生させない
```

## 10. unknown goal_idのCancel

Action Protocol v1では、Runtimeが保持していない`goal_id`に対する`CANCEL_REQUEST`は無応答で破棄します。

```text
unknown goal_id + CANCEL_REQUEST
  -> drop
  -> no Application dispatch
  -> no CANCEL_RESPONSE
  -> no Goal Context creation
```

この規則は、次の両方へ同一に適用します。

```text
完全に未知のgoal_id
Result送信完了後に解放済みのgoal_idへ到着したlate Cancel
```

v1は終了済みGoal履歴とCancel Request単位の独立した`request_id`を必須保持しないため、両者をProtocol上で区別しません。

`CANCEL_RESPONSE(REJECTED)`を返さない理由は次のとおりです。

- Result勝利時のlate Cancel無応答契約と統一する。
- 解放済みGoalをCancel応答目的で復活させない。
- v1の`goal_id`＋pending Context相関を単純に保つ。
- 終端後の第二応答を発生させない。

Client Runtimeは、unknown GoalへのCancelについてCancel Responseの到着を保証として期待してはなりません。呼び出し側が必要なローカル待ち時間を管理します。

## 11. テスト階層

テストは、次の層に分けます。

```text
1. Runtime Contract Test
   固定の小さなPDUを使い、状態、token、event、raceを検証

2. C API / CFFI Contract Test
   buffer ownership、enum写像、opaque handle、closeの冪等性を検証

3. Registry generated Action E2E
   FibonacciAction等の生成型を使い、Goal / Result / Feedback変換を検証

4. Installed package consumer
   公開C++／C Header、CMake target、Python CFFIの配布契約を検証
```

FibonacciAction E2EだけをRuntime Contract Testの代替にしてはなりません。Action型に依存しないRuntime契約と、Registry生成型を使うE2Eを分離します。

## 12. 設計判断

- Cancel acceptとterminal Result commitは、同一Goal Context上で排他的に確定する。
- Cancel acceptが先ならCancelが勝ち、Client起因CancelではCancel Responseを返す。
- terminal Result commitが先ならResultが勝つ。
- Result勝利時はpending Cancel Contextを閉じる。
- `FINISHING`中および終了後の遅延Cancel Requestは副作用なく破棄する。
- Result勝利時はCancel Responseを生成・送信しない。
- unknownまたは解放済み`goal_id`へのCancel Requestは無応答で破棄する。
- 後着するCancel判断はApplication API Errorとする。
- 後着イベントは、先にcommitされた状態とterminal statusを変更しない。
- Response配送中はstate mutexで共通排他し、配送中専用の意味状態を増やさない。
- Contract Testは正常Goal、Goal reject、Cancel accept／reject、Cancel／Result race、複数Goal、Goal Handle誤用を含む。
- Runtime Contract TestをRegistry生成型E2Eから分離する。
- この規則をC++ Runtime、C API、Python/CFFI、およびContract Testで共通適用する。
