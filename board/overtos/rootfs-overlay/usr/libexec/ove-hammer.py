#!/usr/bin/micropython

"""MicroPython lvmusic + network + SQLite/SD hammer."""

import gc
import json
import os
import socket
import struct
import sys
import time


STOP_PATH = "/tmp/ove-hammer.stop"
NETWORK_RESULT_PATH = "/tmp/ove-micropython-net.json"
LVMUSIC_PID_PATH = "/tmp/ove-micropython-lvmusic.pid"
NETWORK_PID_PATH = "/tmp/ove-micropython-network.pid"
DATA_DIRECTORY = "/data/.ove-hammer"
DATABASE_PATH = DATA_DIRECTORY + "/micropython.db"
DATABASE_ERROR_PATH = "/tmp/ove-micropython-db.err"


def emit(value):
    print(value)
    sys.stdout.flush()


def remove(path):
    try:
        os.remove(path)
    except OSError:
        pass


def exists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False


def read(path):
    try:
        file = open(path, "r")
        value = file.read()
        file.close()
        return value
    except OSError:
        return None


def write(path, value):
    file = open(path, "w")
    file.write(value)
    file.close()


def run(command, label):
    result = os.system(command)
    if result != 0:
        raise RuntimeError("%s exited with status %d" % (label, result))


def start_background(command, pid_path):
    remove(pid_path)
    run("%s & echo $! >%s" % (command, pid_path), command)
    value = read(pid_path)
    if value is None:
        raise RuntimeError("missing pid for " + command)
    return int(value.strip())


def stop_process(pid):
    if pid is not None:
        os.system("kill -9 %d 2>/dev/null" % pid)


def print_proc(begin, path, end):
    emit(begin)
    value = read(path)
    if value is None:
        value = "available 0\n"
    sys.stdout.write(value)
    if not value.endswith("\n"):
        sys.stdout.write("\n")
    emit(end)


def touch_report(file, x, y, pressed):
    # Linux/FDPIC uses a 16-byte input_event: two 32-bit timeval fields,
    # followed by type/code/value. Zero timestamps let the driver stamp it.
    report = (
        struct.pack("<llHHi", 0, 0, 3, 0, x) +       # EV_ABS/ABS_X
        struct.pack("<llHHi", 0, 0, 3, 1, y) +       # EV_ABS/ABS_Y
        struct.pack("<llHHi", 0, 0, 1, 330, pressed) +  # BTN_TOUCH
        struct.pack("<llHHi", 0, 0, 0, 0, 0)         # SYN_REPORT
    )
    if file.write(report) != len(report):
        raise OSError("short evdev write")
    file.flush()


def tap(x, y, hold_ms):
    file = open("/dev/input/event0", "wb")
    try:
        touch_report(file, x, y, 1)
        time.sleep_ms(hold_ms)
        touch_report(file, x, y, 0)
    finally:
        file.close()


def send_all(client, value):
    offset = 0
    while offset < len(value):
        sent = client.send(value[offset:])
        if sent <= 0:
            raise OSError("short socket send")
        offset += sent


def network_worker():
    client = None
    started = time.ticks_ms()
    received = 0
    error = None
    try:
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.settimeout(10)
        address = socket.getaddrinfo(
            "172.1.1.1", 8082, socket.AF_INET, socket.SOCK_STREAM)[0][-1]
        client.connect(address)
        send_all(client,
            b"GET /stream HTTP/1.1\r\nHost: 172.1.1.1\r\nConnection: close\r\n\r\n")
        status = client.readline()
        if b" 200 " not in status:
            raise RuntimeError("HTTP status " + str(status))
        while True:
            line = client.readline()
            if line == b"\r\n" or line == b"\n" or line == b"":
                break
        client.settimeout(0.25)
        started = time.ticks_ms()
        while not exists(STOP_PATH):
            try:
                data = client.recv(8192)
                if not data:
                    break
                received += len(data)
            except OSError as exception:
                code = exception.args[0] if exception.args else None
                if code not in (11, 110, 116):
                    raise
    except Exception as exception:
        error = str(exception)
    finally:
        if client is not None:
            client.close()
        elapsed = time.ticks_diff(time.ticks_ms(), started) / 1000
        result = {"bytes": received, "elapsed_s": elapsed, "error": error}
        write(NETWORK_RESULT_PATH, json.dumps(result) + "\n")


def open_database():
    import sqlite3

    database = sqlite3.connect(DATABASE_PATH)
    database.execute(
        "PRAGMA journal_mode=DELETE;"
        "PRAGMA synchronous=FULL;"
        "PRAGMA temp_store=FILE;"
        "PRAGMA temp_store_directory='/data/.ove-hammer';"
        "PRAGMA cache_size=-16;"
        "PRAGMA mmap_size=0;"
    )
    return database


