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
- Actionは単に応答時間の長いService RPCではなく、`goal_id`で識別されるGoal lifecycleとして扱います。
- 1回のGoalは128-bit UUIDの`goal_id`で識別します。
- `goal_id`は原則としてAction Client RuntimeがGoal送信前に生成し、Server Runtimeが重複検査とlifecycle管理を行います。
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

- 1回のGoalを128-bit UUIDの`goal_id`で識別する。
- 通常のHakoniwa ClientではAction Client Runtimeが`goal_id`を生成する。
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
- 汎用的な進捗率を共通ヘッダへ入れず、Action固有のFeedback bodyへ置く。
- 既存Service Request/Responseのバイナリ契約を変更しない。

## 現在の主要な未確定事項

- UUID versionと一意性を要求する範囲
- 終了済み`goal_id`を保持する期間
- 同じ`goal_id`を持つGoal Request再送の扱い
- Protocol Runtimeによる拒否理由の標準化範囲
- Application rejection reasonの表現方法
- `reject_origin`を明示的に持たせるか、reason codeで区別するか
- ApplicationがGoal Requestへ応答しない場合のacceptance timeout
- Goalの受理前に届いたCancelの扱い
- Applicationがキューへ入れたGoalについて、`ACCEPTED`、`QUEUED`、`EXECUTING`をProtocol上区別するか
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

今回のレビューでは、前段で合意した概念と責務境界を、実現方式に依存しない複数Goalのデータモデルとして具体化します。

主な確認事項は以下です。

1. `goal_id`をClient Runtime生成、Server Runtime管理とする責務分担。
2. `goal_id`をGoal lifecycle全体の相関キーとすること。
3. 同一Action Typeに対して、異なる`goal_id`を持つ複数のGoalを許容すること。
4. 重複`goal_id`などProtocol上の不正はRuntimeが自動拒否すること。
5. Protocol上有効で新しい`goal_id`を持つGoal RequestはApplicationへ通知すること。
6. ApplicationがGoalの受理・拒否と実行Policyを決定すること。
7. Runtime拒否とApplication拒否を識別可能にすること。
8. EndpointやClient接続などの実現方式をProtocolの識別モデルへ持ち込まないこと。

このデータモデルが合意できた後、各Goalがどのように状態遷移するかを`04-state-model.md`で設計します。
