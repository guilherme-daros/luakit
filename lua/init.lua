-- The script side of the demo. Everything here is what a mod would write.
--
-- The host loads this once, then takes over: it runs frames, drives the quest
-- coroutine, destroys a creature this script is holding, and reloads the
-- world module. See src/main.cpp for that half.

local luna = require("luna")
local tracker = require("tracker")
local world = require("world")

printL = function(...)
    print("[Lua] " .. tostring(...))
end

local function section(name)
    print("\n--- " .. name .. " ---")
end

-- ------------------------------------------------------------ the basics

section("plain functions")

printL("luna.sum(2, 3) = " .. luna.sum(2, 3))
printL("luna.greet('lua') = " .. luna.greet("lua"))
printL("luna.scale({1,2,3,4}, 2.5) = " .. table.concat(luna.scale({ 1, 2, 3, 4 }, 2.5), ", "))

local ok, err = pcall(luna.scale, { 1, "two", 3 }, 2)
printL("bad element -> " .. tostring(err))

local ok2, err2 = pcall(luna.boom)
printL("luna.boom() -> " .. tostring(err2))

section("a class with hand-written Reg arrays")

local t = tracker.new("latency")
t:add(10):add(12):add(17):add(8)
printL(string.format("%s  #t = %d  mean = %.2f  max = %.1f", tostring(t), #t, t:mean(), t:max()))

local empty = tracker.new("empty")
printL("empty mean -> " .. tostring(select(2, pcall(empty.mean, empty))))
empty = nil

-- ------------------------------------------------------------ the world

section("configuration, read off a table")

world.configure({ title = "the demo world", difficulty = 3, verbose = true })
world.log("info", "starting up", 42, true, { 1, 2 })

section("values Lua owns")

local a = world.vec2(3, 4)
local b = world.vec2()
printL("vec2(3, 4) = " .. tostring(a) .. "  length = " .. a:length())
printL("vec2() = " .. tostring(b))
printL("a.x = " .. a.x .. ", a.y = " .. a.y)

-- One name, two signatures, picked by what is passed.
printL("a:plus(vec2(1, 1)) = " .. tostring(a:plus(world.vec2(1, 1))))
printL("a:plus(10) = " .. tostring(a:plus(10)))

a.x = 30
printL("after a.x = 30 -> " .. tostring(a))

section("objects the engine owns")

local goblin = world.spawn("goblin")
local orc = world.spawn("orc")

goblin:move_by(world.vec2(1, 2))
orc:move_by(world.vec2(4, 6))

printL("goblin = " .. tostring(goblin))
printL("orc    = " .. tostring(orc))
printL("distance(goblin, orc) = " .. world.distance(goblin, orc))
printL("distance(vec2(0,0), vec2(3,4)) = " .. world.distance(world.vec2(0, 0), world.vec2(3, 4)))

-- Fields, inherited and its own. `name` comes from Entity, `hp` from Creature.
goblin:damage(30)
printL("goblin.name = " .. goblin.name .. ", goblin.hp = " .. goblin.hp)
printL("goblin.alive = " .. tostring(goblin.alive))

-- A read-only field says so instead of quietly shadowing itself.
printL("goblin.name = 'x' -> " .. tostring(select(
    2,
    pcall(function()
        goblin.name = "x"
    end)
)))

-- The setter clamps, so writing through the field cannot go out of range.
goblin.hp = 500
printL("after goblin.hp = 500 -> " .. goblin.hp)

-- Enums are strings, and a misspelling is caught with the alternatives.
goblin.facing = "east"
printL("goblin.facing = " .. goblin.facing .. ", opposite = " .. world.opposite(goblin.facing))
printL("goblin.facing = 'up' -> " .. tostring(select(
    2,
    pcall(function()
        goblin.facing = "up"
    end)
)))

section("identity and per-instance state")

-- Two lookups of one creature are one Lua object, so a field set on it stays.
goblin.my_mod_data = { visits = 0 }
local same = world.find("goblin")
printL("world.find('goblin') == goblin -> " .. tostring(same == goblin))
same.my_mod_data.visits = same.my_mod_data.visits + 1
printL("state stashed by this mod survived the round trip: " .. goblin.my_mod_data.visits)

section("containers")

world.spawn_many({ "rat", "bat" })
local names = {}
for name, hp in pairs(world.census()) do
    names[#names + 1] = name .. "=" .. hp
end
table.sort(names)
printL("census: " .. table.concat(names, ", "))

section("shared ownership")

summoned = world.summon("wraith")
printL("summoned " .. tostring(summoned) .. " -- the engine holds a count too")

-- ------------------------------------------------- what the host will drive

section("registering handlers")

local ticks = 0
world.on_tick(function(dt)
    ticks = ticks + 1
    printL(string.format("tick %d (+%.1fs), goblin at %s", ticks, dt, tostring(goblin.position)))
end)

-- A second handler that faults, to show one plugin cannot cost the others
-- their frame.
world.on_tick(function()
    error("this handler is broken")
end)

printL("two handlers registered; the host fires them")

-- A coroutine the host drives. world.wait is a C++ function that suspends
-- this task, so the engine decides when it continues.
function quest(turn)
    printL("quest: starting at turn " .. turn)
    turn = world.wait(2)
    printL("quest: woke up, waiting again")
    world.wait(3)
    printL("quest: done")
end

-- Called by the host after it destroys the goblin out from under us.
function report_stale_handle()
    local ok, err = pcall(function()
        return goblin.hp
    end)
    printL("touching the destroyed goblin -> " .. tostring(err))
    printL("world.find('goblin') is now " .. tostring(world.find("goblin")))
end

-- Called by the host after it drops its own reference to the wraith.
function report_summoned()
    printL("the wraith is still here: " .. tostring(summoned) .. ", hp " .. summoned.hp)
end

-- ----------------------------------------------------- a Lua-side coroutine

section("a coroutine written entirely in Lua")

local function fibonacci(limit)
    local x, y = 0, 1
    while x <= limit do
        coroutine.yield(x)
        x, y = y, x + y
    end
end

local gen = coroutine.wrap(fibonacci)
local out = {}
for n in gen, 50 do
    out[#out + 1] = n
end
printL("fibonacci up to 50: " .. table.concat(out, ", "))
