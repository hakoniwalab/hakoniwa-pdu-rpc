# Action Mux Server契約

> **Status: Implemented contract**
> 本文書は、Action Mux Serverにおける接続所有、Goal所有、切断処理の現行契約を定義します。

## 1. 目的

Action Mux Serverは、単一のlisten endpointで複数のTransport接続を受け入れ、既存の`ActionServicesServer`と同じApplication APIを提供します。

Service RPCのMux実装は切断した`ConnectionSlot`を直ちに破棄できます。一方、ActionはGoal受理後もFeedback、Cancel、Resultまで継続するため、次を前提とします。

```text
Connection lifetime != Goal lifetime
```

Muxは接続管理の都合だけでaccept済みGoalを破棄してはなりません。

## 2. 公開契約

Applicationが扱うGoal identityは、point-to-point Serverと同じです。

```text
action_name + ServerGoalHandle(goal_id)
```

次の値はMux内部のrouting情報であり、Application API、C API、Python APIへ公開しません。

- `connection_id`
- socketまたはTransport session ID
- `ConnectionSlot` index
- PDU Endpoint identity

このため、point-to-point ServerとMux ServerでGoal操作APIの意味を変えません。

## 3. Goal IDの一意性範囲

`goal_id`は接続単位ではなく、**一つのMux Server Runtime内の同一Action名について一意**でなければなりません。

```text
connection A: fibonacci / goal X  -> first request
connection B: fibonacci / goal X  -> duplicate, protocol reject
```

接続が異なっても、同じ`action_name + goal_id`を別Goalとして扱いません。connection identityをGoal identityへ加えないというAction Protocol契約を維持します。

## 4. 所有構造

既存クラスを組み合わせ、接続ごとのGoal Contextを所有します。

```text
ActionServicesMuxServer
  |
  +-- ConnectionSlot A
  |     - connection_id
  |     - Endpoint
  |     - ActionServicesServer
  |     - connection state
  |
  +-- ConnectionSlot B
  |     - connection_id
  |     - Endpoint
  |     - ActionServicesServer
  |     - connection state
  |
  `-- Goal owner index
        (action_name, goal_id) -> connection_id
```

各接続から受け取ったopen済み`Endpoint`を使って、接続ごとに`ActionServicesServer`を初期化します。

### 4.1 `ActionServicesServer`の責務

接続ごとの`ActionServicesServer`が、現在と同じ責務を持ちます。

- accept済みGoalの`GoalInstance`
- Server Goal状態遷移
- Action名から`IActionServerEndpoint`への委譲
- Goal、Cancel、Feedback、Resultの処理

### 4.2 Muxの責務

MuxはGoal状態を重複して保持しません。Muxが持つowner indexは、次のためだけに使います。

- ApplicationからのGoal操作を所有`ConnectionSlot`へ配送する
- 接続をまたぐduplicate Goal IDを検出する
- 切断した接続にactive Goalが残るか判定する
- terminal完了後にorphaned slotを回収する

owner indexを第二のGoal Contextまたは状態機械にしてはなりません。

## 5. Goal所有権のライフサイクル

Muxは、Applicationへ公開したGoal Requestからownerを追跡します。

```text
Goal Requestをpoll
  -> owner = PENDING

accept_goal成功
  -> owner = ACTIVE

reject_goal完了
  -> owner削除

complete成功、または切断後のlocal terminal完了
  -> owner削除
```

同一`action_name + goal_id`のownerが`PENDING`または`ACTIVE`なら、別接続から到着したGoal Requestはduplicateです。到着した接続の`ActionServicesServer::reject_goal()`を使ってbest-effortでProtocol REJECTを返し、Applicationへは通知しません。

自動REJECT送信に失敗した場合は異常ログを残します。無関係な既存Goalのownerは変更しません。

### 5.1 Mux内の直列化

Muxの`poll()`、Goal操作、owner更新、切断状態の反映を一つのMux mutexで直列化します。Transportの切断callbackは状態フラグを立てるだけに限定し、Goal ownerや`ActionServicesServer`を直接変更しません。

これにより、`accept_goal()`の成功と同時に切断した場合も、ownerを`ACTIVE`へ変更する処理と切断処理の順序を一意にします。性能要求が確認されるまでは、接続単位mutexや並列pollへ分割しません。

`is_ready()`はTransportがsocketをacceptした時点ではなく、期待する全接続を`ConnectionSlot`へ取り込み、各`ActionServicesServer`の初期化と開始が完了した時点でtrueになります。したがって、同じ排他区間で観測した場合は次を満たします。

```text
is_ready() == true
  -> connected_count() >= expected_count()
