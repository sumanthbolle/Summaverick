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

# Trimmed vocabulary (experiment)

Gemma 4 has a 262,144-token vocabulary. Each token has a row in `token_embd` (0.38 GB, also the output
layer) and in `per_layer_token_embd` (1.94 GB). `trim_vocab.py` keeps only the tokens the app's languages
can use and drops the other rows.

## Make the file

    pip install gguf
    # 1. token ids the full model uses on the test set (see testset/README.md); ~30 min on an M-series Mac
    SVE_MMPROJ=<projector> TESTSET=<clips dir> tools/model/compare_outputs.sh collect gemma-4-E4B-it-Q4_K_M.gguf tokens.txt
    # 2. trim
    python3 -I tools/model/trim_vocab.py gemma-4-E4B-it-Q4_K_M.gguf gemma-4-E4B-it-trimmed-Q4_K_M.gguf \
        --keep-scripts latin,telugu,devanagari --keep-observed tokens.txt
    # 3. gate: full vs trimmed, greedy, every item must be byte-identical; ~1-4 h (longnotes runs on the CPU)
    SVE_MMPROJ=<projector> TESTSET=<clips dir> tools/model/compare_outputs.sh compare gemma-4-E4B-it-Q4_K_M.gguf gemma-4-E4B-it-trimmed-Q4_K_M.gguf

The rules for which tokens stay are at the top of `trim_vocab.py`. Kept tokens stay in the same order and
special tokens are found by their text, so llama.cpp needs no change. The script reopens its output and
checks it. Keep the Mac from sleeping during step 3 (`caffeinate -s`).

Result on 2026-11-08: 181,840 of 262,144 tokens kept, 4,977,171,584 -> 4,263,876,448 bytes (713 MB smaller),
31/31 items identical, `GATE: PASS`. About 150,000 of the kept tokens are plain ASCII (English and code),
which is why the file does not shrink more.

The full model sometimes writes a token from another script (Tamil, Bengali, Thai, Korean, Sinhala) in
noisy Telugu speech. The trimmed file cannot do that, so on new recordings its transcript can differ from
the full model's at those spots. It writes the Telugu letter or a near match there instead (for example
Telugu `ారు` where the full model wrote Tamil `ாரு`).

## Upload to Hugging Face

    huggingface-cli login
    huggingface-cli repo create <name> --type model
    huggingface-cli upload <user>/<name> gemma-4-E4B-it-trimmed-Q4_K_M.gguf gemma-4-E4B-it-trimmed-Q4_K_M.gguf

Add a model card: derived from unsloth's Gemma 4 E4B Q4_K_M, vocabulary limited to Latin, Telugu and
Devanagari plus common symbols, all other weights unchanged. `general.license` is `apache-2.0`
(https://ai.google.dev/gemma/docs/gemma_4_license); check the terms yourself before publishing.

## Switch the app (after the upload, in a later task)

In `ios/SummaverickEveryday/Engine/ModelManager.swift`, `ModelCatalog.gemma.weights`: set `url` to
`https://huggingface.co/<user>/<name>/resolve/main/gemma-4-E4B-it-trimmed-Q4_K_M.gguf` and `approxBytes`
to the uploaded file's size (`stat -f %z`; 4263876448 for this build). Keep `fileName`: phones that already
have the full file keep using it; new installs save the trimmed one under the same name.
