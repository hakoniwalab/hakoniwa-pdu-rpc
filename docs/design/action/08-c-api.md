# Hakoniwa Action C API設計

> **Status: Draft**  
> 本文書は、Hakoniwa Action Runtimeをユーザーアプリケーションおよび`hakoniwa-pdu-ros`から利用するためのC APIの型、ライフサイクル、操作モデルを定義します。厳密なABI、全関数シグネチャ、設定Schemaは後続レビューで確定します。

## 1. 目的

Action C APIは、既存RPC Service C APIとの構造的対称性を保ちながら、Action固有の長寿命Goal lifecycleと複数同時Goalを扱えることを目的とします。

利用者の主対象は次のとおりです。

```text
Client C API:
  hakoniwa-pdu-rosなどのAdapter

Server C API:
  箱庭上のAction Server Application
```

Client APIをROS 2固有APIにはしません。C APIはpoll型のAction Runtime APIを提供し、`hakoniwa-pdu-ros`がROS 2 ActionのFuture、callback、GoalHandleへ変換します。

## 2. RPC Service C APIとの対称性

既存RPC Service C APIの基本形を踏襲します。

```text
opaque handle
create / destroy
start / stop
明示的poll
イベントenum
caller-owned buffer
*_alloc API
buffer_free
```

対応関係は次のとおりです。

| RPC Service | Action |
| --- | --- |
| `hako_pdu_rpc_client_handle_t` | `hako_pdu_action_client_handle_t` |
| `hako_pdu_rpc_server_handle_t` | `hako_pdu_action_server_handle_t` |
| `client_call()` | `client_send_goal()` |
| `client_cancel()` | `client_cancel_goal()` |
| `client_poll()` | `client_poll()` |
| `server_poll()` | `server_poll()` |
| `request_token` | `event_token` |
| 1回のReply | Goal判断、Feedback、Cancel判断、Result |

Actionでは、RPC Serviceの次の制約を引き継ぎません。

```text
1 client handle = 1 in-flight request
```

Action Client handleは、異なる`goal_id`を持つ複数Goalを同時に管理できます。

## 3. 公開識別子

### 3.1 goal_id

Protocol上のGoal識別子です。

```c
typedef struct {
    uint8_t bytes[16];
} hako_pdu_action_goal_id_t;
```

- Client Runtimeが通常生成します。
- ROS Adapterなどは、外部で生成された互換UUIDを指定できます。
- Client／Server間のProtocol相関に使用します。
- Client poll結果およびServer poll結果へ含めます。

### 3.2 event_token

Server Applicationが、pollで取得した一つの受信イベントへ判断を返すためのRuntime-local tokenです。

```c
typedef uint64_t hako_pdu_action_event_token_t;
```

対象イベント:

```text
GOAL_REQUEST
CANCEL_REQUEST
RUNTIME_CANCEL_REQUEST
```

性質:

- Server handle内だけで有効です。
- Protocolへ送信しません。
- Goal／Cancelのacceptまたはreject時に消費します。
- 一つのevent tokenを複数回判断に使用できません。
- `event_token`はGoal lifecycleの継続操作には使用しません。

### 3.3 goal_token

accept済みGoal ContextをServer Applicationが継続操作するためのRuntime-local tokenです。

```c
typedef uint64_t hako_pdu_action_goal_token_t;
```

用途:

```text
Feedback送信
Cancel判断後の処理
Succeeded完了
Canceled完了
Aborted完了
Goal情報参照
```

性質:

- Goal accept時にRuntimeが払い出します。
- Server handle内だけで有効です。
- Protocolへ送信しません。
- terminal Resultが確定し、Runtime保持責務が完了するまで有効です。
- terminal完了後の操作は`NOT_FOUND`または`INVALID_STATE`になります。

## 4. event_tokenとgoal_tokenを分離する理由

RPC Serviceの`request_token`は、次の単純なライフサイクルです。

```text
Request受信
  -> Applicationへ通知
  -> Reply送信
  -> token破棄
```

Actionは次の長寿命ライフサイクルです。

```text
Goal Request受信
  -> accept / reject
  -> Feedback 0..n
  -> Cancel Request 0..n
  -> terminal Result
```

したがって、一つのtokenに「受信イベントへの一回限りの判断」と「accept済みGoalの継続操作」の二つの責務を持たせません。

```text
event_token:
  受信イベントへの一回限りの判断

goal_token:
  accept済みGoalの継続操作
```

