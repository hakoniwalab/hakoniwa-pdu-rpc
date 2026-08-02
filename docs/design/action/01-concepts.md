# Hakoniwa Actionの基本概念

> **Status: Draft**  
> 本文書はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 1. 目的

本書では、Hakoniwa Action Protocolで使用する基本概念と用語を、特定の実装言語やROS 2 APIに依存せず定義します。

ここでは、状態機械、PDUレイアウト、公開API、エラー処理の詳細には踏み込みません。それらを議論する前提となる意味論を揃えることを目的とします。

## 2. Actionとは何か

Hakoniwa Actionは、ClientがServerへGoalを提示し、ServerがそのGoalを受理または拒否し、受理したGoalについて0回以上のFeedbackを返しながら、最終的にResultと終端状態を返す通信・実行ライフサイクルです。

```text
Goal
  -> Accept / Reject
  -> Feedback (0..N)
  -> Result + Terminal Status
```

ActionはRPC基盤上に実装できますが、単に応答時間の長いService RPCとはみなしません。

Service RPCは、原則として1回のRequestと1回のResponseで完結します。一方、Actionは`goal_id`で識別される実行セッションを持ち、そのセッション中にFeedback、Cancel、終端通知が発生します。

## 3. Action TypeとGoal Execution

### 3.1 Action Type

Action Typeは、実行可能な操作の型を表します。

Action Typeは、少なくとも以下のAction固有データ型を持ちます。

- Goal body
- Result body
- Feedback body

例えばFibonacci Actionでは、Goal bodyに計算する項数、Feedback bodyに途中の数列、Result bodyに最終的な数列を持ちます。

### 3.2 Goal Execution

Goal Executionは、Action Typeに対して行われる1回の具体的な実行です。

同じAction Typeを複数回実行する場合、それぞれは異なるGoal Executionです。各Goal Executionは`goal_id`によって識別されます。

本設計では、Action Typeと1回のGoal Executionを区別します。

```text
Action Type
  ExecuteMission

Goal Execution A
  goal_id = A

Goal Execution B
  goal_id = B
```

## 4. Goal

Goalは、ClientがServerへ提示する実行要求と、そのAction固有の入力データです。

Goalの提示は、実行開始そのものを保証しません。ServerはGoalを受理または拒否します。

したがって、以下を区別します。

- Goalを送信したこと
- GoalがServerへ到達したこと
- Goalが受理されたこと
- Goalの実行が開始されたこと

初期実装でこれらをどこまで個別イベントとして公開するかは、状態モデルおよびAPI設計で決定します。

## 5. goal_id

`goal_id`は、1回のGoal Executionを識別する128-bitの識別子です。

Goal、Goal Response、Feedback、Cancel、Resultは、同じ`goal_id`によって関連付けます。

```text
Goal(goal_id = X)
Feedback(goal_id = X)
Cancel(goal_id = X)
Result(goal_id = X)
```

### 5.1 提案する役割

- Action Type内でGoal Executionを一意に識別する。
- ClientとServer間の相関キーとして使用する。
- ROS 2 Goal UUIDなど、外部Action実装の識別子と対応付けられるようにする。
- Service RPCの`client_name + request_id`へ依存しないAction固有の識別子とする。

### 5.2 未確定事項

- `goal_id`をClientが生成するか、Runtimeが生成するか。
- 一意性の範囲をAction単位、Endpoint単位、システム全体のどこまで要求するか。
- 終了済み`goal_id`をどれだけ保持し、重複Goalを検出するか。

初稿では、Client側で生成され、少なくとも通信相手との関係において十分に一意であることを前提候補とします。

## 6. Goal AcceptanceとGoal Rejection

Serverは受信したGoalを受理または拒否します。

### Goal Acceptance

Goal Acceptanceは、ServerがそのGoal Executionを管理対象として引き受けたことを表します。

受理後は、ServerはFeedback、Cancel処理、Resultまたは異常終端について責任を持ちます。

### Goal Rejection

Goal Rejectionは、ServerがそのGoalを実行対象として受理しなかったことを表します。

拒否理由の例は以下です。

- Action固有入力が不正
- 実行に必要な資源が不足
- 同時実行上限を超過
- Serverが停止処理中
- ポリシー上実行不可

BUSYをGoal Rejectionの理由として扱うか、独立したProtocol Errorとして扱うかは後続設計で決定します。

## 7. Feedback

Feedbackは、受理済みかつ未終端のGoal Executionについて、ServerからClientへ送られるAction固有の途中情報です。

Feedbackは非終端です。Feedbackを受信しても、そのGoal Executionは継続します。

```text
RUNNING
  -> Feedback
RUNNING
```

Feedbackは0回でも複数回でも構いません。

