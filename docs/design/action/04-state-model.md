# Hakoniwa Actionの状態モデル

> **Status: Draft**  
> 本文書はレビューと議論のための初稿です。現時点では確定仕様ではありません。

## 1. 目的

本書では、Hakoniwa Action Protocolにおける1つのGoal lifecycleの抽象状態と、その遷移規則を定義します。

本状態モデルは、以下に依存しません。

- TCP、共有メモリなどのTransport方式
- Endpoint、connection、channel、mux client ID
- C++、PythonなどのAPI表現
- Application内部のthread、task、queue実装

各Goalは`goal_id`によって独立して状態管理します。

同一Action Typeに複数Goalが存在する場合も、あるGoalの状態遷移が別のGoalを暗黙に変化させることはありません。

## 2. 前提

本状態モデルは、前段の設計判断を前提とします。

- 1回のGoalを128-bit UUIDの`goal_id`で識別する。
- Goal Request、Goal Response、Feedback、Cancel、Resultを同じ`goal_id`で相関する。
- 同一Action Typeに対して、異なる`goal_id`を持つ複数Goalを同時に扱える。
- Protocol上有効で新しい`goal_id`を持つGoal RequestはApplicationへ通知する。
- Goalをaccept/rejectするかはApplicationが判断する。
- RuntimeはProtocol上処理不能なGoal Requestだけを自動拒否する。
- Runtime拒否とApplication拒否を識別可能にする。
- Goalの並列実行、直列化、キュー、排他、優先度、preemptionはApplication Policyとする。

## 3. 状態モデルを二つのフェーズへ分ける

Goal lifecycleは、以下の二つのフェーズへ分けて考えます。

1. Goal Requestの受理判定フェーズ
2. acceptされたGoalの実行フェーズ

```text
Goal Request lifecycle
  PENDING_ACCEPTANCE
    -> REJECTED
    -> ACCEPTED

Accepted Goal lifecycle
  ACCEPTED
    -> EXECUTING
    -> terminal
```

この分離により、Goal Requestが届いたことと、ApplicationがGoalの実行を引き受けたことを区別します。

## 4. 受理判定フェーズ

### 4.1 PENDING_ACCEPTANCE

`PENDING_ACCEPTANCE`は、Goal RequestがRuntimeへ到達し、accept/rejectがまだ確定していない状態です。

概念的な流れは以下です。

```text
Goal Request received
  -> Runtime validation
  -> Application notification
  -> Application decision
```

Runtimeは、Goal RequestがProtocol上有効で新しい`goal_id`を持つ場合、Applicationへ通知します。

Applicationは、以下のような条件に基づいてaccept/rejectを判断します。

- Goal bodyの妥当性
- 対象資源の状態
- 同時実行数
- queue容量
- 排他条件
- 安全条件
- 権限または運用Policy

### 4.2 REJECTED

`REJECTED`は、Goal Requestを受理しなかったことを表します。

拒否には少なくとも二つのoriginがあります。

```text
Runtime rejection
Application rejection
```

Runtime rejectionの候補は以下です。

- 不正な`goal_id`
- duplicate `goal_id`
- 不正message
- 対応できないProtocol version
- Goal Contextを生成できないRuntime内部障害

Application rejectionの候補は以下です。

- Goal bodyが業務上不正
- 必要な資源が使用中
- Applicationの同時実行上限
- queue満杯
- 安全条件を満たさない

Runtime rejectionとApplication rejectionは、同じ拒否理由へ統合しません。

### 4.3 REJECTEDとGoal Execution成立の関係

本初稿では、`REJECTED`を「acceptされたGoal Executionの終端状態」には含めません。

```text
Goal Request
  -> REJECTED
  -> accepted Goal Executionは成立しない
```

ただし、Clientから見れば、`goal_id`に対するGoal Request lifecycleは`REJECTED`で完了します。

したがって、以下を区別します。

```text
Goal Request lifecycle terminal
  REJECTED

Accepted Goal terminal
  SUCCEEDED / CANCELED / ABORTED / ERROR
```

この整理はレビュー対象です。

### 4.4 ACCEPTED

`ACCEPTED`は、ApplicationがGoalを受理し、そのGoal lifecycleを継続する責任を引き受けたことを表します。

`ACCEPTED`は、必ずしも処理が実行中であることを意味しません。

```text
ACCEPTED
  = Application has accepted responsibility for the Goal
  != necessarily executing now
```

この区別により、ApplicationはGoalを受理した後、以下のPolicyを選択できます。

- 直ちに実行する
- queueへ入れる
- 資源解放を待つ
- 優先度制御を行う

## 5. 受理後の実行フェーズ

### 5.1 EXECUTING

