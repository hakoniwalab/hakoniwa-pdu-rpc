# Hakoniwa Action Protocol概要

> **Status: Draft**  
> 本文書は、Hakoniwa Action Protocolの通信端点、代表的な通信順序、論理パケット項目を直感的に理解するための概要です。

## 1. 目的

本書は、Action通信について次の3点を示します。

```text
誰から誰へ送るか
何を送るか
どの順番で送るか
```

状態とイベントの網羅的な許容規則は、以下を正とします。

- Server Runtime: `04-state-model.md`
- Client Runtime: `05-client-state-model.md`

本書では、それらの状態遷移を重複して再定義しません。

## 2. 通信端点

```text
Client Application
  -> Client Runtime
  -> Transport
  -> Server Runtime
  -> Server Application
```

Client RuntimeとServer Runtimeの間では、次の共通Protocolイベントを使用します。

```text
GOAL_REQUEST
GOAL_RESPONSE
FEEDBACK
CANCEL_REQUEST
CANCEL_RESPONSE
RESULT
```

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
      |                                 |
```

- Goal Responseが`ACCEPTED`の場合、ClientとServerはGoalを`EXECUTING`として扱います。
- Feedbackは0回以上送信できます。
- Result受信後、Client Runtimeは`FINISHING`へ進み、Application通知後にContextを解放します。

### 3.2 Goal拒否

```text
Client Runtime                    Server Runtime
      |                                 |
      |--- GOAL_REQUEST --------------->|
      |                                 |--- Server ApplicationへGoal通知
      |                                 |<-- reject
      |<-- GOAL_RESPONSE(REJECTED) ------|
      |                                 |
```

拒否されたGoalはaccept済みGoalの主状態を持ちません。

### 3.3 Client起因Cancel

```text
Client Runtime                    Server Runtime
      |                                 |
      |--- CANCEL_REQUEST ------------->|
      |                                 |--- Server ApplicationへCancel通知
      |                                 |<-- accept cancel
      |<-- CANCEL_RESPONSE(ACCEPTED) ----|
      |                                 |
      |<-- FEEDBACK [0..n] --------------|
      |                                 |
      |<-- RESULT(CANCELED) --------------|
      |                                 |
```

- Cancel Request送信だけではClient状態を変更しません。
- Cancel Responseが`ACCEPTED`の場合、ClientとServerはGoalを`CANCELING`として扱います。
- Cancel Responseが`REJECTED`の場合、Goalは`EXECUTING`を維持します。

### 3.4 Runtime起因Cancel

```text
Server Runtime                    Server Application
      |                                 |
      |--- RUNTIME_CANCEL_REQUESTED ---->|
      |                                 |
      |<-- ACCEPT_CANCEL ----------------|
      |                                 |
      |<-- COMPLETE_CANCELED ------------|
      |                                 |
      |--- RESULT(CANCELED) ------------> Client Runtime
```

Runtime起因CancelはClient Cancelを偽装しません。Client endpointが切断済みの場合、Cancel Responseは生成せず、Resultの配送・保持はRuntime Policyへ委譲します。

## 4. 論理パケット一覧

本書では、バイト配置ではなく論理フィールドを定義します。

### 4.1 共通フィールド

| Field | 型・表現 | 説明 |
| --- | --- | --- |
| `protocol_version` | unsigned integer | Protocol互換性を識別する。v1初期値は`1` |
| `message_type` | enum | パケット種別 |
| `action_type_id` | identifier | Action Typeを識別する |
| `goal_id` | 128-bit UUID | Goal lifecycleを識別する |
| `request_id` | unsigned integer | RequestとResponseを相関する。Request/Response系のみ有効 |
| `body_length` | unsigned integer | bodyのbyte長 |

`goal_id`と`request_id`の責務を分離します。

```text
goal_id:
  Goalインスタンスの識別

request_id:
  個々のRequest / Responseの相関
