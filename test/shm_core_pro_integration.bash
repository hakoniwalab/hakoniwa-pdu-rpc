#!/bin/bash
set -euo pipefail

CONDUCTOR=$1
SERVER=$2
CLIENT=$3
DIRECT_CLIENT=$4
FOUNDATION=$5
SCENARIO=$6
WORK=$(mktemp -d "${TMPDIR:-/tmp}/hako-pdu-rpc-shm-${SCENARIO}.XXXXXX")
PIDS=()

cleanup() {
    "$FOUNDATION/bin/hako-cmd" stop >/dev/null 2>&1 || true
    for pid in "${PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    echo "Integration logs: $WORK"
}
trap cleanup EXIT

wait_log() {
    local file=$1 marker=$2 pid=$3
    for ((i=0; i<300; ++i)); do
        grep -qF -- "$marker" "$file" && return 0
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    cat "$file" >&2
    return 1
}

wait_file() {
    local file=$1 pid=$2
    for ((i=0; i<300; ++i)); do
        test -f "$file" && return 0
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    return 1
}

wait_log_count() {
    local file=$1 marker=$2 count=$3 pid=$4
    for ((i=0; i<300; ++i)); do
        test "$(grep -cF -- "$marker" "$file" 2>/dev/null || true)" -ge "$count" && return 0
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
    done
    cat "$file" >&2
    return 1
}

mkdir -p "$WORK/mmap"
export HAKO_CONFIG_PATH="$WORK/core.json"
export HAKO_CORE_MMAP_PATH="$WORK/mmap"
export LD_LIBRARY_PATH="$FOUNDATION/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
printf '{"shm_type":"mmap","core_mmap_path":"%s","asset_timeout_usec":600000000}\n' \
    "$WORK/mmap" > "$HAKO_CONFIG_PATH"

ADD_MAX_CLIENTS=1
if [[ "$SCENARIO" == "compare" || "$SCENARIO" == "busy" ]]; then
    ADD_MAX_CLIENTS=2
fi
cat > "$WORK/service.json" <<JSON
{
  "pduMetaDataSize": 24,
  "services": [
    {
      "name": "Service/Add",
      "type": "hako_srv_msgs/AddTwoInts",
      "maxClients": $ADD_MAX_CLIENTS,
      "pduSize": {
        "server": {"heapSize": 0, "baseSize": 296},
        "client": {"heapSize": 0, "baseSize": 288}
      },
      "clients": [
        {
          "name": "ShmClient",
          "client_endpoint": {"nodeId": "shm_client_node", "endpointId": "shm_client_ep"}
        }
      ]
    },
    {
      "name": "Service/Add2",
      "type": "hako_srv_msgs/AddTwoInts",
      "maxClients": 1,
      "pduSize": {
        "server": {"heapSize": 0, "baseSize": 296},
        "client": {"heapSize": 0, "baseSize": 288}
      },
      "clients": [
        {
          "name": "ShmClient",
          "client_endpoint": {"nodeId": "shm_client_node", "endpointId": "shm_client_ep"}
        }
      ]
    }
  ]
}
JSON
cat > "$WORK/endpoints.json" <<'JSON'
[
  {
    "nodeId": "shm_client_node",
    "endpoints": [
      {"id": "shm_client_ep", "config_path": "endpoint.json"}
    ]
  }
]
JSON
cat > "$WORK/endpoint.json" <<'JSON'
{"comm":"comm.json"}
JSON
cat > "$WORK/comm.json" <<JSON
{
  "protocol": "shm",
  "assetName": "PduRpcShmClient",
  "coreConfig": "$HAKO_CONFIG_PATH",
  "deltaTimeUsec": 1000
}
JSON

"$CONDUCTOR" >"$WORK/conductor.log" 2>&1 &
PIDS+=("$!")
wait_log "$WORK/conductor.log" "CONDUCTOR started" "${PIDS[-1]}"