`EXECUTING`は、ApplicationがGoal bodyに対応する処理を実行している状態です。

```text
ACCEPTED -> EXECUTING
```

Applicationが実行開始をRuntimeへ明示的に通知する必要があるか、Runtimeがaccept時に自動的に`EXECUTING`へ遷移するかは、後続のAPI設計で決定します。

ただし、Protocol上`ACCEPTED`と`EXECUTING`を区別する場合、Runtimeがその遷移を認識できる経路は必要です。

### 5.2 QUEUEDを独立状態とするか

ApplicationがGoalを受理した後、実行開始までqueueで待機させる場合があります。

候補は二つあります。

#### 案A: ACCEPTEDへ包含する

```text
ACCEPTED
  includes accepted-but-not-executing
```

利点:

- Protocol状態が単純になる。
- queue方式をApplication内部へ閉じ込められる。

欠点:

- Clientはqueue待ちかどうかを共通状態から判断できない。

#### 案B: QUEUEDを追加する

```text
ACCEPTED -> QUEUED -> EXECUTING
```

利点:

- Clientが実行待ちを観測できる。

欠点:

- Application固有の実行PolicyがProtocol状態へ入り込む可能性がある。

初稿では`QUEUED`を確定状態へ含めず、レビュー対象とします。

## 6. Cancelの状態モデル

### 6.1 Cancel Requestは終端状態ではない

Cancel Requestを受信しただけでは、Goalは`CANCELED`になりません。

以下を別の出来事として扱います。

1. Cancel Requestを受信する
2. ApplicationがCancelをaccept/rejectする
3. Applicationが実際の処理を停止する
4. Goalが`CANCELED`で終端する

```text
Cancel Request
  != Cancel accepted
  != execution stopped
  != CANCELED terminal
```

### 6.2 Cancelが拒否された場合

CancelがApplicationによって拒否された場合、Goalの実行状態は維持します。

```text
EXECUTING
  -> Cancel Request
  -> Cancel rejected
  -> EXECUTING
```

Cancel Responseは、Cancel Requestに対する判断を返しますが、Goal Resultではありません。

### 6.3 Cancelが受理された場合

Cancelが受理された後、Applicationが停止処理を行う期間を表現する必要があります。

候補状態は以下です。

```text
CANCEL_REQUESTED
CANCELING
```

候補となる遷移は以下です。

```text
EXECUTING
  -> CANCELING
  -> CANCELED
```

ただし、`CANCEL_REQUESTED`または`CANCELING`をProtocol公開状態とするか、Runtime/Application内部状態とするかはレビュー対象です。

### 6.4 CANCELEDへの遷移

`CANCELED`は、Cancel Requestを受理しただけでなく、ApplicationがGoal処理を停止し、Canceled Resultを確定した時点の終端状態です。

```text
Cancel accepted
  -> stop processing
  -> produce Result
  -> CANCELED
```

## 7. 終端状態

acceptされたGoalは、最終的に一つの終端状態へ遷移します。

初稿では以下を候補とします。

```text
SUCCEEDED
CANCELED
ABORTED
ERROR
```

終端後に、別の終端状態へ遷移してはなりません。

```text
terminal -> terminal transition is forbidden
```

### 7.1 SUCCEEDED

ApplicationがGoalの意図した処理を正常に完了したことを表します。

### 7.2 CANCELED

Cancel Requestが受理され、Applicationが処理を停止して終端したことを表します。

### 7.3 ABORTED

ApplicationがGoalを受理した後、業務上または実行上の理由により完了できず、処理を終了したことを表します。

例:

- 対象ロボットが実行不能状態になった
- Goal達成条件を満たせなくなった
- Applicationが安全上の理由で処理を中止した

### 7.4 ERROR

RuntimeまたはProtocol上の障害により、Goal lifecycleを正常に継続できなくなったことを表す候補です。

例:

- Runtime内部状態の破損
- ResultをProtocol上生成または配送できない
- 必須のProtocol invariant違反

`ERROR`をClientへ公開する終端状態とするか、通信エラーとしてGoalの終端状態とは別に扱うかはレビュー対象です。

## 8. 基本状態遷移

初稿の全体像は以下です。

```text
                         +-> REJECTED
                         |
PENDING_ACCEPTANCE -----+
                         |
                         +-> ACCEPTED
                               |
                               +-> EXECUTING
                               |      |
                               |      +-> SUCCEEDED
                               |      +-> ABORTED
                               |      +-> ERROR
                               |      |
                               |      +-> Cancel Request
                               |             |
                               |             +-> rejected -> EXECUTING
                               |             |
                               |             +-> accepted -> CANCELING -> CANCELED
                               |
                               +-> terminal before execution?
```