```

### 4.2 GOAL_REQUEST

| Field | 説明 |
| --- | --- |
| 共通フィールド | `message_type=GOAL_REQUEST` |
| `goal_body` | Action固有Goalデータ |

通常ClientではClient Runtimeが`goal_id`を生成します。ROS BridgeなどのAdapterは、外部UUIDを`goal_id`として指定できます。

### 4.3 GOAL_RESPONSE

| Field | 説明 |
| --- | --- |
| 共通フィールド | `message_type=GOAL_RESPONSE`。Requestと同じ`goal_id`および`request_id` |
| `decision` | `ACCEPTED` / `REJECTED` |
| `reason_code` | reject理由。accept時は`NONE` |

### 4.4 FEEDBACK

| Field | 説明 |
| --- | --- |
| 共通フィールド | `message_type=FEEDBACK`。`request_id`は使用しない |
| `sequence_no` | Goalごとの単調増加番号 |
| `feedback_body` | Action固有Feedbackデータ |

`sequence_no`の初期値は`0`とします。

### 4.5 CANCEL_REQUEST

| Field | 説明 |
| --- | --- |
| 共通フィールド | `message_type=CANCEL_REQUEST` |

v1では、Cancel Requestは単一`goal_id`を対象とします。

ROS 2の全Goal Cancelおよび時刻指定Cancelは、`hakoniwa-pdu-ros`が対象Goalを列挙し、単一Goal Cancelへ分解します。

### 4.6 CANCEL_RESPONSE

| Field | 説明 |
| --- | --- |
| 共通フィールド | `message_type=CANCEL_RESPONSE`。Requestと同じ`goal_id`および`request_id` |
| `decision` | `ACCEPTED` / `REJECTED` |
| `reason_code` | reject理由。accept時は`NONE` |

### 4.7 RESULT

| Field | 説明 |
| --- | --- |
| 共通フィールド | `message_type=RESULT`。`request_id`は使用しない |
| `terminal_status` | `SUCCEEDED` / `CANCELED` / `ABORTED` |
| `result_body` | Action固有Resultデータ |

ResultをServer pushとClient pullのどちらで配送するかは、実装Protocolで確定します。どちらの場合も論理Resultの内容は同じです。

## 5. enum設定値

### 5.1 message_type

```text
1 = GOAL_REQUEST
2 = GOAL_RESPONSE
3 = FEEDBACK
4 = CANCEL_REQUEST
5 = CANCEL_RESPONSE
6 = RESULT
```

`0`は未設定・無効値として予約します。

### 5.2 decision

```text
0 = UNSPECIFIED
1 = ACCEPTED
2 = REJECTED
```

### 5.3 terminal_status

```text
0 = UNSPECIFIED
1 = SUCCEEDED
2 = CANCELED
3 = ABORTED
```

### 5.4 reason_code

初版では次の最小集合を定義します。

```text
0 = NONE
1 = INVALID_REQUEST
2 = UNKNOWN_GOAL_ID
3 = DUPLICATE_GOAL_ID
4 = APPLICATION_REJECTED
5 = COMPLETION_COMMITTED
6 = INTERNAL_ERROR
```

Action固有の詳細理由はbodyまたは将来拡張へ委ねます。

## 6. 型とエンコードの既定方針

- 整数値は符号なし固定幅整数を使用する。
- multi-byte整数のbyte orderはlittle-endianとする。
- `goal_id`は16 byte UUIDとして扱う。
- `request_id`はRequest送信元が採番し、同一endpoint内で未完了Requestと重複しない値とする。
- `sequence_no`はGoal単位で`0`から始まり、Feedbackごとに1増加する。
- bodyはAction TypeごとのSchemaに従う。
- アライメント、padding、固定ヘッダサイズ、body配置はPDUデータモデル／Registry定義で確定する。

## 7. 設計意図

本Protocol概要は、通信の姿を直感的に理解できることを目的とします。

```text
04 / 05:
  状態とイベント処理規則

06:
  通信端点、代表シーケンス、論理パケット項目

PDUデータモデル:
  バイトレイアウト、型幅、アライメント、Schema
```

異常系、重複、遅延、timeout、切断、競合を本書で網羅しません。これらの許容可否と状態遷移は`04-state-model.md`および`05-client-state-model.md`を参照します。

## 8. レビューで確認する事項

1. `request_id`をGoal Request／Cancel Requestの双方で必須とするか。
2. `message_type`、`decision`、`terminal_status`の数値割り当てが妥当か。
3. `reason_code`の初期集合が過不足ないか。
4. `sequence_no`を`0`開始とするか。
5. little-endianをProtocol既定とするか、Transport／CDRへ委譲するか。
6. Result push/pullをどの後続文書で確定するか。

## 9. 対象外

- 状態遷移規則の再定義
- 全異常シーケンスの列挙
- Transport固有のretry、再接続、fragmentation
- C/C++構造体レイアウト
- PDU Registryの具体的Schema
- bodyのAction Type固有定義
