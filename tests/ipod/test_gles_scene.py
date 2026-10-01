#!/usr/bin/env python3
"""Color-area agreement must not certify an incorrectly scanned-out surface."""
import unittest
import gles_scene


def scene(brightness=255):
    pixels = bytearray([brightness, 0, 0] * (320 * 480))
    for y in range(60, 420):
        for x in range(40, 280):
            rgb = ((0, brightness, brightness) if x < 160 else
                   (brightness, 0, brightness) if y < 240 else
                   (brightness, brightness, 0))
            offset = (y * 320 + x) * 3
            pixels[offset:offset + 3] = bytes(rgb)
    return pixels


class SceneTests(unittest.TestCase):
    def test_declared_scene_and_dim_panel(self):
        for brightness in (255, 91, 8):
            pixels = scene(brightness)
            self.assertEqual(gles_scene.fractions(320, 480, pixels),
                             (0.140625, 0.28125, 0.140625))
            self.assertTrue(gles_scene.verdict(320, 480, pixels)[0])

    def test_flat_scanout_preserves_totals_but_breaks_geometry(self):
        pixels = scene()
        packed = b"".join(pixels[(y * 320 + 40) * 3:(y * 320 + 280) * 3]
                          for y in range(60, 420))
        flat = packed + bytes(320 * 480 * 3 - len(packed))
        self.assertEqual(gles_scene.fractions(320, 480, flat),
                         gles_scene.fractions(320, 480, pixels))
        self.assertFalse(gles_scene.verdict(320, 480, flat)[0])

    def test_wrong_color_orientation_and_missing_texture(self):
        pixels = scene()
        for y in range(244, 416):
            for x in range(164, 276):
                offset = (y * 320 + x) * 3
                pixels[offset:offset + 3] = b"\xff\x00\xff"
        self.assertFalse(gles_scene.verdict(320, 480, pixels)[0])
        pixels = scene()
        rows = [pixels[y * 960:(y + 1) * 960] for y in range(480)]
        self.assertFalse(gles_scene.verdict(320, 480, b"".join(reversed(rows)))[0])

    def test_invalid_or_dark_frame(self):
        for width, height, pixels in ((320, 480, bytes(320 * 480 * 3)),
                                      (240, 360, bytes(240 * 360 * 3)),
                                      (320, 480, b"short")):
            self.assertFalse(gles_scene.verdict(width, height, pixels)[0])


if __name__ == "__main__":
    unittest.main()
