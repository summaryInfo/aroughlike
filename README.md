A Simple Rough-like game (MSU CG course, A1 variant)
====================================================

This is an implementation of a simple
rough-like game with software rendering.

## Installing dependencies

 Number of dependencies is minimized, so the only required libs are xcb-shm and xcb.
They can be installed:

 * On Debian derivatives (e.g. Debian/Ubuntu/Mint): `apt update && apt install libx11-xcb-dev libxcb-shm0-dev`
 * On Arch Linux (and derivatives): `pacman -S libxcb`
 * On Void Linux: `xbps-install -S libxcb-devel`

It also requires GNU or BSD make, GCC compiler and pkg-config
Blending code requires SSE4.1

## Building and running

This program can be run with

    make run -j$(nproc)

To compile a program you just need to

    make -j$(nproc)

## Gameplay

 * `w` -- move forward
 * `a` -- move left
 * `s` -- move backward
 * `d` -- move right
 * `DEL` -- reset game
 * `ESC` -- exit game
 * `+`/`=` -- zoom-in
 * `-` -- zoom-out
 * `SPACE` -- action (open exit)

Your goal is to find the way though 10 randonly generated levels.
(If there's a file named `data/map_N.txt` then it is loaded as a map instead of generating a random one)
Levels contains traps (spikes) that damages you.
Traps are activated randomly, but have a very short invactive period after activation.

There are also 4 types of poison placed around the level:

* Big healing poisons -- gives you extra live
* Small healing poison -- gives you a half of a live
* Small invincibility poison -- makes you invulnurable for 1 second (every posion adds one second)
* Big invincibility posion -- same, but gives you two seconds

Taken damage shortens invincibility duration.

Exits from level can be closed, in this case there's a key randomly placed in level.
When you have a key you can open a door with a `space` button.

Map is initially hidden until player sees it.
Visibility is implemented via ray casting on tile map.

## Optional features

From mentioned in task:

* Traps
* Camera and huge levels (5)
* Smooth level transitions (3)
* Status indicators (2)
* Idle/walking animations (2-5)
* Animated objects (2)
* Damage indicators/effects (2)

Not mentioned in task:

* Map visibility
* Keys and closed doors
* Health and invincibility poisons
* Procedural map generation
* Multi-threaded/SIMD renderer
* 7 random characters

# Resources

* Tileset used was downloaded [here](https://pixel-poem.itch.io/dungeon-assetpuck) (and modified to suite the game needs)
* ASCII tiles are from [here](http://97.107.128.126/smf/index.php?topic=172536.0)