"$SERVER" "$WORK/service.json" >"$WORK/server.log" 2>&1 &
PIDS+=("$!")
wait_log "$WORK/server.log" "SERVER registered" "${PIDS[-1]}"

if [[ "$SCENARIO" == "compare" || "$SCENARIO" == "busy" ]]; then
    "$DIRECT_CLIENT" "$WORK/service.json" "$SCENARIO" "$WORK" >"$WORK/direct.log" 2>&1 &
    PIDS+=("$!")
    wait_log "$WORK/direct.log" "DIRECT registered" "${PIDS[-1]}"
fi

"$CLIENT" "$WORK/service.json" "$WORK/endpoints.json" "$SCENARIO" "$WORK" \
    >"$WORK/client.log" 2>&1 &
PIDS+=("$!")
CLIENT_PID=${PIDS[-1]}
wait_log "$WORK/client.log" "CLIENT registered" "$CLIENT_PID"

# The pdu-rpc asset registers on its background thread. Give the polling
# registration time to reach the conductor before issuing Start.
sleep 0.2
"$FOUNDATION/bin/hako-cmd" start >"$WORK/start.log" 2>&1
: > "$WORK/start-go"

case "$SCENARIO" in
    ok)
        wait_log "$WORK/client.log" "PASS shm core-pro OK sum=42" "$CLIENT_PID"
        ;;
    compare)
        wait_log "$WORK/client.log" "PASS S2(a)" "$CLIENT_PID"
        for result in ok invalid canceled; do
            cmp "$WORK/direct-${result}.body" "$WORK/rpc-${result}.body"
        done
        echo "PASS S2(a) OK/INVALID/CANCELED bodies byte-identical to direct polling client"
        ;;
    calls500)
        wait_log "$WORK/client.log" "PASS S2(b) 500/500" "$CLIENT_PID"
        echo "PASS S2(b) 500/500 consecutive calls aligned"
        ;;
    busy)
        wait_log "$WORK/direct.log" "PASS direct core-pro BUSY interim path" "${PIDS[-2]}"
        wait_log "$WORK/client.log" "PASS S2(c) second call refused" "$CLIENT_PID"
        echo "PASS S2(c) core-pro BUSY path preserved"
        ;;
    timeout)
        wait_log "$WORK/client.log" "PASS S2(d)" "$CLIENT_PID"
        echo "PASS S2(d) timeout -> cancel -> CANCELED -> next OK"
        ;;
    time)
        wait_log "$WORK/client.log" "PASS S2(e)" "$CLIENT_PID"
        echo "PASS S2(e) client asset time advanced during server delay"
        ;;
    multi)
        wait_log "$WORK/client.log" "PASS S2(f)" "$CLIENT_PID"
        echo "PASS S2(f) two shm endpoints shared one polling asset"
        ;;
    restart)
        wait_log "$WORK/server.log" "SERVER request a=-1000 b=9" "${PIDS[1]}"
        "$FOUNDATION/bin/hako-cmd" stop >"$WORK/stop-inflight.log" 2>&1
        wait_file "$WORK/restart-error.done" "$CLIENT_PID"
        "$FOUNDATION/bin/hako-cmd" reset >"$WORK/reset.log" 2>&1
        wait_log "$WORK/server.log" "SERVER event Reset" "${PIDS[1]}"
        sleep 0.2
        "$FOUNDATION/bin/hako-cmd" start >"$WORK/restart.log" 2>&1
        wait_log_count "$WORK/server.log" "SERVER event Start" 2 "${PIDS[1]}"
        : > "$WORK/restart-go"
        wait_log "$WORK/client.log" "PASS S2(g) call succeeded" "$CLIENT_PID"
        echo "PASS S2(g) stop/reset error and restart recovery"
        ;;
    *)
        echo "Unknown scenario: $SCENARIO" >&2
        exit 2
        ;;
esac

"$FOUNDATION/bin/hako-cmd" stop >"$WORK/stop.log" 2>&1
echo "PASS integration harness stop ($SCENARIO)"
