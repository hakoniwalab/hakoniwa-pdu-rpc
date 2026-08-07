# Hakoniwa Actionのデータモデル

> **Status: Implemented contract**  
> 本文書は、Hakoniwa ActionのGoal identityとRuntimeデータモデルの現行仕様です。

## 1. 目的

本書では、Hakoniwa Action Protocolで扱う主要な抽象概念と、それらの識別・相関関係を定義します。

特に、以下を明確にします。

- 1回のAction要求と実行をどのように識別するか
- 同一Action Typeに対する複数のGoalをどう表現するか
- Goal、Feedback、Cancel、Resultをどう相関するか
- Protocol RuntimeとAction Server Applicationの責務をどう分離するか
- Goal RequestをProtocol側で拒否する条件と、Applicationへ判断を委ねる条件
- 通信経路や接続方式に依存しないデータモデル

PDUのバイト配置、状態遷移、送受信順序、公開API、Transport実装は、本ディレクトリの各契約文書で規定します。

## 2. 中心となる概念

Hakoniwa Action Protocolの中心となる概念は、Action Type、Goal、Goal Execution、`goal_id`です。

### 2.1 Action Type

Action Typeは、実行可能な処理の型です。

少なくとも以下のAction固有データ型を持ちます。

- Goal body
- Feedback body
- Result body

Action Type自体は、個別の実行状態を持ちません。

### 2.2 Goal

Goalは、ClientがAction Serverへ提示する1回の実行要求です。

Goal Requestを送信した時点では、そのGoalが受理されることは保証されません。

### 2.3 Goal Execution

Goal Executionは、受理されたGoalに対する1回の具体的な実行です。

同一Action Typeに対して複数のGoal Executionが存在できます。

```text
Action Type: MoveRobot

Goal Execution A
  goal_id = UUID-A

Goal Execution B
  goal_id = UUID-B
```

同じAction Typeに対して、`goal_id`が異なれば別のGoalです。

### 2.4 Goal lifecycle

`goal_id`は、Goal RequestからGoal Responseまでを相関し、Goalが受理された場合はFeedback、Cancel、Resultまで続くライフサイクル全体を相関します。

```text
Goal Request(goal_id = X)
  -> Goal Response(goal_id = X)

acceptedの場合:
  -> Feedback(goal_id = X) 0..N
  -> Cancel(goal_id = X) optional
  -> Result(goal_id = X)
```

本設計では、これとは別に「Client Session」というProtocol概念を導入しません。

TCP接続、共有メモリ接続、Endpoint、channel、mux上のClient識別子などはTransportまたはRuntime実装上の情報であり、GoalのProtocol identityではありません。

## 3. goal_id

### 3.1 定義

`goal_id`は、1回のGoalを識別する128-bit UUIDです。

Goal Request、Goal Response、Feedback、Cancel、Resultは、同じ`goal_id`によって相関します。

```text
Goal Request(goal_id = X)
Goal Response(goal_id = X)
Feedback(goal_id = X)
Cancel Request(goal_id = X)
Cancel Response(goal_id = X)
Result(goal_id = X)
```

`goal_id`は単なるRequest番号ではなく、Goalのライフサイクル全体を束ねる相関キーです。

### 3.2 生成責任

通常のHakoniwa Action Clientも上位ApplicationがGoal送信前に`goal_id`を生成します。RuntimeはGoal IDを自動生成しません。

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

### 3.3 Server Runtimeの責務

Action Server Runtimeは、受信した`goal_id`について以下を担当します。

- UUID形式の検査
- 不正値の拒否
- 実行中Goalとの重複検査
- 必要に応じた終了済みGoalとの重複検査
- Goal Contextへの登録
- Goal Response、Feedback、Cancel、Resultの相関
- Goal終了後の破棄または一定期間の保持

```text
生成責任
  Client Runtime / Adapter

検査・登録・相関・管理責任
  Server Runtime
```

### 3.4 Server内部IDとの分離

Server実装が内部handle、pointer、配列index、database keyなどを必要とする場合、それらはProtocol上の`goal_id`と分離します。