## 5. Client APIの利用モデル

### 5.1 主利用者

Client APIの主利用者は`hakoniwa-pdu-ros`などのAdapterです。

```text
ROS Goal Request
  -> Action Client C API send_goal

Action Client poll GOAL_RESPONSE
  -> ROS Goal acceptance / rejection

Action Client poll FEEDBACK
  -> ROS Feedback

ROS Cancel Request
  -> Action Client C API cancel_goal

Action Client poll CANCEL_RESPONSE
  -> ROS Cancel response

Action Client poll RESULT
  -> ROS Result Future completion
```

### 5.2 Client handle

```c
typedef struct hako_pdu_action_client_handle
    hako_pdu_action_client_handle_t;
```

一つのClient handleは、設定に含まれる複数Action Typeと、各Action Typeの複数Goal Contextを管理できます。

### 5.3 Client event

```c
typedef enum {
    HAKO_PDU_ACTION_CLIENT_EVENT_NONE = 0,
    HAKO_PDU_ACTION_CLIENT_EVENT_GOAL_RESPONSE,
    HAKO_PDU_ACTION_CLIENT_EVENT_FEEDBACK,
    HAKO_PDU_ACTION_CLIENT_EVENT_CANCEL_RESPONSE,
    HAKO_PDU_ACTION_CLIENT_EVENT_RESULT
} hako_pdu_action_client_event_t;
```

Transport errorやProtocol errorをeventへ含めるか、`out_error`だけで返すかは後続レビューで確定します。

### 5.4 Client event info

```c
typedef struct {
    char action_name[HAKO_PDU_ACTION_NAME_MAX];
    hako_pdu_action_goal_id_t goal_id;
    uint8_t status;
    size_t pdu_size;
} hako_pdu_action_client_event_info_t;
```

`status`は、イベント種別に応じてGoal Response、Cancel Response、Resultのstatusを保持します。Feedbackでは`UNSPECIFIED`とします。

### 5.5 Client操作の概念形

```c
hako_pdu_action_client_handle_t*
hako_pdu_action_client_create(...);

hako_pdu_action_error_t
hako_pdu_action_client_start(...);

hako_pdu_action_error_t
hako_pdu_action_client_create_goal_buffer(...);

hako_pdu_action_error_t
hako_pdu_action_client_send_goal(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    const uint8_t* goal_pdu,
    size_t goal_pdu_size,
    const hako_pdu_action_goal_id_t* requested_goal_id,
    hako_pdu_action_goal_id_t* out_goal_id);

hako_pdu_action_error_t
hako_pdu_action_client_cancel_goal(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_goal_id_t* goal_id);

hako_pdu_action_client_event_t
hako_pdu_action_client_poll(...);

hako_pdu_action_error_t
hako_pdu_action_client_stop(...);

void
hako_pdu_action_client_destroy(...);
```

`requested_goal_id == NULL`の場合はRuntimeがUUIDを生成し、`out_goal_id`へ返します。外部AdapterがUUIDを保持している場合は、`requested_goal_id`を指定できます。

### 5.6 Client pollの意味

`poll()`は、一回の呼び出しで最大一つのイベントを返します。

```text
NONE
GOAL_RESPONSE
FEEDBACK
CANCEL_RESPONSE
RESULT
```

イベントには必ず`action_name`と`goal_id`を含めます。これによりAdapterは、複数同時Goalを各ROS GoalHandleへ配送できます。

`poll()`はcallbackを実行せず、Runtime内部queueからイベントを取り出します。

## 6. Server APIの利用モデル

### 6.1 主利用者

Server APIはAction Server Applicationが使用します。

Applicationの責務:

```text
Goalをaccept / rejectする
Cancelをaccept / rejectする
Feedbackを生成する
terminal Resultとstatusを確定する
```

Runtimeの責務:

```text
goal_id検証と重複検査
Goal Context管理
状態遷移検証
PDU Header生成
受信queueと送信
遅延／重複イベント処理
```

### 6.2 Server handle

```c
typedef struct hako_pdu_action_server_handle
    hako_pdu_action_server_handle_t;
```

一つのServer handleは、設定に含まれる複数Action Typeを管理します。

### 6.3 Server event

```c
typedef enum {
    HAKO_PDU_ACTION_SERVER_EVENT_NONE = 0,
    HAKO_PDU_ACTION_SERVER_EVENT_GOAL_REQUEST,
    HAKO_PDU_ACTION_SERVER_EVENT_CANCEL_REQUEST,
    HAKO_PDU_ACTION_SERVER_EVENT_RUNTIME_CANCEL_REQUEST
} hako_pdu_action_server_event_t;
```

