# Hakoniwa Actionの責務境界

> **Status: Implemented contract**
> 本文書は、Registry、RPC Runtime、ROS Adapter、Application、Endpoint間の現行責務境界です。

## 1. 目的

本書では、Hakoniwa Action Protocolに関係する各リポジトリおよび利用アプリケーションの責務境界を定義します。

Action対応では、以下の異なる関心事が同時に現れます。

- Action固有データ型とPDUレイアウト
- Goal Executionの状態と通信ライフサイクル
- ROS 2 Action APIとの変換
- Goalを実際に処理する業務ロジック
- 同一Action Typeに対する複数Goalの実行ポリシー
- EndpointおよびTransportによる配送

これらを一つの実装へ集約せず、それぞれの層が所有する責務を明確にします。

## 2. 全体構造

```text
ROS 2 Action Client
        |
        | ROS 2 Goal / Feedback / Cancel / Result
        v
hakoniwa-pdu-ros
        |
        | Hakoniwa Action API
        v
hakoniwa-pdu-rpc
        |
        | Action Request / Response / Feedback PDU
        v
hakoniwa-pdu-endpoint / transport
        |
        v
Hakoniwa Action Server Application
```

データ型と変換コードは`hakoniwa-pdu-registry`が生成し、各層がその生成物を使用します。

```text
ROS .action / Hakoniwa Action IDL
        |
        v
hakoniwa-pdu-registry
        |
        +--> Goal / Result / Feedback types
        +--> Action Request / Response / Feedback packet types
        +--> converters / offsets / size metadata
```

## 3. hakoniwa-pdu-registry

### 3.1 所有する責務

`hakoniwa-pdu-registry`は、Action通信で使用するデータ契約と生成物を所有します。

- ROS 2 `.action`定義のGoal、Result、Feedbackへの分解
- Action Request、Action Response、Action FeedbackのPDU定義生成
- Action共通ヘッダの定義
- `request_kind`、`response_kind`、`status`などの数値契約
- UUID形式の`goal_id`、`sequence_no`などのフィールドレイアウト
- 通常のPDU生成パイプラインによる各言語型とconverterの生成
- offset、aligned size、CDR size、type metadataの生成
- 生成物の決定性と既存Service生成物との互換性

### 3.2 所有しない責務

- `goal_id`の生成
- Goalを受理または拒否する判断
- ClientおよびServerの状態機械
- Feedbackの送信タイミング
- CancelとResultの競合解決
- Action APIの実行時挙動
- ROS 2 Goal UUIDとの対応管理
- 同一Action Typeの並列実行、直列化、キュー、排他判断
- Action固有の業務処理

Registryはデータを表現できるようにしますが、そのデータをいつ送るか、受信後にどう状態遷移するかは決定しません。

## 4. hakoniwa-pdu-rpc

### 4.1 所有する責務

`hakoniwa-pdu-rpc`は、Hakoniwa Actionの通信ライフサイクルと実行時Protocolを所有します。

- 上位Client Application／Adapterが指定した`goal_id`の検証とactive Goalとの衝突検出
- 外部Adapterが指定した互換UUIDの受け入れ
- Goal送信、Goal Response受信、および相関
- `goal_id`による複数Goal Executionの独立管理
- 同一Action Typeおよび同一Endpointに存在する複数Goalの状態管理
- Feedbackの非終端配送
- Cancel RequestおよびCancel Responseの配送
- ResultおよびTerminal Statusの配送
- GoalごとのClientおよびServer状態機械
- 許可された状態遷移と不正操作の検出
- duplicate、遅延、未知の`goal_id`の扱い
- CancelとResultなどの競合規約
- timeout、shutdown、transport errorをProtocol/APIへ反映する方法
- Runtime起因の停止要求をApplicationの正規Cancel経路へ渡す方法
- C++ APIとPython Bindingが共有すべき意味論
- 既存Service RPCとの後方互換性

RPC Runtimeは、各Goal Executionを独立して識別し、配送し、状態管理できなければなりません。ProtocolまたはRuntime APIを、同一Action Typeにつき1 Goal、1 Clientにつき1 Goalという前提へ固定しません。

