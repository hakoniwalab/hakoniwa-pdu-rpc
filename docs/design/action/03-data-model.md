# Hakoniwa Actionのデータモデル

> **Status: Draft**  
> 本文書はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 1. 目的

本書では、Hakoniwa Action Protocolで扱う主要なデータ概念と、それらの識別・相関関係を定義します。

特に、以下を明確にします。

- 1回のAction実行をどのように識別するか
- 同一Action Typeに対する複数のGoal Executionをどう表現するか
- Goal、Feedback、Cancel、Resultをどのように相関するか
- RPC RuntimeとAction Server Applicationの責務をどう分離するか
- Protocolのデータモデルを通信経路や接続方式からどう独立させるか

具体的なPDUのバイト配置、状態遷移、送受信順序、公開API、Transport実装は後続文書で定義します。

## 2. データモデルの基本単位

Hakoniwa Action Protocolの中心となる概念は、Action Type、Goal Execution、`goal_id`です。

### 2.1 Action Type

Action Typeは、実行可能な処理の型です。

少なくとも以下のAction固有データ型を持ちます。

- Goal body
- Feedback body
- Result body

Action Type自体は、個別の実行状態を持ちません。

### 2.2 Goal Execution

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

### 2.3 Goal Executionセッション

Goal Executionは、Goal Requestの送信から終端Resultまで継続する論理的な実行セッションです。

このセッションは`goal_id`によって識別・相関します。

```text
Goal Request(goal_id = X)
  -> Goal Response(goal_id = X)
  -> Feedback(goal_id = X) 0..N
  -> Cancel(goal_id = X) optional
  -> Result(goal_id = X)
```

本設計では、これとは別に「Client Session」というProtocol概念を導入しません。

TCP接続、共有メモリ接続、mux上のClient識別子などはTransportまたはRuntime実装上の情報であり、Goal ExecutionのProtocol identityではありません。

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

`goal_id`は単なるRequest番号ではなく、Goal Executionのライフサイクル全体を束ねる相関キーです。

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
- Goal Response、Feedback、Cancel、Resultの相関
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

各Goal Executionは、Goal Response、Feedback、Cancel、Result、Terminal Statusを独立して持ちます。

```text
Goal A
  Feedback A1
  Feedback A2
  Result A

Goal B
  Cancel B
  Result B / CANCELED
```

同じ`goal_id`を持つGoal Requestは、新しいGoal Executionとして扱いません。

その具体的な扱いを重複エラーとするか、再送または冪等な再照会とするかは後続のProtocol設計で決定します。

### 5.2 実現方式からの独立

複数Goal Executionをどの通信経路で運ぶかは、Protocol上の同一性とは関係しません。

以下はいずれも同じProtocolモデルで扱えます。

```text
1つの通信経路で複数Goalを配送する
複数の通信経路からGoalを配送する
TransportがGoalごとに異なる経路を選択する
```

Protocolが要求するのは、各メッセージを`goal_id`で正しいGoal Executionへ相関できることです。

Endpoint、socket、connection、channel、mux client IDなどをGoal Executionの識別条件には含めません。

## 6. Runtimeの責務

RPC Runtimeは、複数のGoal Executionを扱える共通Protocol能力を提供します。

- `goal_id`ごとのGoal Execution Context管理
- Goal Response、Feedback、Cancel、Resultの相関
- 異なるGoal間で状態やデータを混同しないこと
- あるGoalの終了が、別のGoalを暗黙に終了させないこと
- 複数Goalから発生するイベントを利用アプリケーションへ識別可能な形で通知すること
- Protocol不正、重複ID、未知IDなどの検出
- Transport固有の識別子をProtocol identityとして要求しないこと

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

Hakoniwa Action Protocolは、複数Goalを識別・相関・配送できる能力を提供しますが、上記の実行Policyを一律に決定しません。

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

## 9. Transportとの境界

Transportは、Goal Request、Goal Response、Feedback、Cancel、Resultを配送します。

Protocolは、Transportに以下を要求しません。

- 1 Goalにつき1接続
- 1 Clientにつき1接続
- 1 Action Typeにつき1 Endpoint
- 特定のmultiplex方式
- 特定の接続上限

Transportが使用するEndpoint、connection、channel、client IDなどは、配送を実現するための情報です。

それらをGoal ExecutionのProtocol identityへ含めません。

Transport上の接続失敗は、Goal RequestがAction Runtimeへ到達した後のGoal Rejectionとは別の失敗です。

## 10. Goal Execution Context

Runtimeは各Goal Executionについて、概念上、以下の情報を関連付けます。

```text
GoalExecutionContext
  goal_id
  action_type
  goal body or its reference
  protocol state
  feedback sequence state
  terminal status
  result body or its reference
```

具体的にどのフィールドを保持するか、所有権、コピー方式、永続性は後続設計で決定します。

重要なのは、`goal_id`を主キーとしてGoal Executionのライフサイクル全体を独立管理することです。

Transport固有のrouteやconnection情報をRuntime内部で関連付けることはできますが、それは実装情報であり、Protocol上のGoal Execution identityではありません。

## 11. データ関係

概念上の中心関係は以下です。

```text
Action Type
  1
  |
  | instantiated as
  v
Goal Execution (0..N)
  identified by goal_id
```

Goal Executionの型はAction Typeによって決まり、Goal Executionそのものは`goal_id`で識別します。

Goal、Feedback、Cancel、Resultは、同じ`goal_id`を使用してそのGoal Executionへ関連付けます。

通信経路、Endpoint、接続、Client識別子は、この中心関係へ含めません。

## 12. 今回の設計判断

1. `goal_id`は128-bit UUIDとする。
2. 通常のClientではAction Client RuntimeがGoal送信前に生成する。
3. Adapterは外部で生成された互換UUIDを指定できる。
4. Server RuntimeはUUIDの検査、重複検査、登録、相関、ライフサイクル管理を行う。
5. `goal_id`はGoal Requestから終端Resultまで続くGoal Executionセッション全体の相関キーとする。
6. 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoal Executionが同時に存在できる。
7. 同じ`goal_id`を持つGoal Requestは新しいGoal Executionとして扱わない。
8. Protocol上の独立したClient Session概念は導入しない。
9. Endpoint、connection、channel、mux client IDなどの実現方式をGoal Executionの識別条件に含めない。
10. RPC Runtimeは複数Goalを`goal_id`ごとに独立管理できる能力を提供する。
11. Runtimeは一律のsingle-goal制約をProtocolへ埋め込まない。
12. Goalの受理、並列実行、直列化、キュー、排他、優先度、preemptionはApplication Policyとする。
13. Transport接続失敗、Runtime拒否、Application拒否を別の失敗として扱う。

## 13. レビューで問答したい事項

1. UUID versionをProtocolで固定する必要があるか。
2. `goal_id`の一意性をシステム全体で要求するか、それともServer管理範囲での重複防止を規範とするか。
3. 終了済み`goal_id`をどの期間保持する必要があるか。
4. 同じ`goal_id`を持つGoal Requestの再送を、重複エラーとするか冪等な再照会として扱うか。
5. Applicationがキューへ入れたGoalを、Protocol上`ACCEPTED`とだけ表現するか、`QUEUED`状態を設けるか。
6. Runtime内部資源不足とApplication同時実行上限を、Goal Response上で別理由として表現するか。
7. TransportがGoalごとに経路を変更した場合でも、`goal_id`だけで相関できることをProtocol要件とするか。
