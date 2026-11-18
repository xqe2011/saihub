#!/usr/bin/env bash
set -euo pipefail

SERVER_BIN="${SERVER_BIN:-./build/saihub-cloud}"
BENCH_BIN="${BENCH_BIN:-./build/bench-load}"
DEVICES="${DEVICES:-10000}"
STREAMS="${STREAMS:-50}"
DURATION="${DURATION:-15}"
STREAM_HZ="${STREAM_HZ:-20}"
STREAM_BYTES="${STREAM_BYTES:-512}"
MODE="${1:-directional}" # directional | symmetric

cleanup() {
    if [[ -n "${SRV_PID:-}" ]]; then
        kill -TERM "${SRV_PID}" 2>/dev/null || true
        wait "${SRV_PID}" 2>/dev/null || true
    fi
    ip netns del ns_server 2>/dev/null || true
    ip link del veth_dev 2>/dev/null || true
    ip link del veth_cli 2>/dev/null || true
}
trap cleanup EXIT

cleanup 2>/dev/null || true

# Create isolated server netns with two veth pairs:
# - 10.10.1.0/24: device <-> server
# - 10.10.2.0/24: client <-> server
ip netns add ns_server
ip link add veth_dev type veth peer name veth_srv_dev
ip link add veth_cli type veth peer name veth_srv_cli
ip link set veth_srv_dev netns ns_server
ip link set veth_srv_cli netns ns_server

ip addr add 10.10.1.2/24 dev veth_dev
ip addr add 10.10.2.2/24 dev veth_cli
ip link set veth_dev up
ip link set veth_cli up

ip netns exec ns_server ip link set lo up
ip netns exec ns_server ip addr add 10.10.1.1/24 dev veth_srv_dev
ip netns exec ns_server ip addr add 10.10.2.1/24 dev veth_srv_cli
ip netns exec ns_server ip link set veth_srv_dev up
ip netns exec ns_server ip link set veth_srv_cli up

# Apply tc netem delays with large queue limit (100000 packets)
tc qdisc add dev veth_dev root netem delay 200ms limit 100000
tc qdisc add dev veth_cli root netem delay 50ms limit 100000

if [[ "${MODE}" == "symmetric" ]]; then
    ip netns exec ns_server tc qdisc add dev veth_srv_dev root netem delay 200ms limit 100000
    ip netns exec ns_server tc qdisc add dev veth_srv_cli root netem delay 50ms limit 100000
fi

echo "=== Mode: ${MODE} ==="
echo "--- Ping RTT verification ---"
ping -c 2 -W 1 10.10.1.1 | tail -n 2
ping -c 2 -W 1 10.10.2.1 | tail -n 2

export HOST=0.0.0.0
export PORT=18787
export AUTH_TIMEOUT_MS=60000
export REQUEST_TIMEOUT_MS=55000
export ROUTING_TOKEN_SECRET=local-dev-routing-token-secret
export WEBSOCKET_TOKEN_SECRET=local-dev-websocket-token-secret
export WEBSOCKET_TOKEN_TTL_S=7200
export ADMIN_TOKEN=local-dev-admin-token
export DB_PATH=/tmp/saihub-netem.db
export LOG_LEVEL=warn
rm -f /tmp/saihub-netem.db /tmp/saihub-netem.db-wal /tmp/saihub-netem.db-shm

ip netns exec ns_server taskset -c 0 "${SERVER_BIN}" > /tmp/srv.log 2>&1 &
SRV_PID=$!
sleep 0.3
cat /tmp/srv.log

# Start bench-load and measure server steady-state CPU/RSS via /proc
python3 - <<PY
import os, subprocess, time

srv_pid = ${SRV_PID}
clk_tck = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
page_size = os.sysconf(os.sysconf_names["SC_PAGESIZE"])

def read_stat(pid):
    with open(f"/proc/{pid}/stat") as f:
        raw = f.read()
    rp = raw.rfind(")")
    fields = raw[rp + 2:].split()
    return (int(fields[11]) + int(fields[12])) / clk_tck, int(fields[21]) * page_size

cmd = [
    "${BENCH_BIN}",
    "--device-host", "10.10.1.1",
    "--stream-host", "10.10.2.1",
    "--port", "18787",
    "--devices", "${DEVICES}",
    "--streams", "${STREAMS}",
    "--duration", "${DURATION}",
    "--stream-hz", "${STREAM_HZ}",
    "--stream-bytes", "${STREAM_BYTES}",
]
p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
while True:
    line = p.stdout.readline()
    if not line:
        break
    print(line.strip(), flush=True)
    if line.startswith("STEADY_START"):
        break

time.sleep(1.5)
c0, _ = read_stat(srv_pid)
t0 = time.monotonic()
time.sleep(max(2, ${DURATION} - 3))
c1, rss1 = read_stat(srv_pid)
t1 = time.monotonic()

out, err = p.communicate(timeout=30)
if err:
    print("STDERR:", err.strip())
if out.strip():
    print(out.strip())

dt = t1 - t0
dc = c1 - c0
print(f"SERVER_STEADY_CPU window_s={dt:.2f} cpu_s={dc:.3f} cpu_pct={(dc/dt)*100:.2f}% rss_mib={rss1/(1024*1024):.2f}")
PY
