#!/usr/bin/env python3
"""Copy a multimodal projector GGUF without its image encoder, keeping the audio encoder.

Usage: python3 -I tools/model/strip_vision.py <in.gguf> <out.gguf>   (needs the `gguf` package)

Rule: a tensor is dropped when its name starts with "v." (vision tower) or "mm.input_projection."
(the vision-to-text projection; the audio one is "mm.a.input_projection.*" and is kept).
Everything else is copied byte for byte. Metadata: every key is copied except the "clip.vision.*"
keys, and clip.has_vision_encoder is written as false, so llama.cpp's mtmd loads the file as audio-only.
"""
import os
import sys

from gguf import GGUFReader, GGUFValueType, GGUFWriter

DROP_PREFIXES = ("v.", "mm.input_projection.")
SKIP_KEYS = {"GGUF.version", "GGUF.tensor_count", "GGUF.kv_count", "general.architecture"}


def main(src, dst):
    r = GGUFReader(src)
    arch = r.fields["general.architecture"].contents()
    w = GGUFWriter(dst, arch)
    for key, f in r.fields.items():
        if key in SKIP_KEYS:
            continue
        if key.startswith("clip.vision."):
            continue
        if key == "alignment" or key == "general.alignment":
            continue
        v = f.contents()
        t = f.types[0]
        if key == "clip.has_vision_encoder":
            v = False
        if t == GGUFValueType.ARRAY:
            w.add_array(key, v)
        else:
            w.add_key_value(key, v, t)
    kept = dropped = 0
    for t in r.tensors:
        if t.name.startswith(DROP_PREFIXES):
            dropped += t.n_bytes
            continue
        kept += t.n_bytes
        w.add_tensor(t.name, t.data, raw_dtype=t.tensor_type)
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file(progress=False)
    w.close()
    a, b = os.path.getsize(src), os.path.getsize(dst)
    print(f"before: {a} bytes ({a/1e6:.1f} MB)")
    print(f"after:  {b} bytes ({b/1e6:.1f} MB)")
    print(f"dropped tensors: {dropped} bytes, kept tensors: {kept} bytes")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
