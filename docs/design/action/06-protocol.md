# Hakoniwa Action Protocol概要

> **Status: Draft**  
> 本文書は、Hakoniwa Action Protocolの通信端点、代表的な通信順序、および`hakoniwa-pdu-registry`と整合する論理パケット構成を直感的に理解するための概要です。

## 1. 目的

本書は、Action通信について次を示します。

```text
誰から誰へ送るか
何を送るか
どの順番で送るか
PDU Registry上でどのデータ構造を使うか
```

状態とイベントの網羅的な許容規則は、以下を正とします。

- Server Runtime: `04-state-model.md`
- Client Runtime: `05-client-state-model.md`

本書では、異常系や競合を再網羅しません。

## 2. 通信端点

```text
Client Application
  -> Client Runtime
  -> Transport
  -> Server Runtime
  -> Server Application
```

Client RuntimeとServer Runtimeの間では、次の論理イベントを扱います。

```text
GOAL_REQUEST
GOAL_RESPONSE
FEEDBACK
CANCEL_REQUEST
CANCEL_RESPONSE
RESULT
```

これらは、PDU RegistryのAction packetへ写像します。

## 3. 代表シーケンス

### 3.1 Goal受理と正常完了

```text
Client Runtime                    Server Runtime
      |                                 |
      |--- GOAL_REQUEST --------------->|
      |                                 |--- Server ApplicationへGoal通知
      |                                 |<-- accept
      |<-- GOAL_RESPONSE(ACCEPTED) ------|
      |                                 |
      |<-- FEEDBACK [0..n] --------------|
      |                                 |
      |<-- RESULT(SUCCEEDED) -------------|
```

- Goal Responseが`ACCEPTED`の場合、双方はGoalを`EXECUTING`として扱います。
- Feedbackは0回以上送信できます。
- Result受信後、Client Runtimeは`FINISHING`へ進みます。

### 3.2 Goal拒否

```text
Client Runtime                    Server Runtime
      |                                 |
      |--- GOAL_REQUEST --------------->|
      |                                 |--- Server ApplicationへGoal通知
      |                                 |<-- reject
      |<-- GOAL_RESPONSE(REJECTED) ------|
```

拒否されたGoalはaccept済みGoalの主状態を持ちません。

### 3.3 Client起因Cancel

```text
Client Runtime                    Server Runtime
      |                                 |
      |--- CANCEL_REQUEST ------------->|
      |                                 |--- Server ApplicationへCancel通知
      |                                 |<-- accept / reject
      |<-- CANCEL_RESPONSE --------------|
      |                                 |
      |<-- RESULT(CANCELED) --------------|  accept時
```

- Cancel Request送信だけではClient状態を変更しません。
- Cancel Responseが`ACCEPTED`の場合、双方はGoalを`CANCELING`として扱います。
- `REJECTED`の場合、Goalは`EXECUTING`を維持します。

### 3.4 Runtime起因Cancel

```text
Server Runtime                    Server Application
      |                                 |
      |--- RUNTIME_CANCEL_REQUESTED ---->|
      |<-- ACCEPT_CANCEL ----------------|
      |<-- COMPLETE_CANCELED ------------|
      |--- RESULT(CANCELED) ------------> Client Runtime
```

Runtime起因CancelはClient Cancelを偽装しません。

## 4. PDU RegistryのActionデータ構成

`hakoniwa-pdu-registry`のAction generatorは、ROS 2 `.action`から次の6メッセージを生成します。

```text
<Action>Goal.msg
<Action>Result.msg
<Action>Feedback.msg
<Action>ActionRequest.msg
<Action>ActionResponse.msg
<Action>ActionFeedback.msg
```

packetは、共通HeaderとAction固有bodyの組み合わせです。

```text
<Action>ActionRequest
  hako_action_msgs/ActionRequestHeader header
  <Action>Goal body

<Action>ActionResponse
  hako_action_msgs/ActionResponseHeader header
  <Action>Result body

<Action>ActionFeedback
  hako_action_msgs/ActionFeedbackHeader header
  <Action>Feedback body
```

