# Hakoniwa Action設定モデル

> **Status: Implemented contract**  
> 本文書は、user-facing Action manifest、resolved設定、生成Endpoint設定の現行契約です。

## 1. 目的

Hakoniwa Action Runtimeは、生成済みのAction Packet型だけでは通信経路を初期化できません。

Runtimeは少なくとも、次の構成情報を必要とします。

- どのActionを公開または利用するか
- Actionの型は何か
- Client側とServer側をどのEndpointへ対応付けるか
- 同時利用に備えて何個の通信スロットを予約するか
- 各スロットの論理チャネル名と論理チャネルIDをどう決めるか

本書では、Service定義ファイルに対応するAction定義ファイルの責務と、Action Runtimeが使用する論理チャネルおよびスロットの規則を定義します。

## 2. 設計原則

### 2.1 ProtocolとTransport設定を分離する

Action設定は、共有メモリ、TCP、muxなどの特定Transportへ依存しない論理構成を表します。

```text
Action設定
  Action名
  Action型
  Endpoint対応
  予約スロット数
  論理チャネル規則

Transport設定
  共有メモリ領域
  TCP host / port
  mux接続
  Transport固有のbufferおよびrouting情報
```

Action Protocol上の相関キーは`goal_id`です。Endpoint、接続、共有メモリチャネル、socketなどはProtocol identityではありません。

### 2.2 固定PDUサイズとheap上限を分離する

固定PDUサイズは、主に共有メモリTransportが領域およびPDU定義を事前確保するために必要とします。

TCPやlength-prefixed transportでは、受信フレーム長からPacketサイズを動的に扱えるため、Action設定の共通必須項目として固定PDUサイズを要求しません。一方、可変長bodyを無制限に許可すると、ユーザーコードの不具合によって過大なメモリ確保や送信が発生します。このためAction定義は、Request、Response、Feedbackごとのheap容量上限`bufferHeap`を持ちます。

共有メモリTransportは、Registryが生成したAction Packetのsize情報とPDU metadata sizeから、必要な固定サイズを解決します。

```text
shared memory packet size
  = transport metadata size
  + generated base size
  + configured heap capacity
```

`bufferHeap`は固定送信サイズではありません。送信時には実際の`metadata.total_size`だけを送信し、送受信の両側で実heapサイズが設定上限以下であることを検証します。

## 3. Action定義の基本単位

Action定義は、Action Typeごとに一つの論理チャネル名前空間を持ちます。

例:

```text
Action name: fibonacci
Action type: sample_action_msgs/Fibonacci
```

別のAction Typeは独立した名前空間を持つため、それぞれの論理チャネルIDは0から開始できます。

```text
fibonacci
  channel_id 0..

move_robot
  channel_id 0..
```

実際のPDU識別は、Action名とチャネル名、またはAction名と論理チャネルIDの組み合わせで行います。

```text
PduKey
  robot/action name = fibonacci
  pdu/channel name  = Slot0Request

PduResolvedKey
  robot/action name = fibonacci
  channel_id        = 0
```

## 4. 通信スロット

### 4.1 スロットの目的

共有メモリでは、通信に使用するチャネルを実行前に予約する必要があります。

同一Action Typeで複数Goalを並行処理するため、Action定義は複数の通信スロットを予約します。

各スロットは、次の3論理チャネルを持ちます。

```text
Request
  Goal Request
  Cancel Request

Response
  Goal Response
  Cancel Response
  Result

Feedback
  Feedback
```

1スロットにつき3チャネルです。

### 4.2 論理チャネルID

論理チャネルIDはAction Typeごとに0から連番で自動採番します。

スロット`n`に対する割り当ては次のとおりです。

```text
request_channel_id(n)  = n * 3 + 0
response_channel_id(n) = n * 3 + 1
feedback_channel_id(n) = n * 3 + 2
```

例として、4スロットを予約する場合は次の12チャネルを生成します。

```text
slot 0
  0: request
  1: response
  2: feedback

slot 1
  3: request
  4: response
  5: feedback

slot 2
  6: request
  7: response
  8: feedback

slot 3
  9: request
 10: response
 11: feedback
```

論理チャネルIDはAction Typeの名前空間内でのみ一意です。システム全体でグローバルに一意である必要はありません。

### 4.3 チャネル名