### 4.2 所有しない責務

- `.action`ファイルの解析とPDUコード生成
- ROS 2 Node、Action Server、GoalHandleの実装
- ROS 2固有ステータスへの変換
- Goalの業務上の妥当性判断
- 同一Action TypeのGoalを何件受理するかの判断
- Goalの並列実行、直列化、キューイング、優先度、排他、preemption方針
- ロボット制御、経路計画、計算処理などAction固有の実処理
- Applicationのハング、complete忘れ、worker停止の監視・救済
- Transport固有のSocket、共有メモリ、再接続処理の詳細

RPC Runtimeは通信とGoal lifecycleを管理しますが、Goalを実際に達成する処理や、その実行資源の配分を決定しません。

Runtime自身のメモリ、接続、バッファなどの技術的な問題はApplicationの業務上の`ABORTED`と区別します。ApplicationのハングやRuntimeの致命的内部障害をterminal statusへ変換しません。

### 4.3 RPC内部のTransactionとPacket Binding

`hakoniwa-pdu-rpc`が所有する責務も、論理的には次の二層へ分離します。

```text
Goal Transaction
  goal_idごとのProtocol lifecycle
  Goal／Cancel判断
  EXECUTING／CANCELING／FINISHING
  Cancel／Result競合
          |
          | goal_id / GoalHandle
          v
Action Packet Endpoint
  Action Transactionとpacket経路の対応
  slotおよびchannel binding
  Header encode／decode
  inbound／outbound packet queue
  配送結果とbinding解放
```

Goal TransactionはAction全体の長寿命なProtocol instanceです。その内部にはGoal Request／Response、Feedback、Cancel Request／Response、Resultという複数のpacket交換が存在します。

Action Packet Endpointは、上位のGoal TransactionをProtocol状態として再実装しません。Endpointが保持するのは、上位Transactionを実際のpacket経路へ対応付ける`ActionPacketBinding`です。

```text
ActionPacketBinding
  goal_id
  ingress endpoint / connection
  slot_index
  request channel
  response channel
  feedback channel
  outbound delivery state
```

このbindingの予約、判断待ち、配送待ち、解放はrouting／delivery上の状態であり、`EXECUTING`、`CANCELING`、`FINISHING`というGoal Protocol状態とは区別します。

Applicationからの`accept_goal()`、`reject_goal()`、`complete()`は上位Transactionの判断を表します。Action Packet Endpointは、その判断を対応するbindingとpacketへ変換します。API呼び出しと物理的なTransport送信は同一処理である必要はなく、outbound queueを介して分離できます。

論理イベントとの対応は次のとおりです。

| 上位操作 | Transaction上の意味 | Packet Endpointが生成するイベント |
| --- | --- | --- |
| `accept_goal()` | Goal判断をACCEPTEDとして一度だけ確定 | `GOAL_RESPONSE(ACCEPTED)` |
| `reject_goal()` | Goal判断をREJECTEDとして一度だけ確定 | `GOAL_RESPONSE(REJECTED)` |
| `complete()` | accept済みGoalのterminal結果を確定 | `RESULT` |

`complete()`はGoal Request／Response交換の完了ではなく、Action実行全体のterminal完了を表します。

Goal／Cancel Response、Feedback、ResultはAction Packet EndpointからPDU Endpointへ同期的に送信します。論理判断、packet生成、配送結果を区別し、呼び出し側へTransport固有差分を露出させません。

## 5. hakoniwa-pdu-ros

### 5.1 所有する責務

`hakoniwa-pdu-ros`は、ROS 2 Action APIとHakoniwa Action APIのAdapterを所有します。

- ROS 2 Action Serverの生成と実行
- ROS 2 Goal messageから生成済みHakoniwa Goal PDU bodyへの変換
- Hakoniwa Feedback PDU bodyからROS 2 Feedback messageへの変換
- Hakoniwa Result PDU bodyからROS 2 Result messageへの変換
- ROS 2 Goal UUIDをHakoniwa `goal_id`として指定する処理
- ROS 2 cancel callbackとHakoniwa Cancel APIの接続
- ROS 2 GoalHandleの状態とHakoniwa terminal statusの対応
- 複数のROS GoalHandleと複数のHakoniwa Goal Executionの対応管理
- ROS executorおよび`rclpy` Futureへの非同期完了通知
- ROS Binding設定の読み込みとHakoniwa Action Runtime設定への接続
- ROS固有の変換失敗やNode lifecycleの診断

