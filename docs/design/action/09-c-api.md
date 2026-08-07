# Hakoniwa Action C API設計

> **Status: Implemented contract**
> 本文書は、Hakoniwa Action Runtimeをユーザーアプリケーションおよび`hakoniwa-pdu-ros`から利用するためのC APIとPython CFFIの現行契約です。正確な宣言は`include/hakoniwa/pdu/action/c_action.h`を正とします。

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
| `request_token` | `action_name + Server Goal Handle` |
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

- 上位Client ApplicationまたはROS Adapterなどの外部Protocol層が生成し、必須指定します。
- RuntimeはGoal IDを自動生成しません。
- Client／Server間のProtocol相関に使用します。
- Client poll結果およびServer poll結果へ含めます。

### 3.2 Client／Server Goal Handle

C APIはNative APIと同じく、Client側とServer側で型を分けたGoal Handleを使用します。

```c
typedef struct {
    hako_pdu_action_goal_id_t goal_id;
} hako_pdu_action_client_goal_handle_t;

typedef struct {
    hako_pdu_action_goal_id_t goal_id;
} hako_pdu_action_server_goal_handle_t;
```

どちらもWire上の`goal_id`を保持しますが、異なる側のAPIへ誤って渡せないようCの型を分けます。

Server Applicationは`poll()`で得た次の組を、そのままGoal lifecycle APIへ渡します。

```text
action_name + Server Goal Handle
```

Runtime-localな`event_token`／`goal_token` registryは設けません。one-shot判断、accept済みGoalの存在、terminal後の無効化はNative Endpoint／Servicesがすでに管理しており、C APIで同じ状態を二重管理しないためです。

## 4. Native Servicesとの境界

C APIは新しいGoal transaction modelを実装しません。

```text
C opaque handle
  -> ActionServicesClient / ActionServicesServer
  -> IActionClientEndpoint / IActionServerEndpoint
```

C層が担当するのは、opaque handleの所有、C/C++型変換、buffer所有権、例外境界だけです。Goal状態、slot ownership、送信順序はNative Servicesの契約をそのまま使用します。

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

typedef struct {
    uint8_t bytes[HAKO_PDU_ACTION_GOAL_ID_SIZE];
} hako_pdu_action_goal_id_t;

typedef struct {
    hako_pdu_action_goal_id_t goal_id;
} hako_pdu_action_client_goal_handle_t;
```

一つのClient handleは、設定に含まれる複数Action Typeと、各Action Typeの複数Goal Contextを管理できます。

通常のClient利用者は`hako_pdu_action_client_goal_handle_t`を保持し、UUIDを直接管理しません。ROS Adapterなど、外部ProtocolのUUIDを維持する必要がある利用者だけが`hako_pdu_action_goal_id_t`を明示指定します。

### 5.3 Client event

```c
typedef enum {
    HAKO_PDU_ACTION_CLIENT_EVENT_NONE = 0,
    HAKO_PDU_ACTION_CLIENT_EVENT_GOAL_RESPONSE,
    HAKO_PDU_ACTION_CLIENT_EVENT_FEEDBACK,
    HAKO_PDU_ACTION_CLIENT_EVENT_CANCEL_RESPONSE,
    HAKO_PDU_ACTION_CLIENT_EVENT_RESULT,
    HAKO_PDU_ACTION_CLIENT_EVENT_TIMEOUT,
    HAKO_PDU_ACTION_CLIENT_EVENT_ERROR
} hako_pdu_action_client_event_t;
```

`TIMEOUT`は、Goal確立前のGoal Response timeoutをApplicationへ通知するローカルRuntimeイベントです。通信異常をGoalのterminal statusへ変換しません。

`ERROR`は、正常なAction Protocolイベントとして表現できないProtocol／Runtimeエラーを通知するローカルRuntimeイベントです。具体的な原因は`poll()`の`out_error`へ設定します。`ERROR`はwire上のAction packetでも、Goalのterminal statusでもありません。

### 5.4 Client event info

```c
typedef struct {
    char action_name[HAKO_PDU_ACTION_NAME_MAX];
    hako_pdu_action_client_goal_handle_t goal;
    hako_pdu_action_decision_t decision;
    hako_pdu_action_terminal_status_t terminal_status;
    uint32_t feedback_sequence;
    size_t pdu_size;
} hako_pdu_action_client_event_info_t;
```

`decision`はGoal ResponseまたはCancel Responseで使用します。`terminal_status`はResultで使用し、`feedback_sequence`はFeedbackで使用します。イベント種別に不要なフィールドは`UNSPECIFIED`または`0`とします。

### 5.5 Client操作の概念形

```c
hako_pdu_action_client_handle_t*
hako_pdu_action_client_create(...);