数値の論理チャネルIDだけでは、外部の設定、ログ、PDU定義、デバッグツールから意味を判別できません。

そのため、各論理チャネルは決定的に生成されるチャネル名を持ちます。

チャネル名は次の形式です。

```text
Slot0Request
Slot0Response
Slot0Feedback
Slot1Request
Slot1Response
Slot1Feedback
...
```

命名規則は一度確定した後、設定、Runtime、生成ツール、テスト間で共通化しなければなりません。

チャネル名から論理チャネルIDを解決でき、論理チャネルIDからチャネル名を復元できることを要求します。

## 5. Goalとスロットの動的対応

通信スロットは、特定のGoalへ静的に固定しません。

Client RuntimeはGoal開始時に空きスロットを取得し、`goal_id`とスロットを動的に対応付けます。

```text
Goal Request開始
  -> free slotを取得
  -> goal_id -> slot_indexを登録
  -> slotのRequest channelで送信
```

Server RuntimeはGoal Requestを受信したスロットを記録し、同じGoalのGoal Response、Cancel Response、Feedback、Resultを対応するResponseまたはFeedbackチャネルへ配送します。

```text
goal_id
  -> slot_index
      -> request channel
      -> response channel
      -> feedback channel
```

Goalがrejectされた場合、または受理後にterminal Resultまで完了した場合、Runtimeは対応する応答の配送責務が完了した後にスロットを解放します。

```text
pending Goal rejected
  -> Goal Response(REJECTED) delivery completed
  -> slot release

accepted Goal terminal completion
  -> Result delivery responsibility completed
  -> slot release
```

Goal Response timeoutはslot解放条件ではありません。Clientが応答待ちを諦めた時点でもServerがGoalを処理中またはaccept済みの可能性があるため、Client Runtimeは該当slotをquarantineし、transport stop／disconnect後の明示resetまで別Goalへ再利用しません。

Server Runtimeは`slot_index -> active goal_id`の所有関係も保持します。使用中slotで異なるGoal IDのGoal Requestを受信した場合はApplicationへ配送せず、Protocol上のGoal rejectとして応答します。

スロット割り当てはTransport routing上の資源管理です。Protocol上のGoal identityは引き続き`goal_id`です。

## 6. 予約スロット数

Action定義は、事前に予約する通信スロット数を指定します。

通信スロット数は`slotCount`で指定します。

```json
{
  "name": "fibonacci",
  "type": "sample_action_msgs/Fibonacci",
  "slotCount": 4
}
```

`slotCount`はRuntime／Transportが同時に保持できる通信lane数です。Client接続数でも、Applicationが業務上受理できるGoal数でもありません。共有メモリ実装では1 active Goalが1 slotを占有するため、結果的に通信上のin-flight上限になりますが、Applicationの並列実行Policyとは分離します。

スロットが枯渇した場合、Clientの`send_goal_with_result()`は同期的に`GoalSendResult::NO_FREE_SLOT`を返します。既存Goalのチャネルを再利用したり、待機queueへ暗黙に積んだりしません。

### 6.1 可変長bodyのheap上限

Action定義は、生成される3種類のPacketと1対1に対応するheap容量上限を指定できます。

```json
{
  "bufferHeap": {
    "requestSize": 1048576,
    "responseSize": 1048576,
    "feedbackSize": 1048576
  }
}
```

| 項目 | 対象Packet | 主なbody |
| --- | --- | --- |
| `requestSize` | `<Action>ActionRequest` | Goal Request、Cancel Request |
| `responseSize` | `<Action>ActionResponse` | Goal Response、Cancel Response、Result |
| `feedbackSize` | `<Action>ActionFeedback` | Feedback |

各値の単位はbyte、範囲は`0..2147483647`です。`bufferHeap`全体または個別項目を省略した場合は、各項目に1 MiB（`1048576` byte）を適用します。generatorは省略を警告し、resolved設定には解決後の3項目を必ず出力します。本番構成では明示指定を推奨します。

送信側Action Endpointは、`create_result_buffer()`または`create_feedback_buffer()`を公開し、Registryのbase sizeと対応する`bufferHeap`を使って完全なPDU bufferを確保・初期化します。上位Typed Action層はそのbufferへ生成コンバーターでbodyをencodeします。buffer自体の`resize()`は要求せず、コンバーターがmetadataへ記録した`total_size`を実際のWireサイズとして使用します。