この構成をAction Protocol v1のデータ契約の基準とします。

## 5. Registryで定義済みのHeader

### 5.1 ActionRequestHeader

```text
uint8     version
uint8     request_kind
uint8[2]  reserved
uint8[16] goal_id
```

用途:

```text
GOAL_REQUEST
CANCEL_REQUEST
```

`request_kind`により要求種別を区別します。

### 5.2 ActionResponseHeader

```text
uint8     version
uint8     response_kind
uint8     status
uint8     reserved
uint8[16] goal_id
```

用途:

```text
GOAL_RESPONSE
CANCEL_RESPONSE
RESULT
```

`response_kind`により応答種別を区別し、`status`はその種別に応じた判断またはterminal statusを表します。

### 5.3 ActionFeedbackHeader

```text
uint8     version
uint8[3]  reserved
uint8[16] goal_id
uint32    sequence_no
```

用途:

```text
FEEDBACK
```

## 6. 論理イベントとRegistry packetの対応

| 論理イベント | Registry packet | Header識別 | body |
| --- | --- | --- | --- |
| `GOAL_REQUEST` | `<Action>ActionRequest` | `request_kind=GOAL` | `<Action>Goal` |
| `GOAL_RESPONSE` | `<Action>ActionResponse` | `response_kind=GOAL` | 既定値・未使用 |
| `FEEDBACK` | `<Action>ActionFeedback` | Feedback専用Header | `<Action>Feedback` |
| `CANCEL_REQUEST` | `<Action>ActionRequest` | `request_kind=CANCEL` | 既定値・未使用 |
| `CANCEL_RESPONSE` | `<Action>ActionResponse` | `response_kind=CANCEL` | 既定値・未使用 |
| `RESULT` | `<Action>ActionResponse` | `response_kind=RESULT` | `<Action>Result` |

現行Generatorでは、Request packetのbody型は常に`<Action>Goal`、Response packetのbody型は常に`<Action>Result`です。

そのため、Cancel RequestおよびGoal／Cancel ResponseではbodyをProtocol意味論上使用しません。body領域には生成型の既定値を格納します。

既定値は、生成されたAction body型を通常どおり初期化した値とします。未使用bodyを特別なbyte列として扱いません。

```text
数値型      = 0
bool        = false
string      = empty
可変長配列  = length 0
固定長配列  = 各要素を既定値
ネスト型    = 各フィールドを再帰的に既定値
time        = sec 0 / nanosec 0
duration    = sec 0 / nanosec 0
```

受信側は、`request_kind`または`response_kind`によりbodyが未使用と判断した場合、その内容を検証せず参照しません。

この方式はpacket型を増やさず、既存Generatorの3 packet構成を維持します。

## 7. v1で追加しない共通フィールド

初稿で候補としていた次のフィールドは、現行PDU RegistryのAction Headerには存在しないため、v1 Headerへ追加しません。

```text
action_type_id
request_id
body_length
message_type
```

理由は次のとおりです。

### 7.1 action_type_id

Action Typeは、使用するPDU Schema／packet型およびendpoint設定によって識別します。packet Headerへ重複して保持しません。

### 7.2 request_id

v1は`goal_id`とpending ContextでGoal／Cancel応答を相関します。独立した`request_id`は追加しません。

Cancel再送の厳密な要求単位相関が必要になった場合は、将来のHeader versionで検討します。

### 7.3 body_length

可変長bodyの配置と長さは、PDU Registryの`MetaData / BaseData / HeapData`形式および生成Schemaが管理します。Action Headerへ独自のbody長を追加しません。

### 7.4 message_type

単一の`message_type`は追加せず、Registryで定義済みの次を使用します。

```text
request_kind
response_kind
Feedback専用packet型
```

## 8. 設定値

### 8.1 version

```text
version = 1
```

Headerの`uint8 version`を使用します。

### 8.2 goal_id

```text
uint8[16]
```

128-bit UUIDを16 byteで保持します。