```text
Protocol identity
  goal_id = UUID

Server internal identity
  implementation-specific handle
```

## 4. goal_idの一意性

Protocol上は、異なるGoalが同じ`goal_id`を共有しないことを要求します。

上位Client ApplicationまたはAdapterは、実運用上十分な一意性を持つUUIDを生成します。

Server Runtimeは、自身が管理する範囲で重複を検出します。

最低限、以下の重複を検出できる必要があります。

- 現在処理中または実行中のGoalとの重複

同じ`goal_id`を持つGoal Requestは、新しいGoalとして扱いません。

同一`goal_id`のactive Goal Requestはduplicateとして自動拒否します。all-zeroは禁止し、UUID versionは検査しません。terminal完了後はContextを解放するため、終了済みIDの履歴やServer再起動をまたぐ永続的一意性はProtocol要件に含めません。

## 5. 複数Goal

### 5.1 Protocol上の原則

同一Action Typeに対して、異なる`goal_id`を持つ複数のGoalが同時に存在できる設計とします。

```text
MoveRobot
  Goal A: goal_id = UUID-A
  Goal B: goal_id = UUID-B
  Goal C: goal_id = UUID-C
```

受理された各Goal Executionは、Feedback、Cancel、Result、Terminal Statusを独立して持ちます。

あるGoalの状態変化や終了が、別の`goal_id`を持つGoalへ暗黙に影響してはなりません。

### 5.2 実現方式からの独立

複数Goalをどの通信経路で運ぶかは、Protocol上の同一性とは関係しません。

以下はいずれも同じProtocolモデルで扱えます。

```text
1つの通信経路で複数Goalを配送する
複数の通信経路からGoalを配送する
TransportがGoalごとに異なる経路を選択する
```

Protocolが要求するのは、各メッセージを`goal_id`で正しいGoalへ相関できることです。

Endpoint、socket、connection、channel、mux client IDなどをGoalの識別条件には含めません。

## 6. Goal Requestの判定フロー

Goal Requestを受信したRuntimeは、まずProtocolとして処理可能かを検査します。

```text
Client
  |
  | Goal Request(goal_id, goal_body)
  v
Server Runtime
  |
  +-- Protocol上処理不能
  |      -> RuntimeがGoal Response(REJECTED)を返す
  |
  +-- Protocol上有効かつ新しいgoal_id
         -> ApplicationへGoal Requestを通知
                 |
                 +-- accept
                 |     -> RuntimeがGoal Response(ACCEPTED)を返す
                 |
                 +-- reject(reason)
                       -> RuntimeがGoal Response(REJECTED)を返す
```

この構造により、Protocol RuntimeとApplicationは同じGoal Response経路を使用しながら、拒否判断の責務を分離できます。

## 7. Protocol Runtimeによる自動拒否

RuntimeがApplicationへ通知する前に拒否するのは、Goal Requestを正常な判断対象として成立させられない場合です。

Protocol上の自動拒否条件には、少なくとも以下を含めます。

- `goal_id`の形式が不正
- `goal_id`が禁止値
- 同じ`goal_id`が既に処理中、実行中、または保持中
- Protocol versionまたはmessage kindが不正
- 必須データが欠落している
- PDUが破損している、またはdecodeできない
- Runtimeがshutdown中など、ApplicationへGoal Requestを引き渡せない
- RuntimeがGoal判断用Contextを確保できない内部障害

これらはApplicationの業務判断ではありません。

特に、同一`goal_id`の重複はRuntimeが検出し、Applicationへ新しいGoal Requestとして通知しません。

## 8. Applicationによる受理・拒否

Protocol上有効で、かつ新しい`goal_id`を持つGoal Requestは、Applicationへ通知します。

Applicationは、Goal bodyおよび現在の実行状況に基づいて受理または拒否します。

Applicationによる判断例は以下です。

- Goal bodyが業務上妥当か
- 対象ロボットやデバイスが実行可能状態か
- 同時に何件のGoalを受理するか
- 並列実行するか
- 直列化またはキューへ入れるか
- 必要な資源を確保できるか
- 優先度やpreemptionをどう扱うか
- 安全条件や権限条件を満たしているか

