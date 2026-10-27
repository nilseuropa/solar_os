-- Cycle expressions on the current display. For a port shell, replace nil
-- with a ready display target from `display list`.
local solaros = require("solaros")
local poses = {
    "neutral", "blink_high", "happy", "glee", "blink_low", "sad_down",
    "sad_up", "worried", "focused", "annoyed", "surprised", "skeptic",
    "bored", "unimpressed", "sleepy", "suspicious", "squint", "angry",
    "furious", "scared", "awe",
}
local face <close> = solaros.emotion.open(nil, true)
for _, pose in ipairs(poses) do
    if solaros.should_exit() then break end
    face:set(pose)
    solaros.time.sleep_ms(1400)
end
if not solaros.should_exit() then
    face:set("happy")
    face:look(0.5, -0.2)
    face:react("surprised", 900, 120, "chirp")
    solaros.time.sleep_ms(1400)
end