### 6.4 Server event info

```c
typedef struct {
    hako_pdu_action_event_token_t event_token;
    hako_pdu_action_goal_token_t goal_token;
    char action_name[HAKO_PDU_ACTION_NAME_MAX];
    hako_pdu_action_goal_id_t goal_id;
    size_t pdu_size;
} hako_pdu_action_server_event_info_t;
```

`goal_token`の値:

```text
GOAL_REQUEST:
  accept前なので0

CANCEL_REQUEST:
  対象のaccept済みGoalのgoal_token

RUNTIME_CANCEL_REQUEST:
  対象のaccept済みGoalのgoal_token
```

### 6.5 Goal Request処理

```text
server_poll()
  -> GOAL_REQUEST
  -> event_token + goal_id + Goal PDU

Application判断:
  accept_goal(event_token)
    -> out_goal_token

  または

  reject_goal(event_token)
```

概念形:

```c
hako_pdu_action_error_t
hako_pdu_action_server_accept_goal(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_event_token_t event_token,
    hako_pdu_action_goal_token_t* out_goal_token);

hako_pdu_action_error_t
hako_pdu_action_server_reject_goal(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_event_token_t event_token);
```

Goal Response PDUのbodyはProtocol既定値をRuntimeが生成します。Applicationは未使用Result bodyを組み立てません。

### 6.6 Cancel Request処理

```text
server_poll()
  -> CANCEL_REQUEST
  -> event_token + goal_token + goal_id

Application判断:
  accept_cancel(event_token)
  reject_cancel(event_token)
```

概念形:

```c
hako_pdu_action_error_t
hako_pdu_action_server_accept_cancel(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_event_token_t event_token);

hako_pdu_action_error_t
hako_pdu_action_server_reject_cancel(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_event_token_t event_token);
```

Cancel Response PDUのbodyはProtocol既定値をRuntimeが生成します。

### 6.7 Feedback送信

```c
hako_pdu_action_error_t
hako_pdu_action_server_create_feedback_buffer(...);

hako_pdu_action_error_t
hako_pdu_action_server_send_feedback(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_goal_token_t goal_token,
    const uint8_t* feedback_pdu,
    size_t feedback_pdu_size);
```

Runtimeは`goal_token`から`goal_id`、Action Type、sequence number、現在状態を解決します。

ApplicationはFeedback Headerを直接管理しません。

### 6.8 terminal完了

terminal完了は、一つの共通関数とstatus指定を基本案とします。

```c
typedef enum {
    HAKO_PDU_ACTION_RESULT_SUCCEEDED = 1,
    HAKO_PDU_ACTION_RESULT_CANCELED = 2,
    HAKO_PDU_ACTION_RESULT_ABORTED = 3
} hako_pdu_action_result_status_t;

hako_pdu_action_error_t
hako_pdu_action_server_create_result_buffer(...);

hako_pdu_action_error_t
hako_pdu_action_server_complete(
    hako_pdu_action_server_handle_t* handle,
    hako_pdu_action_goal_token_t goal_token,
    hako_pdu_action_result_status_t status,
    const uint8_t* result_pdu,
    size_t result_pdu_size);
```

個別の`complete_succeeded()`、`complete_canceled()`、`complete_aborted()`へ分ける案は後続レビュー対象です。

`complete()`成功後、Applicationは同じ`goal_token`へ新規Feedbackや別の完了を送れません。

## 7. Buffer所有権

既存RPC C APIと同じ規則を使用します。

```text
caller-supplied buffer API:
  Runtimeが呼び出し元bufferへ書き込む

*_alloc API:
  Runtimeが確保したbufferを返す
  callerがhako_pdu_action_buffer_free()で解放する
```

Language Bindingは、C bufferをnative memoryへコピーした直後に解放します。

Action APIは、意味論ごとのbuffer作成関数を公開します。

```text
Client:
  create_goal_buffer

Server:
  create_feedback_buffer
  create_result_buffer
```

Goal Response、Cancel Request、Cancel Responseの未使用bodyはRuntimeが既定値で生成するため、Application向けbuffer作成APIを必須にしません。

## 8. Header責務

公開C API利用者は、原則としてAction Headerを直接編集しません。

Runtimeが設定するもの:

