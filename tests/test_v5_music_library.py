import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import v5_music_library as lib


class MusicLibraryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        root = patch.object(lib, "ROOT", self.path/"library")
        root.start()
        self.addCleanup(root.stop)
        self.db = lib.connect()
        self.addCleanup(self.db.close)
        self.source = self.path/"sample.wav"
        sf.write(self.source, .2*np.sin(np.arange(12*8000)/8),8000)
        self.track, _ = lib.import_track(self.db,self.source,{})

    def test_import_and_reanalysis(self):
        self.assertFalse(lib.import_track(self.db,self.source,{})[1])
        self.assertEqual(lib.catalogue(self.db),[])
        self.db.execute("UPDATE parts SET start=1,end=10,note='under dialogue'")
        with patch.object(lib,'chat',return_value={"description":"soft melodic pulse"}) as send:
            lib.describe(self.db,self.track,{},"audio-model")
            self.assertEqual(send.call_args.args[2][1]["type"],"input_audio")
        card, = lib.catalogue(self.db)
        self.assertEqual((card["start"],card["end"],card["note"]),(1,10,"under dialogue"))
        self.assertTrue(card["description"])
        self.db.execute("UPDATE parts SET enabled=0")
        self.assertEqual(lib.catalogue(self.db),[])

    def test_model_bounds(self):
        self.db.execute("UPDATE tracks SET note='quiet'")
        cards=lib.catalogue(self.db)
        cue=dict(id=cards[0]["id"],track_start=1,**{"in":2,"out":6})
        self.assertEqual(lib.validate([cue],cards,8)[0]["out"],6)
        for bad in ([cue,cue],[dict(cue,track_start=11)],[dict(cue,id=999)],[dict(cue,**{"out":9})]):
            with self.assertRaises(ValueError):lib.validate(bad,cards,8)


if __name__ == "__main__":
    unittest.main()
