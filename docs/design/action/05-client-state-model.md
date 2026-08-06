# Hakoniwa Action Clientの状態モデル

> **Status: Draft**  
本文書はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 1. 目的

本書では、Hakoniwa Action ProtocolにおけるClient RuntimeのGoal状態とイベント処理規則を定義します。

ClientとServerの接続を単純に保ち、`hakoniwa-pdu-ros`からROS 2 Action Clientへ自然に写像できることを重視します。

基本方針は次のとおりです。

- accept済みGoalの主状態はServerと同じ名称と意味を使用する。
- Goal Response待ちやCancel Response待ちを新しいGoal状態にしない。
- 通信待ちはROS 2のFutureに相当するpending Contextとして管理する。
- ClientとServer間のProtocolイベントは共通のデータ契約を使用する。
- 通信異常をGoalの`CANCELED`または`ABORTED`へ自動変換しない。

## 2. 適用範囲

本状態モデルは、Client Runtimeが`goal_id`単位で管理するGoal Contextを対象とします。

以下は本状態モデルへ含めません。

- Client ApplicationがGoal bodyを構築している状態
- ROS 2固有のexecutor、callback group、Future実装
- Transport固有の再接続手順
- Runtime自身が正規のイベント処理を継続できない致命的内部障害
- Result受信後の具体的なContext保持時間

## 3. Serverと共有する主状態

accept済みGoalについて、Client RuntimeはServerと同じ3つの主状態を使用します。

```text
EXECUTING
CANCELING
FINISHING
```

### 3.1 EXECUTING

ServerがGoalをacceptし、Goalが実行中であるとClient Runtimeが認識している状態です。

Client ApplicationがCancelを要求しただけでは`CANCELING`へ遷移しません。Cancel Responseで受理されたことを確認するまでは`EXECUTING`を維持します。

### 3.2 CANCELING

ServerがCancel Requestをacceptし、Goalが停止処理中であるとClient Runtimeが認識している状態です。

```text
EXECUTING
  -> REQUEST_CANCEL
  -> Cancel Request送信
  -> Cancel Response ACCEPTED受信
  -> CANCELING
```

### 3.3 FINISHING

Client Runtimeがterminal Resultを受信し、Client Applicationへの完了通知、Future解決、callback実行、Goal Context解放を処理している内部状態です。

Server側の`FINISHING`はResult配送・保持を管理します。Client側の`FINISHING`はResult受信後の通知・解放を管理します。名称は共通ですが、各Runtimeが所有する処理は対称です。

```text
EXECUTING -> FINISHING
CANCELING -> FINISHING
FINISHING -> RELEASE
```

## 4. Goal確立前のContext

`SEND_GOAL`を呼び出した時点で、Client RuntimeはGoal Requestを送信するためのContextを作成します。

初期APIでは、通常のHakoniwa Clientも上位ApplicationがUUID形式の`goal_id`を指定します。ROS BridgeなどのAdapterは、外部で生成された互換UUIDをそのまま指定します。Runtimeは自動生成せず、all-zeroまたはactive Goalとの衝突を同期エラーとして拒否します。Runtime自動生成helperはpendingです。

Goal Responseを受信するまでは、accept済みGoalの主状態を持ちません。

```text
SEND_GOAL
  -> 上位Application／Adapter指定のgoal_idを検証する
  -> Goal Context生成
  -> Goal Request送信
  -> goal_response_pending = true
```

Goal Responseの結果に応じて処理します。

```text
Goal Response ACCEPTED
  -> goal_response_pending = false
  -> EXECUTING

Goal Response REJECTED
  -> goal_response_pending = false
  -> Client Applicationへreject通知
  -> RELEASE

REQUEST_SEND_FAILED(GOAL)
  -> goal_response_pending = false
  -> Client Applicationへ送信失敗を通知
  -> RELEASE

RESPONSE_TIMEOUT(GOAL_RESPONSE)
  -> goal_response_pending = false
  -> Client Applicationへtimeoutを通知
  -> RELEASE
```

