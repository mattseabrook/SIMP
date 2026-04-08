// music.h

#ifndef MUSIC_H
#define MUSIC_H

#include <string>
#include <vector>

/*
===============================================================================

    Phantom Engine - Extended MIDI (XMI) Engine

    Extracts or Plays XMI data converted to MIDI

===============================================================================
*/

// Function prototypes
// Note: XMI playback is optional.
void PlayMIDI(const std::vector<uint8_t> &midiData, bool isTransient = false);
void xmiPlay(const std::string &songName, bool isTransient = false);
void pushMainSong(const std::string &songName);
void popMainSong();

#endif // MUSIC_H