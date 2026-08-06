# Hakoniwa Action Protocol 設計

> **Status: Draft**  
> 本文書群はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 目的

このディレクトリでは、`hakoniwa-pdu-rpc` におけるHakoniwa Action Protocolの設計仕様を管理します。

Action対応を実装から逆算して定義するのではなく、先に以下を明文化し、C++ Runtime、Python Binding、`hakoniwa-pdu-ros`、利用アプリケーションが共通に参照できる仕様とします。

- 基本概念と用語
- リポジトリおよびレイヤ間の責務境界
- PDUデータ契約
- Goal lifecycleの状態とイベント処理規則
- Goal、Feedback、Cancel、Resultの通信規約
- エラー、競合、遅延メッセージの扱い
- 公開API、設定、ファイル配置、ビルド契約

## 基本方針

- ActionはROS 2固有機能ではなく、箱庭で利用できる独立した通信・実行ライフサイクルとして定義します。
- Actionは単に応答時間の長いService RPCではなく、`goal_id`で識別されるGoal lifecycleとして扱います。
- 1回のGoalは128-bit UUIDの`goal_id`で識別します。
- 初期APIでは上位Client Application／Adapterが非zeroの`goal_id`を生成し、Client／Server Runtimeが重複検査とlifecycle管理を行います。Runtimeによる自動生成helperはpendingです。
- 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoalを同時に扱えるProtocolとします。
- Goal Request、Goal Response、Feedback、Cancel、Resultは、通信経路や接続方法に依存せず`goal_id`で相関します。
- RPC Runtimeは、複数Goalを`goal_id`ごとに独立して相関・配送・状態管理できる能力を提供します。
- Protocol上有効で新しい`goal_id`を持つGoal RequestはApplicationへ通知します。
- Runtimeが自動拒否するのは、重複`goal_id`や不正messageなど、Applicationへ正常に引き渡せないProtocol上の理由に限定します。
- Goalを受理するか、並列実行するか、直列化するか、キューへ入れるか、拒否するかはAction Server Applicationの実行ポリシーとします。
- Runtimeは一律の「1 Action Typeにつき1 Goal」という制約をProtocolへ埋め込みません。
- Protocol Runtimeによる拒否とApplicationによる拒否を識別可能にします。
- Endpoint、接続、multiplex、transport capacityなどの実現方式はProtocolの識別モデルから分離します。
- Protocol上の独立したClient Session概念は導入しません。
- Feedbackは0回以上送信できる非終端通知とします。
- Resultのデータと、Succeeded、Canceled、Abortedなどのterminal statusを区別します。
- Cancel要求、Cancel受理、停止完了、Canceled Resultを同一概念として扱いません。
- 既存Service RPCのPDUレイアウト、設定、状態遷移、API挙動を変更しません。
- Action対応は追加機能かつ明示的なopt-inとします。

## 文書構成

### 現在のレビュー対象

1. [基本概念](01-concepts.md)
2. [責務境界](02-responsibility-boundaries.md)
3. [データモデル](03-data-model.md)
4. [状態モデル](04-state-model.md)

### 後続で追加する文書

```text
05-protocol.md
06-error-and-race-semantics.md
07-api-design.md
08-configuration.md
09-file-layout.md
10-build-contract.md
11-ros2-mapping.md
12-open-questions.md
```

推奨する読み順は、概念、責務境界、データモデル、状態モデル、通信プロトコル、競合規約、実装契約の順です。

## 現時点の設計判断

以下は、これまでのレビューで合意した方向性です。後続のProtocol、競合規約、API設計では、この前提を狭めない形で具体化します。

- 1回のGoalを128-bit UUIDの`goal_id`で識別する。
- 初期APIでは通常のHakoniwa Clientも上位Applicationが`goal_id`を指定する。Runtime自動生成は後続helperとしてpendingとする。
- ROS BridgeなどのAdapterは、外部で生成された互換UUIDを指定できる。
- Server Runtimeは`goal_id`の形式検査、重複検査、登録、相関、状態管理を担当する。
- `goal_id`は、Goal RequestからGoal Responseまで、受理後は終端Resultまで続くGoal lifecycleの相関キーである。
- 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoalをProtocol上許容する。
- 同じ`goal_id`を持つGoal Requestは、新しいGoalとして扱わない。
- RPC Runtimeは各Goalを`goal_id`ごとに独立管理する。
- Protocol上有効で新しい`goal_id`を持つGoal RequestはApplicationへ通知する。
- RuntimeはProtocol上処理不能なGoal Requestだけを自動拒否する。
- 同時実行数、直列化、キュー、排他、優先度、preemptionはApplication Policyとする。
- Runtime拒否とApplication拒否を識別可能にする。
- Endpoint、Client接続、multiplex、transport capacityはProtocolの識別モデルに含めない。
- Protocol上の独立したClient Session概念は導入しない。
- Transport接続失敗、Runtime拒否、Application拒否を異なる失敗として扱う。
- Action Request、Action Response、Action FeedbackをServiceとは独立したPDU契約として定義する。
- Feedbackに`sequence_no`を持たせる。
- Feedbackの発行契機と周期はServer Applicationが決定する。
- 汎用的な進捗率を共通ヘッダへ入れず、Action固有のFeedback bodyへ置く。
- 既存Service Request/Responseのバイナリ契約を変更しない。

