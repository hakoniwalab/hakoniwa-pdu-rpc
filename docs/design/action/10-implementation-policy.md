# Hakoniwa Action実装・配布方針

> **Status: Implemented contract**
> 本文書は、Action Runtimeのソース配置、公開境界、build、install、testに関する現行契約です。

## 1. レイヤ構造

```text
Application / Adapter
    |
    v
ActionServicesClient / ActionServicesServer / ActionServicesMuxServer
    |
    v
IActionClientEndpoint / IActionServerEndpoint
    |
    v
ActionClientEndpointImpl / ActionServerEndpointImpl
    |
    v
hakoniwa-pdu-endpoint
```

- Services層はAction定義の列挙、Goal Instance、状態遷移、Application APIを所有する。
- Endpoint層はpacket、slot、channel、送受信、相関、Wire順序を所有する。
- Mux ServerはConnectionSlotとGoal owner routingを所有し、Goal状態を複製しない。
- 状態遷移核はI/O、mutex、時刻、Application callbackを参照しない。
- Service RPCのクラスへAction状態を混在させない。

## 2. Namespaceと公開Header

Native C++ APIは次のnamespaceを使用します。

```cpp
namespace hakoniwa::pdu::action {}
```

公開Headerは`include/hakoniwa/pdu/action/`に配置します。

```text
action_types.hpp
action_client_state_machine.hpp
action_server_state_machine.hpp
action_client_endpoint.hpp
action_server_endpoint.hpp
action_services_client.hpp
action_services_server.hpp
action_services_mux_server.hpp
c_action.h
```

公開HeaderにはApplicationが必要とする型、interface、Services API、C ABIだけを置きます。packet binding、slot routing、送信補助、内部queue、Transport callbackは`src/`のprivate実装です。

## 3. Native source

Native実装は既存の`src/`へ配置します。

```text
action_configuration.cpp
action_client_state_machine.cpp
action_server_state_machine.cpp
action_client_endpoint_impl.cpp
action_server_endpoint_impl.cpp
action_services_client.cpp
action_services_server.cpp
action_services_mux_server.cpp
c_action.cpp
```

独立した汎用Transaction abstractionは設けません。Goal InstanceはServices層の単純な構造体として保持し、Endpoint interfaceへ直接委譲します。

## 4. C ABI

C APIは`include/hakoniwa/pdu/action/c_action.h`で宣言し、`hako_pdu_action_*` prefixを使用します。

- Client、Server、Mux Serverはopaque handleとする。
- Client／Server Goal Handleは別型とする。
- C境界の外へC++例外を出さない。
- Nativeの同期失敗理由を`hako_pdu_action_error_t`へlosslessに写像する。
- `*_alloc()`が返すbufferは`hako_pdu_action_buffer_free()`で解放する。
- C層へ独自のGoal状態機械やtoken registryを作らない。

詳細は[`09-c-api.md`](09-c-api.md)を参照してください。

## 5. Python CFFI

Python package名は`hakoniwa_pdu_rpc`です。Action APIは`python/hakoniwa_pdu_rpc/action_cffi.py`に実装します。

```text
ActionClient
ActionServer
ActionMuxServer
ClientGoalHandle
ServerGoalHandle
ActionErrorCode / ActionError
```

Service RPC、RPC Mux、Actionは`cffi_api.py`の一つの`FFI`定義と、一つの`libhakoniwa_pdu_rpc` loaderを共有します。Action専用shared libraryや別`dlopen()`経路は作りません。

Python層はC APIを薄く写像します。

- Goal状態機械、slot ownership、timeout policyを再実装しない。
- Native bufferをPython `bytes`へcopyした後、Native allocationを必ず解放する。
- Native error codeを`ActionError.code`へ保持する。
- Action APIは明示的poll型とし、Service用`RpcFuture`を暗黙に適用しない。
- ROS型、executor、callbackを本packageへ持ち込まない。

## 6. Build contract

Actionはopt-in build optionではなく、既存RPCと同じlibraryへ常時組み込みます。

```text
static target : hakoniwa_pdu_rpc::rpc
shared target : hakoniwa_pdu_rpc::rpc_shared
compat target : hakoniwa_pdu_rpc::hakoniwa_pdu_rpc
```

libraryの出力名とCMake package名は既存Service RPCから変更しません。Windows shared buildは同じC ABIをDLL exportします。

推奨build interfaceは次です。

```bash
python tools/hako.py doctor
python tools/hako.py build
python tools/hako.py test
python tools/hako.py install
python tools/hako.py package-test
```

`hako.py test`は`hakoniwa_pdu_rpc_reviewed_tests`という一つのCMake集約targetをbuildし、reviewed Service／Action testだけをCTestで実行します。WindowsではGitHub-hosted runnerのメモリ上限に合わせ、reviewed test buildを2並列へ制限します。

## 7. Install contract

installには次を含めます。

- static／shared RPC library
- `include/hakoniwa/pdu/action/`の公開Header
- Registry生成済みAction Headerとsize registry
- exported CMake packageとstatic／shared target
- `--python-venv`指定時の`hakoniwa_pdu_rpc` Python package
- Component Receipt

`package-test`はout-of-tree consumerから次を検証します。

- Service RPC C++ API
- Action Mux C++ APIのstatic／shared link
- Action Mux C APIのstatic／shared link
- install treeのRegistry生成Action Header
- isolated Pythonからのinstalled `ActionMuxServer` import

## 8. Test contract

Contract Testは責務ごとに分離します。

```text
state reducer
configuration / packet codec
Services Goal Instance
Endpoint Goal / Cancel / Feedback / Result
TCP point-to-point E2E
TCP Mux E2E
C API
Python CFFI
installed package consumer
```

状態遷移testはTransportや生成Action bodyに依存させません。TCP E2EとFibonacci exampleは、状態遷移testの代替ではなく、Registry生成型と実Transportを接続する統合検証です。

全体の実行範囲は[`docs/test-contract.md`](../../test-contract.md)を参照してください。