Runtimeは、異なる`goal_id`を持つGoalを一律にsingle-goal制約で拒否しません。

同時実行数、排他、BUSY、キュー容量などはApplication Policyです。

## 9. RuntimeとApplicationの抽象境界

公開APIは次の双方向経路を提供します。

```text
Runtime -> Application
  Goal Request received(goal_id, goal_body)

Application -> Runtime
  accept(goal_id)
  reject(goal_id, reason)
```

Applicationが`accept`または`reject`を返した後、Runtimeは同じ`goal_id`を持つGoal ResponseをClientへ返します。

ApplicationがGoalを受理した場合、以後のFeedback、Cancel、Resultも同じ`goal_id`で処理します。

## 10. Rejectionの識別

Clientから見て、Protocol Runtimeによる拒否とApplicationによる拒否は、どちらもGoal Responseの`REJECTED`です。

Server Runtime内部では次の二種類を区別します。

```text
Protocol / Runtime rejection
  Goal Requestを正常なApplication判断へ渡せなかった

Application rejection
  Goal Requestは正常にApplicationへ届いたが、Applicationが受理しなかった
```

Wire上のGoal Responseはいずれも`REJECTED`であり、`reject_origin`や共通reason codeは持ちません。Runtimeによる自動拒否は診断ログへ理由を残し、Application拒否はApplicationが明示的に`reject_goal()`を呼び出した結果として区別します。

## 11. Runtimeの責務

RPC Runtimeは、複数Goalを扱える共通Protocol能力を提供します。

- `goal_id`ごとのGoal Context管理
- Goal RequestのProtocol検査
- duplicate `goal_id`の検出
- 有効なGoal RequestのApplicationへの通知
- Applicationのaccept/reject応答のClientへの配送
- Goal Response、Feedback、Cancel、Resultの相関
- 異なるGoal間で状態やデータを混同しないこと
- Transport固有の識別子をProtocol identityとして要求しないこと

概念上、Runtimeは以下のような集合を管理します。

```text
pending_goals: Map<goal_id, PendingGoalContext>
active_goals:  Map<goal_id, GoalExecutionContext>
```

これは実装データ構造を指定するものではありません。

Applicationの判断待ちと、受理後の実行中Goalを`goal_id`単位で相関できることを示しています。

## 12. Transportとの境界

Transportは、Goal Request、Goal Response、Feedback、Cancel、Resultを配送します。

Protocolは、Transportに以下を要求しません。

- 1 Goalにつき1接続
- 1 Clientにつき1接続
- 1 Action Typeにつき1 Endpoint
- 特定のmultiplex方式
- 特定の接続上限

Transportが使用するEndpoint、connection、channel、client IDなどは、配送を実現するための情報です。

それらをGoalのProtocol identityへ含めません。

## 13. データモデルの設計判断

1. `goal_id`は128-bit UUIDとする。
2. 通常のClientも上位ApplicationがGoal送信前に生成し、RuntimeはGoal IDを自動生成しない。
3. Adapterは外部で生成された互換UUIDを指定できる。
4. `goal_id`はGoal RequestからGoal Responseまで、受理後は終端Resultまで続くライフサイクル全体の相関キーとする。
5. Server RuntimeはUUIDの検査、重複検査、登録、相関、ライフサイクル管理を行う。
6. 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoalを同時に扱える。
7. 同じ`goal_id`を持つGoal Requestは新しいGoalとして扱わない。
8. Protocol上有効で新しい`goal_id`を持つGoal RequestはApplicationへ通知する。
9. RuntimeはProtocol上処理不能なGoal Requestだけを自動拒否する。
10. Goalの業務上の受理、並列実行、直列化、キュー、排他、優先度、preemptionはApplication Policyとする。
11. Runtime拒否とApplication拒否を識別可能にする。
12. Protocol上の独立したClient Session概念は導入しない。
13. Endpoint、connection、channel、mux client IDなどの実現方式をGoalの識別条件に含めない。