### 5.2 所有しない責務

- Hakoniwa Action Protocolそのものの状態機械
- FeedbackキューやCancel/Result競合の独自実装
- Action共通PDUレイアウトの定義
- 同一Action Typeの業務上の同時実行方針
- Action固有の業務処理
- ROS 2以外の利用者に対するProtocol仕様

ROS BridgeがAction状態機械を再実装すると、C++、Python、ROSの各利用者で意味論が分岐します。そのため、状態と競合規約は可能な限り`hakoniwa-pdu-rpc`へ集約します。

## 6. Action Server Application

### 6.1 所有する責務

Action Server Applicationは、Goalを実際に処理するドメインロジックと実行ポリシーを所有します。

- Goal bodyの業務上の検証
- Goalを受理または拒否するアプリケーション判断
- 同一Action Typeの複数Goalを受理するかどうかの判断
- Goalの並列実行、直列化、キューイング
- 同時実行数の上限
- 共有資源の排他、優先度、fairness
- 既存Goalと新規Goalのpreemption方針
- 受理したGoalの実処理
- Action固有Feedbackの生成
- 成功時のResult body生成
- 業務上の中断、失敗、abort判断
- Cancel要求を受けた後の安全な停止処理

同じAction Typeであっても、Applicationによって適切なPolicyは異なります。

```text
画像変換Action
  複数Goalを並列実行

ロボットアーム操作Action
  共有実機のため1 Goalだけ受理

経路計画Action
  最大N件を並列実行し、残りをキューへ格納
```

これらはProtocol差ではなくApplication Policyの差です。

### 6.2 Runtimeとの境界

RuntimeはProtocol上妥当なGoalをApplicationへ渡し、Applicationが返したaccept/reject判断をClientへ通知します。

```text
Runtime -> Application
  on_goal(goal_id, goal_body)
  on_cancel(goal_id, cause)

Application -> Runtime
  accept(goal_id)
  reject(goal_id, reason)
  publish_feedback(goal_id, feedback_body)
  succeed(goal_id, result_body)
  abort(goal_id, result_body or error)
  canceled(goal_id, result_body)
```

複数Goalを扱うため、Applicationへ渡すContextまたはGoalHandleは`goal_id`ごとに独立していなければなりません。

状態モデル（`04-state-model.md`）では、ApplicationがGoalをacceptした時点でProtocol上のGoalインスタンスを`EXECUTING`として生成します。Protocol状態として`ACCEPTED`、`QUEUED`、`EXECUTING`を分離しません。

Applicationは、accept後のGoalを内部queueへ格納したりworker待ちにしたりできます。ただし、それらはApplication内部状態であり、共通Protocol状態へ露出しません。

```text
Protocol Runtime:
  accept -> EXECUTING

Application:
  internal queue / worker wait / actual processing
```

### 6.3 所有しない責務

- PDUヘッダの直接構築
- `sequence_no`の採番方法
- transport送受信
- Clientとの通信相関管理
- Protocol上の不正状態検出
- duplicate `goal_id`のProtocol検査
- ROS 2 APIへの直接変換

アプリケーションはAction固有のbody、実行資源、同時実行Policyを扱い、共通ヘッダや通信Protocolの詳細から分離されることが望まれます。

## 7. hakoniwa-pdu-endpointとTransport

EndpointおよびTransportは、PDUを通信相手へ配送する責務を持ちます。

- Request、Response、Feedback経路の接続
- TCP、UDP、共有メモリなどのtransport実装
- PDUサイズとバッファ管理
- 送受信、切断、再接続、shutdownの低レベルイベント
- RPC Runtimeが利用できるpollまたはwait primitive
- `tcp_mux`による複数Client接続の収容
- `maxClients`による接続数上限の管理

