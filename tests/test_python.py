"""Tests for the comp Python module.

Run by ctest with PYTHONPATH pointing at the built module, or by hand:
    PYTHONPATH=build/python python3 tests/test_python.py
numpy is optional; the tests that need it are skipped without it.
"""

import gc
import os
import shutil
import struct
import tempfile
import unittest
import zlib

import comp

try:
    import numpy as np
except ImportError:
    np = None


def write_png(path, width, height, pixel):
    """Writes an 8-bit RGBA PNG; pixel(x, y) returns 4 ints in 0..255."""
    rows = b"".join(
        b"\x00" + bytes(v for x in range(width) for v in pixel(x, y)) for y in range(height)
    )

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows)))
        f.write(chunk(b"IEND", b""))


def test_pixel(x, y):
    return (x * 10 % 256, y * 20 % 256, 128, 255)


class TempDirTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="comp_python_test_")
        self.png = os.path.join(self.dir, "in.png")
        write_png(self.png, 16, 8, test_pixel)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def path(self, name):
        return os.path.join(self.dir, name)


class GraphTests(unittest.TestCase):
    def test_node_types(self):
        types = comp.node_types()
        self.assertEqual(types[:6], ["ColorSpace", "Grade", "Merge", "Read", "Transform", "Write"])
        self.assertTrue(all(t.startswith("ofx:") for t in types[6:]))

    def test_create_names_and_params(self):
        g = comp.Graph()
        a = g.create("Grade")
        b = g.create("Grade", gain=1.5, channels="rgba")
        c = g.create("Grade", "Hero")
        self.assertEqual([a.name, b.name, c.name], ["Grade1", "Grade2", "Hero"])
        self.assertEqual(a.type, "Grade")
        self.assertEqual(b.params, {"gain": 1.5, "channels": "rgba"})
        self.assertEqual(len(g), 3)
        self.assertIn("Hero", g)
        self.assertEqual(g["Hero"], c)
        self.assertEqual(g.find("Nope"), None)
        with self.assertRaises(KeyError):
            g["Nope"]
        with self.assertRaisesRegex(comp.Error, "a node named 'Hero' already exists"):
            g.create("Grade", "Hero")
        with self.assertRaisesRegex(comp.Error, "unknown node type 'Blur'"):
            g.create("Blur")
        # A failed create adds nothing.
        self.assertEqual(len(g), 3)

    def test_param_types(self):
        n = comp.Graph().create("Transform")
        n["flag"] = True
        n["count"] = 3
        n["amount"] = 0.5
        n["filter"] = "cubic"
        n["translate"] = [10, -2]
        n["tint"] = (0.1, 0.2, 0.3, 1)
        self.assertIs(n["flag"], True)
        self.assertEqual(type(n["count"]), int)
        self.assertEqual(type(n["amount"]), float)
        self.assertEqual(n["translate"], (10.0, -2.0))
        self.assertEqual(n["tint"], (0.1, 0.2, 0.3, 1.0))
        self.assertTrue("filter" in n and "nope" not in n)
        self.assertEqual(n.get("nope", 7), 7)
        with self.assertRaises(KeyError):
            n["nope"]

        with self.assertRaisesRegex(TypeError, "must be a bool, int, float, str"):
            n["bad"] = None
        with self.assertRaisesRegex(TypeError, "got 3 numbers"):
            n["bad"] = (1, 2, 3)
        with self.assertRaisesRegex(ValueError, "32-bit"):
            n["bad"] = 2**40
        self.assertNotIn("bad", n)

        # set() is all or nothing.
        self.assertEqual(n.set(rotate=45.0, scale=2), n)  # handles compare with ==, not "is"
        with self.assertRaises(TypeError):
            n.set(rotate=10.0, scale=object())
        self.assertEqual(n["rotate"], 45.0)

    def test_specs_and_positions(self):
        g = comp.Graph()
        grade = g.create("Grade")
        specs = {s["name"]: s for s in grade.param_specs}
        self.assertEqual(specs["gain"]["kind"], "color")
        self.assertEqual(specs["gain"]["default"], (1.0, 1.0, 1.0, 1.0))
        self.assertEqual(specs["channels"]["options"], ["rgb", "rgba"])
        grade["mix"] = 0.5
        grade.clear("mix")
        self.assertNotIn("mix", grade)
        self.assertIsNone(grade.position)
        grade.position = (10, 20)
        self.assertEqual(grade.position, (10.0, 20.0))
        self.assertIn('"position"', g.to_json())

    @unittest.skipUnless(np, "needs numpy")
    def test_numpy_params(self):
        n = comp.Graph().create("Transform")
        n.set(count=np.int64(4), amount=np.float32(0.25), translate=np.array([1.5, 2.5]))
        self.assertEqual((n["count"], n["amount"], n["translate"]), (4, 0.25, (1.5, 2.5)))

    def test_connections(self):
        g = comp.Graph()
        read = g.create("Read")
        grade = g.create("Grade")
        merge = g.create("Merge")
        g.connect(read, grade)
        g.connect(grade, merge, 0)
        g.connect(read, merge, 1)
        self.assertEqual(merge.inputs, [grade, read])
        self.assertEqual(grade.input(0), read)
        self.assertEqual(set(g.consumers(read)), {grade, merge})
        self.assertTrue(merge.input_optional(1) and not merge.input_optional(0))

        with self.assertRaisesRegex(comp.Error, "would create a cycle"):
            g.connect(merge, grade)
        with self.assertRaisesRegex(comp.Error, "out of range"):
            g.connect(read, grade, 3)
        other = comp.Graph().create("Grade")
        with self.assertRaisesRegex(comp.Error, "does not belong to this graph"):
            g.connect(other, merge, 1)

        g.disconnect(merge, 1)
        self.assertEqual(merge.input(1), None)

    def test_removed_nodes(self):
        g = comp.Graph()
        read = g.create("Read")
        grade = g.create("Grade")
        g.connect(read, grade)
        g.remove(read)
        self.assertEqual(grade.input(0), None)
        self.assertEqual(repr(read), "<removed node>")
        with self.assertRaisesRegex(comp.Error, "removed from its graph"):
            read.name
        with self.assertRaisesRegex(comp.Error, "removed from its graph"):
            read["file"] = "x.exr"
        # A new node with the same name is a different node.
        again = g.create("Read", "Read1")
        self.assertNotEqual(again, read)

    def test_nodes_keep_their_graph_alive(self):
        g = comp.Graph()
        node = g.create("Grade", gain=2.0)
        del g
        gc.collect()
        self.assertEqual(node["gain"], 2.0)
        self.assertEqual(len(node.graph), 1)