```

## 6. Transport切断

### 6.1 未受理Goal

Applicationがまだaccept／rejectしていない`PENDING` Goalは、切断時に無効化します。

- Applicationへ既に渡した`ServerGoalHandle`による後続accept／rejectは失敗する。
- owner indexから削除する。
- 切断済みTransportへGoal Responseを送ろうとしない。

未受理GoalにはServer側の実行Instanceが存在しないため、接続寿命を越えて保持しません。

### 6.2 accept済みGoal

`ACTIVE` Goalが存在する接続は、切断しても`ConnectionSlot`とその`ActionServicesServer`を直ちに破棄しません。slotを`ORPHANED`として保持します。

各active Goalについて、Muxは次のイベントを通常のServer poll経路へ一度だけ投入します。

```text
ServerEventType::RUNTIME_CANCEL_REQUEST
RuntimeCancelCause::TRANSPORT_DISCONNECTED
```

このイベントは停止要求であり、terminal statusではありません。Applicationは通常の`accept_cancel()`または`reject_cancel()`で判断します。

### 6.3 切断後の処理

Transportが存在しないため、切断後は次の契約とします。

- Runtime起因Cancelのaccept／rejectではWire上のCancel Responseを送らない。
- `send_feedback()`は失敗し、原因を診断ログへ残す。
- terminal `complete()`は、上位状態とResult packetを検証したうえでlocal terminal完了として扱う。
- local terminal完了ではResultをWire送信せず、Goal Contextとownerを解放する。
- `ActionServicesServer`内の全Goalが終了したら、orphaned slotを論理的にretireし、接続capacityを再利用できる。

Transportの切断callbackは受信thread上で実行されるため、retireした`Endpoint`オブジェクト自体はMux Serverの`stop()`まで保持します。Goal Contextとrouting ownerはterminal完了時に解放されるため、これはGoal lifetimeの延長ではありません。物理破棄を遅らせることでcallbackのunwindと通信threadのjoinを安全に分離します。

ApplicationがRuntime Cancelをrejectした場合、Goalは`EXECUTING`を継続できます。ただしFeedbackは配送できません。Applicationは最終的に`SUCCEEDED`または`ABORTED`でlocal terminal完了させる必要があります。

watchdogまたは強制terminal化は行いません。Applicationが完了しないorphaned Goalは、明示的なMux Server停止まで保持します。

## 7. 再接続とResult再配送

現在のMux契約は、次を提供しません。

- 切断したClient Sessionの再開
- 新しい接続へのGoal Context移管
- Resultの保持と再取得
- 切断中Resultの再配送
- connection identityを使ったGoal identityの拡張

新しい接続は新しいTransport sessionとして扱い、以前のGoal Contextを移管しません。

## 8. shutdown

Mux Serverの明示的な停止は、切断とは区別します。

`stop()`は新規接続と新規Goalの受付を停止し、残存Contextを解放します。ApplicationのpollやGoal完了を待機するdrain処理、待機期限、強制terminal化は行いません。

## 9. Mux内部の統合経路

Muxは、通常通信の公開契約を変えずに次の内部経路を使用します。

- 切断を通知し、accept済みGoalごとのRuntime Cancel eventをqueueへ投入する経路
- 未受理packet bindingだけを無効化する経路
- 切断後にWire送信せずterminal commitとContext解放を行う経路
- active Goalの有無をMuxが確認する最小query

これらはTransport固有signalや`connection_id`をApplicationへ公開しません。既存の通常`accept_cancel()`、`reject_cancel()`、`complete()`の意味を保ち、Mux内部で接続状態に応じた配送方法を選びます。

## 10. Contract Test

Contract Testは次を固定します。

- 2接続から異なるGoalを受け、同じApplication APIで操作できる。
- 同一Actionの同一Goal IDを別接続から送ると、後着GoalだけをREJECTする。
- Applicationへ`connection_id`を公開しない。
- 未受理Goalの接続断後はaccept／rejectできない。
- active Goalの接続断でRuntime Cancelを一度だけ通知する。
- Goal acceptと接続断が競合しても、Goalを未受理破棄とactive保持の両方へ処理しない。
- Runtime Cancel accept後にlocal `CANCELED`完了するとGoal ownerを解放する。
- Runtime Cancel reject後もlocal `SUCCEEDED`／`ABORTED`完了でownerを解放できる。
- orphaned Goalが残る間は接続slotのGoal Contextを破棄しない。
- orphaned Goal終了後にslotをretireし、接続capacityを再利用できる。
- 切断したPID、socket、別接続を推測して操作しない。

## 11. 対象外

- Client側Mux
- session resume
- Result retention／再取得
- 自動再接続
- watchdogによる強制Cancel／Abort
- graceful shutdown／drain API
- 接続ごとのGoal ID namespace
- 接続間のGoal Context migration
