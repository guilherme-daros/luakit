-- A tour of what the binding can do, written the way a mod would write it.
--
-- This runs top to bottom and prints; it is the half of the demo that needs
-- nothing from the host. init.lua requires it, and then wires up the parts the
-- engine drives from src/main.cpp.
--
-- Returned at the bottom are the few handles the entry point needs to keep
-- hold of, which is the ordinary shape of a mod's setup module.

local luna = require("luna")
local tracker = require("tracker")
local world = require("world")

local function say(...)
    print("[Lua] " .. tostring(...))
end

local function section(name)
    print("\n--- " .. name .. " ---")
end

-- ------------------------------------------------------------ the basics

section("plain functions")

say("luna.sum(2, 3) = " .. luna.sum(2, 3))
say("luna.greet('lua') = " .. luna.greet("lua"))
say("luna.scale({1,2,3,4}, 2.5) = " .. table.concat(luna.scale({ 1, 2, 3, 4 }, 2.5), ", "))

local ok, err = pcall(luna.scale, { 1, "two", 3 }, 2)
say("bad element -> " .. tostring(err))

local ok2, err2 = pcall(luna.boom)
say("luna.boom() -> " .. tostring(err2))

section("a class with hand-written Reg arrays")

local t = tracker.new("latency")
t:add(10):add(12):add(17):add(8)
say(string.format("%s  #t = %d  mean = %.2f  max = %.1f", tostring(t), #t, t:mean(), t:max()))

local empty = tracker.new("empty")
say("empty mean -> " .. tostring(select(2, pcall(empty.mean, empty))))
empty = nil

-- ------------------------------------------------------------ the world

section("configuration, read off a table")

world.configure({ title = "the demo world", difficulty = 3, verbose = true })
world.log("info", "starting up", 42, true, { 1, 2 })

section("values Lua owns")

local a = world.vec2(3, 4)
local b = world.vec2()
say("vec2(3, 4) = " .. tostring(a) .. "  length = " .. a:length())
say("vec2() = " .. tostring(b))
say("a.x = " .. a.x .. ", a.y = " .. a.y)

-- One name, two signatures, picked by what is passed.
say("a:plus(vec2(1, 1)) = " .. tostring(a:plus(world.vec2(1, 1))))
say("a:plus(10) = " .. tostring(a:plus(10)))

a.x = 30
say("after a.x = 30 -> " .. tostring(a))

section("objects the engine owns")

local goblin = world.spawn("goblin")
local orc = world.spawn("orc")

goblin:move_by(world.vec2(1, 2))
orc:move_by(world.vec2(4, 6))

say("goblin = " .. tostring(goblin))
say("orc    = " .. tostring(orc))
say("distance(goblin, orc) = " .. world.distance(goblin, orc))
say("distance(vec2(0,0), vec2(3,4)) = " .. world.distance(world.vec2(0, 0), world.vec2(3, 4)))

-- Fields, inherited and its own. `name` comes from Entity, `hp` from Creature.
goblin:damage(30)
say("goblin.name = " .. goblin.name .. ", goblin.hp = " .. goblin.hp)
say("goblin.alive = " .. tostring(goblin.alive))

-- A read-only field says so instead of quietly shadowing itself.
say("goblin.name = 'x' -> " .. tostring(select(
    2,
    pcall(function()
        goblin.name = "x"
    end)
)))

-- The setter clamps, so writing through the field cannot go out of range.
goblin.hp = 500
say("after goblin.hp = 500 -> " .. goblin.hp)

-- Enums are strings, and a misspelling is caught with the alternatives.
goblin.facing = "east"
say("goblin.facing = " .. goblin.facing .. ", opposite = " .. world.opposite(goblin.facing))
say("goblin.facing = 'up' -> " .. tostring(select(
    2,
    pcall(function()
        goblin.facing = "up"
    end)
)))

section("identity and per-instance state")

-- Two lookups of one creature are one Lua object, so a field set on it stays.
goblin.my_mod_data = { visits = 0 }
local same = world.find("goblin")
say("world.find('goblin') == goblin -> " .. tostring(same == goblin))
same.my_mod_data.visits = same.my_mod_data.visits + 1
say("state stashed by this mod survived the round trip: " .. goblin.my_mod_data.visits)

section("containers")

world.spawn_many({ "rat", "bat" })
local names = {}
for name, hp in pairs(world.census()) do
    names[#names + 1] = name .. "=" .. hp
end
table.sort(names)
say("census: " .. table.concat(names, ", "))

section("shared ownership")

local summoned = world.summon("wraith")
say("summoned " .. tostring(summoned) .. " -- the engine holds a count too")

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
say("fibonacci up to 50: " .. table.concat(out, ", "))

-- The handles init.lua needs. goblin is handed over rather than looked up
-- again so that the stale-handle demonstration holds a reference from before
-- the engine destroys it.
return {
    goblin = goblin,
    summoned = summoned,
}