Endpoint/TransportはActionのGoal、Feedback、Resultの意味や状態遷移を理解しません。

ここでいうEndpoint/Transportは`hakoniwa-pdu-endpoint::Endpoint`です。`hakoniwa-pdu-rpc`内の`ActionServerEndpointImpl`／`ActionClientEndpointImpl`は同じEndpointという名前を持ちますが、上位TransactionとPDU packetを対応付けるAction Packet Endpointです。両者を同一の責務として扱いません。

`maxClients`はTransportが同時に収容できるClient接続数であり、同一Action Typeの同時Goal数やApplicationの実行能力を表しません。

1 Clientが複数Goalを送信する場合も、複数Clientが同じAction TypeへGoalを送信する場合も、Goalの意味上の並列性は`goal_id`とApplication Policyで管理します。

`requestChannelId`や`feedbackChannelId`などの物理配置情報はuser-facing Action manifestへ要求せず、設定generatorがresolved Action設定とEndpoint設定へ同じ値を生成します。

## 8. 責務分担表

| 項目 | Registry | RPC | ROS Bridge | Application | Endpoint/Transport |
|---|---:|---:|---:|---:|---:|
| `.action`解析 | 主 |  |  |  |  |
| PDU型・レイアウト | 主 | 利用 | 利用 | 利用 | 配送 |
| `goal_id`データ定義 | 主 | 利用 | 利用 | 利用 | 透過 |
| `goal_id`生成 |  | 上位Applicationから受領 | ROS UUID指定 | 主（非ROS Client） |  |
| `goal_id`相関・状態管理 |  | 主 | ROS GoalHandle対応 | 利用 | 透過 |
| Goal受理Protocol |  | 主 | Adapter | 判断 | 透過 |
| Protocol上の自動拒否 |  | 主 | 透過 |  | 透過 |
| 業務上の受理判断 |  | 支援 | 透過 | 主 |  |
| 複数Goalの独立管理 | データ定義 | 主 | 対応管理 | 実行 | 配送 |
| 並列・直列・キュー・排他 |  |  | 透過 | 主 |  |
| Client接続数上限 |  | 利用 | 利用 |  | 主 |
| Feedback配送 | データ定義 | 主 | ROS変換 | 生成 | 配送 |
| Cancel状態遷移 |  | 主 | ROS変換 | 停止処理 | 配送 |
| Runtime起因Cancel |  | 主 | Adapter | 停止判断・処理 | 切断通知 |
| Result配送 | データ定義 | 主 | ROS変換 | 生成 | 配送 |
| terminal status意味論 | 数値契約 | 主 | ROS対応 | 選択 | 透過 |
| ROS GoalHandle |  |  | 主 |  |  |
| Action固有処理 |  |  |  | 主 |  |
| transport接続 |  | 利用 | 利用 | 利用 | 主 |

## 9. 横断的な設計原則

### 9.1 Protocolと業務処理を分離する

RPC RuntimeはGoal Executionを正しく管理しますが、そのGoalが何を意味し、どのように達成されるかはApplicationが所有します。

### 9.2 データ契約と状態遷移を分離する

RegistryはPDUを生成します。RPCは生成されたPDUを使って状態遷移を実現します。RegistryへRuntime固有分岐を持ち込みません。

### 9.3 ROS 2を仕様の中心に置かない

ROS 2 Actionは重要な接続先ですが、Hakoniwa Actionの唯一の利用形態ではありません。ROS固有概念はAdapter層へ閉じ込めます。

### 9.4 状態機械をBridgeへ複製しない

Cancel、timeout、late feedback、terminal raceの規約をBridgeごとに実装すると意味論が分岐します。共通化できる状態管理はRPC Runtimeへ置きます。

### 9.5 既存Service RPCを変更しない

ActionのためにService Request/Responseヘッダや既存APIの意味を拡張しません。Actionは独立したデータ契約と明示的な設定で追加します。

### 9.6 複数GoalをProtocol上で制限しない

同一Action Typeおよび同一Endpointに複数のGoal Executionが同時に存在することを許容します。

RuntimeはGoalごとに状態を独立管理しますが、それらを実際に並列実行するかどうかは決定しません。実行資源、排他、優先度、同時実行数はApplicationの責務です。

