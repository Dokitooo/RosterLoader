RosterLoader 1.2.2 - NBA 2K14: your roster loaded at boot (about half a second, no boxes),
no "2K Sports server is temporarily unavailable" box on the title page,
your own Play Now default teams (DefaultHome / DefaultAway, by team ID: Heat 10, Knicks 14, Spurs 25)

SLAM:  copy SLAM\RosterLoader\ (asi + ini) into the game's SLAM folder.
plain: plain\RosterLoader.asi next to your other .asi files (any ASI loader),
       plus plain\RosterLoader\ (the ini) next to it.

New in 1.2.2: smoother on slow PCs - outside the roster load at boot, the game's clock is no longer locked on
every read (it is read thousands of times a second), and file reads that are not a roster pass straight through.
New in 1.2.1: works next to ReShade / ENB (a d3d9.dll in the game folder) - the hidden "Loading Roster" box uses
the Windows d3d9.dll underneath it and leaves the wrapper alone. Set up as soon as the game code is there (before
the game makes its Direct3D device), so the box is hidden in fullscreen too.
New in 1.2: the roster you load in the game is remembered and loads by itself at the next start.
Just load your roster once (Options > Load/Save > Load Roster), nothing to edit.
To always load one fixed roster instead: set AutoLoad= in RosterLoader.ini to its .ROS file name
(from the Saves folder). AutoLoad wins over the remembered roster. AutoLoadLast=0 turns remembering off.
The game window may be in the background while it boots (alt-tab): Enter reaches the game directly.
Log: RosterLoader\RosterLoader.log

Credits: Eddie 2K, for the idea of loading the roster by itself instead of doing it manually.
