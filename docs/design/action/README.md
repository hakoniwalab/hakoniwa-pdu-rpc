# Hakoniwa Action Protocol 設計

> **Status: Draft**  
> 本文書群はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 目的

このディレクトリでは、`hakoniwa-pdu-rpc` におけるHakoniwa Action Protocolの設計仕様を管理します。

Action対応を実装から逆算して定義するのではなく、先に以下を明文化し、C++ Runtime、Python Binding、`hakoniwa-pdu-ros`、利用アプリケーションが共通に参照できる仕様とします。

- 基本概念と用語
- リポジトリおよびレイヤ間の責務境界
- PDUデータ契約
- ClientおよびServerの状態機械
- Goal、Feedback、Cancel、Resultの通信規約
- エラー、競合、遅延メッセージの扱い
- 公開API、設定、ファイル配置、ビルド契約

## 基本方針

- ActionはROS 2固有機能ではなく、箱庭で利用できる独立した通信・実行ライフサイクルとして定義します。
- Actionは単に応答時間の長いService RPCではなく、`goal_id`で識別されるGoal実行セッションとして扱います。
- 1回のGoal Executionは128-bit UUIDの`goal_id`で識別します。
- `goal_id`は原則としてAction Client RuntimeがGoal送信前に生成し、Server Runtimeが重複検査とlifecycle管理を行います。
- 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoal Executionが同時に存在することをProtocol上許容します。
- Action EndpointおよびClient Sessionは配送・通信コンテキストであり、Goal Executionの同一性を決める条件には使用しません。
- RPC Runtimeは、複数Goalを`goal_id`ごとに独立して相関・配送・状態管理できる能力を提供します。
- Goalを並列実行するか、直列化するか、キューへ入れるか、拒否するかはAction Server Applicationの実行ポリシーとします。
- Runtimeは一律の「1 Actionにつき1 Goal」または「1 Clientにつき1 Goal」という制約をProtocolへ埋め込みません。
- `tcp_mux`の`maxClients`はTransportのClient接続数上限であり、Actionの同時実行数とは定義しません。
- Feedbackは0回以上送信できる非終端通知とします。
- Resultのデータと、Succeeded、Canceled、Abortedなどの終端状態を区別します。
- Cancel要求、Cancel受理、Canceled終端を同一概念として扱いません。
- 既存Service RPCのPDUレイアウト、設定、状態遷移、API挙動を変更しません。
- Action対応は追加機能かつ明示的なopt-inとします。

## 文書構成

### 現在のレビュー対象

1. [基本概念](01-concepts.md)
2. [責務境界](02-responsibility-boundaries.md)
3. [データモデル](03-data-model.md)

### 後続で追加する文書

```text
04-state-model.md
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

以下は、これまでのレビューで合意した方向性です。後続の状態モデル、Protocol、API設計では、この前提を狭めない形で具体化します。

- 1回のAction実行を128-bit UUIDの`goal_id`で識別する。
- 通常のHakoniwa ClientではAction Client Runtimeが`goal_id`を生成する。
- ROS BridgeなどのAdapterは、外部で生成された互換UUIDを指定できる。
- Server Runtimeは`goal_id`の形式検査、重複検査、登録、相関、状態管理を担当する。
- 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoal ExecutionをProtocol上許容する。
- Action EndpointおよびClient SessionはGoal Executionの配送・通信コンテキストとして関連付けるが、識別条件には含めない。
- RPC Runtimeは各Goalを`goal_id`ごとに独立管理する。
- 同時実行数、直列化、キュー、排他、優先度、preemptionはApplication Policyとする。
- `maxClients`とAction同時実行能力を分離する。
- Transport接続失敗、Runtime拒否、Application拒否を異なる失敗として扱う。
- Action Request、Action Response、Action FeedbackをServiceとは独立したPDU契約として定義する。
- Feedbackに`sequence_no`を持たせる。
- 汎用的な進捗率を共通ヘッダへ入れず、Action固有のFeedback bodyへ置く。
- 既存Service Request/Responseのバイナリ契約を変更しない。

## 現在の主要な未確定事項

- UUID versionと一意性を要求する範囲
- 終了済み`goal_id`を保持する期間
- 同じ`goal_id`を持つGoal Request再送の扱い
- Goalの受理前に届いたCancelの扱い
- Applicationがキューへ入れたGoalについて、`ACCEPTED`、`QUEUED`、`EXECUTING`をProtocol上区別するか
- Client Session切断時にGoal Executionを継続するかCancelするか
- 同一Client Session上の複数Goalイベントに対する順序保証
- Cancel ResponseとCanceled Resultの役割分担
- Resultとterminal statusをAPI上でどのように返すか
- Feedbackのキュー方式、容量、欠落および順序の扱い
- タイムアウトをProtocol終端として扱うか、ローカル観測結果として扱うか
- Applicationへ公開するGoalHandleまたはContextの責務

未確定事項を確定仕様へ混在させず、各文書内で「設計判断」と「未決定」を明示します。

## 関連Issue

- RPC Action対応: #21
- 設計文書全体: #44
- 概念と責務境界: #45
- Goal identityと複数Goalデータモデル: #48
- Registry Action生成: hakoniwalab/hakoniwa-pdu-registry#17
- ROS 2 Service/Action Bridge: hakoniwalab/hakoniwa-pdu-ros#1

## 現在のレビュー方針

今回のレビューでは、前段で合意した概念と責務境界を、複数Goalを狭めないデータモデルとして具体化します。

主な確認事項は以下です。

1. `goal_id`をClient Runtime生成、Server Runtime管理とする責務分担。
2. 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoal Executionを許容すること。
3. Action EndpointおよびClient Sessionを、Goal Executionの識別条件ではなく配送・通信コンテキストとして扱うこと。
4. RPC Runtimeが複数Goalを独立管理できる共通能力を持つこと。
5. Goalの並列実行、直列化、キュー、排他、優先度、preemptionをApplication Policyとすること。
6. `tcp_mux.maxClients`をTransport接続容量として、active Goal数と分離すること。

このデータモデルが合意できた後、各Goal Executionがどのように状態遷移するかを`04-state-model.md`で設計します。