初版Client APIの`send_goal(..., timeout_usec)`は、このGoal Response待ちにだけ適用します。`timeout_usec=0`はGoal Response timeoutなしを表します。Goalがacceptされた後のResult待ち、およびCancel Response待ちには同じ値を流用しません。それらのtimeout／保持Policyは初版の公開契約へ含めません。

Goal Response timeout時、Goal RequestがServerへ到達し、Server側でaccept済みとなっている可能性があります。しかし、Client Runtimeはこの可能性を理由に追加主状態、状態照会、自動再送、特別な救済処理を導入しません。Client側では通信失敗としてContextを解放し、Server側で残存Goalが発生し得ることは異常ケースとして扱います。

`GOAL_REQUESTING`や`ACCEPTED`などの追加主状態は設けません。これはROS 2 Action ClientがGoal Response待ちをFutureで表現する考え方と整合します。

## 5. Client Application APIイベント

初版で必須とするClient Application起因イベントは、次の2つです。

```text
SEND_GOAL(goal_body)
REQUEST_CANCEL(goal_id)
```

### 5.1 SEND_GOAL

新しいGoal Requestを送信します。`SEND_GOAL`はaccept済みGoalの状態マトリクス外で扱います。

Runtimeは少なくとも次を行います。

- Goal bodyのローカル検査
- UUID形式の`goal_id`生成、またはAdapterが指定した互換UUIDの受け入れ
- Goal Context生成
- Goal Request送信
- Goal Response待ちContext生成

### 5.2 REQUEST_CANCEL

accept済みGoalに対してCancel Requestを送信します。

```text
EXECUTING + REQUEST_CANCEL
  Decision: ALLOW
  Action:
    Cancel Requestを送信
    cancel_response_pending = true
  Next: EXECUTING
```

Cancel Requestを送信しただけではGoal状態を変更しません。

`CANCELING`または`FINISHING`での二重呼び出しは、Client Application APIの誤用として扱います。

Hakoniwa Action Protocol v1の`REQUEST_CANCEL`は単一`goal_id`だけを対象とします。ROS 2の全Goal Cancelや時刻指定Cancelは、`hakoniwa-pdu-ros`が自身の管理するGoalHandleを列挙し、対象Goalごとの単一`REQUEST_CANCEL`へ分解します。一括Cancelを共通Protocolの主状態やイベントへ持ち込みません。

## 6. ClientとServerで共通のProtocolイベント

ClientとServer間では、同じAction Protocolイベントを送信側と受信側で対称に扱います。

```text
GOAL_REQUEST
GOAL_RESPONSE
FEEDBACK
CANCEL_REQUEST
CANCEL_RESPONSE
RESULT
```

Client状態モデルでは、受信イベントを次の抽象名で記述します。

```text
GOAL_RESPONSE_RECEIVED(goal_id, decision)
FEEDBACK_RECEIVED(goal_id, feedback_body, sequence_no)
CANCEL_RESPONSE_RECEIVED(goal_id, decision)
RESULT_RECEIVED(goal_id, terminal_status, result_body)
```

イベント名は説明用です。PDUの具体的な`request_kind`、`response_kind`およびバイトレイアウトはProtocol文書で定義します。

## 7. pending Context

Client固有の通信待ちはGoalの主状態にせず、Goal Contextに保持します。

```text
goal_response_pending = true / false
cancel_response_pending = true / false
result_pending = true / false
```

### 7.1 goal_response_pending

Goal Request送信後、Goal Response受信までを表します。

### 7.2 cancel_response_pending

Cancel Request送信後、Cancel Response受信までを表します。

`cancel_response_pending=true`の間も主状態は`EXECUTING`です。

### 7.3 result_pending

acceptされたGoalについてterminal Resultを待っていることを表します。