hako_pdu_action_error_t
hako_pdu_action_client_start(...);

hako_pdu_action_error_t
hako_pdu_action_client_is_running(...);

hako_pdu_action_error_t
hako_pdu_action_client_create_goal_buffer(...);

hako_pdu_action_error_t
hako_pdu_action_client_send_goal(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    const uint8_t* goal_pdu,
    size_t goal_pdu_size,
    const hako_pdu_action_goal_id_t* goal_id,
    hako_pdu_action_client_goal_handle_t* out_goal,
    uint64_t timeout_usec);

hako_pdu_action_error_t
hako_pdu_action_client_cancel_goal(
    hako_pdu_action_client_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_client_goal_handle_t* goal);

hako_pdu_action_client_event_t
hako_pdu_action_client_poll(...);

hako_pdu_action_error_t
hako_pdu_action_client_stop(...);

void
hako_pdu_action_client_destroy(...);
```

`goal_id`は必須です。Runtimeは指定値を変更せず`out_goal`へ返します。all-zeroまたは同じClient RuntimeでactiveなGoalとの衝突は同期エラーとして拒否します。

`timeout_usec`はGoal Request送信後、Goal Responseを受信するまでにだけ適用します。`0`はGoal Response timeoutなしを表します。Goalがacceptされた後のResult待ち、およびCancel Response待ちには適用しません。

`start()`はEndpointの非同期通信処理を開始しますが、TCP接続完了を保証しません。Action roleとTCP roleは独立しているため、C APIは`is_running()`を公開します。送信側Applicationは`is_running != 0`を確認してから最初のGoalを送信します。

### 5.6 Client pollの意味

`poll()`は、一回の呼び出しで最大一つのイベントを返します。

```text
NONE
GOAL_RESPONSE
FEEDBACK
CANCEL_RESPONSE
RESULT
TIMEOUT
ERROR
```

Goalに相関できるイベントには`action_name`とClient Goal Handleを含めます。これによりAdapterは、複数同時Goalを各ROS GoalHandleへ配送できます。

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

typedef struct {
    hako_pdu_action_goal_id_t goal_id;
} hako_pdu_action_server_goal_handle_t;
```

一つのServer handleは、設定に含まれる複数Action Typeを管理します。

### 6.3 Server event

```c
typedef enum {
    HAKO_PDU_ACTION_SERVER_EVENT_NONE = 0,
    HAKO_PDU_ACTION_SERVER_EVENT_GOAL_REQUEST,
    HAKO_PDU_ACTION_SERVER_EVENT_CANCEL_REQUEST,
    HAKO_PDU_ACTION_SERVER_EVENT_RUNTIME_CANCEL_REQUEST,
    HAKO_PDU_ACTION_SERVER_EVENT_ERROR
} hako_pdu_action_server_event_t;

typedef enum {
    HAKO_PDU_ACTION_RUNTIME_CANCEL_UNSPECIFIED = 0,
    HAKO_PDU_ACTION_RUNTIME_CANCEL_TRANSPORT_DISCONNECTED,
    HAKO_PDU_ACTION_RUNTIME_CANCEL_SERVER_SHUTDOWN,
    HAKO_PDU_ACTION_RUNTIME_CANCEL_INTERNAL_POLICY
} hako_pdu_action_runtime_cancel_cause_t;
```

`ERROR`はClient側と同様にローカルRuntimeイベントであり、wire上のAction eventではありません。具体的な原因は`poll()`の`out_error`へ設定します。

### 6.4 Server event info

```c
typedef struct {
    char action_name[HAKO_PDU_ACTION_NAME_MAX];
    hako_pdu_action_server_goal_handle_t goal;
    hako_pdu_action_runtime_cancel_cause_t runtime_cancel_cause;
    size_t pdu_size;
} hako_pdu_action_server_event_info_t;
```

`runtime_cancel_cause`は`RUNTIME_CANCEL_REQUEST`で使用し、それ以外のイベントでは`UNSPECIFIED`とします。

