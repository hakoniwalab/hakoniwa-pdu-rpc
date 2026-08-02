# Hakoniwa Actionのデータモデル

> **Status: Draft**  
> 本文書はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 1. 目的

本書では、Hakoniwa Action Protocolで扱う主要なデータ概念と、それらの識別・相関関係を定義します。

特に、以下を明確にします。

- 1回のAction実行をどのように識別するか
- 同一Action Typeに対する複数のGoal Executionをどう表現するか
- 同一Client接続上で複数Goalをどう独立管理するか
- RPC Runtime、Action Server Application、Transportの責務をどう分離するか

具体的なPDUのバイト配置、状態遷移、送受信順序、公開APIは後続文書で定義します。

## 2. データモデルの基本単位

Hakoniwa Actionでは、以下を別の概念として扱います。

### 2.1 Action Type

Action Typeは、実行可能な処理の型です。

少なくとも以下のAction固有データ型を持ちます。

- Goal body
- Feedback body
- Result body

Action Type自体は、個別の実行状態を持ちません。

### 2.2 Action Endpoint

Action Endpointは、Action TypeをClientへ公開する論理的な通信先です。

同じAction Typeを複数のEndpointで公開できます。

```text
robot1/move : MoveRobot
robot2/move : MoveRobot
```

EndpointはAction Typeを公開する場所であり、単一のGoal Executionを表すものではありません。

### 2.3 Client Session

Client Sessionは、ClientとServer間のTransportまたはRPC上の接続・通信コンテキストです。

Client SessionはGoal Executionの識別子ではありません。

1つのClient Session上に、複数のGoal Executionが存在できます。

### 2.4 Goal Execution

Goal Executionは、Action Typeに対して行われる1回の具体的な実行です。

各Goal Executionは、一意な`goal_id`によって識別します。

```text
Action Type: MoveRobot

Goal Execution A
  goal_id = UUID-A

Goal Execution B
  goal_id = UUID-B
```

同じAction Typeに対して、`goal_id`が異なれば別のGoal Executionです。

Action EndpointとClient Sessionは、Goal Executionに関連する配送・通信コンテキストですが、Goal Executionの同一性を決める条件には含めません。

## 3. goal_id

### 3.1 定義

`goal_id`は、1回のGoal Executionを識別する128-bit UUIDです。

Goal Request、Goal Response、Feedback、Cancel、Resultは、同じ`goal_id`によって相関します。

```text
Goal Request(goal_id = X)
Goal Response(goal_id = X)
Feedback(goal_id = X)
Cancel Request(goal_id = X)
Cancel Response(goal_id = X)
Result(goal_id = X)
```

### 3.2 生成責任

通常のHakoniwa Action Clientでは、Action Client RuntimeがGoal送信前に`goal_id`を生成します。

```text
Client Application
  send_goal(goal_body)
        |
        v
Action Client Runtime
  generate UUID
  send Goal Request(goal_id, goal_body)
```

利用アプリケーションがUUID生成処理を直接実装することは必須としません。

ROS Bridgeなど、外部ActionシステムのAdapterは、外部で既に生成された互換UUIDを`goal_id`として指定できます。

```text
ROS Goal UUID
      |
      v
Hakoniwa goal_id
```

### 3.3 Server Runtimeの責務

Action Server Runtimeは、受信した`goal_id`について以下を担当します。

- UUID形式の検査
- 不正値の拒否
- 実行中Goalとの重複検査
- 必要に応じた終了済みGoalとの重複検査
- Goal Execution Contextへの登録
- Feedback、Cancel、Resultの相関
- Goal終了後の破棄または一定期間の保持

`goal_id`の値をClient側で生成することと、Server側で一意性・ライフサイクルを管理することは矛盾しません。

```text
生成責任
  Client Runtime / Adapter

検査・登録・管理責任
  Server Runtime
```

### 3.4 Server内部IDとの分離

Server実装が、配列index、pointer、handle、database keyなどの内部識別子を必要とする場合、それらはProtocol上の`goal_id`と分離します。

```text
Protocol identity
  goal_id = UUID

Server internal identity
  implementation-specific handle
```

ClientへServer内部IDを公開することを必須としません。

## 4. goal_idの一意性スコープ

Protocol上は、異なるGoal Executionが同じ`goal_id`を共有しないことを要求します。

初稿では、Client RuntimeまたはAdapterが、実運用上十分なグローバル一意性を持つUUIDを生成することを前提とします。

Server Runtimeは、自身が管理する範囲で重複を検出します。

