# Predictive Input Buffer

A Geode mod for Geometry Dash 2.2081 (Windows).

When you click, the mod quickly simulates an invisible copy of your player a short way ahead. If clicking now would kill you but clicking a few ticks later would survive, your click is held back and sent at that later moment. If clicking now is fine, nothing changes.

1 tick = 1/240 of a second = a quarter of a frame at 60 fps.

## Controls

- **F6** (in a level) cycles the strength: OFF → 1 frame → 2 frames → 4 frames → 8 frames.
- The current strength and the last saved click show in the bottom-left corner.

## Settings

In Geode's mod settings:

- **Starting strength**: what F6 starts on when you enter a level.
- **Predict orbs, pads and gravity portals**: on by default. Turn off if you see glitches near them.
- **Prediction time limit**: the most time spent predicting per click.
- **Show status label**: hides the corner text.

## Not buffered

Ship mode, platformer levels and 2-player levels. Prediction stops at mode, size, dual, mirror and teleport portals.

## Important

This changes your input timing, so completions made with it are **not legit** for demon lists or leaderboards.

## Building

Push to GitHub and the included workflow builds it. Download the `.geode` file from the Actions tab and put it in `Geometry Dash\geode\mods\`.

Or build locally with the Geode CLI: `geode build`.