`goal`はGOAL／CANCEL／Runtime Cancelのすべてで同じProtocol Goal identityを表します。`action_name`と組み合わせてNative ServicesのGoalを一意に選択します。

### 6.5 Goal Request処理

```text
server_poll()
  -> GOAL_REQUEST
  -> action_name + Server Goal Handle + Goal PDU

Application判断:
  accept_goal(action_name, goal)

  または

  reject_goal(action_name, goal)
```

概念形:

```c
hako_pdu_action_error_t
hako_pdu_action_server_accept_goal(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal);

hako_pdu_action_error_t
hako_pdu_action_server_reject_goal(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal);
```

Goal Response PDUのbodyはProtocol既定値をRuntimeが生成します。Applicationは未使用Result bodyを組み立てません。

### 6.6 Cancel Request処理

```text
server_poll()
  -> CANCEL_REQUEST
  -> action_name + Server Goal Handle

Application判断:
  accept_cancel(action_name, goal)
  reject_cancel(action_name, goal)
```

概念形:

```c
hako_pdu_action_error_t
hako_pdu_action_server_accept_cancel(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal);

hako_pdu_action_error_t
hako_pdu_action_server_reject_cancel(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal);
```

Cancel Response PDUのbodyはProtocol既定値をRuntimeが生成します。

`accept_cancel()`／`reject_cancel()`はCancel Responseの同期送信成功後に判断を確定します。Endpointは判断の検証、同期送信、状態確定を同じstate mutex区間で実行し、同一Goalの`complete()`を直列化します。これによりCancel Responseより先にterminal Resultが送信されることを防ぎます。送信失敗時はpending判断を維持するため、Applicationは同じ判断を再実行できます。

### 6.7 Feedback送信

```c
hako_pdu_action_error_t
hako_pdu_action_server_create_feedback_buffer(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    uint8_t* buffer,
    size_t capacity,
    size_t* out_size);

hako_pdu_action_error_t
hako_pdu_action_server_send_feedback(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal,
    const uint8_t* feedback_pdu,
    size_t feedback_pdu_size);
```

Runtimeは`action_name + goal`からAction Type、sequence number、現在状態を解決します。

ApplicationはFeedback Headerを直接管理しません。

`create_feedback_buffer()`は、Registryのgenerated base sizeとAction設定の`bufferHeap.feedbackSize`から最大容量の完全なAction Feedback PDUを確保・初期化します。ApplicationまたはTyped wrapperはそのbufferへbodyをencodeします。Runtimeはencode後の`metadata.total_size`をWireサイズとして使用するため、buffer自体の縮小は不要です。`send_feedback()`がbufferを暗黙生成することはありません。

### 6.8 terminal完了

terminal完了は、一つの共通関数とstatus指定を基本案とします。

```c
typedef enum {
    HAKO_PDU_ACTION_RESULT_SUCCEEDED = 1,
    HAKO_PDU_ACTION_RESULT_CANCELED = 2,
    HAKO_PDU_ACTION_RESULT_ABORTED = 3
} hako_pdu_action_result_status_t;

hako_pdu_action_error_t
hako_pdu_action_server_create_result_buffer(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    uint8_t* buffer,
    size_t capacity,
    size_t* out_size);

hako_pdu_action_error_t
hako_pdu_action_server_complete(
    hako_pdu_action_server_handle_t* handle,
    const char* action_name,
    const hako_pdu_action_server_goal_handle_t* goal,
    hako_pdu_action_result_status_t status,
    const uint8_t* result_pdu,
    size_t result_pdu_size);
```

`create_result_buffer()`は、Registryのgenerated base sizeとAction設定の`bufferHeap.responseSize`から最大容量の完全なAction Response PDUを確保・初期化します。ApplicationまたはTyped wrapperはResult bodyをencodeします。Runtimeはencode後の`metadata.total_size`をWireサイズとして使用するため、buffer自体の縮小は不要です。`complete()`および内部Response送信処理がbufferを暗黙生成することはありません。

`complete()`成功後、Applicationは同じ`action_name + goal`へ新規Feedbackや別の完了を送れません。

