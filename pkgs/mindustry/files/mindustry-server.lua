#!/bin/lua
-- mindustry-server.lua: запуск демонстрационного сервера
-- Автор: Елчинян Арен Артурович, 2026
-- Подсистема: демонстрационный образ Mindustry
-- Первое изменение: 17.10.2026, последнее изменение: 17.10.2026
-- Статус доступа: общедоступный

-- Передать постоянную команду оболочке и проверить результат
local function execute(command)
    local ok, reason, code = os.execute(command)
    if not ok then
        error(string.format("Команда завершилась с ошибкой %s %s", tostring(reason), tostring(code)))
    end
end

-- Считать небольшой текстовый файл
local function read(path)
    local file = io.open(path, "r")
    if not file then
        return nil
    end
    local text = file:read("a")
    file:close()
    return text
end

-- Проверить время создания процесса, чтобы не принять повторно использованный PID
local function start_time(pid)
    local text = read("/proc/" .. pid .. "/stat")
    if not text then
        return nil
    end
    local rest = text:match("^%d+ %b() (.*)")
    if not rest then
        return nil
    end
    local index = 0
    for field in rest:gmatch("%S+") do
        index = index + 1
        if index == 20 then
            return field
        end
    end
end

if arg[1] and arg[1] ~= "--foreground" and arg[1] ~= "--background" then
    io.stderr:write("Использование: lua /root/mindustry-server.lua [--foreground|--background]\n")
    os.exit(2)
end

local pidfile = "/var/run/mindustry-server.pid"
local previous = read(pidfile)
if previous then
    local pid, created = previous:match("^(%d+) (%d+)")
    if pid and start_time(pid) == created then
        print("Сервер уже запущен, PID " .. pid)
        os.exit(0)
    end
end

local commands = table.concat({
    "config name SynapseOS demo",
    "config desc Демо-сервер Mindustry на SynapseOS. https://synapseos.ru",
    "config motd Добро пожаловать в SynapseOS demo! https://synapseos.ru",
    "config autosave true",
    "config autosaveSpacing 300",
    "config autoPause true",
    "config socketInputAddress 127.0.0.1",
    "config socketInput true",
    "rules add waveSpacing 14400",
    "host Islands"
}, ",")
local launch = '/bin/mindustry-server "' .. commands .. '"'
execute("/bin/mkdir -p /var/log /var/run /var/lib/mindustry/server")
if arg[1] == "--foreground" then
    execute(launch)
    return
end

execute("/bin/bash --noprofile --norc -c '" .. launch ..
    " </dev/null >>/var/log/mindustry-server.log 2>&1 & echo $! >" .. pidfile .. "'")
local pid = (read(pidfile) or ""):match("^(%d+)")
local created = pid and start_time(pid)
if not created then
    error("Процесс сервера не создан, проверьте /var/log/mindustry-server.log")
end
local file = assert(io.open(pidfile, "w"))
file:write(pid, " ", created, "\n")
file:close()
print("Сервер запускается в фоне, подключение 127.0.0.1:6567")
print("Журнал: /var/log/mindustry-server.log")
