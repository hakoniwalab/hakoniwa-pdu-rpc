# Hakoniwa Actionの責務境界

> **Status: Draft**  
> 本文書はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 1. 目的

本書では、Hakoniwa Action Protocolに関係する各リポジトリおよび利用アプリケーションの責務境界を定義します。

Action対応では、以下の異なる関心事が同時に現れます。

- Action固有データ型とPDUレイアウト
- Goal Executionの状態と通信ライフサイクル
- ROS 2 Action APIとの変換
- Goalを実際に処理する業務ロジック
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
- `goal_id`、`sequence_no`などのフィールドレイアウト
- 通常のPDU生成パイプラインによる各言語型とconverterの生成
- offset、aligned size、CDR size、type metadataの生成
- 生成物の決定性と既存Service生成物との互換性

### 3.2 所有しない責務

- Goalを受理または拒否する判断
- ClientおよびServerの状態機械
- Feedbackの送信タイミング
- CancelとResultの競合解決
- Action APIの実行時挙動
- ROS 2 Goal UUIDとの対応管理
- Action固有の業務処理

Registryはデータを表現できるようにしますが、そのデータをいつ送るか、受信後にどう状態遷移するかは決定しません。

## 4. hakoniwa-pdu-rpc

### 4.1 所有する責務

`hakoniwa-pdu-rpc`は、Hakoniwa Actionの通信ライフサイクルと実行時Protocolを所有します。

- Goal送信、Goal Response受信、および相関
- `goal_id`によるGoal Execution管理
- Feedbackの非終端配送
- Cancel RequestおよびCancel Responseの配送
- ResultおよびTerminal Statusの配送
- ClientおよびServerの状態機械
- 許可された状態遷移と不正操作の検出
- 重複、遅延、未知の`goal_id`の扱い
- CancelとResultなどの競合規約
- 同時Goal数、BUSY、キューなどのRuntime Policy
- timeout、shutdown、transport errorをProtocol/APIへ反映する方法
- C++ APIとPython Bindingが共有すべき意味論
- 既存Service RPCとの後方互換性

### 4.2 所有しない責務

- `.action`ファイルの解析とPDUコード生成
- ROS 2 Node、Action Server、GoalHandleの実装
- ROS 2固有ステータスへの変換
- Goalの業務上の妥当性判断
- ロボット制御、経路計画、計算処理などAction固有の実処理
- Transport固有のSocket、共有メモリ、再接続処理の詳細

RPC Runtimeは通信とライフサイクルを管理しますが、Goalを実際に達成する処理は行いません。

## 5. hakoniwa-pdu-ros

### 5.1 所有する責務

`hakoniwa-pdu-ros`は、ROS 2 Action APIとHakoniwa Action APIのAdapterを所有します。

- ROS 2 Action Serverの生成と実行
- ROS 2 Goal messageから生成済みHakoniwa Goal PDU bodyへの変換
- Hakoniwa Feedback PDU bodyからROS 2 Feedback messageへの変換
- Hakoniwa Result PDU bodyからROS 2 Result messageへの変換
- ROS 2 Goal UUIDとHakoniwa `goal_id`の対応付け
- ROS 2 cancel callbackとHakoniwa Cancel APIの接続
- ROS 2 GoalHandleの状態とHakoniwa terminal statusの対応
- ROS executorおよび`rclpy` Futureへの非同期完了通知
- ROS Binding設定の読み込みとHakoniwa Action Runtime設定への接続
- ROS固有の変換失敗やNode lifecycleの診断

### 5.2 所有しない責務

- Hakoniwa Action Protocolそのものの状態機械
- FeedbackキューやCancel/Result競合の独自実装
- Action共通PDUレイアウトの定義
- Action固有の業務処理
- ROS 2以外の利用者に対するProtocol仕様

ROS BridgeがAction状態機械を再実装すると、C++、Python、ROSの各利用者で意味論が分岐します。そのため、状態と競合規約は可能な限り`hakoniwa-pdu-rpc`へ集約します。

## 6. Action Server Application

### 6.1 所有する責務

Action Server Applicationは、Goalを実際に処理するドメインロジックを所有します。

- Goal bodyの業務上の検証
- Goalを受理または拒否するアプリケーション判断
- 受理したGoalの実処理
- Action固有Feedbackの生成
- 成功時のResult body生成
- 業務上の中断、失敗、abort判断
- Cancel要求を受けた後の安全な停止処理
- 資源競合や優先度に関するアプリケーションPolicy

### 6.2 Runtimeとの境界

RuntimeとApplicationの境界では、少なくとも以下の操作が必要になる想定です。

