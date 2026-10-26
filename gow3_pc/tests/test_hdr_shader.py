"""Check the shipped HDR epilogue (hdr/hdr_tone.spv) and that no game shader is in the tree.

The games' tone pass shader is not shipped: the runtime derives it from the player's own game and
links this epilogue into it (gpu/shim/gow3_hdr_tone.cpp; tests/test_hdr_tone.cpp covers the
patch and link on a stand-in, and on the real translation when GOW3_HDR_GUEST_SPV points at one).

Requires spirv-dis and spirv-val on PATH, or SPIRV_DIS / SPIRV_VAL overrides.
Run from the repository: python -m unittest discover -s gow3_pc/tests -p test_hdr_shader.py
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

HDR = Path(__file__).resolve().parent.parent / 'hdr'
SHADER = HDR / 'hdr_tone.spv'
SOURCE = HDR / 'hdr_tone.hlsl'
DIS = os.environ.get('SPIRV_DIS') or shutil.which('spirv-dis')
VALIDATOR = os.environ.get('SPIRV_VAL') or shutil.which('spirv-val')


class HdrFilesTests(unittest.TestCase):
    def test_the_hdr_folder_holds_no_game_shader(self):
        # The game's decompiled tone pass (guest hash 3371ae8d) and its SPIR-V were here before
        # 26 Oct 2026; they must not come back (AGENTS.md: no game code in commits).
        names = sorted(p.name for p in HDR.iterdir())
        self.assertEqual([n for n in names if n.startswith('fs_')], [])
        self.assertEqual([n for n in names if n.endswith(('.hlsl', '.spv'))],
                         ['hdr_tone.hlsl', 'hdr_tone.spv'])

    def test_the_epilogue_source_has_nothing_of_the_game_shader(self):
        text = SOURCE.read_text(encoding='utf-8')
        for marker in ('_RESERVED_IDENTIFIER_FIXUP', 'ssbo_1', 'push_constants', 'fs_in_attr',
                       'gl_BaryCoord', 'GetAttributeAtVertex', 'frag_main'):
            self.assertNotIn(marker, text)


@unittest.skipUnless(DIS and VALIDATOR, 'SPIR-V tools are required')
class HdrEpilogueTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # A library (Linkage): universal rules, not Vulkan's, which has no linkage.
        subprocess.run([VALIDATOR, '--target-env', 'spv1.6', str(SHADER)], check=True)
        cls.text = subprocess.check_output([DIS, str(SHADER)], text=True)

    def test_exports_the_one_function_the_runtime_calls(self):
        self.assertEqual(re.findall(r'LinkageAttributes "(\w+)" (\w+)', self.text),
                         [('HdrTone', 'Export')])
        self.assertNotIn('OpEntryPoint', self.text)

    def test_declares_no_capability_the_translated_shader_lacks(self):
        # Ray queries, interlock and the like would pass into the linked shader and fail on
        # devices without them (build.ps1 strips what DXC's library profile adds).
        capabilities = set(re.findall(r'OpCapability (\w+)', self.text))
        self.assertLessEqual(capabilities, {'Shader', 'Linkage', 'Int16'})
        self.assertNotIn('OpExtension', self.text)

    def test_live_settings_are_a_read_only_buffer_at_binding_12(self):
        self.assertNotIn('OpSpecConstant', self.text)
        self.assertRegex(self.text, r'OpDecorate %HdrSettings DescriptorSet 0\s')
        self.assertRegex(self.text, r'OpDecorate %HdrSettings Binding 12\s')
        self.assertRegex(self.text, r'OpMemberDecorate %type_ByteAddressBuffer 0 NonWritable')
        self.assertEqual(len(re.findall(r'= OpVariable', self.text)), 1)


if __name__ == '__main__':
    unittest.main()
