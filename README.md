# ReTempGBA4PSP-mod (2025 Build)

Based on andymcca' Github repo which is based on phoe-nix' Github repo.

- Added a 16:9 Full Screen option
- Fixed Menu text and Game -> Menu crashes
- Fixed a few games that have never worked previously -
  -   NBA Jam 2002
  -   Colin Mcrae Rally
  -   Banjo Kazooie
  -   Starsky and Hutch
  -   Balatro (https://github.com/cellos51/balatro-gba)
  -   ....and probably a few more!
- ~~Swapped X/O buttons around~~    (You can choose which button to use for confirm)
- ~~Couple of minor fixes to make it work under PPSSPP (including w/ HW Rendering)~~    (on PPSSPP will only work with SW Rendering)

## Building

On Windows, use **Docker Desktop** and the PSPDev image; see **[BUILD-DOCKER.md](BUILD-DOCKER.md)** for the exact `docker run` command (mount `source/`, run `make`).
