"""Regression checks without loading ASR models or using the network."""
import contextlib
import io
import json
import pathlib
import runpy
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import asr_bridge
import review
import numpy as np


def word(text, start=1, end=2, lp=None):
    return {"w": text, "t": start, "e": end, "lp": lp, "chunk": 0}


class TranscriptV3Tests(unittest.TestCase):
    def merge(self, words, review_data=None, flags=()):
        with tempfile.TemporaryDirectory(prefix="clipper_merge_test_") as tmp:
            root = pathlib.Path(tmp).resolve()
            self.assertTrue(root.is_relative_to(pathlib.Path(tempfile.gettempdir()).resolve()))
            inputs = {"words.json": {"audio_sec": 60, "language": "ru", "words": words},
                      "map.json": {"segments": [{"start": 0, "end": 60, "label": "music"}]},
                      "review.json": review_data or {}}
            for name, data in inputs.items():
                (root / name).write_text(json.dumps(data), encoding="utf-8")
            argv = ["merge_transcript.py", "--words", str(root / "words.json"),
                    "--out", str(root / "speech.srt"), "--packed", str(root / "packed.md"),
                    "--clean-words", str(root / "clean.json"), "--audiomap", str(root / "map.json"),
                    "--review", str(root / "review.json"), *flags]
            original = sys.argv
            try:
                sys.argv = argv
                with contextlib.redirect_stderr(io.StringIO()):
                    runpy.run_path(str(ROOT / "scripts/merge_transcript.py"), run_name="__main__")
            finally:
                sys.argv = original
            return ((root / "speech.srt").read_text(encoding="utf-8"),
                    (root / "packed.md").read_text(encoding="utf-8"),
                    json.loads((root / "clean.json").read_text(encoding="utf-8")))

    def test_unknown_confidence_keeps_dialogue_under_music(self):
        srt, _, clean = self.merge([word("real dialogue")], flags=["--drop-music-lowconf"])
        self.assertIn("real dialogue", srt)
        self.assertEqual(len(clean["words"]), 1)

    def test_rejected_words_never_return_to_clean_subtitles(self):
        srt, packed, clean = self.merge([word("good", lp=-0.1), word("phantom", 3, 4, -0.9)],
                                        flags=["--drop-music-lowconf"])
        self.assertNotIn("phantom", srt + packed)
        self.assertEqual([w["w"] for w in clean["words"]], ["good"])

    def test_markers_only_appear_in_llm_context(self):
        srt, packed, _ = self.merge([word("before"), word("after", 40, 41)])
        self.assertIn("музыка", packed)
        self.assertNotIn("музыка", srt)

    def test_repairs_reach_both_phrases_and_clean_words(self):
        replacement = word("corrected", 1, 2)
        srt, packed, clean = self.merge([word("wrong")],
            {"corrections": [{"start": 0, "end": 10, "words": [replacement]}]})
        self.assertIn("corrected", srt)
        self.assertIn("corrected", packed)
        self.assertEqual(clean["words"], [replacement])
        self.assertNotIn("wrong", srt + packed)

    def test_disagreement_is_not_enough_to_overwrite(self):
        chunk = {"start": 0, "end": 10, "suspicious": True, "zone": "speech", "reasons": []}
        result = review.consensus_corrections([chunk], [word("original")],
                                              [word("second")], [word("third")], .6)
        self.assertEqual(result, [])
        self.assertTrue(chunk["suspicious"])

    def test_independent_review_and_retry_can_repair(self):
        chunk = {"start": 0, "end": 10, "suspicious": True, "zone": "speech", "reasons": []}
        fixed = [word("confirmed")]
        result = review.consensus_corrections([chunk], [word("wrong")], fixed, fixed, .6)
        self.assertEqual(result[0]["words"], fixed)
        self.assertFalse(chunk["suspicious"])

    def test_chunk_limit_and_full_audio_coverage(self):
        audio = np.random.default_rng(7).normal(0, .1, int(76.17 * asr_bridge.SR)).astype("float32")
        for target in (5, 20, 25, 30):
            chunks = asr_bridge.plan_chunks(audio, target)
            self.assertEqual(chunks[0][0], 0)
            self.assertAlmostEqual(chunks[-1][1], len(audio) / asr_bridge.SR)
            self.assertTrue(all(0 < b - a <= 25.00001 for a, b in chunks))
            self.assertTrue(all(chunks[i][1] == chunks[i + 1][0] for i in range(len(chunks) - 1)))

    def test_phrase_limit_includes_incoming_word(self):
        ns = runpy.run_path(str(ROOT / "scripts/merge_transcript.py"))
        phrases = ns["build_phrases"]([word("one", 0, 11.9), word("two", 11.95, 13.5)], [], .5, 12)
        self.assertEqual(len(phrases), 2)


if __name__ == "__main__":
    unittest.main()