Resultをpushで受信するか、Clientが明示的に取得要求を送るかはProtocol文書で決定します。どちらの場合も、待ち状態を新しいGoal主状態として追加しません。

## 8. Client Application APIイベント × 状態マトリクス

| Event | EXECUTING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `REQUEST_CANCEL` | `cancel_response_pending=false`なら`ALLOW`: Cancel Request送信、pendingを設定。`SAME`。すでにpendingなら`APPLICATION_API_ERROR` | `APPLICATION_API_ERROR`: ServerがCancel受理済み。`SAME` | `APPLICATION_API_ERROR`: terminal Result受信済み。`SAME` |

unknown `goal_id`へのAPI呼び出しも`APPLICATION_API_ERROR`とします。

通信再送や重複PDUへの冪等処理はRuntime／Protocolの責務です。Client Application APIの二重呼び出しを冪等に救済することとは区別します。

## 9. Protocol受信イベント × 状態マトリクス

| Event | EXECUTING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `FEEDBACK_RECEIVED` | `ALLOW`: sequenceを検査しApplicationへ通知。`SAME` | `ALLOW`: Serverが送信した停止進捗をApplicationへ通知。`SAME` | 遅延Feedbackとして`IGNORE`または診断記録。`SAME` |
| `CANCEL_RESPONSE_RECEIVED(ACCEPTED)` | `cancel_response_pending=true`なら`ALLOW`: pendingを解除し`CANCELING`へ。falseなら`IGNORE`: 診断を記録し`SAME` | 重複応答として`IDEMPOTENT`または診断記録。`SAME` | 遅延応答として`IGNORE`または診断記録。`SAME` |
| `CANCEL_RESPONSE_RECEIVED(REJECTED)` | `cancel_response_pending=true`なら`ALLOW`: pendingを解除し`EXECUTING`維持。falseなら`IGNORE`: 診断を記録し`SAME` | Server状態との不整合として診断記録し`IGNORE`。`SAME` | 遅延応答として`IGNORE`または診断記録。`SAME` |
| `RESULT_RECEIVED` | `ALLOW`: terminal statusとResultを確定し`FINISHING`へ | `ALLOW`: terminal statusとResultを確定し`FINISHING`へ | 重複Resultとして冪等処理または診断記録。`SAME` |

pending Contextが存在しないCancel Responseは、Client Application APIの誤用ではありません。相関不能、遅延、重複したProtocol応答としてApplicationへ通知せず、診断を記録して`IGNORE`します。

`RESULT_RECEIVED`に含まれるterminal statusは、少なくとも`SUCCEEDED`、`CANCELED`、`ABORTED`を扱います。

Server側がRuntime起因Cancelを開始した場合、Client endpointが切断済みならCancel Responseを受信しない可能性があります。再接続後の状態照会、Result再取得、遅延Cancel Responseの扱いは後続Protocolで定義します。

## 10. Client Runtime / Transportイベント

初版では、Runtimeが正常に観測・通知できる通信系イベントだけを状態モデルへ含めます。

```text
REQUEST_SEND_FAILED(goal_id, request_kind)
RESPONSE_TIMEOUT(goal_id, response_kind)
TRANSPORT_DISCONNECTED
CLIENT_SHUTDOWN_REQUESTED
```

Runtime自体のクラッシュ、メモリ破壊、プロセス強制終了など、正規のイベント処理を継続できない障害は対象外です。

## 11. Runtime / Transportイベント × 状態マトリクス