### 8.3 request_kind

初期割り当て案:

```text
0 = UNSPECIFIED
1 = GOAL
2 = CANCEL
```

### 8.4 response_kind

初期割り当て案:

```text
0 = UNSPECIFIED
1 = GOAL
2 = CANCEL
3 = RESULT
```

### 8.5 status

`status`は`response_kind`によって意味を切り替えます。

```text
response_kind = GOAL / CANCEL:
  0 = UNSPECIFIED
  1 = ACCEPTED
  2 = REJECTED

response_kind = RESULT:
  0 = UNSPECIFIED
  1 = SUCCEEDED
  2 = CANCELED
  3 = ABORTED
```

### 8.6 sequence_no

```text
uint32 sequence_no
初期値 = 0
Feedback送信成功ごとに1増加
```

採番はGoalごとにServer Runtimeが所有します。送信失敗時は番号を進めず、明示的な再試行では同じ番号を使用します。Client RuntimeはGoalごとの期待値と一致するFeedbackだけを上位Applicationへ配送し、重複、逆転、欠番を診断対象として無視します。`uint32`の最大値後はmoduloで0へ戻ります。

ROS 2 ActionのFeedback契約にはsequence番号がないため、ROS Bridgeは`sequence_no`をROS Feedbackへ露出しません。これは箱庭Runtime内部の配送検査用フィールドです。

### 8.7 未使用bodyの既定値

Cancel Request、Goal Response、Cancel Responseで使用しないbodyは、PDU Registryが生成した型の通常の既定値で初期化します。

```text
CANCEL_REQUEST:
  <Action>Goal body = default initialized

GOAL_RESPONSE:
  <Action>Result body = default initialized

CANCEL_RESPONSE:
  <Action>Result body = default initialized
```

未使用bodyへProtocol上の情報を埋め込むことは禁止します。受信側は未使用bodyを検証・解釈しません。

### 8.8 reserved

reserved領域は送信時に`0`で初期化し、受信時は値に依存しません。

## 9. Registryとの責務境界

```text
04 / 05:
  状態とイベント処理規則

06:
  通信端点、代表シーケンス、Header値の意味

hakoniwa-pdu-registry:
  .msg定義
  固定Header型
  Action固有body型
  バイトレイアウト
  アライメント
  MetaData / BaseData / HeapData
```

byte order、padding、可変長body、固定サイズはPDU Registryの生成規則へ委ねます。本Protocol文書では独自に再定義しません。

## 10. 現時点の設計判断

- PDU Registryの既存Action Headerをv1データ契約の基準とする。
- packetはRequest、Response、Feedbackの3種類とし、kindで論理イベントを識別する。
- Action TypeはSchema／endpointで識別し、Headerへ`action_type_id`を追加しない。
- v1では`request_id`を追加せず、`goal_id`とpending Contextで相関する。
- body長はPDU Registryへ委ね、Headerへ`body_length`を追加しない。
- Cancel RequestおよびGoal／Cancel Responseでは生成済みbody型を通常の型既定値で初期化し、意味論上は使用しない。
- 受信側は未使用bodyを検証・解釈しない。
- `sequence_no`は`uint32`で0開始とする。
- reservedは0送信、受信時無視とする。

## 11. 最小レビュー事項

1. `request_kind`の数値割り当てを`GOAL=1`、`CANCEL=2`とするか。
2. `response_kind`の数値割り当てを`GOAL=1`、`CANCEL=2`、`RESULT=3`とするか。
3. `status`をresponse kind依存の共用フィールドとするか。
4. 未使用bodyを生成型の通常の既定値で初期化し、受信側が解釈しない方針でよいか。
5. `sequence_no`はGoalごとに0開始、送信成功時のみ加算する。

## 12. 対象外

- 状態遷移規則の再定義
- 全異常シーケンスの列挙
- Transport固有のretry、再接続、fragmentation
- PDU Registry生成物のバイトレイアウト再定義
- Action固有Goal／Result／Feedback bodyの内容
