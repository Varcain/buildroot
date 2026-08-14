#!/usr/bin/env python3
"""Run or offline-validate the native-Linux STM32F746 hammer."""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVER_SOURCE = ROOT / "scripts" / "hammer_stream_server.py"
SYSMON_RE = re.compile(
    r"sysmon:\s+(?P<fps>\d+) FPS \(refr_cnt: (?P<refr>\d+) \| "
    r"redraw_cnt: (?P<redraw>\d+)\), refr (?P<refr_ms>\d+)ms "
    r"\(render (?P<render_ms>\d+)ms \| flush (?P<flush_ms>\d+)ms\), "
    r"CPU (?P<cpu>\d+)%"
)


def run(argv, timeout, check=True):
    result = subprocess.run(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        timeout=timeout,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {argv!r}\n{result.stdout}"
        )
    return result


def ssh_args(host, command, jump=None, config=None):
    argv = ["ssh"]
    if config:
        argv += ["-F", str(config)]
    argv += [
        "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=accept-new",
    ]
    if jump:
        argv += ["-J", jump]
    return argv + [host, command]


def pi_exec(pi, command, timeout=30, config=None):
    return run(ssh_args(pi, command, config=config), timeout=timeout).stdout


def deploy_server(pi, config=None):
    remote = "/tmp/native-linux-hammer-stream-server.py"
    scp = ["scp"]
    if config:
        scp += ["-F", str(config)]
    run(scp + [
        "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
        "-o", "StrictHostKeyChecking=accept-new", str(SERVER_SOURCE),
        f"{pi}:{remote}",
    ], timeout=60)
    command = (
        "pidfile=/tmp/native-linux-hammer-stream-server.pid; "
        "if test -r \"$pidfile\"; then read old_pid <\"$pidfile\"; "
        "if test -r /proc/$old_pid/cmdline && "
        "tr '\\0' ' ' </proc/$old_pid/cmdline | "
        "grep -q native-linux-hammer-stream-server.py; then "
        "kill \"$old_pid\" 2>/dev/null || true; fi; fi; "
        f"nohup python3 {remote} >/tmp/native-linux-hammer-stream-server.log "
        "2>&1 </dev/null & echo $! >\"$pidfile\"; sleep 1; "
        "curl -fsS http://127.0.0.1:8082/metrics"
    )
    return json.loads(pi_exec(pi, command, config=config))


def pi_http(pi, endpoint, config=None):
    output = pi_exec(
        pi, f"curl -fsS http://127.0.0.1:8082/{endpoint}", timeout=30,
        config=config,
    )
    return output


def parse_key_values(text, marker):
    match = re.search(re.escape(marker) + r"([^\r\n]*)", text)
    if not match:
        return {}
    values = {}
    for field in match.group(1).strip().split():
        if "=" not in field:
            continue
        key, value = field.split("=", 1)
        if re.fullmatch(r"-?\d+", value):
            values[key] = int(value)
        else:
            try:
                values[key] = float(value)
            except ValueError:
                values[key] = value
    return values


def parse_json_marker(text, marker):
    match = re.search(re.escape(marker) + r"(\{[^\r\n]*\})", text)
    if not match:
        return {"error": "marker missing"}
    try:
        return json.loads(match.group(1))
    except json.JSONDecodeError as error:
        return {"error": f"invalid JSON: {error}"}