`ACCEPTED`から実行開始前に`ABORTED`または`CANCELED`へ遷移できるかは、レビュー対象です。

例えば、queue待ちのGoalにCancel Requestが届いた場合、実行開始せずに`CANCELED`へ遷移できる可能性があります。

## 9. RuntimeとApplicationの責務

### 9.1 Runtime

Runtimeは以下を担当します。

- `goal_id`ごとのProtocol状態保持
- 状態遷移要求の検査
- 許可されない遷移の拒否
- Runtime rejectionの処理
- Applicationのaccept/reject判断をGoal Responseへ反映
- Feedback、Cancel、Resultを現在状態と相関すること
- 終端後の状態を変更しないこと
- 複数Goalの状態を混同しないこと

### 9.2 Application

Applicationは以下を判断します。

- Goalをaccept/rejectするか
- acceptしたGoalをいつ実行開始するか
- 並列、直列、queue、排他、優先度
- Cancelをaccept/rejectするか
- Cancel受理後にどう安全に停止するか
- SUCCEEDED、CANCELED、ABORTEDのどれで終端するか

RuntimeはApplicationの業務Policyを代わりに決定しません。

## 10. Client側状態とServer正規状態

Server Runtimeが管理する状態をProtocol上の正規状態とします。

Client Runtimeは、通信上の都合で以下のローカル観測状態を持つ可能性があります。

```text
NOT_SENT
SENDING
WAITING_FOR_RESPONSE
RESPONSE_LOST
RESULT_WAIT_TIMEOUT
```

これらはClientローカル状態であり、Server側のGoal状態とは分離します。

例えば、Client側でResult待ちがtimeoutしても、Server側のGoalが自動的に`ABORTED`または`ERROR`になったことを意味しません。

ClientローカルtimeoutとProtocol上のGoal終端を混同しません。

## 11. acceptance timeout

ApplicationがGoal Request通知を受けた後、accept/rejectを返さない場合があります。

候補となる扱いは以下です。

1. Runtime設定時間後にRuntime rejectionとする
2. timeoutをClientローカル観測に限定する
3. Application応答まで無期限に待つ

初稿では、acceptance timeoutの有無と責務を確定しません。

ただし、`PENDING_ACCEPTANCE`が無期限に残る可能性を状態モデル上認識し、後続のProtocolおよび設定設計で決定します。

## 12. 不正な状態遷移

少なくとも以下は不正な遷移候補です。

```text
REJECTED -> ACCEPTED
SUCCEEDED -> EXECUTING
CANCELED -> SUCCEEDED
ABORTED -> CANCELED
terminal -> Feedback
unknown goal_id -> state transition
```

Runtimeは、不正な状態遷移をApplication実装の都合で黙って受け入れません。

具体的なエラー応答と競合規約は`06-error-and-race-semantics.md`で定義します。

## 13. 今回の初稿での提案

1. Goal lifecycleを受理判定フェーズと受理後実行フェーズへ分ける。
2. `PENDING_ACCEPTANCE`をaccept/reject未確定状態とする。
3. `REJECTED`をGoal Request lifecycleの終端とし、accepted Goalの終端状態には含めない。
4. `ACCEPTED`と`EXECUTING`を区別する。
5. `ACCEPTED`は受理済みだが実行開始前のGoalを表現できる。
6. `QUEUED`は初稿では確定状態に含めない。
7. Cancel Request、Cancel Response、処理停止、`CANCELED`終端を区別する。
8. Cancel拒否時は元のGoal状態を維持する。
9. `CANCELING`をProtocol公開状態とするかは未確定とする。
10. accepted Goalの終端候補を`SUCCEEDED`、`CANCELED`、`ABORTED`、`ERROR`とする。
11. Clientローカル状態とServer正規状態を分離する。
12. 各Goalを`goal_id`ごとに独立して状態管理する。

## 14. レビューで確認したい事項

1. `REJECTED`はGoal Request lifecycleの終端であり、accepted Goal Execution未成立と整理してよいか。
2. `ACCEPTED`と`EXECUTING`をProtocol上区別してよいか。
3. queue待ちを`ACCEPTED`へ包含し、`QUEUED`を共通Protocol状態に含めない方がよいか。
4. Cancel受理後の`CANCELING`をProtocol公開状態とする必要があるか。
5. `ACCEPTED`状態のGoalを、実行開始前に`CANCELED`または`ABORTED`で終端できるか。
6. `ERROR`をGoalの終端状態として公開するか、Runtime/通信エラーとして別概念にするか。
7. Client側の観測状態とServer側の正規状態を分離する方針でよいか。
8. Applicationがaccept/rejectへ応答しない場合、acceptance timeoutをProtocol、設定、Clientローカル観測のどこへ置くか。
