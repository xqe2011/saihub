#!/usr/bin/env python3
import argparse
import os
import signal
import subprocess
import tempfile
import time

def read_proc_stats(pid: int):
    with open(f"/proc/{pid}/stat", "r") as f:
        raw = f.read()
    rp = raw.rfind(")")
    fields = raw[rp + 2:].split()
    utime = int(fields[11])
    stime = int(fields[12])
    rss_pages = int(fields[21])
    clk_tck = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    page_size = os.sysconf(os.sysconf_names["SC_PAGESIZE"])
    return (utime + stime) / clk_tck, rss_pages * page_size

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server-bin", required=True)
    ap.add_argument("--bench-bin", required=True)
    ap.add_argument("--port", type=int, default=18787)
    ap.add_argument("--devices", type=int, default=10000)
    ap.add_argument("--streams", type=int, default=50)
    ap.add_argument("--duration", type=int, default=15)
    ap.add_argument("--stream-hz", type=int, default=20)
    ap.add_argument("--stream-bytes", type=int, default=256)
    ap.add_argument("--batch", type=int, default=50)
    ap.add_argument("--cpu", type=int, default=0)
    ap.add_argument("--db-path", default="")
    args = ap.parse_args()

    tmp_db = None
    db_path = args.db_path
    if not db_path:
        tmp_db = tempfile.NamedTemporaryFile(prefix="saihub-bench-", suffix=".db", delete=False)
        db_path = tmp_db.name
        tmp_db.close()

    env = os.environ.copy()
    env["HOST"] = "127.0.0.1"
    env["PORT"] = str(args.port)
    env["AUTH_TIMEOUT_MS"] = "60000"
    env["REQUEST_TIMEOUT_MS"] = "55000"
    env["ROUTING_TOKEN_SECRET"] = "local-dev-routing-token-secret"
    env["WEBSOCKET_TOKEN_SECRET"] = "local-dev-websocket-token-secret"
    env["WEBSOCKET_TOKEN_TTL_S"] = "7200"
    env["ADMIN_TOKEN"] = "local-dev-admin-token"
    env["DB_PATH"] = db_path
    env["LOG_LEVEL"] = "warn"

    srv_cmd = ["taskset", "-c", str(args.cpu), args.server_bin] if args.cpu >= 0 else [args.server_bin]
    srv = subprocess.Popen(srv_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    try:
        line = srv.stdout.readline()
        print("Server:", line.strip())

        bench_cmd = [
            args.bench_bin,
            "--host", "127.0.0.1",
            "--port", str(args.port),
            "--devices", str(args.devices),
            "--streams", str(args.streams),
            "--duration", str(args.duration),
            "--stream-hz", str(args.stream_hz),
            "--stream-bytes", str(args.stream_bytes),
            "--batch", str(args.batch),
        ]
        bench = subprocess.Popen(bench_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        steady_started = False
        while True:
            bline = bench.stdout.readline()
            if not bline:
                break
            print(bline.strip())
            if bline.startswith("STEADY_START"):
                steady_started = True
                break

        if not steady_started:
            raise RuntimeError("Benchmark did not reach STEADY_START")

        time.sleep(1.0)
        cpu0, _ = read_proc_stats(srv.pid)
        t0 = time.monotonic()

        time.sleep(max(2, args.duration - 2))
        cpu1, rss1 = read_proc_stats(srv.pid)
        t1 = time.monotonic()

        bench_out, bench_err = bench.communicate(timeout=30)
        if bench_err:
            print("Bench stderr:", bench_err)
        if bench_out.strip():
            print(bench_out.strip())

        dt = t1 - t0
        dcpu = cpu1 - cpu0
        print(
            f"SERVER_STEADY_CPU"
            f" window_s={dt:.2f}"
            f" cpu_s={dcpu:.3f}"
            f" cpu_pct={(dcpu / dt) * 100.0:.2f}%"
            f" rss_mib={rss1 / (1024 * 1024):.2f}"
        )
    finally:
        srv.send_signal(signal.SIGTERM)
        try:
            srv.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            srv.kill()
        if tmp_db is not None:
            for ext in ("", "-wal", "-shm"):
                try:
                    os.unlink(db_path + ext)
                except FileNotFoundError:
                    pass

if __name__ == "__main__":
    main()