def parse_rt_scope(text):
    match = re.search(
        r"__HAMMER_RT_SCOPE_BEGIN__\s*(.*?)__HAMMER_RT_SCOPE_END__",
        text, re.S,
    )
    if not match:
        return None
    raw = {}
    for line in match.group(1).splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) != 2:
            continue
        key, value = fields
        if re.fullmatch(r"\d+", value):
            raw[key] = int(value)
        else:
            raw[key] = value
    if raw.get("available") != 1:
        return None
    histogram = [
        {
            "upper_us": upper,
            "count": raw.get(f"dispatch_hist_le_{upper}_us", 0),
        }
        for upper in (1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 32,
                      50, 100, 250, 500, 750, 1000)
    ]
    return {
        "measurement": raw.get("measurement"),
        "physical_reference": True,
        "reference_pin": raw.get("reference_pin"),
        "response_pin": raw.get("response_pin"),
        "period_ns": raw.get("period_us", 0) * 1000,
        "reference_high_us": raw.get("reference_high_us"),
        "timer_hz": raw.get("timer_hz"),
        "work_iterations": raw.get("work_iterations"),
        "measurement_elapsed_ns": raw.get("measurement_elapsed_ns"),
        "scheduler": raw.get("scheduler"),
        "priority": raw.get("priority"),
        "releases": raw.get("releases"),
        "executions": raw.get("executions"),
        "missed_releases": raw.get("missed"),
        "late_finishes": raw.get("late_finish"),
        "irq_overrun": raw.get("irq_overrun"),
        "pending": raw.get("pending"),
        "max_consecutive_missed": raw.get("max_consecutive_missed"),
        "oldest_release_age_max_ns": raw.get("oldest_release_age_max_ns"),
        "irq_entry_age_max_ns": raw.get("irq_entry_age_max_ns"),
        "irq_signal_age_max_ns": raw.get("irq_signal_age_max_ns"),
        "dispatch_ns": {
            "min": raw.get("dispatch_min_ns"),
            "average": raw.get("dispatch_avg_ns"),
            "p99_upper_us": raw.get("dispatch_p99_us_ceiling"),
            "p99_9_upper_us": raw.get("dispatch_p999_us_ceiling"),
            "max": raw.get("dispatch_max_ns"),
            "jitter": raw.get("dispatch_jitter_ns"),
        },
        "work_ns": {
            "min": raw.get("work_min_ns"),
            "max": raw.get("work_max_ns"),
        },
        "dispatch_histogram": histogram,
        "proc_values": raw,
    }


def percentile(values, fraction):
    ordered = sorted(values)
    index = round((len(ordered) - 1) * fraction)
    return ordered[index]


def distribution(values):
    if not values:
        return {}
    return {
        "count": len(values),
        "min": min(values),
        "p05": percentile(values, 0.05),
        "median": percentile(values, 0.50),
        "mean": sum(values) / len(values),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values),
    }


def parse_lvgl(text):
    block = re.search(
        r"__LVGL_LOG_BEGIN__\s*(.*?)__LVGL_LOG_END__", text, re.S
    )
    if not block:
        return {"samples": 0, "active_samples": 0, "distributions": {}}
    samples = [
        {key: int(value) for key, value in match.groupdict().items()}
        for match in SYSMON_RE.finditer(block.group(1))
    ]
    active = [
        sample for sample in samples
        if sample["fps"] > 0 and sample["redraw"] > 0 and
        sample["render_ms"] > 0
    ]
    distributions = {
        field: distribution([sample[field] for sample in active])
        for field in ("fps", "refr_ms", "render_ms", "flush_ms", "cpu")
    }
    return {
        "samples": len(samples),
        "active_samples": len(active),
        "distributions": distributions,
    }


def snapshot_block(text, label):
    match = re.search(
        rf"__SNAPSHOT_{label}_BEGIN__\s*(.*?)"
        rf"__SNAPSHOT_{label}_END__", text, re.S
    )
    return match.group(1) if match else ""


def section(block, name):
    match = re.search(rf"\[{re.escape(name)}\]\s*(.*?)(?=\n\[|\Z)",
                      block, re.S)
    return match.group(1).strip() if match else ""


def cpu_values(block):
    line = next(
        (line for line in section(block, "stat").splitlines()
         if line.startswith("cpu ")), ""
    )
    fields = line.split()[1:]
    if not fields or not all(field.isdigit() for field in fields):
        return None
    values = [int(field) for field in fields]
    return {"total": sum(values), "idle": sum(values[3:5])}