```text
version
request_kind
response_kind
status
goal_id
sequence_no
reserved
```

ApplicationまたはAdapterが設定するもの:

```text
Goal body
Feedback body
Result body
```

`hakoniwa-pdu-ros`はROS messageとbodyを変換し、Protocol Headerと状態管理はAction Runtimeへ委ねます。

## 9. エラー型

既存RPC C APIと対称な共通error enumを設けます。

```c
typedef enum {
    HAKO_PDU_ACTION_OK = 0,
    HAKO_PDU_ACTION_ERROR_INVALID_ARGUMENT,
    HAKO_PDU_ACTION_ERROR_INITIALIZE,
    HAKO_PDU_ACTION_ERROR_START,
    HAKO_PDU_ACTION_ERROR_NOT_RUNNING,
    HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL,
    HAKO_PDU_ACTION_ERROR_NOT_FOUND,
    HAKO_PDU_ACTION_ERROR_INVALID_STATE,
    HAKO_PDU_ACTION_ERROR_DUPLICATE_GOAL,
    HAKO_PDU_ACTION_ERROR_INTERNAL
} hako_pdu_action_error_t;
```

Protocol上のGoal rejectやCancel rejectはC API呼び出し失敗ではありません。Applicationの正常な判断として相手側へResponseを送ります。

## 10. Mux Serverとの対称性

既存RPC C APIと同様に、static serverとmux serverで同型のApplication APIを提供します。

```text
hako_pdu_action_server_*
hako_pdu_action_mux_server_*
```

Mux利用時も、Applicationは同じ`event_token`と`goal_token`を使用します。Connection identityはRuntime内部へ保持します。

ただし、Actionでは次を満たす必要があります。

```text
Connection lifetime != Goal lifetime
```

Connection切断時にGoal Contextを破棄するか、継続するか、Runtime Cancelへ移行するかは設定および後続アーキテクチャ実装で決定します。公開tokenが生のConnectionSlotアドレスやindexへ依存してはいけません。

## 11. 利用シーケンス

### 11.1 Client

```text
create
start
create_goal_buffer
Goal body設定
send_goal -> goal_id

poll GOAL_RESPONSE
poll FEEDBACK 0..n
必要ならcancel_goal(goal_id)
poll CANCEL_RESPONSE
poll RESULT

stop
destroy
```

### 11.2 Server

```text
create
start

poll GOAL_REQUEST -> event_token
accept_goal(event_token) -> goal_token

send_feedback(goal_token) 0..n

poll CANCEL_REQUEST -> event_token + goal_token
accept_cancel(event_token) または reject_cancel(event_token)

complete(goal_token, status, Result body)

stop
destroy
```

## 12. 現時点の設計判断

- RPC Service C APIと同じhandle、start/stop、poll、buffer ownershipモデルを採用する。
- Client APIは`hakoniwa-pdu-ros`などのAdapter利用を主対象とする。
- Server APIはAction Server Application利用を主対象とする。
- 一つのClient handleで複数Goalを管理する。
- Protocol相関には`goal_id`を使用する。
- Server Application向けには`event_token`と`goal_token`を分離する。
- `event_token`は受信イベントへの一回限りの判断に使用する。
- `goal_token`はaccept済みGoalの継続操作に使用する。
- ApplicationへProtocol Header編集を要求しない。
- Goal／Cancel Responseの未使用bodyはRuntimeが既定値で生成する。
- Client／Serverともcallbackではなくpollを基準とする。
- Mux APIはstatic server APIと表面的に対称化する。

## 13. 最小レビュー事項

1. `event_token`と`goal_token`を分離するか。
2. Goal accept時に`goal_token`を払い出すか。
3. Cancel Request eventに`goal_token`を含めるか。
4. Client handleで複数同時Goalを許容するか。
5. Client pollをGoal Response、Feedback、Cancel Response、Resultの共通入口とするか。
6. Server terminal APIを共通`complete(status, result)`とするか。
7. Goal／Cancel Response PDUをRuntimeが自動生成するか。
8. C API利用者へAction Headerを直接公開しない方針でよいか。
9. Mux serverでも同じtokenモデルを維持するか。

## 14. 対象外

- 正式なHeader fileのABI確定
- 全関数の厳密な引数順序
- Python Action APIおよびFuture設計
- ROS 2 Action GoalHandleへの具体的マッピング
- JSON設定Schema
- Mux切断時のGoal継続ポリシー
- 実装ファイル配置とCMake追加