`complete()`、`send_feedback()`および内部の`send_response_packet()`は、エンコード済みPDUだけを受け取ります。send処理の途中でpacketを暗黙生成してはなりません。Action Endpointは共通Headerを設定する前に、次を検証します。

```text
metadata.total_size <= PduData.size()       # outbound capacity buffer
metadata.total_size == received span.size() # inbound wire packet
actual_heap_storage_size = metadata.total_size - metadata.heap_off
actual_heap_storage_size <= align(bufferHeap.<packet kind>)
```

PDU heapは8-byte境界へalignされるため、上限比較にも同じalign規則を適用します。`metadata.heap_off`はRegistryのgenerated base sizeから求めた位置と完全一致しなければならず、baseとheapの間へ任意の領域を挿入して上限検査を迂回することはできません。

上限超過、metadata不整合、または変換時のbuffer不足は送信失敗とし、Action設計で定義するRuntime Errorとして上位レイヤへ通知します。受信側もdecode前に同じ検証を行い、上限を超えるPacketを拒否します。

Goal ResponseおよびCancel Responseはbodyを使用しません。Runtimeは内部のcontrol response factoryで最初からheap size 0の完全なAction Response packetを生成します。Result用の`responseSize`領域を確保してから縮小する処理は行いません。

## 7. Endpoint対応

Action定義は、Action Client RuntimeおよびAction Server RuntimeをEndpoint定義へ対応付けます。

概念例:

```json
{
  "name": "fibonacci",
  "type": "sample_action_msgs/Fibonacci",
  "slotCount": 4,
  "clientEndpoint": {
    "nodeId": "fibonacci-client"
  },
  "serverEndpoint": {
    "nodeId": "fibonacci-server"
  }
}
```

この対応は論理構成です。各Endpointが共有メモリ、TCP、muxなどのどのTransportを利用するかはEndpointまたはTransport設定側で定義します。

Point-to-point設定はgeneratorがClient／Server Endpointを生成します。Mux Serverは明示指定されたMux Endpoint設定から複数接続を受け入れ、routingをRuntime内部で管理します。user-facing point-to-point manifestからMux topologyを暗黙生成しません。

## 8. 最小Action定義例

point-to-point TCP構成では、Action定義を次のように表現できます。

```json
{
  "actions": [
    {
      "name": "fibonacci",
      "type": "sample_action_msgs/Fibonacci",
      "slotCount": 4,
      "bufferHeap": {
        "requestSize": 1048576,
        "responseSize": 1048576,
        "feedbackSize": 1048576
      },
      "clientEndpoint": {
        "nodeId": "fibonacci-client"
      },
      "serverEndpoint": {
        "nodeId": "fibonacci-server"
      }
    }
  ]
}
```

この定義から、Runtimeまたは設定生成ツールは次を自動展開します。

```text
Action namespace
  fibonacci

reserved slots
  4

logical channels
  4 slots * 3 channels = 12 channels

channel ids
  0..11

channel names
  Slot0Request
  Slot0Response
  Slot0Feedback
  ...
  Slot3Feedback

```

## 9. Transport別の解釈

### 9.1 TCP Transport

TCP raw transportは、Packetサイズを受信フレームから判断します。固定PDUサイズや共有メモリチャネルの事前確保は行いません。

実装は、論理チャネル名、slot index、packet kind、transport sessionなどを内部routingへマッピングできます。

Action v1の設定generatorとRuntimeはTCP transportを対象とします。

## 10. Runtimeが保持する情報

Runtimeは、少なくとも次の情報を保持します。

```text
ActionDefinition
  name
  type
  slot_count
  buffer_heap(request, response, feedback)
  client_endpoint
  server_endpoint

ChannelDefinition
  slot_index
  channel_kind
  channel_id
  channel_name
  packet_type
  packet_size optional / transport-specific

ActiveSlot
  slot_index
  goal_id
  lifecycle state
  response routing
  feedback routing
```

`goal_id`からActiveSlotを検索でき、slot indexから対応する3チャネルを決定できる必要があります。

## 11. 設定の設計判断

