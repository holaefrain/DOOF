#pragma once

// LayerViewPrefs — one layer's canvas view settings: §3.4's log/linear vertical-axis toggle and
// the editing-curve selector. Held per layer, so switching layers restores how you were looking
// at that one instead of carrying the previous layer's settings across.
//
// Owned by the processor, not the editor. The host creates and destroys the editor whenever it
// likes, so anything that must survive a closed window cannot live there - and these persist in
// the preset besides.
//
// Deliberately free of any JUCE dependency, same reasoning as LayerAudibility: the processor owns
// it and the GUI consumes it, and keeping it plain means neither has to include the other's
// headers. editingPitch is a bool rather than EnvelopeCanvas::ActiveCurve for the same reason -
// the editor converts at the boundary.
//
// The defaults below are the pre-Phase-4 behaviour, which is what a preset written before these
// existed is taken to mean.
struct LayerViewPrefs
{
    bool pitchLogScale = false; // vertical axis for the pitch curve; false = linear
    bool editingPitch  = true;  // which curve double-click-to-add targets; false = amp
};
