-- The mod's entry point.
--
-- Runs the tour in examples.lua, then registers the parts the engine drives.
-- The host loads this once and takes over from there: it runs frames, resumes
-- the quest coroutine, destroys a creature this script is holding, and reloads
-- the world module. See src/main.cpp for that half.

local world = require("world")
local demo = require("examples")

local function say(...)
    print("[Lua] " .. tostring(...))
end

print("\n--- registering handlers ---")

local goblin = demo.goblin
local summoned = demo.summoned

local ticks = 0
world.on_tick(function(dt)
    ticks = ticks + 1
    say(string.format("tick %d (+%.1fs), goblin at %s", ticks, dt, tostring(goblin.position)))
end)

-- A second handler that faults, to show one plugin cannot cost the others
-- their frame.
world.on_tick(function()
    error("this handler is broken")
end)

say("two handlers registered; the host fires them")

-- A coroutine the host drives. world.wait is a C++ function that suspends this
-- task, so the engine decides when it continues.
function quest(turn)
    say("quest: starting at turn " .. turn)
    turn = world.wait(2)
    say("quest: woke up, waiting again")
    world.wait(3)
    say("quest: done")
end

-- Called by the host after it destroys the goblin out from under us.
function report_stale_handle()
    local ok, err = pcall(function()
        return goblin.hp
    end)
    say("touching the destroyed goblin -> " .. tostring(err))
    say("world.find('goblin') is now " .. tostring(world.find("goblin")))
end

-- Called by the host after it drops its own reference to the wraith.
function report_summoned()
    say("the wraith is still here: " .. tostring(summoned) .. ", hp " .. summoned.hp)
end