最低限、以下の重複を検出できる必要があります。

- 現在実行中のGoal Executionとの重複
- 同じGoal Requestの再送として判定すべき重複

以下は未確定です。

- UUID versionを固定するか
- zero UUIDを禁止するか
- Serverが終了済み`goal_id`を保持する期間
- Server再起動をまたいだ重複検出を要求するか
- システム全体での永続的一意性をProtocol要件とするか

## 5. 複数Goal Execution

### 5.1 Protocol上の原則

同一Action Typeに対して、異なる`goal_id`を持つ複数のGoal Executionが同時に存在できる設計とします。

```text
MoveRobot
  Goal A: goal_id = UUID-A
  Goal B: goal_id = UUID-B
  Goal C: goal_id = UUID-C
```

各Goalは、Feedback、Cancel、Result、Terminal Statusを独立して持ちます。

同じ`goal_id`を持つGoal Requestは、新しいGoal Executionとして扱いません。その具体的な扱いを重複エラーとするか、再送または冪等な再照会とするかは後続のProtocol設計で決定します。

```text
Goal A
  Feedback A1
  Feedback A2
  Result A

Goal B
  Cancel B
  Result B / CANCELED
```

### 5.2 同一Clientからの複数Goal

同一Client Sessionから送信された複数Goalも、異なる`goal_id`を持つ独立したGoal Executionとして表現できます。

```text
Client Session A
  Goal A1: UUID-A1
  Goal A2: UUID-A2
  Goal A3: UUID-A3
```

Runtimeが一律に以下の制限を設ける設計にはしません。

- 1 Action Typeにつき1 active Goal
- 1 Action Endpointにつき1 active Goal
- 1 Client Sessionにつき1 active Goal

この制限をProtocolへ埋め込むと、将来の並列実行、多重化、キューイング、複数ロボット操作などの利用形態を妨げるためです。

### 5.3 複数Clientからの複数Goal

異なるClient Sessionから、同じAction Endpointへ複数Goalを送信できます。

```text
Client A -> Goal A
Client B -> Goal B
Client C -> Goal C
```

これらも`goal_id`ごとに独立して管理します。

## 6. Runtimeの責務

RPC Runtimeは、複数のGoal Executionを扱える共通Protocol能力を提供します。

- `goal_id`ごとのGoal Execution Context管理
- Goal Response、Feedback、Cancel、Resultの相関
- 異なるGoal間で状態やデータを混同しないこと
- あるGoalの終了が、別のGoalを暗黙に終了させないこと
- 複数Goalから発生するイベントを利用アプリケーションへ識別可能な形で通知すること
- Protocol不正、重複ID、未知IDなどの検出

概念上、Runtimeは以下のような集合を管理します。

```text
active_goals: Map<goal_id, GoalExecutionContext>
```

これは実装データ構造を指定するものではなく、複数Goalを`goal_id`単位で独立管理する必要性を示しています。

## 7. Applicationの責務

同時に何件のGoalを受理し、どのように処理するかは、Action Server ApplicationのPolicyです。

Applicationは、例えば以下を選択できます。

### 7.1 並列実行

```text
Goal A -> ACCEPT -> EXECUTING
Goal B -> ACCEPT -> EXECUTING
```

### 7.2 直列実行・キュー

```text
Goal A -> ACCEPT -> EXECUTING
Goal B -> ACCEPT -> QUEUED
```

### 7.3 排他による拒否

```text
Goal A -> ACCEPT
Goal B -> REJECT(resource busy)
```

### 7.4 優先度またはpreemption

```text
Goal A -> EXECUTING
Goal B -> higher priority
Application decides whether to preempt A
```

Hakoniwa Action Protocolは、複数Goalを識別し配送できる能力を提供しますが、上記の実行Policyを一律に決定しません。

Application Policyに含まれるものの例は以下です。

- 最大同時実行数
- Goalの業務上の受理条件
- 資源排他
- キュー方式とキュー容量
- 優先順位
- preemption
- 同じ対象物に対する競合解決
- ロボットやデバイスの安全条件

## 8. Runtime拒否とApplication拒否

Goal Rejectionには、少なくとも二つの異なる原因があります。

### 8.1 RuntimeまたはProtocolによる拒否

RuntimeがApplicationへ渡す前に拒否できる候補です。

- 不正なUUID
- duplicate `goal_id`
- Protocol version不整合
- 破損したPDU
- Server shutdown中
- Runtime自身がGoal Contextを生成できない内部資源不足