`complete()`は、accept済みGoalに対する`SUCCEEDED`または`ABORTED`、Cancel受理後の`CANCELING`に対する`CANCELED`または`ABORTED`を同期送信します。Runtimeは送信開始前にGoalを`FINISHING`へcommitして二重Resultと後続Feedbackを拒否します。Endpoint実装内では、このpacket binding状態を上位Protocol状態と区別して`RESULT_COMMITTED`と表現します。同期送信が成功した時点でServer側Goal Contextとslot ownershipを解放します。

Applicationから渡されたResult packetの形式・容量検証はterminal commitより前に行います。検証失敗はApplication入力エラーとしてGoalを`EXECUTING`に保ち、修正したpacketで再実行できます。

検証通過後のTransport送信失敗ではGoalを`FINISHING`のまま保持し、slotを再利用しません。これにより、送信成否が不明なGoalのlaneへ別Goalを重ねません。Contextとslotは明示的なstop／resetまたはRuntime破棄で解放します。

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

caller-supplied bufferが不足した場合、`poll()`は`BUFFER_TOO_SMALL`と必要サイズを返し、そのイベントをC handle内に1件保持します。呼び出し側は十分なbufferまたは`poll_alloc()`で同じイベントを再取得できます。C層が保持するのは未配送bufferイベントだけであり、Goal状態やslot ownershipはNative Servicesが引き続き所有します。

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
    HAKO_PDU_ACTION_ERROR_SEND,
    HAKO_PDU_ACTION_ERROR_BUFFER_TOO_SMALL,
    HAKO_PDU_ACTION_ERROR_NOT_FOUND,
    HAKO_PDU_ACTION_ERROR_INVALID_STATE,
    HAKO_PDU_ACTION_ERROR_DUPLICATE_GOAL,
    HAKO_PDU_ACTION_ERROR_NO_FREE_SLOT,
    HAKO_PDU_ACTION_ERROR_INVALID_PACKET,
    HAKO_PDU_ACTION_ERROR_INTERNAL
} hako_pdu_action_error_t;
```

Protocol上のGoal rejectやCancel rejectはC API呼び出し失敗ではありません。Applicationの正常な判断として相手側へResponseを送ります。

Native APIは`GoalSendResult`で同期失敗理由を公開し、C APIはそれを対応するerror codeへlosslessに変換します。管理中のGoal ID重複は`DUPLICATE_GOAL`、通信slot不足は`NO_FREE_SLOT`、packet検証失敗は`INVALID_PACKET`、Transport送信失敗は`SEND`です。これらは送信受理前のC APIエラーであり、Protocol上のGoal Response accept/rejectとは別です。Serverが受信時に検出したduplicate Goal RequestはProtocol上のGoal rejectとして処理し、Server Applicationへ新規Goalイベントを公開しません。

## 10. Mux Serverとの対称性

既存RPC C APIと同様に、static serverとmux serverで同型のApplication APIを提供します。

```text
hako_pdu_action_server_*
hako_pdu_action_mux_server_*
```

Mux利用時も、Applicationは同じ`action_name + Server Goal Handle`を使用します。Connection identityはRuntime内部へ保持します。

Mux C APIは、point-to-point Serverと同じ操作を次のprefixで提供します。

```text
hako_pdu_action_mux_server_create / destroy
hako_pdu_action_mux_server_start / stop
hako_pdu_action_mux_server_poll / poll_alloc
hako_pdu_action_mux_server_accept_goal / reject_goal
hako_pdu_action_mux_server_accept_cancel / reject_cancel
hako_pdu_action_mux_server_create_feedback_buffer[_alloc]
hako_pdu_action_mux_server_create_result_buffer[_alloc]
hako_pdu_action_mux_server_send_feedback
hako_pdu_action_mux_server_complete
```

接続状態の観測に限り、Mux固有の次のAPIを追加します。

```text
hako_pdu_action_mux_server_connected_count
hako_pdu_action_mux_server_expected_count
hako_pdu_action_mux_server_is_ready
```

これらは接続数とready状態だけを返し、Goal操作へconnection IDを要求しません。C層にGoal routing用の独自token registryも作りません。

ただし、Actionでは次を満たす必要があります。

```text
Connection lifetime != Goal lifetime
```

Connection切断時のGoal Context、Runtime Cancel、local terminal完了は[Action Mux Server契約](13-mux-server.md)に従います。公開Goal Handleは生のConnectionSlotアドレスやindexへ依存しません。

## 11. 利用シーケンス

### 11.1 Client

```text
create
start
create_goal_buffer
Goal body設定
send_goal -> Client Goal Handle

