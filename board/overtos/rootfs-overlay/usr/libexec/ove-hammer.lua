#!/usr/bin/lua

local socket = require("socket")

local mode = arg[1]
local DBI, cjson, lfs, process
if mode == "database" then
    DBI = require("DBI")
    cjson = require("cjson")
    lfs = require("lfs")
elseif mode == "network" then
    cjson = require("cjson")
    lfs = require("lfs")
else
    cjson = require("cjson")
    process = require("ove.process")
end

local stop_path = "/tmp/ove-hammer.stop"
local database_ready_path = "/tmp/ove-lua-db-ready"

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

local function database_worker()
    local path = "/data/.ove-hammer-lua.db"
    os.remove(path)
    os.remove(path .. "-journal")
    os.remove(path .. "-wal")
    os.remove(path .. "-shm")

    local database, err = DBI.Connect("SQLite3", path)
    assert(database, err)
    -- LuaDBI defaults to an implicit transaction. This workload owns explicit
    -- BEGIN IMMEDIATE/COMMIT boundaries, so switch the binding to autocommit.
    database:autocommit(true)
    assert(sql_exec(database, "PRAGMA journal_mode=DELETE"))
    assert(sql_exec(database, "PRAGMA synchronous=FULL"))
    assert(sql_exec(database, "PRAGMA temp_store=MEMORY"))
    assert(sql_exec(database, "CREATE TABLE events(id INTEGER PRIMARY KEY,payload BLOB)"))
    assert(sql_exec(database, "CREATE TABLE meta(n INTEGER NOT NULL)"))
    assert(sql_exec(database, "INSERT INTO meta VALUES(0)"))
    write_file(database_ready_path, "ready\n")

    local insert = "INSERT INTO events(payload) VALUES" ..
        "(randomblob(1024)),(randomblob(1024)),(randomblob(1024)),(randomblob(1024))," ..
        "(randomblob(1024)),(randomblob(1024)),(randomblob(1024)),(randomblob(1024))"
    local transactions = 0
    local started = socket.gettime()
    local failure
    while not stopped() do
        local ok, why = sql_exec(database, "BEGIN IMMEDIATE")
        if ok then ok, why = sql_exec(database, insert) end
        if ok then ok, why = sql_exec(database, "UPDATE meta SET n=n+8") end
        if ok then
            ok, why = sql_exec(database,
                "DELETE FROM events WHERE id<(SELECT max(id)-127 FROM events)")
        end
        if ok then ok, why = sql_exec(database, "COMMIT") end
        if not ok then
            sql_exec(database, "ROLLBACK")
            failure = tostring(why)
            break
        end
        transactions = transactions + 1
        if transactions % 20 == 0 then
            local vacuum_ok, vacuum_err = sql_exec(database, "VACUUM")
            if not vacuum_ok then
                failure = tostring(vacuum_err)
                break
            end
        end
    end

    local integrity = "unavailable"
    local statement = database:prepare("PRAGMA integrity_check")
    if statement and statement:execute() then
        local row = statement:fetch(false)
        if row then integrity = tostring(row[1]) end
        statement:close()
    end
    database:close()
    write_file("/tmp/ove-lua-db-result.json", cjson.encode({
        transactions = transactions,
        rows = transactions * 8,
        elapsed_s = socket.gettime() - started,
        integrity = integrity,
        error = failure,
    }) .. "\n")
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
    os.remove(database_ready_path)
    os.remove("/tmp/ove-lua-db-result.json")
    os.remove("/tmp/ove-lua-net-result.json")

    local lvmusic = assert(process.spawn("/usr/bin/lvmusic", {}, {
        stdout = "/dev/console",
        stderr = "/dev/console",
    }))
    assert(process.setpriority(lvmusic, -5))
    socket.sleep(15)
    local touch = assert(process.spawn("/usr/bin/touchctl", {"tap", "120", "160", "1500"}))
    assert(wait_for(touch, socket.gettime() + 15) == 0)
    socket.sleep(2)

    local database = assert(process.spawn("/usr/bin/lua", {arg[0], "database"}))
    assert(process.setpriority(database, 0))
    local ready_deadline = socket.gettime() + 30
    while not read_file(database_ready_path) do
        local result, status, kind = process.wait(database, true)
        assert(result == 0, string.format("database setup failed: %s %s", status, kind))
        assert(socket.gettime() < ready_deadline, "database setup timed out")
        socket.sleep(0.1)
    end
    local network = assert(process.spawn("/usr/bin/lua", {arg[0], "network"}))
    assert(process.setpriority(network, 10))

    print(string.format("__HAMMER_BEGIN__:lua duration=%d lvmusic=%d network=%d sqlite=%d",
        duration, lvmusic, network, database))
    print("__RT_SCOPE_BEFORE__")
    io.write(read_file("/proc/rt_scope") or "available 0\n")
    print("__RT_SCOPE_BEFORE_END__")
    socket.sleep(duration)
    write_file(stop_path, "stop\n")

    local deadline = socket.gettime() + 120
    local db_status, db_kind = wait_for(database, deadline)
    local net_status, net_kind = wait_for(network, deadline)
    if not db_status then process.kill(database, process.SIGKILL) end
    if not net_status then process.kill(network, process.SIGKILL) end
    process.kill(lvmusic, process.SIGKILL)
    wait_for(lvmusic, socket.gettime() + 10)

    print("__HAMMER_SQLITE__:" .. (read_file("/tmp/ove-lua-db-result.json") or
        cjson.encode({error = db_kind or "missing result"}) .. "\n"))
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

if mode == "database" then
    database_worker()
elseif mode == "network" then
    network_worker()
else
    controller(tonumber(arg[1]) or 300)
end