```text
Runtime -> Application
  on_goal(goal_id, goal_body)
  on_cancel(goal_id)

Application -> Runtime
  accept(goal_id)
  reject(goal_id, reason)
  publish_feedback(goal_id, feedback_body)
  succeed(goal_id, result_body)
  abort(goal_id, result_body or error)
  canceled(goal_id, result_body)
```

このAPI形状は未確定です。ここでは責務の方向だけを示します。

### 6.3 所有しない責務

- PDUヘッダの直接構築
- `sequence_no`の採番方法
- transport送受信
- Clientとの相関管理
- Protocol上の不正状態検出
- ROS 2 APIへの直接変換

アプリケーションはAction固有のbodyと意味を扱い、共通ヘッダや通信Protocolの詳細から分離されることが望まれます。

## 7. hakoniwa-pdu-endpointとTransport

EndpointおよびTransportは、PDUを通信相手へ配送する責務を持ちます。

- Request、Response、Feedback経路の接続
- TCP、UDP、共有メモリなどのtransport実装
- PDUサイズとバッファ管理
- 送受信、切断、再接続、shutdownの低レベルイベント
- RPC Runtimeが利用できるpollまたはwait primitive

Endpoint/TransportはActionのGoal、Feedback、Resultの意味や状態遷移を理解しません。

`requestChannelId`や`feedbackChannelId`などの物理配置情報をRPC設定とEndpoint設定のどちらが所有するかは、既存の設定分離Issueと整合させて後続設計で決定します。

## 8. 責務分担表

| 項目 | Registry | RPC | ROS Bridge | Application | Endpoint/Transport |
|---|---:|---:|---:|---:|---:|
| `.action`解析 | 主 |  |  |  |  |
| PDU型・レイアウト | 主 | 利用 | 利用 | 利用 | 配送 |
| `goal_id`相関 | データ定義 | 主 | ROS UUID対応 | 利用 | 透過 |
| Goal受理Protocol |  | 主 | Adapter | 判断 | 透過 |
| 業務上の受理判断 |  | 支援 | 透過 | 主 |  |
| Feedback配送 | データ定義 | 主 | ROS変換 | 生成 | 配送 |
| Cancel状態遷移 |  | 主 | ROS変換 | 停止処理 | 配送 |
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

## 10. 境界上の未確定事項

### 10.1 Goal受理判断の分割

Runtimeが自動的に拒否すべき条件と、Applicationへ判断を委ねる条件を分ける必要があります。

Runtime側候補:

- duplicate `goal_id`
- protocol version不正
- 同時実行上限超過
- shutdown中

Application側候補:

- Goal bodyの業務上の不正
- ロボット状態が実行条件を満たさない
- ドメイン上の優先度や排他

### 10.2 goal_id生成責任

候補は以下です。

- Action Client Application
- RPC Client Runtime
- ROS BridgeなどのAdapter

ROS Goal UUIDをそのまま使用する場合と、ROSを使わないClientの場合を共通に説明できる規約が必要です。

### 10.3 Timeoutの所有者

以下を分離する必要があります。

- transport timeout
- Goal acceptance timeout
- Action execution deadline
- Bridge側のROS request deadline
- Application固有の処理期限

すべてを一つのtimeoutへ統合すると、どの層がGoalを終了させたのか不明確になります。

### 10.4 Feedback保持方針

FeedbackをRuntimeがキューイングするか、Endpointイベントをそのまま通知するか、最新値だけを保持するかは、RPCと利用APIの境界に関わります。

## 11. 初稿で提案する責務上の結論

1. Registryはデータ契約を所有し、状態機械を所有しない。
2. RPCはHakoniwa ActionのProtocolとGoal Execution lifecycleを所有する。
3. ROS BridgeはROS 2との変換を所有し、Action状態機械を独自に実装しない。
4. ApplicationはGoalの業務処理、Feedback生成、Result生成、安全なCancel処理を所有する。
5. Endpoint/Transportは配送を所有し、Action意味論を所有しない。
6. Goalの受理判断は、Protocol上の自動拒否とApplication判断に分割する。

## 12. レビューで問答したい事項

1. Goalのaccept/reject判断をRuntimeとApplicationでどこまで分けるか。
2. `goal_id`生成をRPC Client Runtimeの責務とするべきか。
3. Feedbackの`sequence_no`採番はRuntimeが所有すべきか。
4. Result bodyの有効性をProtocolで規定するか、Action Typeの契約へ委ねるか。
5. `ABORTED`をApplication判断、`ERROR`をRuntime判断として分けられるか。
6. Action execution deadlineをRPC共通機能に含めるか。
7. ROS Bridgeに残さざるを得ない状態管理はどこまでか。