def process_values(block):
    values = {}
    for line in section(block, "processes").splitlines():
        match = re.match(r"(\d+) \((.*)\) (.*)", line)
        if not match:
            continue
        rest = match.group(3).split()
        if len(rest) < 15:
            continue
        values[match.group(1)] = {
            "name": match.group(2),
            "user_jiffies": int(rest[11]),
            "system_jiffies": int(rest[12]),
            "children_user_jiffies": int(rest[13]),
            "children_system_jiffies": int(rest[14]),
        }
    return values


def cpu_summary(text):
    before_block = snapshot_block(text, "BEFORE")
    after_block = snapshot_block(text, "AFTER")
    before_cpu = cpu_values(before_block)
    after_cpu = cpu_values(after_block)
    result = {
        "before_top": section(before_block, "top"),
        "after_top": section(after_block, "top"),
        "process_jiffy_delta": {},
    }
    if before_cpu and after_cpu:
        total = after_cpu["total"] - before_cpu["total"]
        idle = after_cpu["idle"] - before_cpu["idle"]
        if total > 0:
            result["total_jiffies"] = total
            result["overall_busy_percent"] = (total - idle) * 100.0 / total
    before_processes = process_values(before_block)
    after_processes = process_values(after_block)
    for pid, before in before_processes.items():
        after = after_processes.get(pid)
        if not after or after["name"] != before["name"]:
            continue
        delta = {
            "name": before["name"],
            **{
                key: after[key] - before[key]
                for key in (
                    "user_jiffies", "system_jiffies",
                    "children_user_jiffies", "children_system_jiffies",
                )
            },
        }
        self_jiffies = delta["user_jiffies"] + delta["system_jiffies"]
        children_jiffies = (
            delta["children_user_jiffies"] +
            delta["children_system_jiffies"]
        )
        delta["self_jiffies"] = self_jiffies
        delta["children_jiffies"] = children_jiffies
        if before_cpu and after_cpu and total > 0:
            delta["self_cpu_percent"] = self_jiffies * 100.0 / total
            delta["with_children_cpu_percent"] = (
                (self_jiffies + children_jiffies) * 100.0 / total
            )
        result["process_jiffy_delta"][pid] = delta
    return result


DISKSTAT_FIELDS = (
    "reads_completed", "reads_merged", "sectors_read", "read_ms",
    "writes_completed", "writes_merged", "sectors_written", "write_ms",
    "io_in_progress", "io_ms", "weighted_io_ms", "discards_completed",
    "discards_merged", "sectors_discarded", "discard_ms", "flush_completed",
    "flush_ms",
)


def diskstats_values(block):
    devices = {}
    for line in section(block, "diskstats").splitlines():
        fields = line.split()
        if len(fields) < 14 or not all(value.isdigit() for value in fields[:2]):
            continue
        counters = fields[3:]
        if not all(value.isdigit() for value in counters):
            continue
        devices[fields[2]] = {
            name: int(value)
            for name, value in zip(DISKSTAT_FIELDS, counters)
        }
    return devices


def data_mount(block):
    for line in section(block, "mounts").splitlines():
        fields = line.split()
        if len(fields) >= 4 and fields[1] == "/data":
            return {
                "device": fields[0],
                "mountpoint": fields[1],
                "filesystem": fields[2],
                "options": fields[3].split(","),
            }
    return None


def storage_summary(text):
    before_block = snapshot_block(text, "BEFORE")
    after_block = snapshot_block(text, "AFTER")
    before = diskstats_values(before_block)
    after = diskstats_values(after_block)
    devices = {}
    for name in sorted(before.keys() & after.keys()):
        delta = {
            field: after[name].get(field, 0) - before[name].get(field, 0)
            for field in DISKSTAT_FIELDS
            if field in before[name] and field in after[name]
        }
        delta["bytes_read"] = delta.get("sectors_read", 0) * 512
        delta["bytes_written"] = delta.get("sectors_written", 0) * 512
        devices[name] = {
            "before": before[name],
            "after": after[name],
            "delta": delta,
        }
    return {
        "data_mount_before": data_mount(before_block),
        "data_mount_after": data_mount(after_block),
        "devices": devices,
    }


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def matching_build_file(build_dir, directory_glob, filename):
    matches = sorted(build_dir.glob(directory_glob))
    if len(matches) != 1:
        return None
    return matches[0] / filename


