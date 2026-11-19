# Sid Meier's Pirates! - Couch Co-op mod

> ## ⚠️ GOG version 2.0.0.4 ONLY
> **This mod works ONLY with the GOG.com version of Sid Meier's Pirates! (2004), installer 2.0.0.4
> (game version 1.0.2.0, build 30927). It will NOT work with any other store version of the game - not Steam,
> not retail CD/DVD, not any other release or patch.** On any other version the mod switches itself off and the
> game runs without it.

Two players, two controllers, one TV. Player 1 sails the flagship, Player 2 sails another ship of the fleet, and
both fight in sea battles. Also: full Xbox-style controller support, native widescreen (e.g. 1920x1080, no
stretching, no black bars) and an in-game mod menu.

Made for playing with a young child: Player 2 can sail freely, and a gentle "babysitter" brings his ship back to
Player 1 if it leaves the screen or is left alone for a while.

**You need your own copy of the game.** This download contains only the mod - no game files.

## Supported game version
Only this exact version is supported:

- **Sid Meier's Pirates! (2004), GOG.com release, installer 2.0.0.4** (main menu shows "Version 1.0.2.0 (30927)")
- `Pirates!.exe` SHA-1: `aeca627205dfb744654e6a421124fa8f66a6f2ea`

Any other version (Steam, other stores, retail CD/DVD, other patches) is **not supported**. The mod checks the game when it starts:
if `Pirates!.exe` is not the supported one, it shows a message, switches itself off, and the game runs normally.
The mod never changes any game file on disk.

## Install
1. Download the release zip and unzip it.
2. Copy `version.dll` and `PiratesCoop.ini` into the game folder (the folder that contains `Pirates!.exe`).
3. Connect two XInput (Xbox-style) controllers and start the game as usual.
4. Co-op starts once your fleet has at least 2 ships (see `RequireTwoShips` in the ini).

## Uninstall
Delete `version.dll`, `PiratesCoop.ini` and `PiratesCoop*.log` from the game folder.

