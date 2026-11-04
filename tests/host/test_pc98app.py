#!/usr/bin/env python3
"""Host tests for the PC-98 application (user/bin/pc98app, docs/PC98_APP.md): everything of it that needs no desktop, compiled into
tests/host/pc98apptool.c together with libpc98, with AddressSanitizer and UBSan.

  - the glue modules, one test each (the C functions are in pc98apptool.c, listed by `pc98apptool list`): the key table, the config file's
    parser, the pacer on a fake clock, the sound pipe under underrun and overrun, the disk overlay, the font and the drum samples;
  - the idle skip against programs written for it (tests/host/pc98app/idle.asm): what it must skip, what it must not, and that the guest
    cannot tell the difference;
  - the machine as the application runs it (emu.c): boot, keys, the pacer's frames, the disk overlay through the HLE BIOS, "save as".

Every program and every disk image is made by the tests. Nothing from a real PC-98 is used. Needs gcc (and nasm for the programs).

    python tools/hosttest.py discover -s tests/host -p "test_pc98app*.py" -v         (Windows: runs in WSL)
"""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x86ref  # noqa: E402
import pc98apphost as H  # noqa: E402

UNIT_TESTS = [
    "keymap_game_keys", "keymap_table_is_consistent", "keys_press_release", "keys_repeat_is_ignored", "keys_two_shift_keys",
    "keys_release_without_press", "keys_release_all",
    "config_defaults", "config_parse_every_key", "config_bad_lines_are_skipped_and_reported", "config_round_trip", "recent_list",
    "pacer_real_time", "pacer_speed_settings", "pacer_speed_change_does_not_jump", "pacer_forgives_a_long_stall",
    "pacer_caps_each_pass_and_loses_nothing", "pacer_lead_and_wait", "pacer_measures_speed_and_frames",
    "apipe_timeline_without_a_chip", "apipe_buzzer_pitch", "apipe_overrun_drops_the_oldest", "apipe_underrun_is_counted_silence",
    "apipe_volume_curve", "apipe_fm_stream_is_resampled",
    "disk_loads_the_formats", "disk_errors_are_sentences", "disk_overlay_never_reaches_the_file", "disk_save_as_is_always_a_new_file",
    "disk_virtual_config_comments_out_the_memory_lines", "disk_damaged_images_never_crash",
    "wav_parse", "emu_rhythm_dir", "emu_font",
]


def wav16(frames, rate=44100):
    n = len(frames)
    body = b"".join(struct.pack("<h", v) for v in frames)
    return (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
            + b"data" + struct.pack("<I", len(body)) + body)


def font_rom():
    """A font ROM as pc98gfx accepts one (288,768 bytes, a blank space and a non-blank 'A' in the 8x16 table): every glyph blank but 'A'."""
    rom = bytearray(288768)
    rom[0x800 + 0x41 * 16:0x800 + 0x41 * 16 + 16] = bytes([0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66] * 2)
    return bytes(rom)


