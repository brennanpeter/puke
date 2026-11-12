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
        self.assertEqual(comp.node_types(), ["Grade", "Merge", "Read", "Transform", "Write"])

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
        read = g.create("Read", file=self.png)
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
        read = comp.Graph().create("Read", file=self.png)
        mv = memoryview(comp.Engine().render(read))
        self.assertEqual((mv.shape, mv.format, mv.readonly), ((8, 16, 4), "f", True))
        self.assertAlmostEqual(mv[2, 3, 0], 30 / 255, places=5)

    @unittest.skipUnless(np, "needs numpy")
    def test_numpy_view(self):
        g = comp.Graph()
        read = g.create("Read", file=self.png)
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

    def test_write_and_read_back(self):
        g = comp.Graph()
        read = g.create("Read", file=self.png)
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


if __name__ == "__main__":
    unittest.main()