def initialize_database():
    for suffix in ("", "-journal", "-wal", "-shm"):
        remove(DATABASE_PATH + suffix)
    database = open_database()
    database.execute(
        "CREATE TABLE events(id INTEGER PRIMARY KEY,payload BLOB);"
        "CREATE TABLE meta(n INTEGER NOT NULL);"
        "INSERT INTO meta VALUES(0);"
    )
    return database


TRANSACTION = (
    "BEGIN IMMEDIATE;"
    "INSERT INTO events(payload) VALUES"
    "(randomblob(1024)),(randomblob(1024)),(randomblob(1024)),(randomblob(1024)),"
    "(randomblob(1024)),(randomblob(1024)),(randomblob(1024)),(randomblob(1024));"
    "UPDATE meta SET n=n+8;"
    "DELETE FROM events WHERE id<(SELECT max(id)-127 FROM events);"
    "COMMIT;"
)


def wait_for_file(path, timeout_ms):
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        if exists(path):
            return True
        time.sleep_ms(100)
    return False


def controller(duration):
    remove(STOP_PATH)
    remove(NETWORK_RESULT_PATH)
    remove(DATABASE_ERROR_PATH)
    lvmusic_pid = None
    network_pid = None
    database = None
    transactions = 0
    failure = None
    started = None
    stage = "startup"

    try:
        stage = "start lvmusic"
        lvmusic_pid = start_background(
            "/bin/nice -n -5 /usr/bin/lvmusic >/dev/console 2>&1", LVMUSIC_PID_PATH)
        time.sleep(15)
        stage = "inject Play"
        tap(120, 160, 1500)
        time.sleep(2)

        if not exists(DATA_DIRECTORY):
            os.mkdir(DATA_DIRECTORY)
        stage = "initialize SQLite"
        database = initialize_database()
        stage = "start network worker"
        network_pid = start_background(
            "/bin/nice -n 10 /usr/bin/micropython -X heapsize=64K %s network" %
            sys.argv[0],
            NETWORK_PID_PATH)

        emit("__HAMMER_BEGIN__:micropython duration=%d lvmusic=%d network=%d sqlite=native" %
            (duration, lvmusic_pid, network_pid))
        print_proc("__RT_SCOPE_BEFORE__", "/proc/rt_scope", "__RT_SCOPE_BEFORE_END__")

        stage = "SQLite workload"
        started = time.ticks_ms()
        deadline = time.ticks_add(started, duration * 1000)
        while time.ticks_diff(deadline, time.ticks_ms()) > 0:
            try:
                database.execute(TRANSACTION)
                transactions += 1
                if transactions % 20 == 0 and time.ticks_diff(deadline, time.ticks_ms()) > 0:
                    gc.collect()
                    database.execute("VACUUM;")
            except Exception as exception:
                failure = str(exception)
                write(DATABASE_ERROR_PATH, failure + "\n")
                break
    except Exception as exception:
        failure = "%s: %s" % (stage, exception)
        write(DATABASE_ERROR_PATH, failure + "\n")
    finally:
        write(STOP_PATH, "stop\n")
        if network_pid is not None and not wait_for_file(NETWORK_RESULT_PATH, 30000):
            stop_process(network_pid)
        stop_process(lvmusic_pid)

    elapsed = 0 if started is None else time.ticks_diff(time.ticks_ms(), started) / 1000
    integrity = "unavailable"
    rows = None
    meta = None
    if database is not None:
        try:
            integrity = database.scalar("PRAGMA integrity_check")
            rows = database.scalar("SELECT count(*) FROM events")
            meta = database.scalar("SELECT n FROM meta")
        except Exception as exception:
            if failure is None:
                failure = str(exception)
        database.close()

    emit("__HAMMER_SQLITE__:" + json.dumps({
        "transactions": transactions,
        "rows": rows,
        "meta": meta,
        "elapsed_s": elapsed,
        "integrity": integrity,
        "error": failure,
    }))
    network = read(NETWORK_RESULT_PATH)
    emit("__HAMMER_NETWORK__:" +
        (network.strip() if network is not None else json.dumps({"error": "missing result"})))
    print_proc("__RT_SCOPE_AFTER__", "/proc/rt_scope", "__RT_SCOPE_AFTER_END__")
    print_proc("__FS_AFTER__", "/proc/lxp_fs", "__FS_AFTER_END__")
    emit("__HAMMER_END__:micropython")
    if failure is not None:
        raise RuntimeError(failure)


if len(sys.argv) > 1 and sys.argv[1] == "network":
    network_worker()
else:
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    if seconds <= 0:
        raise ValueError("duration must be positive")
    controller(seconds)
