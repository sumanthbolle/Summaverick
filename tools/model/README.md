# Slim audio projector

The app only uses the audio encoder of Gemma 4 E4B's projector. The ggml-org file
`mmproj-gemma-4-E4B-it-Q8_0.gguf` (559.9 MB) also carries an unused image encoder. `strip_vision.py`
writes a copy without it (331.8 MB).

## Make the slim file

    pip install gguf
    python3 -I tools/model/strip_vision.py mmproj-gemma-4-E4B-it-Q8_0.gguf mmproj-gemma-4-E4B-it-audio-Q8_0.gguf

Rule: tensors starting with `v.` (image tower) and `mm.input_projection.` (image-to-text projection) are
dropped. `mm.a.*` (audio) and `a.*` are kept byte for byte. All metadata is copied except `clip.vision.*`,
and `clip.has_vision_encoder` is set to false so mtmd loads the file as audio-only.

## Check the output is identical

Run the same smoke test with both files (greedy decoding) and compare:

    export SVE_GEMMA=<path to gemma-4-E4B-it-Q4_K_M.gguf> SVE_TEST_WAV=<wav>
    SVE_MMPROJ=<original> core/tests/smoke_gemma.sh transcribe > orig.txt
    SVE_MMPROJ=<slim>     core/tests/smoke_gemma.sh transcribe > slim.txt
    cmp orig.txt slim.txt

Do the same for `live`; its output also prints timings (`rtf`, seconds), which differ between runs and
should be ignored when comparing.

## Upload to Hugging Face

    pip install -U huggingface_hub
    huggingface-cli login
    huggingface-cli repo create <name> --type model
    huggingface-cli upload <user>/<name> mmproj-gemma-4-E4B-it-audio-Q8_0.gguf mmproj-gemma-4-E4B-it-audio-Q8_0.gguf

Add a short model card: derived from Gemma 4 E4B, image encoder removed. The source file's metadata
has `general.license = apache-2.0` (link: https://ai.google.dev/gemma/docs/gemma_4_license); check the
licence terms yourself before publishing.

## Switch the app

In `ios/SummaverickEveryday/Engine/ModelManager.swift`, change `ModelHost`:

- `projectorURL` to `https://huggingface.co/<user>/<name>/resolve/main/mmproj-gemma-4-E4B-it-audio-Q8_0.gguf`
- `projectorBytes` to the slim file's size in bytes (`stat -f %z <file>`; the build here is 331821760)

Leave `fileName` (`mmproj-gemma-4-E4B-it-Q8_0.gguf`) as it is: phones that already have the full file keep
working and new installs save the slim one under the same name. The switch is one-way for the app: phones that already have the full file keep it and do not re-download.
Until you change `ModelHost`, the app keeps
downloading the full file.
