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

local function stopped()
    return lfs.attributes(stop_path) ~= nil
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

local function sql_exec(database, sql)
    local statement, err = database:prepare(sql)
    if not statement then return nil, err end
    local result, execute_err = statement:execute()
    statement:close()
    if result == false or result == nil then return nil, execute_err end
    return true
end

local function connect_database(path)
    local database, err = DBI.Connect("SQLite3", path)
    if not database then return nil, "open: " .. tostring(err) end
    -- LuaDBI defaults to an implicit transaction. This workload owns explicit
    -- BEGIN IMMEDIATE/COMMIT boundaries, so switch the binding to autocommit.
    database:autocommit(true)
    local ok, why = sql_exec(database, "PRAGMA journal_mode=DELETE")
    if not ok then why = "journal_mode: " .. tostring(why) end
    if ok then
        ok, why = sql_exec(database, "PRAGMA synchronous=FULL")
        if not ok then why = "synchronous: " .. tostring(why) end
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
    local integrity = "unavailable"
    local statement = database:prepare("PRAGMA integrity_check")
    if statement and statement:execute() then
        local row = statement:fetch(false)
        if row then integrity = tostring(row[1]) end
        statement:close()
    end
    return integrity
end

local function network_worker()
    local client = assert(socket.tcp())
    assert(client:settimeout(10))
    assert(client:connect("172.1.1.1", 8082))
    assert(client:send("GET /stream HTTP/1.1\r\nHost: 172.1.1.1\r\nConnection: close\r\n\r\n"))
    local status = assert(client:receive("*l"))
    assert(status:match(" 200 "), status)
    while true do
        local line = assert(client:receive("*l"))
        if line == "" then break end
    end
    assert(client:settimeout(0.25))

    local bytes = 0
    local started = socket.gettime()
    while not stopped() do
        local data, err, partial = client:receive(16384)
        if data then bytes = bytes + #data end
        if partial then bytes = bytes + #partial end
        if err and err ~= "timeout" then break end
    end
    client:close()
    write_file("/tmp/ove-lua-net-result.json", cjson.encode({
        bytes = bytes,
        elapsed_s = socket.gettime() - started,
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

local function controller(duration)
    os.remove(stop_path)
    os.remove("/tmp/ove-lua-net-result.json")
    os.remove("/tmp/ove-lua-vacuum.err")

    local lvmusic = assert(process.spawn("/usr/bin/lvmusic", {}, {
        stdout = "/dev/console",
        stderr = "/dev/console",
    }))
    assert(process.setpriority(lvmusic, -5))
    socket.sleep(15)
    local touch = assert(process.spawn("/usr/bin/touchctl", {"tap", "120", "160", "1500"}))
    assert(wait_for(touch, socket.gettime() + 15) == 0)
    socket.sleep(2)

    local path = "/data/.ove-hammer-lua.db"
    local database, database_err = initialize_database(path)
    assert(database, database_err)
    local network = assert(process.spawn("/usr/bin/lua", {arg[0], "network"}))
    assert(process.setpriority(network, 10))

    print(string.format("__HAMMER_BEGIN__:lua duration=%d lvmusic=%d network=%d sqlite=controller",
        duration, lvmusic, network))
    print("__RT_SCOPE_BEFORE__")
    io.write(read_file("/proc/rt_scope") or "available 0\n")
    print("__RT_SCOPE_BEFORE_END__")

    local started = socket.gettime()
    local deadline = started + duration
    local transactions = 0
    local failure
    while socket.gettime() < deadline do
        local ok, why = database_transaction(database)
        if not ok then
            failure = tostring(why)
            break
        end
        transactions = transactions + 1
        if transactions % 20 == 0 and socket.gettime() < deadline then
            -- Lua and SQLite cannot hold VACUUM's complete temporary image in
            -- one FDPIC arena. Use a short-lived CLI arena, then reopen the
            -- database that VACUUM atomically replaced.
            database:close()
            database = nil
            collectgarbage("collect")
            local vacuum, vacuum_err = process.spawn(
                "/usr/bin/sqlite3", {path, "VACUUM;"}, {
                    stdout = "/dev/null",
                    stderr = "/tmp/ove-lua-vacuum.err",
                })
            if not vacuum then
                failure = "VACUUM spawn: " .. tostring(vacuum_err)
                break
            end
            local status, kind = wait_for(vacuum, socket.gettime() + 180)
            if status == nil then
                process.kill(vacuum, process.SIGKILL)
                wait_for(vacuum, socket.gettime() + 10)
                failure = "VACUUM " .. tostring(kind)
                break
            end
            if status ~= 0 then
                failure = string.format("VACUUM exit=%s kind=%s", status, kind)
                break
            end
            database, database_err = connect_database(path)
            if not database then
                failure = tostring(database_err)
                break
            end
        end
    end
    write_file(stop_path, "stop\n")

    if not database then database, database_err = connect_database(path) end
    local integrity = database and database_integrity(database) or "unavailable"
    if database then database:close() end
    local elapsed = socket.gettime() - started

    local net_status, net_kind = wait_for(network, socket.gettime() + 120)
    if not net_status then process.kill(network, process.SIGKILL) end
    process.kill(lvmusic, process.SIGKILL)
    wait_for(lvmusic, socket.gettime() + 10)

    print("__HAMMER_SQLITE__:" .. cjson.encode({
        transactions = transactions,
        rows = transactions * 8,
        elapsed_s = elapsed,
        integrity = integrity,
        error = failure,
    }))
    print("__HAMMER_NETWORK__:" .. (read_file("/tmp/ove-lua-net-result.json") or
        cjson.encode({error = net_kind or "missing result"}) .. "\n"))
    print("__RT_SCOPE_AFTER__")
    io.write(read_file("/proc/rt_scope") or "available 0\n")
    print("__RT_SCOPE_AFTER_END__")
    print("__FS_AFTER__")
    io.write(read_file("/proc/lxp_fs") or "provider_available 0\n")
    print("__FS_AFTER_END__")
    print("__HAMMER_END__:lua")
end

if mode == "network" then
    network_worker()
else
    controller(tonumber(arg[1]) or 300)
end