poll GOAL_RESPONSE
poll FEEDBACK 0..n
必要ならcancel_goal(Client Goal Handle)
poll CANCEL_RESPONSE
poll RESULT

stop
destroy
```

### 11.2 Server

```text
create
start

poll GOAL_REQUEST -> action_name + Server Goal Handle
accept_goal(action_name, goal)

send_feedback(action_name, goal) 0..n

poll CANCEL_REQUEST -> action_name + Server Goal Handle
accept_cancel(action_name, goal) または reject_cancel(action_name, goal)

complete(action_name, goal, status, Result body)

stop
destroy
```

## 12. C APIの設計判断

- RPC Service C APIと同じhandle、start/stop、poll、buffer ownershipモデルを採用する。
- Client APIは`hakoniwa-pdu-ros`などのAdapter利用を主対象とする。
- Server APIはAction Server Application利用を主対象とする。
- 一つのClient handleで複数Goalを管理する。
- Protocol相関には`goal_id`を使用する。
- Client Applicationまたは外部AdapterがGoal IDを明示指定し、Runtimeは自動採番しない。送信後の操作にはClient Goal Handleを使用する。
- `send_goal()`のtimeoutはGoal Response待ちにだけ適用し、accept後のResultおよびCancel Responseには適用しない。
- `TIMEOUT`と`ERROR`はローカルRuntimeイベントであり、Goalのterminal statusではない。
- Server Applicationは`poll()`で得た`action_name + Server Goal Handle`を継続操作へ使用する。
- C層に独自token registryを設けず、Native Servicesのone-shot判断とGoal Contextを再利用する。
- ApplicationへProtocol Header編集を要求しない。
- Goal／Cancel Responseの未使用bodyはRuntimeが既定値で生成する。
- Client／Serverともcallbackではなくpollを基準とする。
- Python CFFIは本C APIのhandle、Goal Handle、poll、error codeを薄く写像し、Python側にGoal状態機械を二重実装しない。
- Pythonの`*_alloc()`AdapterはNative bufferを`bytes`へcopyし、呼び出し元へ返す前に`hako_pdu_action_buffer_free()`で解放する。
- Mux APIはstatic server APIと表面的に対称化する。

## 13. 対象外

- Python ActionのFuture／callback Adapter設計
- ROS 2 Action GoalHandleへの具体的マッピング

## 14. Python CFFI Adapter契約

Python CFFIは、本C APIを次のPython型へ直接写像します。

```text
hako_pdu_action_client_handle_t -> ActionClient
hako_pdu_action_server_handle_t -> ActionServer
hako_pdu_action_mux_server_handle_t -> ActionMuxServer
Client Goal Handle              -> ClientGoalHandle
Server Goal Handle              -> ServerGoalHandle
C error code                    -> ActionErrorCode / ActionError
Client/Server poll              -> immutable dataclass result
```

Service RPC、Mux、Actionは一つの`FFI`定義と一つの
`libhakoniwa_pdu_rpc` loaderを共有します。Action固有の別shared libraryや
別`dlopen()`経路は作成しません。

Python側のGoal IDは16 byteかつall-zeroでない`bytes`とし、Nativeと同じidentityを保持します。CFFI AdapterはGoal状態機械、slot ownership、timeout policyを再実装しません。

`poll()`とbuffer生成はC APIの`*_alloc()`を使用します。Native bufferはPython `bytes`へcopyし、Python Applicationへ制御を返す前に必ず`hako_pdu_action_buffer_free()`で解放します。Native pointerはPython APIから公開しません。

`ActionError` は`code` に`ActionErrorCode`を保持します。Goal ID重複、slot不足、packet不正、Transport送信失敗などの同期失敗理由は、C APIのerror codeを変更せずPythonへ公開します。

`ActionMuxServer`は`ActionServer`と同じ`ActionServerPollResult`、`ServerGoalHandle`、Goal／Cancel判断、Feedback／Result APIを使用します。Mux固有に公開するのは`connected_count()`、`expected_count()`、`is_ready()`だけで、connection IDやrouting tokenは公開しません。

Python Action APIは明示的poll型です。Service側の`RpcFuture`やworker threadをAction側に暗黙に適用しません。
