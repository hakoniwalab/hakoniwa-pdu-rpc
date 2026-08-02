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
- Feedbackは0回以上送信できる非終端通知とします。
- Resultのデータと、Succeeded、Canceled、Abortedなどの終端状態を区別します。
- Cancel要求、Cancel受理、Canceled終端を同一概念として扱いません。
- 既存Service RPCのPDUレイアウト、設定、状態遷移、API挙動を変更しません。
- Action対応は追加機能かつ明示的なopt-inとします。

## 文書構成

### 現在のレビュー対象

1. [基本概念](01-concepts.md)
2. [責務境界](02-responsibility-boundaries.md)

### 後続で追加する文書

```text
03-data-model.md
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

## 確定に近い前提

以下はRegistry側のAction PDU設計および関連Issueで採用されている前提です。ただし、本設計文書のレビューによって表現や責務境界を調整する可能性があります。

- 1回のAction実行を128-bitの`goal_id`で識別する。
- Action Request、Action Response、Action FeedbackをServiceとは独立したPDU契約として定義する。
- Feedbackに`sequence_no`を持たせる。
- 汎用的な進捗率を共通ヘッダへ入れず、Action固有のFeedback bodyへ置く。
- 既存Service Request/Responseのバイナリ契約を変更しない。

## 現在の主要な未確定事項

- 同一Actionに複数の同時Goalを許可する範囲
- Goalの受理前に届いたCancelの扱い
- Cancel ResponseとCanceled Resultの役割分担
- Resultとterminal statusをAPI上でどのように返すか
- Feedbackのキュー方式、容量、欠落および順序の扱い
- 終了済み`goal_id`を保持する期間
- タイムアウトをProtocol終端として扱うか、ローカル観測結果として扱うか

未確定事項を確定仕様へ混在させず、各文書内で「提案」「決定」「未決定」を明示します。

## 関連Issue

- RPC Action対応: #21
- 設計文書全体: #44
- 概念と責務境界: #45
- Registry Action生成: hakoniwalab/hakoniwa-pdu-registry#17
- ROS 2 Service/Action Bridge: hakoniwalab/hakoniwa-pdu-ros#1

## レビュー方針

今回の初稿では、実装APIや具体的な状態遷移を確定しません。まず、以下について合意することを目的とします。

1. Actionをどのような抽象概念として扱うか。
2. Goal、Feedback、Result、Cancelをどのように区別するか。
3. Registry、RPC、ROS Bridge、利用アプリケーションの責務をどこで分けるか。

これらが合意できた後、データモデルと状態モデルの設計へ進みます。