| Event | EXECUTING | CANCELING | FINISHING |
| --- | --- | --- | --- |
| `REQUEST_SEND_FAILED(CANCEL)` | pendingを解除し、Client Applicationへ送信失敗を通知。Goal terminal statusは変更しない。`SAME` | 通常は対象外。発生時は診断記録。`SAME` | 通常は対象外。`SAME` |
| `RESPONSE_TIMEOUT(CANCEL_RESPONSE)` | timeout policyへ`DEFER`。Goal terminal statusは変更しない。`SAME` | 通常は対象外 | 通常は対象外 |
| `RESPONSE_TIMEOUT(RESULT)` | timeout policyへ`DEFER`。Goal terminal statusは変更しない。`SAME` | 同左 | 通常は対象外 |
| `TRANSPORT_DISCONNECTED` | Applicationへ通信切断を通知し、再接続Policyへ`DEFER`。`SAME` | 同左。Server側の停止処理を推測しない。`SAME` | Result通知・Context解放の状況に応じて保持Policyへ`DEFER` |
| `CLIENT_SHUTDOWN_REQUESTED` | shutdown policyへ`DEFER`。Goalを自動的に`CANCELED`または`ABORTED`へ変更しない | 同左 | Application通知とContext解放Policyへ`DEFER` |

Goal確立前の`REQUEST_SEND_FAILED(GOAL)`および`RESPONSE_TIMEOUT(GOAL_RESPONSE)`は4節で定義します。どちらもaccept済みGoalの主状態を生成せず、Client Applicationへ通信失敗を通知してGoal確立前Contextを解放します。

Goal Response timeout後にServer側へGoalが残る可能性はありますが、初版Client Runtimeは状態照会、自動再送、受理状態UNKNOWNなどの特別な救済を提供しません。このケースはServer側の異常ケースとして記録し、Client状態モデルは単純な通信失敗として閉じます。

## 12. 通信異常とGoal terminal statusの分離

Client Runtimeは、通信切断、timeout、送信失敗をGoalのterminal statusへ自動変換しません。

```text
TRANSPORT_DISCONNECTED
  != CANCELED
  != ABORTED

RESPONSE_TIMEOUT
  != CANCELED
  != ABORTED
```

accept済みGoalの通信異常時点では、Server上のGoalが`EXECUTING`、`CANCELING`、terminal済みのいずれであるかをClientが確定できないためです。

accept済みGoalについては、Client Runtimeが通信異常をClient Applicationへ通知し、Goal Contextを保持したうえで、再接続、状態照会、Result再取得、保持期限による解放などのPolicyへ委譲します。

Goal確立前のGoal Request送信失敗またはGoal Response timeoutは、Client Applicationへ通信失敗を通知してContextを解放します。Goal Response timeout時にServer側でGoalが残り得ることを理由に、Client側へ新しい主状態や救済Protocolを追加しません。

## 13. ROS 2 Action Clientとの親和性

Client状態モデルは、ROS 2 Action ClientのGoalHandleとFutureの構造へ自然に写像できることを重視します。

| Hakoniwa Client | ROS 2 Action Client | 考え方 |
| --- | --- | --- |
| `SEND_GOAL` | `async_send_goal` / `send_goal_async` | Goal送信API |
| `goal_response_pending` | GoalHandle Future pending | Goal Response待ちを主状態にしない |
| `EXECUTING` | accepted Goalの`EXECUTING` | Server状態を観測 |
| `REQUEST_CANCEL` | `async_cancel_goal` / `cancel_goal_async` | Cancel要求API |
| `cancel_response_pending` | Cancel Future pending | Cancel応答待ちを主状態にしない |
| `CANCELING` | `CANCELING` | ServerがCancelを受理した状態 |
| `RESULT_RECEIVED` | Result Future completion | terminal Result受信 |
| `FINISHING` | Future解決、callback実行、GoalHandle後処理 | Hakoniwa Runtime内部状態 |
| 単一`goal_id` Cancel | ROS 2の単一・複数Goal Cancel | Bridgeが複数対象をgoal_id単位へ分解 |
| Goal Context管理 | ROS 2 status topic生成元 | Bridgeが管理中GoalからROS側statusを生成 |

