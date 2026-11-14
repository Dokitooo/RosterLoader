RosterLoader 1.2 - NBA 2K14: your roster loaded at boot (about half a second, no boxes),
no "2K Sports server is temporarily unavailable" box on the title page,
your own Play Now default teams (DefaultHome / DefaultAway, by team ID: Heat 10, Knicks 14, Spurs 25)

SLAM:  copy SLAM\RosterLoader\ (asi + ini) into the game's SLAM folder.
plain: plain\RosterLoader.asi next to your other .asi files (any ASI loader),
       plus plain\RosterLoader\ (the ini) next to it.

New in 1.2: the roster you load in the game is remembered and loads by itself at the next start.
Just load your roster once (Options > Load/Save > Load Roster), nothing to edit.
To always load one fixed roster instead: set AutoLoad= in RosterLoader.ini to its .ROS file name
(from the Saves folder). AutoLoad wins over the remembered roster. AutoLoadLast=0 turns remembering off.
The game window may be in the background while it boots (alt-tab): Enter reaches the game directly.
Log: RosterLoader\RosterLoader.log

Credits: Eddie 2K.
