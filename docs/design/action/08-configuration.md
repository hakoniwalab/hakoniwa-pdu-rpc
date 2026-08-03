# Hakoniwa Action設定モデル

> **Status: Draft**  
> 本文書はレビューと議論のための初稿です。現時点では確定仕様ではありません。

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

### 2.2 PDUサイズは共通必須設定にしない

固定PDUサイズは、主に共有メモリTransportが領域およびPDU定義を事前確保するために必要とします。

TCPやlength-prefixed transportでは、受信フレーム長からPacketサイズを動的に扱えるため、Action設定の共通必須項目としてPDUサイズを要求しません。

共有メモリTransportは、Registryが生成したAction Packetのsize情報とPDU metadata sizeから、必要な固定サイズを解決します。

```text
shared memory packet size
  = generated packet size
  + transport metadata size
```

必要な場合に限りTransport設定側で明示的な上書きを許可できますが、Action論理定義へ重複して記述することを標準にはしません。

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

初期候補は次のとおりです。

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

Goalがrejectされた場合、または受理後にterminal Resultまで完了した場合、Runtimeは対応するスロットを解放します。

```text
pending Goal rejected
  -> slot release

accepted Goal terminal completion
  -> Result delivery responsibility completed
  -> slot release
```

スロット割り当てはTransport routing上の資源管理です。Protocol上のGoal identityは引き続き`goal_id`です。

## 6. 予約スロット数

Action定義は、事前に予約する通信スロット数を指定します。

既存Service設定との対称性から、初期実装では`maxClients`という名前を利用できます。

```json
{
  "name": "fibonacci",
  "type": "sample_action_msgs/Fibonacci",
  "maxClients": 4
}
```

ただしActionでは、1 Clientが複数Goalを並行実行する可能性があります。そのため、この値の実質的な意味は「同時に使用可能な通信スロット数」です。

将来的に設定名を整理する場合は、次の候補があります。

```text
slotCount
maxConcurrentGoals
maxChannels
```

初期実装で`maxClients`を採用する場合でも、Runtime内部ではClient数ではなくスロット数として扱うことを明記します。

スロットが枯渇した場合のClient API挙動は、後続のエラー契約で定義します。少なくとも、既存Goalのチャネルを再利用して混線させてはなりません。

## 7. Endpoint対応

Action定義は、Action Client RuntimeおよびAction Server RuntimeをEndpoint定義へ対応付けます。

概念例:

```json
{
  "name": "fibonacci",
  "type": "sample_action_msgs/Fibonacci",
  "maxClients": 4,
  "clientEndpoint": {
    "nodeId": "fibonacci-client"
  },
  "serverEndpoint": {
    "nodeId": "fibonacci-server"
  }
}
```

この対応は論理構成です。各Endpointが共有メモリ、TCP、muxなどのどのTransportを利用するかはEndpointまたはTransport設定側で定義します。

複数Client Endpoint、動的Endpoint、mux上のroutingなどの詳細は初期実装範囲外とし、後続で拡張します。

## 8. 最小Action定義例

初期のstatic Endpoint構成では、Action定義を次のように表現できます。

```json
{
  "actions": [
    {
      "name": "fibonacci",
      "type": "sample_action_msgs/Fibonacci",
      "maxClients": 4,
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

shared memory only
  generated packet sizesをRegistryから解決
  metadata sizeを加算
  PDU definitionsを事前登録
```

## 9. Transport別の解釈

### 9.1 共有メモリ

共有メモリTransportは、予約スロットに対応する全論理チャネルを起動時にPDU定義へ登録します。

- チャネルIDはAction Typeごとに0から連番
- チャネル名は決定的な命名規則から生成
- PDUサイズはRegistry生成情報から解決
- Goal開始時に空きスロットを割り当て
- terminal完了後にスロットを解放

### 9.2 動的サイズを扱えるTransport

TCPやlength-prefixed transportは、Packetサイズを受信フレームから判断できます。

これらのTransportは、共有メモリと同じAction設定インターフェースを受け取りますが、固定PDUサイズや事前の共有メモリチャネル確保を必要としない場合があります。

実装は、論理チャネル名、slot index、packet kind、transport sessionなどを内部routingへマッピングできます。

Action設定インターフェースを共通化する目的は、すべてのTransportへ共有メモリの実装制約を強制することではありません。

## 10. Runtimeが保持する情報

初期実装のRuntimeは、少なくとも次の情報を保持します。

```text
ActionDefinition
  name
  type
  slot_count
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

## 11. 設計判断

1. Action定義ファイルをService定義ファイルに対応する論理構成として追加する。
2. PDUサイズはAction設定の共通必須項目にしない。
3. 固定PDUサイズは共有メモリTransportがRegistry生成情報から解決する。
4. 同一Action Typeの並行Goalに備え、複数の通信スロットを事前予約する。
5. 1スロットはRequest、Response、Feedbackの3論理チャネルを持つ。
6. 論理チャネルIDはAction Typeごとに0から自動採番する。
7. 各論理チャネルは外部から意味を識別できる決定的なチャネル名を持つ。
8. Goal開始時に`goal_id`と空きスロットを動的に対応付ける。
9. Goal終了後にスロットを解放する。
10. スロットとチャネルはTransport routing資源であり、Protocol identityは`goal_id`のままとする。
11. 初期実装はstatic Endpoint対応を対象とし、dynamic Endpointおよびmux routingは後続で設計する。

## 12. 未確定事項

- 設定項目名を`maxClients`のままにするか、`slotCount`へ変更するか
- チャネル名の最終的な大文字小文字および接尾辞規則
- Action定義ファイルの正式ファイル名と配置
- 複数Client Endpointを一つのAction定義へ記載する方式
- slot枯渇時の同期エラー、待機、timeoutの契約
- reject時およびResult送信失敗時の正確なslot解放条件
- dynamic transportで論理チャネルをwire上へ表現する必要があるか
- mux transport sessionとslot lifetimeの対応
- Registry size情報の具体的な参照API