1. Action定義ファイルをService定義ファイルに対応する論理構成として追加する。
2. 固定PDUサイズはAction設定の共通必須項目にしないが、可変長bodyの安全上限`bufferHeap`を省略可能な共通契約とする。
3. 固定PDUサイズは共有メモリTransportがRegistryのbase size、Action定義のheap capacity、metadata sizeから解決する。
4. 同一Action Typeの並行Goalに備え、複数の通信スロットを事前予約する。
5. 1スロットはRequest、Response、Feedbackの3論理チャネルを持つ。
6. 論理チャネルIDはAction Typeごとに0から自動採番する。
7. 各論理チャネルは外部から意味を識別できる決定的なチャネル名を持つ。
8. Goal開始時に`goal_id`と空きスロットを動的に対応付ける。
9. Goal rejectまたはterminal Resultの配送責務が完了した後にスロットを解放する。
10. スロットとチャネルはTransport routing資源であり、Protocol identityは`goal_id`のままとする。
11. point-to-point Endpoint設定はgeneratorで静的に生成し、Mux routingは`ActionServicesMuxServer`内部で所有する。

## 12. TCP v1のuser-facing manifest

TCP v1では、ユーザーが編集する入力を一つのmanifestとして提供します。ただし、論理Action定義とTransport deploymentは別の階層へ置き、責務を混在させません。

```json
{
  "version": 1,
  "actions": [
    {
      "name": "fibonacci",
      "type": "sample_action_msgs/Fibonacci",
      "slotCount": 4,
      "bufferHeap": {
        "requestSize": 1048576,
        "responseSize": 1048576,
        "feedbackSize": 1048576
      },
      "clientEndpoint": {"nodeId": "fibonacci-client"},
      "serverEndpoint": {"nodeId": "fibonacci-server"}
    }
  ],
  "transport": {
    "protocol": "tcp",
    "packetVersion": "v2",
    "queueDepth": 64,
    "endpoints": {
      "fibonacci-client": {
        "role": "client",
        "remote": {"address": "127.0.0.1", "port": 54011}
      },
      "fibonacci-server": {
        "role": "server",
        "local": {"address": "0.0.0.0", "port": 54011}
      }
    }
  }
}
```

正式なSchemaは`config/schema/action-schema.json`、実例は`config/sample/action.json`を参照します。

### 12.1 Action roleとTCP role

Action Client／ServerはGoal lifecycle上の役割です。TCP client／serverは接続確立上の役割です。両者を固定対応させません。

したがって、次のどちらも許可します。

```text
Action Client = TCP client, Action Server = TCP server
Action Client = TCP server, Action Server = TCP client
```

point-to-point構成では、一つのActionが参照する二つのEndpointのTCP roleが相補的であることをgeneratorが検証します。

### 12.2 TCP packet size

TCP raw transportでは、encode済み`PduData`の`metadata.total_size`をframeへ記録し、受信時にはdecodeされた実payloadサイズがcallbackへ渡されます。`PduData.size()`は確保容量であり、そのままWireサイズにはしません。

```text
send size    = metadata.total_size
receive size = received span.size()
```

固定PDUサイズはuser-facing TCP設定へ追加しません。TCPは実際の`metadata.total_size`を送信し、送受信時に`bufferHeap`の上限を検証します。Action v1のTransport契約はTCPです。

## 13. 自動生成される設定

次のコマンドでuser-facing manifestからRuntime設定を生成します。

```bash
python tools/generate_action_config.py \
  --config config/sample/action.json \
  --output .hako/action
```

生成物は次のとおりです。

```text
.hako/action/
├── resolved-action.json
├── endpoints.json
├── queue.json
├── endpoints/
│   ├── fibonacci-client.json
│   └── fibonacci-server.json
└── transport/
    ├── fibonacci-client.json
    └── fibonacci-server.json
```

- `resolved-action.json`: channel、packet type、Endpoint IDを展開した設定
- `endpoints.json`: `EndpointContainer`が読むnode／Endpoint対応
- `endpoints/*.json`: Endpointが読むcache／comm参照
- `transport/*.json`: 既存TCP Endpointが読むrole、address、port、packet version
- `queue.json`: Action Endpoint用queue設定

Endpoint IDは`<nodeId>-action-tcp`として決定的に生成します。channel ID、channel名、packet type、Endpoint IDをユーザーへ重複指定させません。

TCP v1は`PduResolvedKey`を使用するため、生成Endpoint設定に固定サイズの`pdu_def_path`を含めません。