箱庭独自の`FINISHING`はROS 2の公開Goal Statusへ変換しません。`hakoniwa-pdu-ros`内部でResult Future解決やcallback実行を行う後処理として閉じ込めます。

Hakoniwa共通ProtocolはROS 2のstatus topic相当のブロードキャストを持ちません。`hakoniwa-pdu-ros`が自身の管理するGoalHandleとHakoniwa Goal状態からROS 2のstatus topicを生成します。status topicはClient側主状態へ新しいイベントを追加する理由にはしません。

Client RuntimeはServer状態を推測して先行遷移しません。

```text
REQUEST_CANCEL
  -> EXECUTING維持

CANCEL_RESPONSE ACCEPTED
  -> CANCELING

RESULT_RECEIVED
  -> FINISHING
```

この制約により、Hakoniwa ClientとROS 2 GoalHandleの状態認識を一致させやすくします。

## 14. 現時点の設計判断

- accept済みGoalの主状態はServerと同じ`EXECUTING`、`CANCELING`、`FINISHING`を使用する。
- Goal Response待ち、Cancel Response待ち、Result待ちはpending Contextとして管理する。
- `GOAL_REQUESTING`、`CANCEL_REQUESTING`、`RESULT_WAITING`などの主状態を追加しない。
- Client Application APIは初版では`SEND_GOAL`と`REQUEST_CANCEL`を必須とする。
- 初期APIでは通常ClientとAdapterのどちらも上位層が`goal_id`を指定し、Runtime自動生成はpendingとする。
- Hakoniwa Protocol v1のCancelは単一`goal_id`を対象とし、ROS 2の一括CancelはBridgeが単一Goal Cancelへ分解する。
- Cancel Request送信だけでは`CANCELING`へ遷移しない。
- Cancel Response ACCEPTED受信時に`CANCELING`へ遷移する。
- pending ContextのないCancel Responseは診断を記録して`IGNORE`する。
- Result受信時に`FINISHING`へ遷移し、Application通知後にContextを解放する。
- ClientとServer間のProtocolイベントは共通のデータ契約を使用する。
- Client Application APIの二重呼び出しは`APPLICATION_API_ERROR`として扱う。
- Goal Request送信失敗とGoal Response timeoutは、Client側では通信失敗としてGoal確立前Contextを解放する。
- Goal Response timeout後にServer側Goalが残る可能性に対し、Client側へ特別な状態、照会、自動再送を追加しない。
- Hakoniwa共通ProtocolはROS 2 status topic相当を持たず、ROS Bridgeが管理中GoalからROS statusを生成する。
- 通信異常をGoalの`CANCELED`または`ABORTED`へ自動変換しない。
- Runtimeの致命的内部障害からの正規状態遷移は初版の対象外とする。

## 15. レビューで確認する事項

1. `CANCELING`中に受信したFeedbackを常にClient Applicationへ通知するか。
2. 重複Cancel Responseを冪等処理またはIGNOREのどちらにするか。
3. `FINISHING`中の遅延FeedbackおよびCancel ResponseをIGNOREとするか、診断対象とするか。
4. 重複Resultを冪等処理するために保持すべき情報は何か。
5. ResultをServer pushとClient pullのどちらで配送するか。
6. Cancel ResponseおよびResultのtimeout Policyをどこで定義するか。
7. accept済みGoalの通信切断後に、再接続、状態照会、Result再取得、Context解放条件をどう定義するか。
8. Client shutdown時に未終端GoalへCancelを試行するか。

## 16. 対象外

- PDUのバイトレイアウト
- 公開APIの具体的な関数シグネチャ
- ROS 2 executorおよびFuture実装の詳細
- Transport固有のretryおよび再接続実装
- Goal Response timeout後の自動照会、自動再送、受理状態UNKNOWNの導入
- Runtimeが正規のイベント処理を継続できない致命的障害
- Resultおよび終了済み`goal_id`の具体的な保持時間
