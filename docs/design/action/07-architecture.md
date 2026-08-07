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

### 3.1 Endpointという用語の区別

本アーキテクチャには、名前が似ている二種類のEndpointがあります。

```text
ActionServerEndpointImpl / ActionClientEndpointImpl
  = Action Packet Endpoint
  = 上位Goal TransactionとAction packetのmapping境界

hakoniwa-pdu-endpoint::Endpoint
  = PDU Endpoint
  = byte列とchannelを配送する通信境界
```

Action Packet EndpointはHeader、packet queue、slot、channel、connection associationを扱います。PDU EndpointはActionのGoalや状態遷移を解釈しません。

Goal lifecycleの状態機械は、`hakoniwa-pdu-rpc`内の上位Goal Transaction責務として一か所に保持します。初期実装でFacadeが同一クラスに見える場合も、Goal Protocol stateとPacket Binding stateを別のモデルとして実装します。

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
- accept済み`goal_id`ごとのClient Goal Context管理
- Client状態遷移関数の適用
- Goal Response、Feedback、Cancel Response、Resultの意味解釈
- Runtimeの開始、停止、Context解放

次の責務は持ちません。

- Goal Response受信前のpre-accept Context管理
- packet bindingおよびslot ownership
- Action Headerのencode／decodeとWire相関
- Transport実装

pre-accept Contextとpacket／slot管理は`ActionClientEndpointImpl`へ委譲します。`GOAL_RESPONSE(ACCEPTED)`をpollした時点で、`ActionServicesClient`が`EXECUTING`のClient Goal Contextを生成します。`GOAL_RESPONSE(REJECTED)`では生成しません。

## 7. ActionClientEndpointImpl

`ActionClientEndpointImpl`は、1 Action Type分のClient Protocol Runtimeです。

内部に次を持ちます。

```text
ActionClientEndpointImpl
  - pre-accept Packet Binding Registry
  - Request sender
  - Response receiver queue
  - Feedback receiver queue
  - Protocol dispatcher
  - Endpoint reference
  - Time source
```

Packet Binding Registryは`goal_id`をキーとして、少なくとも次を管理します。

```text
goal_id
slot ownership
Goal Response pending
Cancel Response pending
Result pending
Feedback sequence state
```

主な責務:

- 上位Applicationが指定した`goal_id`の検証と保持
- pre-accept packet binding生成
- Goal Request送信
- Cancel Request送信
- Goal Response、Cancel Response、Resultの相関
- FeedbackのGoal別配送
- terminal Result受信後のpacket bindingとslot解放

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

`ActionServerEndpointImpl`は、1 Action Type分のServer Action Packet Endpointです。上位のGoal Transactionと、PDU Endpointが扱うpacket経路を対応付けます。

内部に次を持ちます。

```text
ActionServerEndpointImpl
  - Action Packet Binding Registry
  - Request receiver queue
  - Application event queue
  - Response / Feedback / Result packet builder / sender
  - Header codec / packet dispatcher
  - slot / channel / connection mapping
  - Endpoint reference
```

Action Packet Binding Registryは上位Transactionの`goal_id`またはGoalHandleをキーとして、少なくとも次を管理します。

```text
goal_id
slot_index
Request / Response / Feedback channel
Endpoint or connection association
Goal判断packetのdelivery state
Result delivery / retention state
```

主な責務:

- Goal Request Header検証
- 受信slotと上位Goal Transactionのbinding生成
- UUID形式および既存bindingとの重複検査
- Application／上位Transactionへの新規Goal packet通知
- 上位判断に基づくGoal Response packet生成と配送
- Cancel Request packetのbinding解決
- FeedbackおよびResult packetのrouting
- outbound packetの配送状態とbinding解放

次はAction Packet Endpointの責務ではありません。

- Goalを業務上accept／rejectする判断
- `EXECUTING`／`CANCELING`／`FINISHING`のProtocol状態機械
- Cancel／Result競合の意味論的判断
- Application worker、queue、priority、preemption

これらのうちProtocol共通部分は`hakoniwa-pdu-rpc`内の上位Goal Transaction層が所有し、業務PolicyはServer Applicationが所有します。

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

### 12.1 初期Mux実装の選択

実現方法として次の選択肢を検討しました。

```text
A. Goal ContextをConnectionSlot内のActionServer Runtimeが保持し続ける
B. 接続断時にGoal Contextを接続非依存Registryへ移管する
C. Muxより上位に共有ActionServer Runtimeを置き、接続AdapterだけをSlotに持つ
```

初期Mux実装では**方式A**を採用します。接続ごとの`ActionServicesServer`がGoal Contextを保持し、active Goalを持つ切断済み`ConnectionSlot`はorphaned slotとしてterminal完了まで保持します。

Mux自身は`(action_name, goal_id) -> connection_id`のrouting indexだけを持ち、Goal状態を重複管理しません。接続をまたぐGoal IDの一意性、切断時のRuntime Cancel、orphaned slotの回収条件は[Action Mux Server契約](12-mux-server.md)で規定します。

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
7. MuxでGoal Contextを接続ごとの`ActionServicesServer`に保持し、Muxはrouting indexだけを持つか。

## 16. 対象外

- 公開APIの具体的な関数シグネチャ
- JSON設定Schemaの詳細
- クラスの具体的なメンバー変数
- queueの物理構成と容量
- thread safety実装
- session resumeおよび接続間のGoal Context migration
- C API、Python APIの具体的な形
- ROS 2 executor統合の詳細
