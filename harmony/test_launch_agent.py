"""Verify that viewing stays within the authorized app debug sandbox."""
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from harmony import launch_agent


class AppOnlyLaunchTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "build-harmony").mkdir()
        self.args = SimpleNamespace(
            no_control=True, no_clipboard_autosync=False, stay_awake=False,
            new_display=None, display_id=None, max_size=0, max_fps=0,
            video_bit_rate=8000000, no_audio=True, audio_codec="opus",
            audio_bit_rate=128000, audio_source="output", video_source="display",
        )
        self.commands = []
        self.config = None

    def device(self, argv, **kwargs):
        self.assertEqual(argv[:3], ["hdc", "-t", "test-device"])
        command = argv[3:]
        self.commands.append(command)
        output = ""
        if command == ["shell", "id -u"]:
            output = "2000\n"
        elif command[:3] == ["file", "send", "-b"]:
            self.assertEqual(command[3], launch_agent.BUNDLE)
            local = Path(command[4])
            self.assertEqual(local.stat().st_mode & 0o777, 0o600)
            self.config = json.loads(local.read_text())["args"]
            self.assertIn("media_only=true", self.config)
            self.assertEqual(self.config[4], "0")
            self.assertTrue(command[5].startswith("./data/storage/el2/base/haps/entry/files/"))
        elif command[:3] == ["shell", "-b", launch_agent.BUNDLE]:
            if command[3].startswith("tail "):
                marker = next(arg[11:] for arg in self.config if arg.startswith("session_id="))
                output = "[harmony] session ready: " + marker + "\n"
        elif command[0] == "shell":
            self.assertTrue(command[1].startswith("/system/bin/aa start "))
            output = "start ability successfully\n"
        else:
            self.assertEqual(command[0], "fport")
        return subprocess.CompletedProcess(argv, 0, stdout=output, stderr="")

    def test_viewer_uses_no_root_process_or_system_paths(self):
        desktop = Mock()
        desktop.wait.return_value = 0
        desktop.poll.return_value = 0
        with patch.object(launch_agent, "ROOT", self.root), \
                patch.object(launch_agent, "build_agent") as build, \
                patch.object(launch_agent.subprocess, "run", side_effect=self.device), \
                patch.object(launch_agent.subprocess, "Popen", return_value=desktop) as spawn:
            result = launch_agent.run_agent(
                self.args, [], ["--no-control", "--no-audio"], Path("sdk"),
                "test-device", "hdc", Path("server"), Path("client"),
            )
        self.assertEqual(result, 0)
        build.assert_called_once()
        spawn.assert_called_once()  # The desktop client is the only process.
        self.assertNotIn("SCRCPY_HARMONY_CONTROL_PORT", spawn.call_args.kwargs["env"])
        self.assertEqual(list((self.root / "build-harmony").iterdir()), [])
        self.assertNotIn(["shell", "id -u"], self.commands)
        self.assertEqual(sum(c[0] == "fport" and c[1] != "rm" for c in self.commands), 1)
        for command in self.commands:
            text = " ".join(command)
            for forbidden in ("/data/local/tmp/", "/data/app/", "/proc/", "chown", "--control-only"):
                self.assertNotIn(forbidden, text)

    def test_control_on_non_root_device_fails_before_install(self):
        self.args.no_control = False
        with patch.object(launch_agent, "build_agent") as build, \
                patch.object(launch_agent.subprocess, "run", side_effect=self.device), \
                patch.object(launch_agent.subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(RuntimeError, "--no-control"):
                launch_agent.run_agent(
                    self.args, [], [], Path("sdk"), "test-device", "hdc",
                    Path("server"), Path("client"),
                )
        build.assert_not_called()
        spawn.assert_not_called()


if __name__ == "__main__":
    unittest.main()