class BoxTests(unittest.TestCase):
    def test_box(self):
        b = comp.Box(1, 2, 5, 10)
        self.assertEqual((b.width, b.height, b.area), (4, 8, 32))
        self.assertEqual(b, (1, 2, 5, 10))
        self.assertEqual(tuple(b), (1, 2, 5, 10))
        self.assertEqual(b.intersect((3, 0, 20, 4)), (3, 2, 5, 4))
        self.assertEqual(b.unite((0, 0, 1, 1)), (0, 0, 5, 10))
        self.assertEqual(b.padded(1), (0, 1, 6, 11))
        self.assertTrue(comp.Box().empty() and b.contains(1, 2) and not b.contains(5, 2))
        self.assertEqual(repr(b), "Box(1, 2, 5, 10)")
        self.assertEqual(len({comp.Box(0, 0, 1, 1), comp.Box(0, 0, 1, 1)}), 1)


class RenderTests(TempDirTest):
    def test_info_and_render(self):
        g = comp.Graph()
        read = g.create("Read", file=self.png, colorspace="Raw")  # raw values: no sRGB decode
        e = comp.Engine(threads=2, tile_size=4)
        info = e.info(read)
        self.assertEqual(info.data_window, (0, 0, 16, 8))
        self.assertEqual(info.channels, ["R", "G", "B", "A"])

        img = e.render(read)  # default region: the data window
        self.assertEqual((img.region, img.width, img.height), ((0, 0, 16, 8), 16, 8))
        r, gch, b, a = img.pixel(3, 2)
        self.assertAlmostEqual(r, 30 / 255, places=5)
        self.assertAlmostEqual(gch, 40 / 255, places=5)
        self.assertEqual(img.pixel(100, 100), (0.0, 0.0, 0.0, 0.0))
        self.assertEqual(img.channel_index("A"), 3)

        # Any region, zero outside the data window.
        part = e.render(read, region=(-2, -2, 2, 2))
        self.assertEqual(part.region, (-2, -2, 2, 2))
        self.assertEqual(part.pixel(-1, -1), (0.0, 0.0, 0.0, 0.0))
        self.assertEqual(part.pixel(1, 1), img.pixel(1, 1))

    def test_memoryview_without_numpy(self):
        read = comp.Graph().create("Read", file=self.png, colorspace="Raw")
        mv = memoryview(comp.Engine().render(read))
        self.assertEqual((mv.shape, mv.format, mv.readonly), ((8, 16, 4), "f", True))
        self.assertAlmostEqual(mv[2, 3, 0], 30 / 255, places=5)

    @unittest.skipUnless(np, "needs numpy")
    def test_numpy_view(self):
        g = comp.Graph()
        read = g.create("Read", file=self.png, colorspace="Raw")
        xf = g.create("Transform", translate=(5, 3), filter="nearest")
        g.connect(read, xf)
        img = comp.Engine().render(xf)
        a = np.asarray(img)
        self.assertEqual((a.shape, a.dtype), ((8, 16, 4), np.float32))
        self.assertFalse(a.flags.writeable)
        # Index [row, col] is pixel (x0 + col, y0 + row): the data window moved by (5, 3).
        self.assertEqual(img.region, (5, 3, 21, 11))
        self.assertAlmostEqual(float(a[2 - 0, 3 - 0, 0]), 30 / 255, places=5)
        self.assertEqual(tuple(float(v) for v in a[2, 3]), img.pixel(5 + 3, 3 + 2))
        # A view, not a copy, and it keeps the image alive.
        self.assertTrue(np.shares_memory(a, np.asarray(img)))
        del img
        gc.collect()
        self.assertAlmostEqual(float(a[2, 3, 0]), 30 / 255, places=5)

    def test_cache_and_stats(self):
        g = comp.Graph()
        read = g.create("Read", file=self.png)
        grade = g.create("Grade", gain=2.0)
        g.connect(read, grade)
        e = comp.Engine(tile_size=4, cache_bytes=16 << 20)
        self.assertEqual(e.cache_bytes, 16 << 20)
        e.render(grade)
        self.assertEqual(e.last_stats.tiles_from_cache, 0)
        self.assertGreater(e.cache_bytes_used, 0)
        e.render(grade)
        self.assertEqual(e.last_stats.tiles_computed, 0)
        self.assertEqual(e.last_stats.tiles_from_cache, (16 // 4) * (8 // 4))
        grade["gain"] = 3.0
        e.render(grade)
        self.assertEqual(e.last_stats.nodes_computed, 1)  # only the Grade
        e.cache_bytes = 0
        self.assertEqual(e.cache_bytes_used, 0)

    def test_render_in_threads_while_editing(self):
        # Renders release the GIL and work from a snapshot: other threads can
        # keep editing the graph, and several engines can render at once.
        import threading
        g = comp.Graph()
        read = g.create("Read", file=self.png, colorspace="Raw")
        grade = g.create("Grade", multiply=1.0)
        g.connect(read, grade)
        errors, results = [], []

        def worker():
            e = comp.Engine(threads=2, tile_size=4)
            try:
                for _ in range(20):
                    results.append(e.render(grade).pixel(3, 2)[0])
            except Exception as ex:  # pragma: no cover
                errors.append(ex)

        threads = [threading.Thread(target=worker) for _ in range(3)]
        for t in threads:
            t.start()
        for i in range(200):
            grade["multiply"] = 1.0 + (i % 2)  # flips between 1 and 2
        for t in threads:
            t.join()
        self.assertEqual(errors, [])
        base = 30 / 255
        self.assertTrue(all(abs(v - base) < 1e-5 or abs(v - 2 * base) < 1e-5 for v in results))

    def test_write_and_read_back(self):
        g = comp.Graph()
        read = g.create("Read", file=self.png, colorspace="Raw")
        grade = g.create("Grade", multiply=0.5)
        write = g.create("Write", file=self.path("out/out.####.exr"), datatype="float")
        g.connect(read, grade)
        g.connect(grade, write)
        e = comp.Engine()
        written = write.execute(e, frame=7)
        self.assertEqual(written, self.path("out/out.0007.exr"))
        self.assertTrue(os.path.exists(written))

        check = g.create("Read", file=self.path("out/out.####.exr"))
        back = e.render(check, frame=7)
        self.assertAlmostEqual(back.pixel(3, 2)[0], 0.5 * 30 / 255, places=5)
        self.assertAlmostEqual(back.pixel(3, 2)[3], 1.0, places=5)  # Grade leaves alpha by default

        with self.assertRaisesRegex(TypeError, "only Write nodes can be executed"):
            grade.execute(e)

    def test_errors(self):
        g = comp.Graph()
        grade = g.create("Grade")
        e = comp.Engine()
        with self.assertRaisesRegex(comp.Error, "Grade1: input 0 is not connected"):
            e.render(grade)
        read = g.create("Read", file=self.path("missing.exr"))
        g.connect(read, grade)
        with self.assertRaisesRegex(comp.Error, "Read1: cannot open"):
            e.render(grade)
        self.assertTrue(issubclass(comp.Error, Exception))
        with self.assertRaises(ValueError):
            comp.Engine(threads=-1)
        with self.assertRaisesRegex(comp.Error, "tile size"):
            comp.Engine(tile_size=0)


class ColorTests(TempDirTest):
    def test_config(self):
        c = comp.ColorConfig.current()
        self.assertEqual(c.source, "built-in")  # ctest clears $OCIO
        self.assertEqual(c.working_space, "ACEScg")
        self.assertIn("sRGB - Texture", c.color_spaces)
        self.assertTrue(c.has("matte_paint") and not c.has("nope"))
        self.assertEqual(c.resolve("matte_paint"), "sRGB - Texture")
        with self.assertRaisesRegex(comp.Error, "unknown color space 'nope'"):
            c.resolve("nope")
        with self.assertRaisesRegex(comp.Error, "OCIO config"):
            comp.ColorConfig.from_file(self.path("missing.ocio"))

    def test_png_is_srgb_by_default(self):
        # Reading an 8-bit file decodes sRGB into the working space: the same
        # as reading it raw and converting with a ColorSpace node.
        g = comp.Graph()
        auto = g.create("Read", file=self.png)
        raw = g.create("Read", file=self.png, colorspace="Raw")
        cs = g.create("ColorSpace", **{"in": "sRGB - Texture", "out": "ACEScg"})
        g.connect(raw, cs)
        e = comp.Engine()
        a, b = e.render(auto), e.render(cs)
        for x, y in [(0, 0), (3, 2), (15, 7)]:
            for va, vb in zip(a.pixel(x, y), b.pixel(x, y)):
                self.assertAlmostEqual(va, vb, places=6)
        self.assertNotAlmostEqual(a.pixel(3, 2)[1], raw_value := 40 / 255, places=3)
        self.assertAlmostEqual(e.render(raw).pixel(3, 2)[1], raw_value, places=5)

    def test_round_trip_through_png(self):
        g = comp.Graph()
        read = g.create("Read", file=self.png)
        write = g.create("Write", file=self.path("rt.png"), datatype="uint8")
        g.connect(read, write)
        e = comp.Engine()
        write.execute(e)
        back = g.create("Read", file=self.path("rt.png"))
        raw = g.create("Read", file=self.path("rt.png"), colorspace="Raw")
        # The file holds the original 8-bit values again...
        self.assertAlmostEqual(e.render(raw).pixel(3, 2)[0], 30 / 255, places=5)
        # ...and reads back to the same working-space pixels.
        for va, vb in zip(e.render(back).pixel(3, 2), e.render(read).pixel(3, 2)):
            self.assertAlmostEqual(va, vb, places=3)


class OfxTests(TempDirTest):
    """Uses the test plugins in tests/ofx (ctest sets $OFX_PLUGIN_PATH)."""

    def test_plugin_node(self):
        self.assertIn("ofx:org.comp.test.Gain", comp.node_types())
        g = comp.Graph()
        read = g.create("Read", file=self.png, colorspace="Raw")
        gain = g.create("ofx:org.comp.test.Gain", gain=2.0)
        self.assertEqual(gain.name, "Gain1")
        self.assertEqual(gain.type, "ofx:org.comp.test.Gain")
        g.connect(read, gain)
        e = comp.Engine(tile_size=4)
        self.assertAlmostEqual(e.render(gain).pixel(3, 2)[0], 2 * 30 / 255, places=5)
        gain["mode"] = "add"
        self.assertAlmostEqual(e.render(gain).pixel(3, 2)[0], 30 / 255 + 2, places=5)

        gain["fail"] = True
        with self.assertRaisesRegex(comp.Error, "gain plugin asked to fail"):
            e.render(gain)
        with self.assertRaisesRegex(comp.Error, "is a generator"):
            g.create("ofx:org.comp.test.Generator")


class ProjectTests(TempDirTest):
    def test_save_and_load(self):
        g = comp.Graph()
        read = g.create("Read", file=self.png)
        grade = g.create("Grade", gain=(2.0, 1.0, 0.5, 1.0), multiply=2.0, gamma=1)
        g.connect(read, grade)
        path = self.path("shot.comp")
        g.save(path)
        self.assertFalse(os.path.exists(path + ".partial"))

        back = comp.Graph.load(path)
        self.assertEqual([n.name for n in back.nodes], ["Read1", "Grade1"])
        self.assertEqual(back["Grade1"].params, grade.params)
        self.assertEqual(type(back["Grade1"]["gamma"]), int)  # types survive
        self.assertEqual(back["Grade1"].input(0), back["Read1"])
        self.assertEqual(back.to_json(), g.to_json())

        # Same pixels from the loaded graph.
        e = comp.Engine()
        a, b = e.render(grade), e.render(back["Grade1"])
        self.assertEqual(bytes(memoryview(a)), bytes(memoryview(b)))

    def test_json_text(self):
        g = comp.Graph.from_json('{"format": "comp", "version": 1, "nodes": ['
                                 '{"name": "G", "type": "Grade", "params": {"mix": 0.5}}]}')
        self.assertEqual(g["G"]["mix"], 0.5)
        self.assertIn('"mix": 0.5', g.to_json())
        with self.assertRaisesRegex(comp.Error, "unknown node type 'Blur'"):
            comp.Graph.from_json('{"format": "comp", "version": 1, "nodes": [{"name": "B", "type": "Blur"}]}')
        with self.assertRaisesRegex(comp.Error, "cannot open project"):
            comp.Graph.load(self.path("missing.comp"))


if __name__ == "__main__":
    unittest.main()