### 9.7 Transport容量とAction実行能力を分離する

`maxClients`はClient接続数の上限です。Actionの同時Goal数、Applicationのworker数、共有資源の数とは別の設定・概念として扱います。

## 10. 境界の適用規則

### 10.1 Goal受理判断の分割

Runtimeが自動的に拒否する条件と、Applicationへ判断を委ねる条件を分離します。

Runtime側:

- duplicate `goal_id`
- UUIDまたはprotocol version不正
- 空きslotなし

Application側:

- Goal bodyの業務上の不正
- ロボット状態が実行条件を満たさない
- 同時実行数上限
- workerまたは共有資源の不足
- ドメイン上の優先度、排他、キュー、preemption

### 10.2 goal_id生成責任

次を現行契約とします。

- 通常のHakoniwa Clientも上位Applicationが送信前にGoal IDを生成し、Runtimeは自動生成しない。
- ROS BridgeなどのAdapterは、外部で生成された互換UUIDを指定できる。
- Action Server Runtimeはactive Goalの重複検査、登録、状態管理を担当する。
- all-zeroは禁止し、UUID versionは検査しない。
- terminal完了後に解放したGoal IDの履歴は保持しない。

### 10.3 Application実行状態の公開範囲

ApplicationがGoalをacceptした時点でProtocol状態を`EXECUTING`とします。

- `ACCEPTED`相当の独立状態は設けない。
- `QUEUED`相当の共通Protocol状態は設けない。
- Application内部のqueue、worker待ち、実処理状態はApplicationが管理する。

Clientへ公開する実行状況はAction固有Feedback bodyで表現します。

### 10.4 Timeoutの所有者

以下を分離します。

- transport timeout
- Goal acceptance timeout
- Action execution deadline
- Bridge側のROS request deadline
- Application固有の処理期限
- Application queue wait timeout

すべてを一つのtimeoutへ統合すると、どの層がGoalを終了させたのか不明確になります。

### 10.5 Feedback保持方針

Endpointは受信packetをFIFOへ積み、`poll()`が`goal_id`とslotを検証して1件ずつApplicationへ通知します。最新値だけへ集約せず、Goalごとの相関を維持します。

## 11. 責務上の設計判断

1. Registryはデータ契約を所有し、状態機械を所有しない。
2. RPCはHakoniwa ActionのProtocolとGoal Execution lifecycleを所有する。
3. RPCは同一Action Typeの複数Goal Executionを`goal_id`ごとに独立管理できる設計とする。
4. ROS BridgeはROS 2との変換を所有し、Action状態機械を独自に実装しない。
5. ApplicationはGoalの業務処理、受理判断、並列実行、直列化、キュー、排他、優先度、preemption、Feedback生成、Result生成、安全なCancel処理を所有する。
6. Endpoint/Transportは配送とClient接続収容を所有し、Action意味論を所有しない。
7. `maxClients`はTransportの接続数上限であり、Actionの同時実行数とは定義しない。
8. `goal_id`は上位ApplicationまたはAdapterが送信前に用意し、Client Runtimeはall-zeroおよびactiveなIDとの衝突を同期エラーとして拒否する。Server Runtimeは受信後の重複検査とlifecycle管理を行い、RuntimeはGoal IDを自動生成しない。
9. Goalの受理判断は、Protocol上の自動拒否とApplication判断に分割する。
10. Applicationのaccept後、Protocol状態は直ちに`EXECUTING`とし、Application内部のqueue状態はProtocolへ露出しない。
11. 通信切断などRuntimeが観測可能な条件から停止を要求する場合、RuntimeはApplicationの正規Cancel経路を利用する。
12. Applicationハングやcomplete忘れの監視・強制終了はRuntime責務に含めない。
13. RPC内部ではGoal TransactionとAction Packet Bindingを分離し、Action Packet Endpointは上位Transactionをslot、channel、connectionおよびpacketへ対応付ける。
14. APIによる論理判断のcommitと物理Transport送信は分離可能とし、`complete()`をGoal Responseの代用にはしない。
