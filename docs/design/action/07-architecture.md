# Hakoniwa Action Runtimeアーキテクチャ

> **Status: Draft**  
> 本文書は、既存Service RPCの実装構造を基礎に、Hakoniwa Action Runtimeのコンポーネント構成と責務境界を定義します。

## 1. 目的

Action対応では、既存Service RPCと異なる独自レイヤ構造を新設しません。

基本方針は次のとおりです。

```text
既存Service RPCと同じレイヤ構造
  + Action固有のProtocol Runtime
```

ServiceとActionでは通信ライフサイクルが異なりますが、Application、Services管理層、Endpoint Interface、Endpoint実装、PDU Endpointという積層は共通化します。

## 2. 既存Service RPCの構造

既存Service RPCは、次の構造を持ちます。

```text
User Application
    |
    v
RpcServicesClient / RpcServicesServer
    |
    v
IRpcClientEndpoint / IRpcServerEndpoint
    |
    v
RpcClientEndpointImpl / RpcServerEndpointImpl
    |
    v
hakoniwa-pdu-endpoint::Endpoint
    |
    v
configured PDU transport
```

各層の役割は次のとおりです。

### 2.1 RpcServicesClient / RpcServicesServer

複数Serviceを管理するApplication向け窓口です。

主な責務:

- JSON設定の読み込み
- node、client、serviceとEndpoint設定の対応付け
- ServiceごとのEndpoint Runtime生成
- service名によるRuntime選択
- 複数Serviceを横断したpoll
- Runtimeの開始、停止、解放

### 2.2 IRpcClientEndpoint / IRpcServerEndpoint

Service単位のProtocol Runtime Interfaceです。

Application向け管理層と具体的実装の境界を提供します。

### 2.3 RpcClientEndpointImpl / RpcServerEndpointImpl

1 Service分のProtocol、状態、相関、queueおよびPDU送受信を担当します。

Client側の主な責務:

- Request／Response PDU定義の登録
- Request送信
- Response受信callback
- pending response queue
- request_idによる相関
- timeoutおよびCancel lifecycle

Server側の主な責務:

- Request／Response PDU定義の登録
- Request受信callback
- pending request queue
- Header検証
- Request／Cancelの振り分け
- client単位の状態管理
- Reply送信

### 2.4 hakoniwa-pdu-endpoint

RPCの意味論を持たないPDU通信層です。

主な責務:

- PDU definitionとchannelの管理
- PDU送信
- 受信callback
- transport接続
- multiplexer接続

## 3. Action Runtimeの基本構造

Action Runtimeは、既存Service RPCと対称な構造とします。

```text
User Application
    |
    v
ActionServicesClient / ActionServicesServer
    |
    v
IActionClientEndpoint / IActionServerEndpoint
    |
    v
ActionClientEndpointImpl / ActionServerEndpointImpl
    |
    v
hakoniwa-pdu-endpoint::Endpoint
    |
    v
configured PDU transport
```

ServiceとActionの対応は次のとおりです。

| Service RPC | Action Runtime |
| --- | --- |
| `RpcServicesClient` | `ActionServicesClient` |
| `RpcServicesServer` | `ActionServicesServer` |
| `IRpcClientEndpoint` | `IActionClientEndpoint` |
| `IRpcServerEndpoint` | `IActionServerEndpoint` |
| `RpcClientEndpointImpl` | `ActionClientEndpointImpl` |
| `RpcServerEndpointImpl` | `ActionServerEndpointImpl` |
| `hakoniwa-pdu-endpoint::Endpoint` | 同じEndpointを再利用 |

## 4. 構造的に共通化する部分

Action Runtimeは、次の既存パターンを維持します。

- JSON設定からRuntimeを構築する。
- Services層が複数Action Typeを管理する。
- Action TypeごとにEndpoint Runtimeを持つ。
- Endpoint RuntimeはInterfaceを介してServices層から利用する。
- PDU定義登録、送信、subscribe callbackは`hakoniwa-pdu-endpoint`を利用する。
- 受信callbackではApplication処理を直接実行せず、pending queueへ格納する。
- Applicationは明示的なpollまたは上位Bindingの非同期APIを通じてイベントを受け取る。
- C API、Python Binding、ROS AdapterはNative Runtimeの上に積層する。
- EndpointおよびTransportはActionの状態遷移を解釈しない。

## 5. Action固有の差分

同じ構造を使用しますが、Endpoint実装内部のProtocol RuntimeはServiceと異なります。

### 5.1 相関単位

Service RPC:

