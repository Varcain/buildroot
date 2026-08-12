#!/usr/bin/lua

local socket = require("socket")

local mode = arg[1]
local DBI, cjson, lfs, process
if mode == "network" then
    cjson = require("cjson")
    lfs = require("lfs")
else
    DBI = require("DBI")
    cjson = require("cjson")
    lfs = require("lfs")
    process = require("ove.process")
end

local stop_path = "/tmp/ove-hammer.stop"
local network_ready_path = "/tmp/ove-lua-net-ready"
local network_result_path = "/tmp/ove-lua-net-result.json"
local data_directory = "/data/.ove-hammer"

local function exists(path)
    return lfs.attributes(path) ~= nil
end

local function stopped()
    return exists(stop_path)
end

local function write_file(path, value)
    local file, err = io.open(path, "w")
    assert(file, err)
    file:write(value)
    file:close()
end

local function read_file(path)
    local file = io.open(path, "r")
    if not file then return nil end
    local value = file:read("*a")
    file:close()
    return value
end

local function print_proc(begin_marker, path, end_marker, fallback)
    print(begin_marker)
    local value = read_file(path) or fallback
    io.write(value)
    if value:sub(-1) ~= "\n" then io.write("\n") end
    print(end_marker)
end

local function sql_exec(database, sql)
    local statement, err = database:prepare(sql)
    if not statement then return nil, err end
    local result, execute_err = statement:execute()
    statement:close()
    if result == false or result == nil then return nil, execute_err end
    return true
end

local function sql_scalar(database, sql)
    local statement, err = database:prepare(sql)
    if not statement then return nil, err end
    local result, execute_err = statement:execute()
    if result == false or result == nil then
        statement:close()
        return nil, execute_err
    end
    local row = statement:fetch(false)
    statement:close()
    if not row then return nil, "query returned no row" end
    return row[1]
end

local function connect_database(path)
    local database, err = DBI.Connect("SQLite3", path)
    if not database then return nil, "open: " .. tostring(err) end
    database:autocommit(true)
    local ok, why = sql_exec(database, "PRAGMA journal_mode=DELETE")
    if not ok then why = "journal_mode: " .. tostring(why) end
    if ok then
        ok, why = sql_exec(database, "PRAGMA synchronous=FULL")
        if not ok then why = "synchronous: " .. tostring(why) end
    end
    if ok then
        ok, why = sql_exec(database, "PRAGMA temp_store=FILE")
        if not ok then why = "temp_store: " .. tostring(why) end
    end
    if ok then
        ok, why = sql_exec(database,
            "PRAGMA temp_store_directory='/data/.ove-hammer'")
        if not ok then why = "temp_store_directory: " .. tostring(why) end
    end
    if ok then
        ok, why = sql_exec(database, "PRAGMA cache_size=-16")
        if not ok then why = "cache_size: " .. tostring(why) end
    end
    if ok then
        ok, why = sql_exec(database, "PRAGMA mmap_size=0")
        if not ok then why = "mmap_size: " .. tostring(why) end
    end
    if not ok then
        database:close()
        return nil, why
    end
    return database
end

local function initialize_database(path)
    os.remove(path)
    os.remove(path .. "-journal")
    os.remove(path .. "-wal")
    os.remove(path .. "-shm")
    local database, err = connect_database(path)
    if not database then return nil, err end
    local ok, why = sql_exec(database,
        "CREATE TABLE events(id INTEGER PRIMARY KEY,payload BLOB)")
    if ok then ok, why = sql_exec(database, "CREATE TABLE meta(n INTEGER NOT NULL)") end
    if ok then ok, why = sql_exec(database, "INSERT INTO meta VALUES(0)") end
    if not ok then
        database:close()
        return nil, why
    end
    return database
end

local insert_sql = "INSERT INTO events(payload) VALUES" ..
    "(randomblob(1024)),(randomblob(1024)),(randomblob(1024)),(randomblob(1024))," ..
    "(randomblob(1024)),(randomblob(1024)),(randomblob(1024)),(randomblob(1024))"

local function database_transaction(database)
    local ok, why = sql_exec(database, "BEGIN IMMEDIATE")
    if not ok then why = "begin: " .. tostring(why) end
    if ok then
        ok, why = sql_exec(database, insert_sql)
        if not ok then why = "insert: " .. tostring(why) end
    end
    if ok then
        ok, why = sql_exec(database, "UPDATE meta SET n=n+8")
        if not ok then why = "update: " .. tostring(why) end
    end
    if ok then
        ok, why = sql_exec(database,
            "DELETE FROM events WHERE id<(SELECT max(id)-127 FROM events)")
        if not ok then why = "delete: " .. tostring(why) end
    end
    if ok then
        ok, why = sql_exec(database, "COMMIT")
        if not ok then why = "commit: " .. tostring(why) end
    end
    if not ok then sql_exec(database, "ROLLBACK") end
    return ok, why
