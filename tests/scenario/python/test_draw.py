"""Circuit.draw() Python tests.

Covers the text path (str / ascii / __str__), the unified output= dispatch,
the matplotlib image path (skipped cleanly when matplotlib is absent), and the
ImportError contract for the optional viz dependency.

    python -m unittest tests.scenario.python.test_draw -v
"""
import os
import sys
import unittest

try:
    import qplayer
except ModuleNotFoundError:
    _REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    sys.path.insert(0, os.path.join(_REPO, "bindings"))
    import qplayer

try:
    import matplotlib  # noqa: F401
    _HAVE_MPL = True
except ImportError:
    _HAVE_MPL = False


def _bell():
    c = qplayer.Circuit(2, 2)
    c.h(0).cx(0, 1).measure(0, 0).measure(1, 1)
    return c


class TextDraw(unittest.TestCase):
    def test_text_default_has_wires_and_gates(self):
        text = _bell().draw()
        self.assertIn("q0", text)
        self.assertIn("q1", text)
        self.assertIn("[H]", text)
        self.assertIn("[X]", text)
        self.assertIn("[M]", text)
        self.assertIn("c:", text)            # classical wire from measures

    def test_str_equals_text_draw(self):
        c = _bell()
        self.assertEqual(str(c), c.draw(output="text"))

    def test_ascii_style_has_no_unicode(self):
        text = _bell().draw(style="ascii")
        self.assertIn("*", text)             # ascii control marker
        self.assertIn("[X]", text)
        # No multi-byte glyphs: pure-ascii output encodes to ascii cleanly.
        text.encode("ascii")

    def test_toffoli_target_labelled_x(self):
        c = qplayer.Circuit(3, 0)
        c.ccx(0, 1, 2)
        text = c.draw()
        self.assertIn("[X]", text)
        self.assertNotIn("CCX", text)

    def test_bad_output_raises_value_error(self):
        with self.assertRaises(ValueError):
            _bell().draw(output="nope")


class MplDraw(unittest.TestCase):
    @unittest.skipUnless(_HAVE_MPL, "matplotlib not installed")
    def test_mpl_returns_figure(self):
        import matplotlib.figure
        fig = _bell().draw(output="mpl")
        self.assertIsInstance(fig, matplotlib.figure.Figure)

    @unittest.skipUnless(_HAVE_MPL, "matplotlib not installed")
    def test_mpl_saves_file(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "circ.png")
            _bell().draw(output="mpl", filename=path)
            self.assertTrue(os.path.exists(path))
            self.assertGreater(os.path.getsize(path), 0)

    @unittest.skipIf(_HAVE_MPL, "matplotlib present; ImportError path not reachable")
    def test_mpl_without_matplotlib_raises_importerror(self):
        with self.assertRaises(ImportError):
            _bell().draw(output="mpl")


if __name__ == "__main__":
    unittest.main()