class Units(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not x86ref.have_gcc():
            raise unittest.SkipTest("needs gcc")
        cls.dir = H.make_image_dir("units")
        boot = bytes([0x90] * 16)
        (cls.dir / "a.d88").write_bytes(H.boot_d88(boot, cyls=1))
        (cls.dir / "b.hdi").write_bytes(H.boot_hdi(boot))
        (cls.dir / "c.fdi").write_bytes(H.fdi(boot, cyls=4))
        (cls.dir / "d.hdm").write_bytes(H.raw_floppy(boot))
        (cls.dir / "junk.bin").write_bytes(b"\x01" * 5000)
        (cls.dir / "empty.d88").write_bytes(b"")
        (cls.dir / "vcfg.d88").write_bytes(H.boot_d88(b"DEVICE=HIMEM.SYS\r\nDEVICE=EMM386.EXE /UMB\r\nDOS=HIGH\r\nDOS=UMB\r\n"))
        bad = bytearray(H.boot_d88(boot, cyls=1))
        struct.pack_into("<I", bad, 0x20, 0x00FFFFF0)               # the first track starts far beyond the end of the file
        (cls.dir / "damaged.d88").write_bytes(bytes(bad))
        (cls.dir / "FONT.ROM").write_bytes(font_rom())
        (cls.dir / "rhythm").mkdir()
        for i, n in enumerate(["BD", "SD", "TOP", "HH", "TOM", "RIM"]):
            (cls.dir / "rhythm" / f"2608_{n}.WAV").write_bytes(wav16(list(range(100 + i))))

    def test_the_list_of_unit_tests_is_complete(self):
        rc, lines = H.run_tool(["list"])
        self.assertEqual(rc, 0)
        self.assertEqual(sorted(lines), sorted(UNIT_TESTS), "pc98apptool.c and UNIT_TESTS disagree")


def _make_unit_test(name):
    def test(self):
        rc, lines = H.run_tool(["test", name, self.dir])
        self.assertEqual(rc, 0, "\n".join(lines))
        self.assertIn("PASS " + name, lines)
    test.__name__ = "test_" + name
    return test


for _n in UNIT_TESTS:
    setattr(Units, "test_" + _n, _make_unit_test(_n))


# ---------------------------------------------------------------------------------------------------------------- the idle skip

CNT, DONE, WORK, MARK, KEYS, TRACE = 0x600, 0x602, 0x604, 0x606, 0x610, 0x700


class IdleSkip(unittest.TestCase):
    """The idle skip against the programs of tests/host/pc98app/idle.asm (one source, ten programs). Every program runs on the 386 model
    at 16 MHz with the skip off and on, and the guest must not be able to tell: its counters (and, for the interrupt-driven ones, where
    its main loop was at every interrupt) must be the same."""

    @classmethod
    def setUpClass(cls):
        if not x86ref.have_gcc():
            raise unittest.SkipTest("needs gcc")
        cls.dir = H.make_image_dir("idle")
        cls.img = {}
        for mode in range(1, 11):
            extra = ["TRACE=1"] if mode == 6 else []
            (cls.dir / f"mode{mode}.d88").write_bytes(H.boot_d88(H.assemble("idle", [f"MODE={mode}", *extra])))
            cls.img[mode] = cls.dir / f"mode{mode}.d88"

    def run_mode(self, mode, idle, mcycles=60, extra=()):
        rc, lines = H.run_tool(["run", "--disk", self.img[mode], "--cycles", mcycles, "--idle", "on" if idle else "off",
                                "--dump", "600:20", "--dump", "700:80", *extra])
        self.assertEqual(rc, 0, "\n".join(lines))
        end, idl = H.kv(H.find(lines, "END"), "END"), H.kv(H.find(lines, "IDLE"), "IDLE")
        mem = H.memory(lines)
        return dict(end=end, idle=idl, mem=mem, lines=lines, cycles=int(end["cycles"]), skipped=int(idl["skipped"]),
                    instr=int(end["instructions"]), frames=int(end["frames"]))

    def both(self, mode, **kw):
        off, on = self.run_mode(mode, False, **kw), self.run_mode(mode, True, **kw)
        self.assertEqual(off["end"]["stopped"], "0")
        self.assertEqual(on["end"]["stopped"], "0")
        self.assertEqual(int(off["idle"]["skips"]), 0)                       # the switch really turns it off
        self.assertLess(abs(on["cycles"] - off["cycles"]), 200, "the skip must not run past the time it was given")
        self.assertLess(abs(on["instr"] - off["instr"]), off["instr"] // 200 + 50, "executed and skipped instructions must add up")
        self.assertEqual(on["frames"], off["frames"])
        return off, on

    def test_the_frame_wait_of_the_games_is_skipped_and_the_guest_cannot_tell(self):
        off, on = self.both(1)
        self.assertEqual(H.word(on["mem"], MARK), 0x4B4F)
        for addr in (CNT, DONE, WORK):
            self.assertEqual(H.word(on["mem"], addr), H.word(off["mem"], addr), hex(addr))
        self.assertEqual(H.word(off["mem"], CNT), 211)                       # one VSYNC interrupt per frame, 211 frames in 3.75 s of machine time
        self.assertEqual(H.word(off["mem"], DONE), 211)
        self.assertEqual(H.find(on["lines"], "HASH"), H.find(off["lines"], "HASH"), "the whole machine state is the same")
        self.assertGreater(on["skipped"], 0.9 * on["cycles"], "the wait is about 97 % of this program")

    def test_a_wait_with_a_call_in_it_is_skipped_too(self):
        # CALL and RET write the return address to the same place every time: the writes net to zero, the loop is still a fixed point
        off, on = self.both(2)
        for addr in (CNT, DONE, WORK):
            self.assertEqual(H.word(on["mem"], addr), H.word(off["mem"], addr), hex(addr))
        self.assertGreater(on["skipped"], 0.9 * on["cycles"])

    def test_the_timer_interrupt_arrives_when_it_would_have(self):
        off, on = self.both(6)
        self.assertEqual(H.word(off["mem"], CNT), 374)                       # 100 Hz for 3.75 s
        for addr in range(CNT, CNT + 8, 2):
            self.assertEqual(H.word(on["mem"], addr), H.word(off["mem"], addr), hex(addr))
        # where the main loop was at each of the first 40 interrupts: always at the start of its wait (work counter = 100 x turns), with and without the skip
        for a in range(TRACE, TRACE + 80):
            self.assertEqual(on["mem"][a], off["mem"][a], hex(a))
        self.assertEqual(H.word(off["mem"], TRACE + 2 * 5), 32500)                   # (slot 5 was last written by interrupt 325: 325 turns of 100)
        self.assertGreater(on["skipped"], 0.9 * on["cycles"])

    def test_a_pending_interrupt_is_never_skipped_over(self):
        # the failure this test is named for: a request raised at the end of the stretch that was probed is taken at the next instruction, so
        # nothing may be skipped before it (the timer interrupts then came late, in pairs, and the main loop missed every fifth one)
        off, on = self.both(6, mcycles=16)
        self.assertEqual(H.word(on["mem"], DONE), H.word(off["mem"], DONE))
        self.assertEqual(H.word(on["mem"], CNT), H.word(on["mem"], DONE))    # no tick lost to the main loop

    def test_polling_the_display_status_is_skipped_up_to_the_next_edge(self):
        off, on = self.both(3)
        self.assertEqual(H.word(on["mem"], DONE), H.word(off["mem"], DONE))  # one turn per VSYNC, counted the same
        self.assertEqual(H.word(off["mem"], DONE), 211)
        self.assertGreater(on["skipped"], 0, "some of it is skippable (between the edges of HBLANK and VSYNC)")
        self.assertLess(on["skipped"], on["cycles"] // 2, "but the status byte changes every scan line: far less than a memory wait")

    def test_loops_that_count_or_write_are_left_alone(self):
        for mode, what in ((4, "a delay loop that counts in CX"), (5, "a loop that increments memory"), (7, "a loop that writes a port"),
                           (9, "a loop that writes a port whose reads the skip allows")):
            off, on = self.both(mode, mcycles=20)
            self.assertEqual(on["skipped"], 0, what)
            self.assertEqual(int(on["idle"]["skips"]), 0, what)
            self.assertEqual(H.find(on["lines"], "HASH"), H.find(off["lines"], "HASH"), what + ": identical, bit for bit")
        # (mutation check, by hand: dropping the "writes net to zero" rule makes mode 5 skip; dropping the port rule makes mode 7 skip)

    def test_sources_of_interrupts_that_cannot_fire_do_not_stop_a_skip(self):
        # the games' start-up: a free-running timer counting 3 (an edge every 24 clocks) with its interrupt masked; counting those edges as
        # events stopped every skip within a few clocks and made the idle skip a loss in TH01's first minute
        off, on = self.both(10, mcycles=20)
        self.assertGreater(on["skipped"], 0.9 * on["cycles"])
        self.assertEqual(H.word(on["mem"], DONE), 0)

    def test_a_key_that_arrives_while_the_loop_waits_is_seen_on_time(self):
        keys = ["--key", "30:29:60", "--key", "45:2A:60"]                   # Z at 30 M clocks, X at 45 M, each held 60 ms
        off, on = self.both(8, extra=keys)
        for run in (off, on):
            self.assertEqual(H.word(run["mem"], DONE), 4)                    # make and break of both keys: four bytes read from the keyboard
            self.assertEqual(run["mem"][KEYS], 0xAA)                         # the last: the break code of X
        self.assertGreater(on["skipped"], 0.5 * on["cycles"])

    def test_host_key_events_go_through_the_key_table(self):
        # Fairies codes: KEY_Z = 2Ch press and release, KEY_F12 (58h) is not a PC-98 key, the auto-repeat (2) never reaches the guest
        events = ["--hostkey", "30:2C:1", "--hostkey", "32:2C:2", "--hostkey", "33:2C:2", "--hostkey", "34:58:1", "--hostkey", "36:2C:0"]
        off, on = self.both(8, extra=events)
        for run in (off, on):
            self.assertEqual(H.word(run["mem"], DONE), 2, "one make and one break")
            self.assertEqual(run["mem"][KEYS], 0xA9)


# ---------------------------------------------------------------------------------------------------------------- the machine as the application runs it

TINY_BOOT = bytes([0x0E, 0x1F, 0xB8, 0x00, 0xA0, 0x8E, 0xC0, 0xBE, 0x19, 0x00, 0x31, 0xFF, 0xFC, 0x30, 0xE4, 0xAC, 0x08, 0xC0, 0x74, 0x03,
                   0xAB, 0xEB, 0xF8, 0xFA, 0xF4]) + b"TOUHOU OS: SHRINE BOOT OK\x00"          # tests/host/pc98boot/tiny.asm (it ends with CLI; HLT)


class Machine(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not x86ref.have_gcc():
            raise unittest.SkipTest("needs gcc")
        cls.dir = H.make_image_dir("machine")

    def put(self, name, data):
        p = self.dir / name
        p.write_bytes(data)
        return p

    def run_machine(self, args, mcycles=20):
        rc, lines = H.run_tool(["run", "--cycles", mcycles, *args])
        return rc, lines

    def test_every_image_format_boots_to_its_first_sector(self):
        images = [("d88", H.boot_d88(TINY_BOOT)), ("fdi", H.fdi(TINY_BOOT, cyls=4)), ("hdm", H.raw_floppy(TINY_BOOT)), ("hdi", H.boot_hdi(TINY_BOOT))]
        for ext, data in images:
            p = self.put("boot." + ext, data)
            rc, lines = self.run_machine(["--disk", p, "--text", "--cycles", 5])
            self.assertEqual(rc, 0, ext + ": " + "\n".join(lines[-5:]))
            self.assertIn("|TOUHOU OS: SHRINE BOOT OK" + " " * 55, lines, ext)
            self.assertTrue(any(l.startswith("pc98app: boot ") for l in lines))
        # a floppy is the boot drive when there is one: its program runs, not the hard disk's
        hd = self.put("hd.hdi", H.boot_hdi(bytes([0xFA, 0xF4])))                      # CLI; HLT
        fd = self.put("fd.d88", H.boot_d88(TINY_BOOT))
        rc, lines = self.run_machine(["--disk", hd, "--disk", fd, "--text", "--cycles", 5])
        self.assertIn("pc98app: boot 90 (80386 16 MHz, FM board)", lines)
        self.assertIn("|TOUHOU OS: SHRINE BOOT OK" + " " * 55, lines)

    def test_a_guest_write_goes_to_the_copy_and_never_to_the_file(self):
        img = H.boot_d88(H.assemble("diskwrite"))
        p = self.put("w.d88", img)
        saved = self.dir / "w-saved.d88"
        if saved.exists():
            saved.unlink()
        rc, lines = self.run_machine(["--disk", p, "--cycles", 20, "--dump", "600:4", "--disk-changed", "--save", "0:%s" % saved])
        self.assertEqual(rc, 0, "\n".join(lines))
        mem = H.memory(lines)
        self.assertEqual(mem[0x600], 0, "the BIOS answered the write with 00h")
        self.assertEqual(H.word(mem, 0x602), 0x4B4F)
        self.assertEqual(p.read_bytes(), img, "the file the image came from is untouched")
        changed = [l for l in lines if l.startswith("DISK 0 floppy 1 changed=")]
        self.assertTrue(changed and int(changed[0].split("=")[1]) in (2, 3), changed)  # one 1024-byte sector covers two or three 512-byte blocks
        self.assertEqual(lines[lines.index(changed[0]) + 1], "DISKTOTAL " + changed[0].split("=")[1])
        self.assertIn("SAVE 0 rc=0 ok", lines)
        new = saved.read_bytes()
        self.assertEqual(len(new), len(img))
        diff = [i for i in range(len(img)) if img[i] != new[i]]
        self.assertTrue(diff)
        # the D88 file holds its sectors in order: the difference is one sector's data, 1024 bytes of it at most, and it holds the guest's text
        self.assertLessEqual(diff[-1] - diff[0] + 1, 1024)
        self.assertEqual(img.count(b"WRITTEN BY THE GUEST"), 1, "the text is in the program only")
        self.assertEqual(new.count(b"WRITTEN BY THE GUEST"), 2, "and now in the sector the guest wrote too")
        # asking again for the same name is refused: a saved image is always a new file
        rc, lines = self.run_machine(["--disk", p, "--cycles", 20, "--save", "0:%s" % saved])
        self.assertTrue(any(l.startswith("SAVE 0 rc=-1") and "exists" in l for l in lines), lines[-4:])
        self.assertEqual(p.read_bytes(), img)

    def test_a_machine_that_is_behind_skips_pictures_never_emulation(self):
        p = self.put("idle.d88", H.boot_d88(H.assemble("idle", ["MODE=1"])))
        rc, lines = H.run_tool(["run", "--disk", p, "--cycles", 60, "--step-ns", 200000000, "--idle", "off"])
        end = H.kv(H.find(lines, "END"), "END")
        self.assertEqual(int(end["frames"]), 211, "all the frames were emulated")
        # 200 ms of fake time per pass, but the pacer hands out at most a tenth of a second of machine time per pass
        self.assertLess(int(end["pictures"]), int(end["frames"]) // 3, "and only the newest picture of each pass was drawn")
        self.assertGreater(int(end["pictures"]), 20)
        rc, lines = H.run_tool(["run", "--disk", p, "--cycles", 60, "--step-ns", "4000000", "--idle", "off"])
        end = H.kv(H.find(lines, "END"), "END")
        self.assertGreater(int(end["pictures"]), 200, "at real time with small steps every frame is drawn")
        self.assertEqual(int(H.kv(H.find(lines, "PACER"), "PACER")["resyncs"]), 0)

    def test_what_stops_the_machine_is_reported_with_what_and_where(self):
        cases = [
            (bytes([0x0F, 0x07]), "opcode 0F 07 at 1FC0:"),                          # LOADALL: not an instruction the 386 model has
            (bytes([0xCB]), "boot sector gave control back to the BIOS"),             # RETF into the BIOS: nothing bootable
            (bytes([0xFA, 0xF4]), "halted with interrupts off"),                      # CLI; HLT
        ]
        for i, (code, text) in enumerate(cases):
            p = self.put("stop%d.d88" % i, H.boot_d88(code))
            rc, lines = self.run_machine(["--disk", p, "--cycles", 5])
            stop = [l for l in lines if l.startswith("STOP ")]
            self.assertTrue(stop, "%r: %s" % (code, lines[-3:]))
            self.assertIn(text, stop[0])
            self.assertEqual(H.kv(H.find(lines, "END"), "END")["stopped"], "1")
        self.assertTrue(any("pc98app: stopped: " in l for l in lines))

    def test_the_fm_board_reaches_the_sound_pipe_with_its_pitch(self):
        # SSG channel A at period 62 on the -86 board: 7,987,200 / (128 x 62) = 1006.5 Hz; the chip's frames are resampled from 55,466.67 Hz to 48 kHz
        p = self.put("fm.d88", H.boot_d88(H.assemble("fmtone")))
        wav = self.dir / "fm.wav"
        rc, lines = H.run_tool(["run", "--disk", p, "--cycles", 32, "--wav", wav, "--idle", "on"])
        self.assertEqual(rc, 0, "\n".join(lines))
        a = H.kv(H.find(lines, "AUDIO"), "AUDIO")
        self.assertAlmostEqual(int(a["frames"]), 48000 * 2, delta=2000)
        import array
        data = wav.read_bytes()[44:]
        left = array.array("h", data[:len(data) // 4 * 4])[0::2]
        body = left[len(left) // 4:]                                                # past the start-up
        crossings = sum(1 for i in range(1, len(body)) if (body[i] < 0) != (body[i - 1] < 0))
        hz = crossings / 2.0 / (len(body) / 48000.0)
        self.assertAlmostEqual(hz, 7987200 / 128 / 62, delta=4)
        self.assertGreater(max(abs(v) for v in body), 1000)
        idle = H.kv(H.find(lines, "IDLE"), "IDLE")
        self.assertGreater(int(idle["skipped"]), 0.5 * 32e6, "the spin loop after the setup is skipped while the chip keeps making sound")

    def test_the_sound_pipe_makes_audio_for_the_buzzer(self):
        # a program that sets counter 1 to 1 kHz and switches the buzzer on (port 37h, bit set/reset of port C bit 3)
        prog = bytes([0xB0, 0x76, 0xE6, 0x77,                     # mov al,76h ; out 77h,al  (counter 1, LSB+MSB, mode 3)
                      0xB8, 0x00, 0x08, 0xE6, 0x73, 0x88, 0xE0, 0xE6, 0x73,   # mov ax,0800h ; out 73h,al ; mov al,ah ; out 73h,al  (reload 2048: ~975 Hz)
                      0xB0, 0x06, 0xE6, 0x37,                     # mov al,06h ; out 37h,al  (port C bit 3 low: buzzer on)
                      0xEB, 0xFE])                                # jmp $
        p = self.put("beep.d88", H.boot_d88(prog))
        wav = self.dir / "beep.wav"
        rc, lines = H.run_tool(["run", "--disk", p, "--cycles", 32, "--fm", "none", "--wav", wav, "--idle", "on"])
        self.assertEqual(rc, 0, "\n".join(lines))
        a = H.kv(H.find(lines, "AUDIO"), "AUDIO")
        self.assertAlmostEqual(int(a["frames"]), 48000 * 2, delta=2000)             # two seconds of machine time, 48 kHz
        data = wav.read_bytes()[44:]
        import array
        samples = array.array("h", data[:len(data) // 4 * 4])
        left = samples[0::2]
        crossings = sum(1 for i in range(1, len(left)) if (left[i] < 0) != (left[i - 1] < 0))
        hz = crossings / 2.0 / (len(left) / 48000.0)
        self.assertAlmostEqual(hz, 1996800 / 2048, delta=20)                        # the 8253's clock over the reload value
        self.assertGreater(int(a["peak"]), 3000)


if __name__ == "__main__":
    unittest.main()