end

local function database_integrity(database)
    local value = sql_scalar(database, "PRAGMA integrity_check")
    return value and tostring(value) or "unavailable"
end

local function network_worker(duration)
    os.remove(network_ready_path)
    os.remove(network_result_path)
    local client
    local bytes = 0
    local started
    local reached_eof = false
    local error_message

    local ok, why = xpcall(function()
        client = assert(socket.tcp())
        assert(client:settimeout(10))
        assert(client:connect("172.1.1.1", 8082))
        local request = string.format(
            "GET /stream?seconds=%d HTTP/1.1\r\nHost: 172.1.1.1\r\nConnection: close\r\n\r\n",
            duration)
        assert(client:send(request))
        local status = assert(client:receive("*l"))
        assert(status:match(" 200 "), status)
        while true do
            local line = assert(client:receive("*l"))
            if line == "" then break end
        end
        assert(client:settimeout(0.25))
        started = socket.gettime()
        write_file(network_ready_path, "ready\n")

        while not stopped() do
            local data, receive_error, partial = client:receive(16384)
            if data then bytes = bytes + #data end
            if partial then bytes = bytes + #partial end
            if receive_error == "closed" then
                reached_eof = true
                break
            end
            if receive_error and receive_error ~= "timeout" then
                error(receive_error)
            end
        end
    end, debug.traceback)
    if not ok then error_message = tostring(why) end
    if client then client:close() end

    local elapsed = started and (socket.gettime() - started) or 0
    if not error_message and reached_eof and not stopped() and elapsed < duration - 1 then
        error_message = string.format("early EOF after %.3fs", elapsed)
    end
    if not error_message and not reached_eof and not stopped() then
        error_message = "stream ended without EOF or stop"
    end
    write_file(network_result_path, cjson.encode({
        bytes = bytes,
        elapsed_s = elapsed,
        eof = reached_eof,
        error = error_message,
    }) .. "\n")
end

local function wait_for(pid, deadline)
    while socket.gettime() < deadline do
        local result, status, kind = process.wait(pid, true)
        if not result then return nil, status end
        if result ~= 0 then return status, kind end
        socket.sleep(0.1)
    end
    return nil, "timeout"
end

local function terminate(pid)
    if not pid then return end
    local status = wait_for(pid, socket.gettime() + 0.1)
    if status ~= nil then return end
    process.kill(pid, process.SIGTERM)
    status = wait_for(pid, socket.gettime() + 2)
    if status ~= nil then return end
    process.kill(pid, process.SIGKILL)
    wait_for(pid, socket.gettime() + 10)
end

local function touch_report(file, x, y, pressed)
    local event = "<i4i4I2I2i4"
    local report =
        string.pack(event, 0, 0, 3, 0, x) ..
        string.pack(event, 0, 0, 3, 1, y) ..
        string.pack(event, 0, 0, 1, 330, pressed) ..
        string.pack(event, 0, 0, 0, 0, 0)
    assert(file:write(report))
    assert(file:flush())
end

local function tap(x, y, hold_seconds)
    local file = assert(io.open("/dev/input/event0", "wb"))
    local ok, why = xpcall(function()
        touch_report(file, x, y, 1)
        socket.sleep(hold_seconds)
        touch_report(file, x, y, 0)
    end, debug.traceback)
    file:close()
    assert(ok, why)
end

local function wait_for_network_ready(network, timeout)
    local deadline = socket.gettime() + timeout
    while socket.gettime() < deadline do
        if exists(network_ready_path) then return true end
        if exists(network_result_path) then
            return nil, read_file(network_result_path) or "network exited before ready"
        end
        local status, kind = wait_for(network, socket.gettime() + 0.1)
        if status ~= nil then
            return nil, string.format("network exited status=%s kind=%s", status, kind)
        end
    end
    return nil, "network readiness timeout"
end

local function cleanup(state)
    pcall(write_file, stop_path, "stop\n")
    if state.database then
        pcall(function() state.database:close() end)
        state.database = nil
    end
    if state.vacuum then terminate(state.vacuum); state.vacuum = nil end
    if state.network then terminate(state.network); state.network = nil end
    if state.lvmusic then terminate(state.lvmusic); state.lvmusic = nil end
end

