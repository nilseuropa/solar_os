import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples/python"))
import model_bundle as B


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


with patch.dict(sys.modules, solaros=types.SimpleNamespace()):
    RUNNER = load_module("bundle_runner", ROOT / "examples/python/infer.py")


class BundleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        self.manifest = json.loads((ROOT / "tests/fixtures/model_bundle/raw.json").read_text())
        self.binary = b"test-model"
        self.manifest["model"]["sha256"] = hashlib.sha256(self.binary).hexdigest()
        (self.folder / self.manifest["model"]["file"]).write_bytes(self.binary)
        self.path = self.folder / "bundle.json"
        self.write()
        self.closed, self.calls = [], []
        self.info = {"backend": "espdl", "backend_version": "3.3.13", "target": "esp32s3",
                     "inputs": copy.deepcopy(self.manifest["inputs"]), "outputs": copy.deepcopy(self.manifest["outputs"])}
        for port in self.info["inputs"].values():
            port["bytes"] = 8
        self.fake = types.SimpleNamespace(
            time=types.SimpleNamespace(sleep_ms=lambda _: None, uptime_ms=lambda: 1000),
            inference=types.SimpleNamespace(load=lambda *_: 7, close=self.closed.append,
                info=lambda _: self.info, set_mode=lambda *a: self.calls.append(a),
                run=self.run_native, reset=lambda _: None))
        self.patch = patch.object(B, "solaros", self.fake)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def write(self):
        self.path.write_text(json.dumps(self.manifest))

    def run_native(self, handle, inputs, timeout):
        self.calls.append((handle, inputs, timeout))
        return {"outputs": {"sum": {"dtype": "int8", "shape": [2, 4], "exponents": [0],
                                     "bytes": 8, "data": bytes([3,1,255,5,1,4,7,252])}},
                "input_us": 1, "inference_us": 2, "output_us": 3, "elapsed_us": 6}

    def native_client(self, existing=None):
        self.info.update(bundle=True, bundle_id="test", bundle_version="1", image_inputs={"a":False,"b":False})
        self.fake.inference.find = lambda path: self.calls.append(("find",path)) or existing
        self.fake.inference.load_bundle = lambda path,timeout: self.calls.append(("load_bundle",path,timeout)) or 7
        self.fake.inference.run_bundle = lambda *args: dict(self.run_native(*args), result={"kind":"raw"}, transforms={}, preprocess_us=0, postprocess_us=1)

    def test_native_client_loads_without_reading_manifest_and_keeps_resident(self):
        self.native_client()
        with patch("builtins.open",side_effect=AssertionError("client must not parse manifest")):
            with B.ModelBundle("/model/bundle.json") as bundle:
                self.assertEqual(bundle.run({"a":b"a"*8,"b":b"b"*8})["result"]["kind"],"raw")
            self.assertIsNone(bundle.handle)
        self.assertIn(("load_bundle","/model/bundle.json",60000),self.calls)
        self.assertFalse(self.closed)

    def test_native_client_reuses_path_or_attaches_handle_and_explicitly_unloads(self):
        self.native_client(existing=7)
        for value in ("/model/bundle.json",7,"7"):
            with self.subTest(value=value):
                self.calls.clear()
                bundle=B.ModelBundle(value,"dual")
                self.assertEqual(bundle.handle,7)
                self.assertFalse(any(call[0]=="load_bundle" for call in self.calls))
                bundle.close(); bundle.close()
        self.assertEqual(self.closed,[7,7,7])

    def test_native_failure_and_context_exception_preserve_shared_model(self):
        self.native_client(existing=7)
        with patch.object(self.fake.inference,"run_bundle",side_effect=OSError("deadline")), self.assertRaises(OSError):
            with B.ModelBundle(7) as bundle:
                bundle.run({"a":b"a"*8,"b":b"b"*8})
        self.assertFalse(self.closed)
        self.assertIsNone(bundle.handle)
        self.info["bundle"]=False
        with self.assertRaisesRegex(ValueError,"not a model bundle"):
            B.ModelBundle(7)
        self.assertFalse(self.closed)

    def test_nonimage_named_inputs_residency_and_explicit_close(self):
        with B.ModelBundle(str(self.path), "auto") as bundle:
            for _ in range(3):
                result = bundle.run({"a": b"a" * 8, "b": b"b" * 8})
                self.assertEqual(result["result"]["kind"], "raw")
                self.assertEqual(result["preprocess_us"], 0)
                self.assertEqual(result["transforms"], {})
            bundle.reset()
            self.assertEqual(self.closed, [])
            handle = bundle.handle
        self.assertEqual(self.closed, [])
        self.assertIsNone(bundle.handle)
        self.assertEqual(result["outputs"]["sum"]["data"][0], 3)
        bundle.close()
        self.assertEqual(self.closed, [])
        self.fake.inference.close(handle)
        self.assertEqual(self.closed, [7])
        with self.assertRaises(ValueError):
            bundle.run({"a": b"", "b": b""})

    def test_compatibility_or_descriptor_failure_releases_loaded_model(self):
        for kind in ("version", "shape", "names", "exponents"):
            with self.subTest(kind=kind):
                self.closed.clear()
                old = copy.deepcopy(self.info)
                if kind == "version":
                    self.info["backend_version"] = "0"
                elif kind == "names":
                    del self.info["inputs"]["a"]
                else:
                    self.info["inputs"]["a"][kind] = [99]
                with self.assertRaises(ValueError):
                    B.ModelBundle(str(self.path))
                self.assertEqual(self.closed, [7])
                self.info = old

    def test_hash_and_invalid_manifest_fail_before_loading(self):
        self.manifest["model"]["sha256"] = "0" * 64
        self.write()
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            B.ModelBundle(str(self.path))
        self.assertFalse(self.closed)
        self.assertFalse(self.calls)
        for edit in (
            lambda m: m.update(schema=True), lambda m: m.update(extra=1),
            lambda m: m["model"].update(file="../model.espdl"),
            lambda m: m["inputs"]["a"].update(shape=[True,4]),
            lambda m: m["inputs"]["a"].update(shape=[4096,4096]),
            lambda m: m["inputs"]["a"]["adapter"].update(type="missing"),
        ):
            candidate = copy.deepcopy(self.manifest)
            edit(candidate)
            with self.assertRaises(ValueError):
                B.validate_manifest(candidate, str(self.folder))

    def test_missing_input_or_execution_failure_keeps_model_until_close(self):
        bundle = B.ModelBundle(str(self.path))
        with self.assertRaises(ValueError):
            bundle.run({"a": b""})
        with patch.object(self.fake.inference, "run", side_effect=OSError("timeout")), self.assertRaises(OSError):
            bundle.run({"a": b"", "b": b""})
        self.assertFalse(self.closed)
        bundle.close()

    def test_raw_file_reads_are_bounded_by_the_port_size(self):
        class InputFile(io.BytesIO):
            def read(self, size=-1):
                self_test.assertLessEqual(size, 8)
                return super().read(size)
        self_test = self
        with B.ModelBundle(str(self.path)) as bundle, patch.object(RUNNER,"solaros",self.fake), \
                patch.object(RUNNER,"json_input_file",return_value={"a":"a.bin","b":"b.bin"}), \
                patch('builtins.open',side_effect=lambda *_:InputFile(b'a'*8)), patch('sys.stdout',io.StringIO()):
            RUNNER.files(bundle,'inputs.json',1)

    def test_reference_manifests_and_image_semantics(self):
        for path in (ROOT / "tests/fixtures/model_bundle").glob("*.json"):
            B.validate_manifest(json.loads(path.read_text()), str(path.parent))
        image = json.loads((ROOT / "tests/fixtures/model_bundle/classification.json").read_text())
        for key, value in (("layout", "NCHW"), ("std", [0]), ("mean", [float("nan")]),
                           ("pad", [256]), ("resize", "bilinear"), ("mean", [1,2])):
            changed = copy.deepcopy(image)
            changed["inputs"]["input.1"]["adapter"]["options"][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                B.validate_manifest(changed, ".")

    def test_classification_softmax_signed_logits_and_identity(self):
        bundle = types.SimpleNamespace(labels=["a","b","c"])
        outputs = {"score": {"dtype":"int8","exponents":[-2],"data":bytes([255,0,4])}}
        result = B.classification(bundle,outputs,{}, {"tensor":"score","activation":"softmax","top":3})
        self.assertEqual([item["id"] for item in result["classes"]], [2,1,0])
        self.assertAlmostEqual(sum(item["score"] for item in result["classes"]), 1)
        outputs["score"] = {"dtype":"float32","exponents":[],"data":__import__('struct').pack('<3f',.1,.9,.2)}
        result = B.classification(bundle,outputs,{}, {"tensor":"score","activation":"identity","top":1})
        self.assertEqual(result["classes"][0]["id"], 1)
        self.assertAlmostEqual(result["classes"][0]["score"], .9)

    def test_letterbox_inverse_coordinates_and_bounded_detector(self):
        transform = {"source_width":640,"source_height":480,"crop_x":10,"crop_y":20,
                     "crop_width":400,"crop_height":200,"resized_width":224,"resized_height":112,
                     "pad_left":0,"pad_top":56}
        self.assertEqual(B.map_box([0,56,224,168],transform),[10,20,410,220])
        self.assertEqual(B.map_box([-100,-100,1000,1000],transform),[0,0,639,479])
        options = {"input":"img","stages":[{"score":"score","bbox":"box","stride":8,"bins":8}],
                   "label":"person","candidate_limit":2,"limit":1,"score_threshold":.7}
        output = {"score":{"shape":[1,1,4,1],"data":bytes([80]*4),"exponents":[-7]},
                  "box":{"data":bytes([0]*128),"exponents":[-3]}}
        result = B.pico_detection(None,output,{"img":transform},options)
        self.assertLessEqual(len(result["detections"]),1)
        self.assertTrue(result["truncated"])
        output["score"]["data"] = bytes([0]*4)
        self.assertEqual(B.pico_detection(None,output,{"img":transform},options)["detections"],[])

class StreamOwnershipTest(unittest.TestCase):
    def setUp(self):
        self.events = []
        self.clock = 1000
        self.frames = [1,2,3]
        self.bundle = types.SimpleNamespace(handle=7, manifest={"id":"test","version":"1", "inputs":{
            "img":{"adapter":{"type":"image"}}}},run=self.infer)
        self.fake = types.SimpleNamespace(
            time=types.SimpleNamespace(uptime_ms=lambda:self.clock,sleep_ms=lambda _:None),
            streams=types.SimpleNamespace(open=lambda _:10,acquire_frame=lambda _:self.frames.pop(0),
                release_frame=lambda f:self.events.append(("release",f)),close=lambda h:self.events.append(("source_close",h)),
                frame_info=lambda f:{"timestamp_us":(0 if f==1 else 999000),"width":2,"height":1}),
            image=types.SimpleNamespace(from_frame=self.decode,close=lambda h:self.events.append(("image_close",h))))
        self.patch = patch.object(RUNNER,"solaros",self.fake)
        self.patch.start();self.addCleanup(self.patch.stop)

    def decode(self, frame):
        self.events.append(("decode",frame))
        return frame+100

    def infer(self, inputs):
        self.events.append(("infer",inputs["img"]))
        return {"input_us":0,"inference_us":1,"output_us":0,"elapsed_us":1,"call_ms":1,
                "preprocess_us":1,"postprocess_ms":0,"transforms":{},
                "result":{"kind":"classification","classes":[]}}

    def test_stale_discard_lease_release_before_inference_and_cleanup(self):
        output=io.StringIO()
        with patch('sys.stdout',output):
            RUNNER.stream(self.bundle,"stream:camera0",2,250)
        self.assertEqual(self.events,[('release',1),('decode',2),('release',2),('infer',102),('image_close',102),
                                      ('decode',3),('release',3),('infer',103),('image_close',103),('source_close',10)])
        records=[json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([r['sequence'] for r in records],[0,1])
        self.assertEqual(records[1]['stale_frames_dropped'],1)
        self.assertEqual(records[0]['frame']['timestamp_us'],999000)

    def test_inference_failure_releases_image_and_source(self):
        self.frames=[2]
        self.bundle.run=lambda _: (_ for _ in ()).throw(OSError('timeout'))
        with self.assertRaises(OSError):
            RUNNER.stream(self.bundle,"stream:camera0",1,250)
        self.assertEqual(self.events,[('decode',2),('release',2),('image_close',102),('source_close',10)])

    def test_decode_failure_releases_each_frame_before_retry(self):
        self.frames=[2,3]
        original=self.fake.image.from_frame
        self.fake.image.from_frame=lambda f: (_ for _ in ()).throw(OSError('bad jpeg')) if f==2 else original(f)
        with patch('sys.stdout',io.StringIO()):
            RUNNER.stream(self.bundle,"stream:camera0",1,250)
        self.assertEqual(self.events,[('release',2),('decode',3),('release',3),('infer',103),('image_close',103),('source_close',10)])

    def test_frame_release_failure_still_releases_decoded_image(self):
        self.frames=[2]
        self.fake.streams.release_frame=lambda _: (_ for _ in ()).throw(OSError('release'))
        with self.assertRaises(OSError):
            RUNNER.stream(self.bundle,"stream:camera0",1,250)
        self.assertEqual(self.events,[('decode',2),('image_close',102),('source_close',10)])


if __name__ == "__main__":
    unittest.main()