```text
client endpoint
  -> 基本1 in-flight request
  -> request_idで相関
```

Action Runtime:

```text
action endpoint
  -> 複数in-flight Goal
  -> goal_idごとのGoal Contextで相関
```

### 5.2 通信イベント

Service RPC:

```text
Request
Response
Cancel
```

Action Runtime:

```text
Goal Request
Goal Response
Feedback
Cancel Request
Cancel Response
Result
```

### 5.3 状態管理

Service RPCはClientまたはclient_name単位のRPC状態を管理します。

Action Runtimeはaccept済み`goal_id`ごとに、次の状態を管理します。

```text
EXECUTING
CANCELING
FINISHING
```

Goal Response待ち、Cancel Response待ち、Result待ちはGoal Context内のpending情報として管理します。

## 6. ActionServicesClient

`ActionServicesClient`は、複数Action Client Runtimeを管理するApplication向け窓口です。

主な責務:

- Action設定の読み込み
- client、node、Action Type、Endpointの対応付け
- Action Typeごとの`ActionClientEndpointImpl`生成
- Action Type名によるRuntime選択
- 全Action Typeを横断したpoll
- Runtimeの開始、停止、Context解放

次の責務は持ちません。

- Goal lifecycleの状態遷移
- Goal Response、Feedback、Cancel Response、Resultの意味解釈
- `goal_id`ごとのpending Context管理
- Transport実装

これらは`ActionClientEndpointImpl`へ委譲します。

## 7. ActionClientEndpointImpl

`ActionClientEndpointImpl`は、1 Action Type分のClient Protocol Runtimeです。

内部に次を持ちます。

```text
ActionClientEndpointImpl
  - Goal Context Registry
  - Request sender
  - Response receiver queue
  - Feedback receiver queue
  - Protocol dispatcher
  - Client state transition logic
  - Endpoint reference
  - Time source
```

Goal Context Registryは`goal_id`をキーとして、少なくとも次を管理します。

```text
goal_id
main state
Goal Response pending
Cancel Response pending
Result pending
Feedback sequence state
Application notification state
```

主な責務:

- `goal_id`生成または外部UUID受け入れ
- Goal Context生成
- Goal Request送信
- Cancel Request送信
- Goal Response、Cancel Response、Resultの相関
- FeedbackのGoal別配送
- Client状態モデルの適用
- terminal Result受信後の通知とContext解放

## 8. ActionServicesServer

`ActionServicesServer`は、複数Action Server Runtimeを管理するApplication向け窓口です。

主な責務:

- Action設定の読み込み
- node、Action Type、Endpointの対応付け
- Action Typeごとの`ActionServerEndpointImpl`生成
- 全Action Typeを横断したApplicationイベントpoll
- Runtimeの開始、停止、解放

Applicationへ返すイベントには、少なくとも次が含まれます。

```text
Goal Request
Cancel Request
Runtime Cancel Requested
```

Goal受理、Cancel受理、Feedback生成、完了判断はServer Applicationの責務です。

## 9. ActionServerEndpointImpl

`ActionServerEndpointImpl`は、1 Action Type分のServer Protocol Runtimeです。

内部に次を持ちます。

```text
ActionServerEndpointImpl
  - Goal Context Registry
  - Request receiver queue
  - Application event queue
  - Response / Feedback / Result sender
  - Protocol dispatcher
  - Server state transition logic
  - Endpoint reference
```

Goal Context Registryは`goal_id`をキーとして、少なくとも次を管理します。

```text
goal_id
main state
Goal identity information
Cancel decision state
terminal status
Result delivery / retention state
Endpoint or connection association
```

主な責務:

- Goal Request Header検証
- UUID形式および重複`goal_id`検査
- Applicationへの新規Goal通知
- Application判断に基づくGoal Response送信
- Cancel RequestのGoal相関
- ApplicationへのCancel通知
- Feedback送信
- Result送信と保持
- Server状態モデルの適用

## 10. PDU構成

Action Endpoint Runtimeは、PDU Registryの既存Action packetを使用します。

```text
<Action>ActionRequest
<Action>ActionResponse
<Action>ActionFeedback
```

論理イベントはHeaderのkindおよびpacket型で識別します。

```text
GOAL_REQUEST     -> ActionRequest  / request_kind=GOAL
CANCEL_REQUEST   -> ActionRequest  / request_kind=CANCEL
GOAL_RESPONSE    -> ActionResponse / response_kind=GOAL
CANCEL_RESPONSE  -> ActionResponse / response_kind=CANCEL
RESULT           -> ActionResponse / response_kind=RESULT
FEEDBACK         -> ActionFeedback
```