local function controller(duration, state)
    os.remove(stop_path)
    os.remove(network_ready_path)
    os.remove(network_result_path)

    state.lvmusic = assert(process.spawn("/usr/bin/lvmusic", {}, {
        stdout = "/dev/console",
        stderr = "/dev/console",
    }))
    assert(process.setpriority(state.lvmusic, -5))
    socket.sleep(15)
    tap(120, 160, 1.5)
    socket.sleep(2)

    local made, mkdir_err = lfs.mkdir(data_directory)
    if not made then
        local attributes = lfs.attributes(data_directory)
        assert(attributes and attributes.mode == "directory", mkdir_err)
    end
    local path = data_directory .. "/lua.db"
    local database_err
    state.database, database_err = initialize_database(path)
    assert(state.database, database_err)
    state.network = assert(process.spawn(
        "/usr/bin/lua", {arg[0], "network", tostring(duration)}))
    assert(process.setpriority(state.network, 10))
    local ready, ready_error = wait_for_network_ready(state.network, 15)
    assert(ready, ready_error)

    print(string.format(
        "__HAMMER_BEGIN__:lua duration=%d lvmusic=%d network=%d sqlite=controller network_ready=1",
        duration, state.lvmusic, state.network))
    print_proc("__FS_BEFORE__", "/proc/lxp_fs", "__FS_BEFORE_END__",
        "provider_available 0\n")
    print_proc("__RT_SCOPE_BEFORE__", "/proc/rt_scope", "__RT_SCOPE_BEFORE_END__",
        "available 0\n")

    local started = socket.gettime()
    local deadline = started + duration
    local transactions = 0
    local failure
    while socket.gettime() < deadline do
        local ok, why = database_transaction(state.database)
        if not ok then
            failure = tostring(why)
            break
        end
        transactions = transactions + 1
        if transactions % 20 == 0 and socket.gettime() < deadline then
            -- LuaDBI and VACUUM's complete temporary image cannot safely share
            -- this 256 KiB FDPIC arena under the full concurrent workload.
            state.database:close()
            state.database = nil
            collectgarbage("collect")
            local vacuum_error
            state.vacuum, vacuum_error = process.spawn(
                "/usr/bin/sqlite3", {
                    path,
                    "PRAGMA temp_store_directory=\"/data/.ove-hammer\";VACUUM;",
                }, {
                    stdout = "/dev/null",
                    stderr = "/tmp/ove-lua-vacuum.err",
                })
            if not state.vacuum then
                failure = "VACUUM spawn: " .. tostring(vacuum_error)
                break
            end
            local status, kind = wait_for(state.vacuum, socket.gettime() + 180)
            if status == nil then
                terminate(state.vacuum)
                state.vacuum = nil
                failure = "VACUUM " .. tostring(kind)
                break
            end
            state.vacuum = nil
            if status ~= 0 then
                failure = string.format("VACUUM exit=%s kind=%s", status, kind)
                break
            end
            state.database, database_err = connect_database(path)
            if not state.database then
                failure = tostring(database_err)
                break
            end
        end
    end
    local elapsed = socket.gettime() - started
    write_file(stop_path, "stop\n")

    if not state.database then state.database, database_err = connect_database(path) end
    local integrity = state.database and database_integrity(state.database) or "unavailable"
    local meta = state.database and sql_scalar(state.database, "SELECT n FROM meta") or nil
    local live_rows = state.database and
        sql_scalar(state.database, "SELECT count(*) FROM events") or nil
    if state.database then state.database:close(); state.database = nil end

    local net_status, net_kind = wait_for(state.network, socket.gettime() + 120)
    if net_status == nil then
        terminate(state.network)
        failure = failure or ("network " .. tostring(net_kind))
    elseif net_status ~= 0 then
        failure = failure or string.format("network exit=%s kind=%s", net_status, net_kind)
    end
    state.network = nil
    terminate(state.lvmusic)
    state.lvmusic = nil

    print("__HAMMER_SQLITE__:" .. cjson.encode({
        transactions = transactions,
        rows = transactions * 8,
        meta = meta,
        live_rows = live_rows,
        elapsed_s = elapsed,
        integrity = integrity,
        error = failure,
    }))
    print("__HAMMER_NETWORK__:" .. (read_file(network_result_path) or
        cjson.encode({error = "missing result"}) .. "\n"))
    print_proc("__RT_SCOPE_AFTER__", "/proc/rt_scope", "__RT_SCOPE_AFTER_END__",
        "available 0\n")
    print_proc("__FS_AFTER__", "/proc/lxp_fs", "__FS_AFTER_END__",
        "provider_available 0\n")
    print("__HAMMER_END__:lua")
    if failure then error(failure, 0) end
end

if mode == "network" then
    local duration = assert(tonumber(arg[2]), "network duration required")
    network_worker(duration)
else
    local duration = tonumber(arg[1]) or 300
    assert(duration > 0 and duration == math.floor(duration), "duration must be a positive integer")
    local state = {}
    local ok, why = xpcall(function() controller(duration, state) end, debug.traceback)
    cleanup(state)
    if not ok then
        io.stderr:write("__HAMMER_FATAL__:" .. tostring(why) .. "\n")
        os.exit(1)
    end
end