## 状態モデルの設計判断

状態モデルは、Action Typeやworkerではなく、accept済みの`goal_id`インスタンスだけを対象とします。

```text
Goal Request
  -> reject: Goalインスタンスを生成しない
  -> accept: DOINGでGoalインスタンスを生成
```

Action Type全体の状態、およびApplication内部workerの状態はProtocolへ持ち込みません。

accept済みGoalは、非同期イベント競合を制御するため、次の3状態をMUSTで持ちます。

```text
DOING
CANCELING
FINISHING
```

基本遷移は次のとおりです。

```text
normal:
  [*] -> DOING -> FINISHING -> [*]

cancel:
  [*] -> DOING -> CANCELING -> FINISHING -> [*]
```

- 初期状態は実体stateにせず、initial pseudo-stateで表現する。
- Goal Requestをrejectした場合、Goalインスタンスを生成しない。
- Cancel Requestを受信しただけでは`CANCELING`へ遷移しない。
- ApplicationがCancelをacceptした時点で`CANCELING`へ遷移する。
- terminal statusとResultを確定した時点で`FINISHING`へ遷移する。
- `FINISHING`では新規Feedback、Cancel、重複完了によって終端結果を変更しない。
- Result送信後のRuntime保持責務が完了した時点でGoalインスタンスを破棄する。

## イベント×状態マトリクス

今回の状態モデルは、状態図だけではなく、イベント×状態マトリクスを正規の設計手法とします。

```text
縦軸: イベント
横軸: DOING / CANCELING / FINISHING
各セル:
  Decision
  Action
  Next state
```

イベント発生元は、主に次へ分けます。

```text
Server Application
Client / Protocol
Server Runtime / Transport
```

各セルでは、次のいずれかを選択します。

```text
ALLOW
REJECT
IGNORE
IDEMPOTENT
DEFER
```

これにより、次のような非同期競合を網羅的に検討します。

- Result確定とFeedback発行の競合
- 通常完了とCancel Requestの競合
- Cancel受理と通常成功の競合
- 重複Cancel
- 重複完了
- FINISHING中のCancel
- Result送信失敗
- Transport切断

## 現在の主要な未確定事項

- イベントの洗い出しに不足がないか
- イベント発生元と受信側の分類が正しいか
- `CANCELING`中のFeedbackを許可するか
- `CANCELING`中の`COMPLETE_SUCCEEDED`を許可するか
- `DOING`中の`COMPLETE_CANCELED`を許可するか
- Cancel判断待ち中に通常完了した場合のCancel Response
- `CANCELING`中の重複Cancelを冪等応答にするか
- `FINISHING`中のCancel Requestへの応答
- 重複完了を冪等またはエラーのどちらにするか
- Result送信失敗時の保持、再送、解放条件
- Transport切断時にApplication実行を継続するか
- Server Runtime状態をClientへ明示公開する必要があるか
- UUID versionと一意性を要求する範囲
- 終了済み`goal_id`を保持する期間
- 同じ`goal_id`を持つGoal Request再送の扱い
- Protocol Runtimeによる拒否理由の標準化範囲
- Application rejection reasonの表現方法
- Feedbackのキュー方式、容量、欠落および順序の扱い
- Applicationへ公開するGoalHandleまたはContextの責務

未確定事項を確定仕様へ混在させず、各文書内で「設計判断」と「未決定」を明示します。

## 関連Issue

- RPC Action対応: #21
- 設計文書全体: #44
- 概念と責務境界: #45
- Goal identityと複数Goalデータモデル: #48
- Goal lifecycle状態モデル: #50
- Registry Action生成: hakoniwalab/hakoniwa-pdu-registry#17
- ROS 2 Service/Action Bridge: hakoniwalab/hakoniwa-pdu-ros#1

## 現在のレビュー方針

今回のレビューでは、以下を順に確認します。

1. `DOING`、`CANCELING`、`FINISHING`の3状態で必要十分か。
2. 発生し得るイベントを発生元ごとに網羅できているか。
3. イベント×状態マトリクスの各セルについて、許可、拒否、無視、冪等、委譲のどれにするか。
4. 各セルで必要な副作用と次状態が明確か。
5. 競合、重複、遅延、送信失敗などのイレギュラーケースが漏れていないか。
6. 状態モデルで確定する事項と、後続のProtocol・競合規約へ送る事項を分離できているか。

この状態・イベントモデルが合意できた後、各イベントをどのmessage交換で成立させるかを`05-protocol.md`で設計します。