PDUのバイトレイアウト、固定Header、Action固有body型、alignmentは`hakoniwa-pdu-registry`の責務です。

## 11. pending queueとApplication実行

既存Service RPCと同様に、Endpoint受信callback内でAction Applicationの処理を実行しません。

```text
Endpoint receive callback
  -> PDU byte列をqueueへ格納
  -> Runtime poll
  -> Header検証とProtocol dispatch
  -> Application eventとして通知
```

この分離により、Transport threadとApplication execution contextを分離します。

Feedback、Response、Resultは論理的に別イベントですが、queueを物理的に分割するか、単一受信queueでHeader dispatchするかは実装設計で決定します。

## 12. Muxとの関係

既存Service RPCのMux構造は次のとおりです。

```text
EndpointCommMultiplexer
  -> ConnectionSlot
       - connection_id
       - Endpoint
       - RpcServicesServer
       - disconnected flag
```

Actionでも、接続ごとにEndpointを生成する外枠は再利用できます。

```text
EndpointCommMultiplexer
  -> ConnectionSlot
       - connection_id
       - Endpoint
       - Action Services接続Adapter
       - disconnected flag
```

ただしActionでは、次の関係を前提とします。

```text
Connection lifetime
  != Goal lifetime
```

Service RPCのようにConnectionSlot削除と同時に全Action Goal Contextを無条件破棄してはなりません。

Transport切断時はServer状態モデルに従い、必要に応じてRuntime起因CancelをApplicationへ通知します。ApplicationがGoalを継続、Cancel、Abortのいずれにするかを決定します。

### 12.1 Mux実装上の設計余地

次の実現方法は後続の実装設計で選択します。

```text
A. Goal ContextをConnectionSlot内のActionServer Runtimeが保持し続ける
B. 接続断時にGoal Contextを接続非依存Registryへ移管する
C. Muxより上位に共有ActionServer Runtimeを置き、接続AdapterだけをSlotに持つ
```

本アーキテクチャでは方式を確定しませんが、Goal Contextを接続寿命へ従属させないことを要求します。

## 13. BindingとAdapter

```text
ROS 2 Action
  -> hakoniwa-pdu-ros Adapter
  -> Python / C APIまたはNative API
  -> ActionServicesClient / ActionServicesServer
  -> Action Endpoint Runtime
```

`hakoniwa-pdu-ros`の責務:

- ROS 2 GoalHandle、Future、callbackとの変換
- ROS 2 UUIDとHakoniwa `goal_id`の対応
- ROS 2 bulk Cancelの単一Goal Cancelへの分解
- ROS 2 GoalStatusArrayの生成

`hakoniwa-pdu-rpc`の責務:

- ROS非依存のAction lifecycle
- Goal Contextと状態管理
- PDU送受信
- Protocolイベントの相関と配送

## 14. 実装上の基本判断

- Service RPCの既存クラスへAction状態を混在させない。
- Serviceと同じレイヤ構造を持つAction専用Runtimeを追加する。
- `hakoniwa-pdu-endpoint`を変更せず再利用する。
- Services層は構成とApplication窓口を担当する。
- Endpoint Impl層は1 Action Type分のProtocol Runtimeを担当する。
- Goal Contextは`goal_id`単位でEndpoint Impl層が管理する。
- 同一Action Typeで複数Goalを同時管理できる。
- callbackは受信queueへの格納に限定し、Protocol処理はpoll側で行う。
- PDU RegistryのAction packet構成を使用する。
- Connection lifetimeとGoal lifetimeを分離する。

## 15. 最小レビュー事項

1. Service RPCと同じレイヤ構造を採用するか。
2. Serviceクラスを拡張せず、Action専用の並行クラス群を追加するか。
3. Goal Context RegistryをAction Endpoint Implが所有するか。
4. 1 Action Typeにつき1 Endpoint Runtimeとするか。
5. Endpoint callbackをqueue格納のみに限定するか。
6. Connection lifetimeとGoal lifetimeを分離するか。
7. MuxでGoal Contextをどの層に保持するかを後続実装設計へ残すか。

## 16. 対象外

- 公開APIの具体的な関数シグネチャ
- JSON設定Schemaの詳細
- クラスの具体的なメンバー変数
- queueの物理構成と容量
- thread safety実装
- MuxでのGoal Context所有方式の最終決定
- C API、Python APIの具体的な形
- ROS 2 executor統合の詳細