## Settings
Everything is in `PiratesCoop.ini` (each line is explained there). Many options can also be changed in game in the
mod menu (**RB + D-pad Up**, either player): ship speed, extra ships/upgrades/gold/crew, happy crew, fame,
friendly nations, never dizzy in sword fights, Player 1 vs Player 2 sword fights, and more.
The default screen size is 1920x1080 (`Width`/`Height` under `[Display]`; 0 = the game's own setting).

## Main features
- Player 2 controls a second ship of the fleet while sailing and in sea battles (point the stick where to go)
- SELECT (Back) passes control between the players in sword fights, town menus and other one-player scenes
- Xbox-style controls everywhere, on-screen keyboard for names, mouse ignored while controllers are used
- Native widescreen: wider view at sea, menus shown whole, backgrounds continued to the TV edges
- Smooth sea-battle camera zoom (hold D-pad Up/Down), D-pad zoom while sailing
- Optional: reduced sails = full speed on every heading; Player 2's ship stops at the coast
- Merchant/plunder screen and tavern fixed for controllers and widescreen
- Land battles: the D-pad moves the selected unit
- Log file `PiratesCoop.log` in the game folder (set `Verbose=1` for details) - please attach it to bug reports

## Known issues
- Do not Alt+Tab (or otherwise switch away from) the game while it runs full screen: it can crash with
  "R6025 - pure virtual function call".
- Fleet size is limited by the game (bigger fleets are not supported yet).
- Player 1 vs Player 2 sword fights (mod menu, off by default) are new and may need tuning.

## Building from source
`src/` contains the full source. Build with 32-bit MinGW-w64: `cd src && bash build.sh` (needs
`i686-w64-mingw32-gcc`). The result is `out/version.dll`, a proxy for Windows' version.dll that loads the mod
into the game and passes every normal version.dll call on to the real one.

## Credits
- Sid Meier's Pirates! is (c) 2004 Firaxis Games / 2K. This is an unofficial fan mod, not affiliated with or
  endorsed by Firaxis Games, 2K or GOG.
- On-screen text font: DejaVu Sans Bold (DejaVu fonts license, based on Bitstream Vera).

## Controls (Xbox-style controller)
```
=== MENUS / TOWN + PORT SCREENS ===
LEFT STICK or D-PAD = move highlight
A = choose
B = back / cancel
START = pause (where the game allows it)
SELECT (tap) = pass control to the other player
RB + D-PAD UP = mod menu (either player)

=== NAME ENTRY (on-screen keyboard) ===
LEFT STICK or D-PAD = move on the keyboard
A = type letter
B = delete
X = space
Y = capitals
START = done

=== CREW SIGNUPS ===
A = tick the highlighted box
START = confirm and continue

=== PLUNDER / TRADE SCREEN ===
D-PAD UP/DOWN = pick a row or button
D-PAD LEFT/RIGHT = move one item of cargo between the ships
A = press the highlighted button
B = Continue (leave)

=== WORLD MAP ===
LEFT or RIGHT STICK (either player) = scroll the map
A or B = close the map

=== SAILING - PLAYER 1 ===
LEFT STICK = steer
D-PAD UP = full sails
D-PAD DOWN = reduced sails
D-PAD LEFT = zoom out
D-PAD RIGHT = zoom in
LT (hold) = zoom in
RT (hold) = zoom out
A = bombard / act on the nearest town when in range
B = change view (chase camera on/off)
X = attack a ship in range
Y = next fleet ship (on land: back to your ship)
SELECT = next fleet ship (skips Player 2's ship)
RB (tap, works on release) = world map
RB + D-PAD UP = mod menu
LB = captain's status page
START = pause
RIGHT STICK CLICK = same as right mouse click

=== SAILING - PLAYER 2 ===
LEFT STICK or D-PAD = point where your ship should go (let go = keep course; after 20 s follows Player 1)
SELECT or Y = switch to another fleet ship (never Player 1's)
RB + D-PAD UP = mod menu

=== SEA BATTLE - PLAYER 1 ===
LEFT STICK = steer
A = fire a broadside (always allowed, Player 2's ship never blocks it)
X or B = change shot type: round -> chain -> grape
Y = sails up / down (takes at most ~1 s)
D-PAD UP (hold) = camera slowly zooms out (up to 2x the game's farthest view; stays where you let go)
D-PAD DOWN (hold) = camera slowly zooms back in to the game's normal height
D-PAD LEFT = zoom out (game step)
D-PAD RIGHT = zoom in (game step)
RB (tap, works on release) = chase camera on/off
RB + D-PAD UP = mod menu
START = pause

=== SEA BATTLE - PLAYER 2 ===
LEFT STICK or D-PAD = point where your ship should go
A = fire (also LB or RB; always allowed, Player 1's ship never blocks it)
X or B = shot type: round -> chain -> grape
Y = sails up / down (instant)
RAM the enemy flagship = boarding + sword fight (mod menu option)

=== SWORD FIGHT ===
A = thrust
UP + A = high chop
DOWN + A = low slash
B = parry
UP + B = jump
DOWN + B = duck
X = taunt
LT / RT = taunt
START = pause
SELECT (tap) = pass the sword to the other player (only way to pass it)

=== DANCING ===
Y or D-PAD UP = forward
A or D-PAD DOWN = back
X or D-PAD LEFT = left
B or D-PAD RIGHT = right
LT = circle right
RT = circle left
START = pause
SELECT (tap) = pass control

=== SNEAKING IN/OUT OF TOWNS ===
LEFT STICK = walk
A = action / run
START = pause
SELECT (tap) = pass control

=== LAND BATTLES ===
RIGHT STICK (either player) = scroll the camera
LEFT STICK (A not held, either player) = scroll the camera
X = fire at the chosen target
D-PAD = move the selected unit (8 directions; camera does not move)
A (hold) + LEFT STICK = move the selected unit
START = pause
SELECT (tap) = pass control

=== SHIPWRIGHT ===
highlight starts on Repair if the ship is damaged, on Sell if it is already repaired

=== MOD MENU (open with RB + D-PAD UP) ===
D-PAD or LEFT STICK UP/DOWN = choose a line
D-PAD or LEFT STICK LEFT/RIGHT = change the value (speed, ship, crew)
A = do it / switch ON-OFF
B or START or SELECT = close
```
