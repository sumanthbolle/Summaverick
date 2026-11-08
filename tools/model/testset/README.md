# Test set for the trimmed-vocabulary gate

Audio here is private (the owner's recordings) and git-ignored. Only this README is in git.

Contents: 16 kHz mono 16-bit WAVs, at most 60 s each.

- `rec-<id>-<k>.wav`: clips of the owner's recordings. Each recording gives a clip from 0 s, and recordings
  longer than 150 s also give clips from 40 % and 75 % of their length. Clips shorter than 2 s are skipped.
- `te-reference.wav`: the first 60 s of the Telugu reference clip (the finance talk used since the
  Gemma 4 E4B transcription test).

## Getting the recordings

With the iPhone connected:

    D=00008140-001668892E22801C
    xcrun devicectl device info files --device $D --domain-type appDataContainer \
        --domain-identifier com.summaverick.everyday --subdirectory "Library/Application Support/Summaverick/recordings"
    xcrun devicectl device copy from --device $D --domain-type appDataContainer \
        --domain-identifier com.summaverick.everyday \
        --source "Library/Application Support/Summaverick/recordings/<file>.wav" --destination <dir>/<file>.wav

Then cut the clips with Python's `wave` module as above (any tool that keeps 16 kHz mono 16-bit works).
If the phone cannot be read, use the Telugu reference clip and the WAVs already on the Mac, and say in the
result that the set is smaller.

The directory does not have to be this one: `compare_outputs.sh` reads `TESTSET`.