def scheduler_from_kernel_config(config):
    if config is None or not config.is_file():
        return "unknown"
    text = config.read_text(errors="replace")
    for symbol in ("CONFIG_PREEMPT", "CONFIG_PREEMPT_VOLUNTARY",
                   "CONFIG_PREEMPT_NONE"):
        if re.search(rf"^{symbol}=y$", text, re.M):
            return symbol
    return "unknown"


def record_build_metadata(output, build_output, build_dir=None,
                          configuration_paths=None, artifact_paths=None):
    build_dir = build_dir or build_output / "build"
    linux_config = matching_build_file(build_dir, "linux-[0-9]*", ".config")
    if configuration_paths:
        files = dict(configuration_paths)
        linux_config = files.get("linux.config", linux_config)
    else:
        files = {
            "buildroot.config": build_output / ".config",
            "linux.config": linux_config,
            "uboot.config": matching_build_file(
                build_dir, "uboot-*", ".config"
            ),
            "busybox.config": matching_build_file(
                build_dir, "busybox-*", ".config"
            ),
            "lvgl.config": matching_build_file(
                build_dir, "native-linux-hammer-*", "lv_conf.h"
            ),
        }
    copied = {}
    for name, source in files.items():
        if source is not None and source.is_file():
            destination = output / name
            shutil.copy2(source, destination)
            copied[name] = sha256(destination)
    if artifact_paths:
        image_files = dict(artifact_paths)
    else:
        image_files = {
            name: build_output / "images" / name
            for name in (
                "rootfs.cramfs", "zImage", "xipImage", "uImage.xip",
                "qspi-hammer-xip.img", "qspi-hammer-xip.manifest",
                "stm32f746-disco-hammer.dtb", "u-boot.bin",
            )
        }
    images = {
        name: {
            "path": str(path),
            "bytes": path.stat().st_size,
            "sha256": sha256(path),
        }
        for name, path in image_files.items()
        if path.is_file()
    }
    commit = run(["git", "rev-parse", "HEAD"], timeout=10).stdout.strip()
    status = run(["git", "status", "--short"], timeout=10).stdout
    return {
        "buildroot_commit": commit,
        "worktree_status": status.splitlines(),
        "identities": {
            "buildroot": "2026.08-git",
            "linux": "5.15.211",
            "uboot": "2026.07",
            "lvgl": "9.5.0",
            "architecture": "ARM Cortex-M7 NOMMU FDPIC",
            "scheduler": scheduler_from_kernel_config(linux_config),
        },
        "configuration_sha256": copied,
        "images": images,
        "build_output": str(build_output),
    }


def named_paths(values, option):
    paths = []
    for value in values:
        if "=" not in value:
            raise ValueError(f"{option} requires LABEL=PATH: {value}")
        label, raw_path = value.split("=", 1)
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", label):
            raise ValueError(f"invalid {option} label: {label}")
        path = Path(raw_path)
        if not path.is_file():
            raise ValueError(f"{option} path is not a file: {path}")
        paths.append((label, path))
    return paths


