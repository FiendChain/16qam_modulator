#!/bin/sh
# aplay equivalent on windows which uses vlc to pipe in the raw audio data
# change the raw sampling rate if it is specified differently
./vlc.sh --demux=rawaud --rawaud-channels 1 --rawaud-samplerate 17390 -