### 8.2 Application Policyによる拒否

Applicationが業務上の判断として拒否する候補です。

- Goal bodyが業務上不正
- 対象ロボットが実行可能状態ではない
- 必要な資源を別Goalが使用中
- Applicationの同時実行上限に到達
- キューが満杯
- 権限または運用Policyにより実行不可

Runtime capacity failureとApplication Policy rejectionは、同じ`BUSY`へ安易に統合せず、後続のデータ契約で識別可能にすることを検討します。

## 9. Transportとの分離

### 9.1 maxClients

`tcp_mux`の`maxClients`は、Transportが同時に収容できるClient接続数の上限です。

```text
maxClients
  = transport client connection capacity
```

これは以下を意味しません。

```text
maxClients
  != maximum active goals
  != application concurrency limit
```

1つのClient Session上に複数Goalを多重化できるため、active Goal数が`maxClients`を上回ることもProtocol上はあり得ます。

```text
maxClients = 2

Client A
  Goal A1
  Goal A2
  Goal A3

Client B
  Goal B1
  Goal B2

active goals = 5
```

### 9.2 Transport capacity failure

Transportの接続上限によってClientが接続できない場合、そのClientはGoal Requestを送信する段階へ到達していません。

したがって、これはGoal Rejectionとは別の失敗です。

```text
Transport connection rejected
  -> no Goal Execution created

Goal rejected by Application
  -> Goal Request reached Action Server
```

## 10. Goal Execution Context

Runtimeは各Goal Executionについて、概念上、以下の情報を関連付けます。

```text
GoalExecutionContext
  goal_id
  action_endpoint
  action_type
  client/session reference
  goal body or its reference
  protocol state
  feedback sequence state
  terminal status
  result body or its reference
```

具体的にどのフィールドを保持するか、所有権、コピー方式、永続性は後続設計で決定します。

重要なのは、状態管理の主キーをClient Sessionだけにせず、`goal_id`を中心に各Goalを独立管理することです。

## 11. データ関係

概念上の関係は以下です。

```text
Action Type
  1
  |
  | instantiated as
  v
Goal Execution (0..N)
  identified by goal_id

Action Endpoint
  1
  |
  | routes
  v
Goal Execution (0..N)

Client Session
  1
  |
  | submits
  v
Goal Execution (0..N)
```

Goal Executionの型はAction Typeによって決まり、第一識別子は`goal_id`です。

Action EndpointとClient SessionはGoal Executionに関連付けられますが、配送・通信コンテキストであり、Goal Executionの同一性を決めません。

## 12. 今回の設計判断

1. `goal_id`は128-bit UUIDとする。
2. 通常のClientではAction Client RuntimeがGoal送信前に生成する。
3. Adapterは外部で生成された互換UUIDを指定できる。
4. Server RuntimeはUUIDの検査、重複検査、登録、相関、ライフサイクル管理を行う。
5. 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoal Executionが同時に存在できる。
6. Action EndpointおよびClient Sessionは配送・通信コンテキストであり、Goal Executionの識別条件には含めない。
7. RPC Runtimeは複数Goalを`goal_id`ごとに独立管理できる能力を提供する。
8. Runtimeは一律のsingle-goal制約をProtocolへ埋め込まない。
9. Goalの受理、並列実行、直列化、キュー、排他、優先度、preemptionはApplication Policyとする。
10. `tcp_mux.maxClients`はTransport接続容量であり、active Goal数やApplication同時実行数とは別概念とする。
11. Transport接続失敗、Runtime拒否、Application拒否を別の失敗として扱う。

## 13. レビューで問答したい事項

1. UUID versionをProtocolで固定する必要があるか。
2. `goal_id`の一意性をシステム全体で要求するか、それともServer管理範囲での重複防止を規範とするか。
3. 終了済み`goal_id`をどの期間保持する必要があるか。
4. 同じ`goal_id`を持つGoal Requestの再送を、重複エラーとするか冪等な再照会として扱うか。
5. Applicationがキューへ入れたGoalを、Protocol上`ACCEPTED`とだけ表現するか、`QUEUED`状態を設けるか。
6. Runtime内部資源不足とApplication同時実行上限を、Goal Response上で別理由として表現するか。
7. Client Session切断時に、そのClientが送信したGoalを継続するかCancelするかを、Protocol、設定、Applicationのどこで決めるか。
8. 1つのClient Session上で複数Goalイベントを配送する際、順序保証をGoal単位に限定するか。
