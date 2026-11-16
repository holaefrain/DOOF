#pragma once
#include <juce_core/juce_core.h>
#include <cmath>

// PitchSnap — §3.5's "pitch snap: snap nodes to nearest harmonic note", read as the nearest note
// of the equal-tempered scale (user decision, 2026-11-16).
//
// Everything here works in drawn values, at A4 = 440 Hz. Master tuning is applied to the Sub's
// output, not to the curve (SubVoice::setTuning), so snapping the drawn value to the 440 grid is
// exactly what puts the *played* pitch on the tuned grid: a node snapped to C1 plays C1 at
// whatever reference master tuning sets. Snapping to an already-tuned grid would apply the tuning
// twice.
//
// "Nearest" is measured in pitch, not in Hz - the note with the fewest cents away - which is what
// a musician means by nearest and is not the same thing at all: 2 Hz is a semitone at 35 Hz and a
// few cents at 400.
//
// juce_core only, so it can be tested in the lean DOOFTests target and used by both the canvas and
// the parameter display.
namespace PitchSnap
{
    constexpr double kA4Hz   = 440.0;
    constexpr int    kA4Note = 69;

    // Frequency of a MIDI note, at A4 = 440 Hz.
    inline double noteToFrequency(int midiNote)
    {
        return kA4Hz * std::exp2((midiNote - kA4Note) / 12.0);
    }

    // The MIDI note nearest a frequency, in pitch. hz must be positive.
    inline int nearestNote(double hz)
    {
        jassert(hz > 0.0);
        return (int) std::lround(kA4Note + 12.0 * std::log2(hz / kA4Hz));
    }

    // How far hz is from its nearest note, in cents: -50 to +50.
    inline double centsFromNearest(double hz)
    {
        return 1200.0 * std::log2(hz / noteToFrequency(nearestNote(hz)));
    }

    // The nearest note's frequency.
    inline double snap(double hz)
    {
        return noteToFrequency(nearestNote(hz));
    }

    // The nearest note's frequency that also lies within [low, high]. A value near an edge can be
    // nearest to a note just outside it; stepping one semitone inward keeps the result both on a
    // note and in range, where clamping after snapping would leave it on neither. If no note fits
    // at all - a range narrower than a semitone - the value is returned unsnapped, clamped.
    inline double snapWithin(double hz, double low, double high)
    {
        int note = nearestNote(hz);

        if (noteToFrequency(note) < low)
            ++note;
        else if (noteToFrequency(note) > high)
            --note;

        const double snapped = noteToFrequency(note);
        if (snapped < low || snapped > high)
            return juce::jlimit(low, high, hz);

        return snapped;
    }

    // Note names as Ableton prints them, where middle C (60) is C3 and MIDI 0 is C-2.
    inline juce::String noteName(int midiNote)
    {
        static const char* const names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const int octave = (midiNote >= 0 ? midiNote / 12 : (midiNote - 11) / 12) - 2;
        return juce::String(names[((midiNote % 12) + 12) % 12]) + juce::String(octave);
    }
}
