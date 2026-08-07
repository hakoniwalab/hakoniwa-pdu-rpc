# Hakoniwa Action Protocol設計

> **Status: Implemented contract**  
> 本文書群は、`hakoniwa-pdu-rpc`のAction実装、公開API、設定、テストが従う現行契約です。

## 1. 目的

Hakoniwa Actionは、ROS 2から独立した長時間処理の通信Protocolです。Goal Request、Goal Response、Feedback、Cancel、Resultを`goal_id`で相関し、生成PDUと`hakoniwa-pdu-endpoint`上で動作します。

```text
Application
    |
    v
ActionServicesClient / ActionServicesServer / ActionServicesMuxServer
    |
    v
IActionClientEndpoint / IActionServerEndpoint
    |
    v
hakoniwa-pdu-endpoint
    |
    v
TCP transport
```

## 2. 規範

- 1回のGoalは、上位ApplicationまたはAdapterが指定する非zeroの128-bit `goal_id`で識別する。
- RuntimeはGoal IDを自動生成せず、active Goalとの衝突を拒否する。
- 同一Action Typeで異なる`goal_id`の複数Goalを扱える。
- Goal Request、Goal Response、Feedback、Cancel、Resultは`goal_id`で相関する。
- Goalの受理、実行順序、並列性、Feedback内容、terminal ResultはApplicationが決定する。
- Runtimeはpacket相関、slot ownership、状態遷移、Wire順序、Protocol違反の処理を担当する。
- Feedbackは0回以上送信できる非終端通知である。
- Cancel要求、Cancel受理、実行停止、`CANCELED` Resultは別の事象である。
- Resultは`SUCCEEDED`、`CANCELED`、`ABORTED`のterminal statusとAction固有bodyから成る。
- Transport異常をActionのterminal statusへ自動変換しない。
- Point-to-point ServerとMux Serverは同じ`action_name + ServerGoalHandle`契約を公開し、接続identityをApplicationへ漏らさない。
- 既存Service RPCのPDUレイアウト、状態遷移、API契約を変更しない。

## 3. 状態モデル

受理済みGoalのProtocol状態は次の3状態です。

```text
normal:
  [*] -> EXECUTING -> FINISHING -> [*]

cancel:
  [*] -> EXECUTING -> CANCELING -> FINISHING -> [*]
```

- rejectされたGoalはGoal Instanceを生成しない。
- Cancel Requestの受信だけでは`CANCELING`へ遷移しない。
- ApplicationがCancelをacceptした時点で`CANCELING`へ遷移する。
- terminal Resultをcommitした時点で`FINISHING`へ遷移する。
- `FINISHING`ではFeedback、Cancel、重複completeによって結果を変更しない。
- Result配送またはlocal terminal完了後にGoal Contextを解放する。

ServerとClientは同じ状態名を使いますが、Context、Event、遷移関数は別モジュールです。状態遷移核は現在状態とEventから、遷移結果、NOP、または理由付きERRORを返します。通信やApplication callbackは状態遷移核の責務ではありません。

## 4. 文書構成

1. [基本概念](01-concepts.md)
2. [責務境界](02-responsibility-boundaries.md)
3. [データモデル](03-data-model.md)
4. [Server状態モデル](04-state-model.md)
5. [Client状態モデル](05-client-state-model.md)
6. [通信Protocol](06-protocol.md)
7. [Runtimeアーキテクチャ](07-architecture.md)
8. [設定契約](08-configuration.md)
9. [C API／Python CFFI契約](09-c-api.md)
10. [実装・配布方針](10-implementation-policy.md)
11. [Cancel／Result競合契約](11-cancel-result-race.md)
12. [Endpoint Transaction契約](12-endpoint-transaction-state.md)
13. [Action Mux Server契約](13-mux-server.md)

推奨する読み順は、基本概念、責務境界、データモデル、Server／Client状態モデル、通信Protocol、設定、公開API、Endpoint Transaction、Mux Serverです。

## 5. 実装との対応

| 契約 | 主な実装 |
| --- | --- |
| 設定load | `src/action_configuration.cpp` |
| 設定生成 | `tools/generate_action_config.py` |
| Server状態遷移 | `src/action_server_state_machine.cpp` |
| Client状態遷移 | `src/action_client_state_machine.cpp` |
| Server Services | `src/action_services_server.cpp` |
| Client Services | `src/action_services_client.cpp` |
| Server Endpoint | `src/action_server_endpoint_impl.cpp` |
| Client Endpoint | `src/action_client_endpoint_impl.cpp` |
| Mux Server | `src/action_services_mux_server.cpp` |
| C API | `src/c_action.cpp` |
| Python CFFI | `python/hakoniwa_pdu_rpc/action_cffi.py` |

## 6. 検証契約

`python tools/hako.py test`は、Service RPCとActionのreviewed Contract TestをbuildしてCTestで実行します。Actionの検証範囲は次のとおりです。

- 設定生成とload
- Server／Client状態遷移核
- Services層のGoal Instance管理
- Goal Response、Feedback、Cancel、Resultのpacket相関
- 可変長bodyと`bufferHeap`上限
- duplicate Goal、slot collision、送信失敗、Wire順序
- TCP point-to-point E2E
- TCP Mux複数Clientと切断処理
- C APIとPython CFFI
- installed C++／C／Python package consumer

テストの実行範囲と障害判定は[`docs/test-contract.md`](../../test-contract.md)を参照してください。