def archive_build_only(output, build_output, build_dir, blockers):
    output.mkdir(parents=True, exist_ok=True)
    summary = {
        "status": "BUILD_PASS",
        "phase": "native-linux-image",
        "hardware_execution": "NOT_RUN",
        "hardware_blockers": blockers,
        "captured_at_utc": datetime.now(timezone.utc).isoformat(),
        "image_contract": {
            "root": "read-only XIP-enabled CramFS in QSPI",
            "data": "VFAT on SD partition 1 (data-only) or legacy partition 2",
            "shared_physical_medium": False,
            "sd_bus_max_frequency_hz": 2000000,
            "rendering": "software LVGL draw + Linux fbdev pwrite; no DMA2D",
            "latency": "TIM3/PB4 hardware edge to SCHED_FIFO kernel thread/PG7",
        },
        "build": record_build_metadata(output, build_output, build_dir),
    }
    (output / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(f"BUILD_PASS: hardware=NOT_RUN output={output}")


def target_identity(target, jump, config=None):
    command = (
        "printf '[uname]\\n'; uname -a; "
        "printf '[cmdline]\\n'; cat /proc/cmdline; "
        "printf '[meminfo]\\n'; cat /proc/meminfo; "
        "printf '[interrupts]\\n'; cat /proc/interrupts; "
        "printf '[rt-scope]\\n'; test ! -r /proc/rt_scope || cat /proc/rt_scope; "
        "printf '[pinctrl]\\n'; for p in /sys/kernel/debug/pinctrl/*/pinmux-pins; "
        "do test -r \"$p\" && echo \"$p\" && cat \"$p\"; done; "
        "printf '[pwm]\\n'; for p in /sys/class/pwm/pwmchip*; "
        "do test -e \"$p\" && echo \"$p\"; done; "
        "printf '[mounts]\\n'; cat /proc/mounts; "
        "printf '[ip]\\n'; ip addr; "
        "printf '[framebuffers]\\n'; "
        "for p in /sys/class/graphics/fb*/name; do test -r \"$p\" && "
        "echo \"$p:$(cat \"$p\")\"; done; "
        "printf '[inputs]\\n'; "
        "for p in /sys/class/input/event*/device/name; do test -r \"$p\" && "
        "echo \"$p:$(cat \"$p\")\"; done; "
        "printf '[uptime]\\n'; cat /proc/uptime"
    )
    return run(ssh_args(target, command, jump, config), timeout=60).stdout


def analyze(duration, wall_seconds, returncode, text, server_metrics):
    sqlite = parse_key_values(text, "__HAMMER_SQLITE__:")
    timing = parse_key_values(text, "__HAMMER_TIMING__:")
    network = parse_json_marker(text, "__HAMMER_NETWORK__:")
    latency = parse_rt_scope(text)
    if latency is None:
        latency = parse_json_marker(text, "__HAMMER_LATENCY__:")
    lvgl = parse_lvgl(text)
    failures = []
    kernel_fault_patterns = {
        "kernel OOM": (
            r"invoked oom-killer", r"Out of memory: Killed process",
        ),
        "kernel oops": (r"\bOops:", r"\bBUG:", r"Kernel panic"),
    }
    for fault_name, patterns in kernel_fault_patterns.items():
        if any(re.search(pattern, text, re.I) for pattern in patterns):
            failures.append(f"{fault_name} observed during benchmark capture")
    transactions = sqlite.get("transactions")
    expected_rows = transactions * 8 if isinstance(transactions, int) else None
    if returncode != 0:
        failures.append(f"SSH/benchmark exit status {returncode}")
    if "__HAMMER_END__:native-linux-shell" not in text:
        failures.append("completion marker missing")
    if not isinstance(transactions, int) or transactions <= 0:
        failures.append("no SQLite transactions")
    if sqlite.get("rows") != expected_rows or sqlite.get("meta") != expected_rows:
        failures.append("rows/meta do not equal transactions * 8")
    expected_live = min(expected_rows, 128) if expected_rows is not None else None
    if sqlite.get("live_rows") != expected_live:
        failures.append(f"live_rows={sqlite.get('live_rows')} expected={expected_live}")
    if sqlite.get("integrity") != "ok" or sqlite.get("error") != "none":
        failures.append(
            f"SQLite integrity/error={sqlite.get('integrity')}/{sqlite.get('error')}"
        )
    error_match = re.search(r"__HAMMER_DB_ERROR_BYTES__:(\d+)", text)
    error_bytes = int(error_match.group(1)) if error_match else None
    if error_bytes != 0:
        failures.append(f"SQLite stderr bytes={error_bytes}")
    if network.get("requested_s") != duration:
        failures.append(f"network duration={network.get('requested_s')} expected={duration}")
    if network.get("completed") != 1 or network.get("errors") != 0:
        failures.append("network did not complete cleanly")
    if network.get("bytes", 0) <= 0 or network.get("elapsed_s", 0) < duration:
        failures.append("network transfer was empty or ended before its deadline")
    if server_metrics != network:
        failures.append("guest and server network metrics differ")
    # Keep the same settled-playback sufficiency threshold as the established
    # LXP + oveRTOS hardware harness. Under overload, one LVGL perf interval can
    # cover several seconds, so duration is not a valid minimum sample count.
    required_samples = 30 if duration >= 30 else 1
    if lvgl["active_samples"] < required_samples:
        failures.append(
            f"active LVGL samples={lvgl['active_samples']} required={required_samples}"
        )
    try:
        sqlite_elapsed = timing["ended_uptime"] - timing["started_uptime"]
    except (KeyError, TypeError):
        sqlite_elapsed = None
        failures.append("SQLite elapsed time missing")
    if sqlite_elapsed is not None and sqlite_elapsed < duration:
        failures.append(
            f"SQLite elapsed={sqlite_elapsed:.2f}s ended before {duration}s deadline"
        )
    if sqlite_elapsed and transactions:
        sqlite["elapsed_s"] = sqlite_elapsed
        sqlite["transactions_per_second"] = transactions / sqlite_elapsed
    planned = duration * 1000
    if latency.get("physical_reference"):
        if latency.get("measurement") != "tim3-hardware-to-sched-fifo-kthread":
            failures.append("physical scope result has the wrong measurement class")
        releases = latency.get("releases")
        executions = latency.get("executions", 0)
        missed = latency.get("missed_releases", 0)
        pending = latency.get("pending", 0)
        if not isinstance(releases, int) or releases < planned:
            failures.append(
                f"physical scope releases={releases} shorter than {planned}"
            )
        measurement_elapsed_ns = latency.get("measurement_elapsed_ns")
        if isinstance(measurement_elapsed_ns, int) and \
                measurement_elapsed_ns > 0:
            scope_elapsed = measurement_elapsed_ns / 1_000_000_000
        else:
            try:
                scope_elapsed = (
                    timing["scope_captured_uptime"] - timing["started_uptime"]
                )
            except (KeyError, TypeError):
                scope_elapsed = None
                failures.append("physical scope capture time missing")
        if scope_elapsed is not None:
            latency["elapsed_s"] = scope_elapsed
        if isinstance(releases, int) and scope_elapsed is not None:
            expected_releases = round(scope_elapsed * 1000)
            if abs(releases - expected_releases) > 50:
                failures.append(
                    "physical scope duration mismatch: "
                    f"releases={releases} expected~={expected_releases}"
                )
        pending_execution = 1 if pending else 0
        if isinstance(releases, int) and \
                executions + missed + pending_execution != releases:
            failures.append("physical scope execution/miss/pending accounting is inconsistent")
        if latency.get("reference_pin") != "PB4_TIM3_CH1_Arduino_D3" or \
                latency.get("response_pin") != "PG7_GPIO_Arduino_D4":
            failures.append("physical scope D3/D4 pin contract is wrong")
    else:
        if latency.get("measurement") != "timer-to-sched-fifo":
            failures.append("latency result missing or wrong measurement class")
        if latency.get("releases") != planned:
            failures.append(
                f"latency releases={latency.get('releases')} expected={planned}"
            )
        if latency.get("executions", 0) + \
                latency.get("missed_releases", 0) != planned:
            failures.append("latency execution/miss accounting is inconsistent")
    network_mbps = (
        network.get("bytes", 0) * 8 /
        max(float(network.get("elapsed_s", 0)), 0.001) / 1_000_000
    )
    return {
        "status": "PASS" if not failures else "FAIL",
        "failures": failures,
        "requested_duration_s": duration,
        "wall_s": wall_seconds,
        "sqlite": sqlite,
        "sqlite_error_bytes": error_bytes,
        "network": network,
        "network_mbps": network_mbps,
        "lvgl": lvgl,
        "cpu": cpu_summary(text),
        "storage": storage_summary(text),
        "latency": latency,
        "measurement_limitations": [
            "Linux uses a SCHED_FIFO kernel thread while oveRTOS uses its portable "
            "highest-priority host task; both share the TIM3/PB4-to-PG7 physical path.",
            "LVGL uses software rendering and Linux fbdev pwrite; DMA2D is not used.",
            "Linux rootfs is QSPI XIP CramFS; only FAT /data uses the SD medium.",
        ],
        "directly_comparable_to_dma2d_lxp": False,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=int, default=300)
    parser.add_argument("--allow-smoke", action="store_true",
                        help="allow a non-comparable duration other than 300 seconds")
    parser.add_argument("--pi", default="pi")
    parser.add_argument("--jump", default="pi")
    parser.add_argument("--target", default="root@172.1.1.2")
    parser.add_argument("--ssh-config", type=Path,
                        default=Path.home() / ".ssh" / "config")
    parser.add_argument("--skip-server-deploy", action="store_true")
    parser.add_argument("--deploy-only", action="store_true")
    parser.add_argument("--archive-build-only", action="store_true")
    parser.add_argument("--hardware-blocker", action="append", default=[])
    parser.add_argument(
        "--build-output", type=Path, default=ROOT / "output-hammer-qspi-xip",
        help="Buildroot O= directory whose configurations and images are archived",
    )
    parser.add_argument(
        "--build-dir", type=Path,
        help="overridden Buildroot BUILD_DIR (defaults to BUILD_OUTPUT/build)",
    )
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--serial-log", type=Path,
        help="offline-validate an already captured serial benchmark log",
    )
    parser.add_argument(
        "--serial-returncode", type=int,
        help="benchmark exit status when it cannot be recovered from SERIAL_LOG",
    )
    parser.add_argument(
        "--identity-log", type=Path,
        help="boot/identity log to archive with an offline serial result",
    )
    parser.add_argument(
        "--runtime-path", default="/usr/bin/native-linux-hammer",
        help="target path used for an offline serial run",
    )
    parser.add_argument(
        "--configuration", action="append", default=[], metavar="LABEL=PATH",
        help=(
            "record this exact configuration; when present, replaces automatic "
            "configuration discovery"
        ),
    )
    parser.add_argument(
        "--artifact", action="append", default=[], metavar="LABEL=PATH",
        help=(
            "hash this exact deployed artifact; when present, replaces automatic "
            "image discovery"
        ),
    )
    args = parser.parse_args()
    if args.duration < 1 or args.duration > 3600:
        parser.error("--duration must be between 1 and 3600")
    if args.duration != 300 and not args.allow_smoke:
        parser.error("non-300-second runs require --allow-smoke and are not comparable")
    try:
        configurations = named_paths(args.configuration, "--configuration")
        artifacts = named_paths(args.artifact, "--artifact")
    except ValueError as error:
        parser.error(str(error))

    if args.archive_build_only:
        output = (
            args.output or args.build_output / "hammer-results" / "poc-build"
        )
        archive_build_only(
            output, args.build_output, args.build_dir, args.hardware_blocker
        )
        return

    if args.serial_log:
        text = args.serial_log.read_text(errors="replace")
        returncode = args.serial_returncode
        if returncode is None:
            statuses = re.findall(r"echo \$\?\s*\n(\d+)", text.replace("\r", ""))
            if not statuses:
                parser.error(
                    "--serial-returncode is required when SERIAL_LOG has no echo $?"
                )
            returncode = int(statuses[-1])
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        output = (
            args.output or args.build_output / "hammer-results" /
            f"native-linux-serial-{stamp}"
        )
        output.mkdir(parents=True, exist_ok=False)
        benchmark_name = "benchmark-serial.log"
        shutil.copy2(args.serial_log, output / benchmark_name)
        network = parse_json_marker(text, "__HAMMER_NETWORK__:")
        summary = analyze(args.duration, None, returncode, text, network)
        summary["capture_mode"] = "serial-offline"
        summary["captured_at_utc"] = datetime.now(timezone.utc).isoformat()
        summary["benchmark_log"] = benchmark_name
        if args.identity_log:
            identity_name = "identity-and-boot.log"
            shutil.copy2(args.identity_log, output / identity_name)
            summary["target_identity_log"] = identity_name
        summary["runtime_staging"] = {
            "path": args.runtime_path,
            "embedded_in_recorded_rootfs": args.runtime_path.startswith("/usr/"),
            "source_sha256": sha256(
                ROOT / "package" / "native-linux-hammer" /
                "native-linux-hammer"
            ),
        }
        summary["build"] = record_build_metadata(
            output, args.build_output, args.build_dir,
            configurations, artifacts,
        )
        (output / "result.json").write_text(
            json.dumps(summary, indent=2) + "\n"
        )
        print(
            f"{summary['status']}: sqlite={summary['sqlite'].get('transactions')} "
            f"net={summary['network_mbps']:.3f} Mbps "
            f"LVGL={summary['lvgl']['active_samples']} samples "
            f"missed={summary['latency'].get('missed_releases')} output={output}"
        )
        if summary["failures"]:
            raise SystemExit("; ".join(summary["failures"]))
        return

    if not args.skip_server_deploy:
        metrics = deploy_server(args.pi, args.ssh_config)
        if metrics.get("active") != 0:
            raise RuntimeError(f"new stream server unexpectedly active: {metrics}")
    for endpoint in ("reset", "ready", "metrics"):
        try:
            response = pi_http(args.pi, endpoint, args.ssh_config)
            if endpoint != "ready":
                print(f"Pi /{endpoint}: {response.strip()}")
        except RuntimeError:
            if endpoint != "ready":
                raise
    if args.deploy_only:
        return

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    output = (
        args.output or args.build_output / "hammer-results" /
        f"native-linux-{stamp}"
    )
    output.mkdir(parents=True, exist_ok=False)
    identity = target_identity(args.target, args.jump, args.ssh_config)
    (output / "identity.log").write_text(identity)
    command = f"exec /usr/bin/native-linux-hammer {args.duration}"
    started = time.monotonic()
    result = run(
        ssh_args(args.target, command, args.jump, args.ssh_config),
        timeout=args.duration + 900,
        check=False,
    )
    wall_seconds = time.monotonic() - started
    (output / "benchmark.log").write_text(result.stdout)
    server_metrics = json.loads(pi_http(args.pi, "metrics", args.ssh_config))
    summary = analyze(
        args.duration, wall_seconds, result.returncode, result.stdout,
        server_metrics,
    )
    summary["captured_at_utc"] = datetime.now(timezone.utc).isoformat()
    summary["target_identity_log"] = "identity.log"
    summary["benchmark_log"] = "benchmark.log"
    summary["build"] = record_build_metadata(
        output, args.build_output, args.build_dir,
        configurations, artifacts,
    )
    (output / "result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(
        f"{summary['status']}: sqlite={summary['sqlite'].get('transactions')} "
        f"net={summary['network_mbps']:.3f} Mbps "
        f"LVGL={summary['lvgl']['active_samples']} samples "
        f"missed={summary['latency'].get('missed_releases')} output={output}"
    )
    if summary["failures"]:
        raise SystemExit("; ".join(summary["failures"]))


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