汎用的な進捗率は共通ヘッダに持たせません。進捗率、途中経路、現在の処理対象などの意味はActionごとに異なるため、Action固有のFeedback bodyに定義します。

Feedbackの順序確認には`sequence_no`を使用する案を採用していますが、欠落、重複、逆転をどのように扱うかはProtocol設計で定義します。

## 8. Result

Resultは、Goal Executionが終了した際に返されるAction固有の最終出力データです。

本設計では、Result bodyとTerminal Statusを別概念として扱う案を採用します。

```text
Result
  Action固有の最終出力

Terminal Status
  Goal Executionがどのように終了したか
```

例えば、同じResult bodyの型を使用しながら、以下の終端状態があり得ます。

- SUCCEEDED
- CANCELED
- ABORTED
- ERROR

Result bodyがどの終端状態でも有効か、成功時のみ有効かはAction TypeまたはProtocol契約で明確にする必要があります。

## 9. Terminal Status

Terminal Statusは、Goal Executionがそれ以上状態遷移しない終端結果を表します。

初期候補は以下です。

- `SUCCEEDED`: Goalが正常に完了した。
- `CANCELED`: Cancel要求に基づいて実行が終了した。
- `ABORTED`: Goalは受理されたが、Serverまたは利用アプリケーションの判断で正常完了できなかった。
- `ERROR`: Protocol、Runtime、通信などの異常により正常なAction結果として完了できなかった。

`ABORTED`と`ERROR`の境界は未確定です。Action業務処理の失敗と通信基盤の失敗を分離できる定義が必要です。

## 10. Cancel

Cancelは、Clientが受理済みまたは処理中のGoal Executionを停止するよう要求する操作です。

Cancel要求を送信しただけでは、Goal Executionが`CANCELED`になったことを意味しません。

以下を区別します。

1. Cancel Request
2. Cancelの受理または拒否
3. 実際の停止処理
4. `CANCELED`としての終端

```text
Client                     Server
  | Cancel Request           |
  |------------------------->|
  | Cancel Response          |
  |<-------------------------|
  |                          | stopping...
  | Result + CANCELED        |
  |<-------------------------|
```

Cancel Responseと最終Resultの両方を必須とするか、Cancel Responseが何を保証するかはProtocol設計で決定します。

## 11. ClientとServer

### Client

ClientはGoalを提示し、Goal Response、Feedback、Resultを受信し、必要に応じてCancelを要求します。

ClientはAction固有のGoal bodyを生成しますが、Goalを受理するかどうかや業務処理を実行する責任は持ちません。

### Server

ServerはGoalを受信し、受理または拒否し、受理したGoal Executionのライフサイクルを管理します。

ただし、RPC Runtimeそのものが業務処理を実行するわけではありません。受理判断や実際の処理は、Server Runtimeを利用するアプリケーションと協調して行います。

## 12. Service RPCとの違い

| 観点 | Service RPC | Action |
|---|---|---|
| 相関 | request ID | goal ID |
| 基本形 | Request / Response | Goal / Feedback / Cancel / Result |
| 中間通知 | 原則なし | 0回以上のFeedback |
| 終了前操作 | Cancelは既存RPC契約に依存 | Goal Executionに対するCancel |
| ライフサイクル | 1往復を中心とする | Goal Executionセッションを持つ |
| 終端状態 | Responseの成否 | Succeeded / Canceled / Aborted / Error |
| ROS依存 | なし | なし |

Action対応のために既存Service RPCの意味やPDUレイアウトを変更しません。

## 13. 初稿で提案する概念上の結論

1. Actionは「長時間RPC」ではなく、RPC基盤上に構築されるGoal Executionセッションとする。
2. 1回のGoal Executionは128-bitの`goal_id`で識別する。
3. Goalの送信と受理を区別する。
4. Feedbackは非終端通知とする。
5. Result bodyとTerminal Statusを区別する。
6. Cancel Request、Cancel Response、Canceled終端を区別する。
7. ROS 2はHakoniwa Actionの利用先・Adapterであり、概念定義の所有者ではない。

## 14. レビューで問答したい事項

1. Actionを独立したGoal Executionセッションとして捉える定義でよいか。
2. GoalとGoal Requestという言葉を分ける必要があるか。
3. Result bodyとTerminal Statusを常に分離すべきか。
4. Goal Rejectionは終端状態に含めるか、それともGoal Execution成立前として扱うか。
5. Cancel Responseは「要求を受理した」ことだけを示すのか、「停止可能と判断した」ことまで示すのか。
6. `ABORTED`と`ERROR`をどの責務境界で分けるか。
7. `goal_id`の生成責任をClient、Runtime、Adapterのどこへ置くか。
