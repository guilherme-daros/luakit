local luna = require("luna")
local tracker = require("tracker")

printL = function(...)
    print("[Lua] " .. tostring(...))
end

printL("luna.sum(2, 3) = " .. luna.sum(2, 3))
printL("luna.greet('lua') = " .. luna.greet("lua"))
printL("luna.scale({1,2,3,4}, 2.5) = " .. table.concat(luna.scale({ 1, 2, 3, 4 }, 2.5), ", "))

local ok, err = pcall(luna.scale, { 1, "two", 3 }, 2)
printL("bad element -> " .. tostring(err))

local ok2, err2 = pcall(luna.boom)
printL("luna.boom() -> " .. tostring(err2))

local t = tracker.new("latency")
t:add(10):add(12):add(17):add(8)
printL(string.format("%s  #t = %d  mean = %.2f  max = %.1f", tostring(t), #t, t:mean(), t:max()))

local empty = tracker.new("empty")
printL("empty mean -> " .. tostring(select(2, pcall(empty.mean, empty))))

-- Dropping the last reference makes the collector run ~Tracker
empty = nil
collectgarbage()
printL("(collected)")
