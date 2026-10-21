import importlib.util
import json
import math
import pathlib
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
MOTION_PATH = REPOSITORY / "Project" / "Scripts" / "HEngineUI" / "motion.py"
SURVIVOR_MOTION_PATH = (
    REPOSITORY / "Project" / "Assets" / "UI" / "SurvivorHUD.uimotion.json"
)
SURVIVOR_STYLE_PATH = REPOSITORY / "Project" / "Assets" / "UI" / "SurvivorHUD.rcss"
SPEC = importlib.util.spec_from_file_location("he_ui_motion_test_target", MOTION_PATH)
MOTION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOTION)


def asset(tracks, duration=1.0, interrupt="replace", unlock="ready", render_transitions=None):
    return {
        "format": "UiMotion",
        "version": 1,
        "budgets": {"maxActiveTimelines": 1, "maxTracksPerTimeline": 16},
        "timelines": {
            "go": {
                "duration": duration,
                "interrupt": interrupt,
                "unlockMarker": unlock,
                "tracks": tracks,
                "renderTransitions": render_transitions or [],
            }
        },
    }


class FakeRml:
    def __init__(self):
        self.time = 0.0
        self.calls = []
        self.fail_classes = False
        self.fail_time = False
        self.classes = {}
        self.styles = {
            ("doc", "screen-a", "display"): "flex",
            ("doc", "screen-b", "display"): "none",
            ("doc", "screen-c", "display"): "none",
        }

    def GetPresentationTime(self):
        if self.fail_time:
            raise RuntimeError("presentation clock unavailable")
        return self.time

    def SetClass(self, document, element, name, enabled):
        self.calls.append(("class", document, element, name, enabled))
        if self.fail_classes:
            raise RuntimeError("document was destroyed")
        self.classes[(document, element, name)] = bool(enabled)

    def GetClass(self, document, element, name):
        return self.classes.get((document, element, name), False)

    def SetStyle(self, document, element, name, value):
        self.calls.append(("style", document, element, name, value))
        self.styles[(document, element, name)] = value

    def GetStyle(self, document, element, name):
        return self.styles.get((document, element, name))

    def SetRenderTransition(self, effect, progress, reduce_motion, reduce_flash):
        self.calls.append(("render", effect, progress, reduce_motion, reduce_flash))

    def ClearRenderTransition(self):
        self.calls.append(("render-clear",))


class UiMotionTests(unittest.TestCase):
    def director(self, motion_asset, fake=None):
        fake = fake or FakeRml()
        director = MOTION.UIAnimationDirector("doc", motion_asset, fake)
        director.register_screen("a", "screen-a", visible=True)
        director.register_screen("b", "screen-b")
        return director, fake

    def test_strict_schema_and_numeric_types(self):
        with self.assertRaises(MOTION.UiMotionError):
            MOTION.UIAnimationDirector("doc", {"format": "UiMotion", "version": True}, FakeRml())
        invalid = asset([])
        invalid["timelines"]["go"]["surprise"] = True
        with self.assertRaises(MOTION.UiMotionError):
            MOTION.UIAnimationDirector("doc", invalid, FakeRml())

    def test_fixed_time_transition_unlocks_at_marker(self):
        director, fake = self.director(asset([
            {"at": 0.0, "action": "class", "target": "$source", "name": "exit", "enabled": True},
            {"at": 0.5, "action": "marker", "name": "switch"},
            {"at": 0.75, "action": "marker", "name": "ready"},
        ]))
        director.play_transition("go", "a", "b")
        self.assertTrue(director.is_input_locked())
        fake.time = 0.5
        director.update()
        self.assertEqual(director.get_screen_state("b"), MOTION.ScreenState.ENTERING)
        fake.time = 0.75
        director.update()
        self.assertFalse(director.is_input_locked())
        fake.time = 1.0
        director.update()
        self.assertEqual(director.get_screen_state("b"), MOTION.ScreenState.ACTIVE)

    def test_cancel_rolls_back_dom_and_ticket(self):
        director, fake = self.director(asset([
            {"at": 0.0, "action": "display", "target": "$target", "value": "flex"},
            {"at": 0.0, "action": "class", "target": "$target", "name": "enter", "enabled": True},
        ]))
        director.play_transition("go", "a", "b")
        director.cancel_transition()
        self.assertFalse(director.is_transitioning())
        self.assertFalse(director.is_input_locked())
        self.assertIn(("style", "doc", "screen-b", "display", "none"), fake.calls)
        self.assertIn(("class", "doc", "screen-b", "enter", False), fake.calls)

    def test_callback_and_destroyed_document_cannot_leak_lock(self):
        director, fake = self.director(asset([
            {"at": 0.0, "action": "class", "target": "$source", "name": "exit", "enabled": True},
            {"at": 0.1, "action": "event", "name": "explode"},
        ]))

        def callback(action, name, ticket):
            fake.fail_classes = True
            raise RuntimeError((action, name, ticket))

        director.play_transition("go", "a", "b", callback=callback)
        fake.time = 0.1
        with self.assertRaises(RuntimeError):
            director.update()
        self.assertFalse(director.is_transitioning())
        self.assertFalse(director.is_input_locked())

    def test_replace_and_accessibility_tracks(self):
        motion_asset = asset([
            {"at": 0.0, "action": "class", "target": "$source", "name": "travel", "enabled": True, "reduceMotion": "skip"},
            {"at": 0.0, "action": "class", "target": "$source", "name": "flash", "enabled": True, "reduceFlash": "skip"},
            {"at": 0.2, "action": "marker", "name": "ready"},
        ])
        director, fake = self.director(motion_asset)
        director.set_accessibility(True, True)
        first = director.play_transition("go", "a", "b")
        second = director.play_transition("go", "a", "b")
        self.assertGreater(second, first)
        self.assertEqual([call for call in fake.calls if call[0] == "class" and call[4]], [])
        fake.time = 1.0
        director.update()
        self.assertFalse(director.is_input_locked())

    def test_render_transition_interpolates_and_clears(self):
        motion_asset = asset(
            [{"at": 1.0, "action": "marker", "name": "ready"}],
            render_transitions=[{
                "start": 0.0,
                "end": 1.0,
                "from": 0.0,
                "to": 1.0,
                "effect": "star_petal_reveal",
            }],
        )
        director, fake = self.director(motion_asset)
        director.set_accessibility(False, True)
        director.play_transition("go", "a", "b")
        fake.time = 0.5
        director.update()
        render_calls = [call for call in fake.calls if call[0] == "render"]
        self.assertAlmostEqual(render_calls[-1][2], 0.5)
        self.assertTrue(render_calls[-1][4])
        fake.time = 1.0
        director.update()
        self.assertEqual(fake.calls[-1], ("render-clear",))

    def test_marker_reentrancy_defers_new_timeline_and_stops_old_tracks(self):
        motion_asset = {
            "format": "UiMotion",
            "version": 1,
            "budgets": {"maxActiveTimelines": 1, "maxTracksPerTimeline": 16},
            "timelines": {
                "replace": {
                    "duration": 1.0,
                    "interrupt": "replace",
                    "unlockMarker": "ready",
                    "tracks": [
                        {"at": 0.1, "action": "marker", "name": "switch"},
                        {"at": 0.1, "action": "class", "target": "$target", "name": "stale-track", "enabled": True},
                    ],
                },
                "modal": {
                    "duration": 0.5,
                    "interrupt": "replace",
                    "unlockMarker": "ready",
                    "tracks": [
                        {"at": 0.0, "action": "display", "target": "$target", "value": "flex"},
                        {"at": 0.0, "action": "marker", "name": "switch"},
                        {"at": 0.5, "action": "marker", "name": "ready"},
                    ],
                },
            },
        }
        fake = FakeRml()
        director = MOTION.UIAnimationDirector("doc", motion_asset, fake)
        director.register_screen("a", "screen-a", visible=True)
        director.register_screen("b", "screen-b")
        director.register_screen("c", "screen-c", modal=True)
        screens = MOTION.UIScreenStack(director)

        def immediately_open_next_modal(action, name, unused_ticket):
            if action == "marker" and name == "switch":
                screens.show_modal("c", "modal")

        director.play_transition(
            "replace", "a", "b", "replace", immediately_open_next_modal)
        fake.time = 0.1
        director.update()

        self.assertEqual(director._stack, ["b", "c"])
        self.assertEqual(director.get_screen_state("b"), MOTION.ScreenState.SUSPENDED)
        self.assertEqual(director.get_screen_state("c"), MOTION.ScreenState.ENTERING)
        self.assertIn(
            ("class", "doc", "screen-b", "stale-track", True),
            fake.calls,
        )
        self.assertNotIn(
            ("class", "doc", "screen-c", "stale-track", True),
            fake.calls,
        )

        fake.time = 0.6
        director.update()
        self.assertEqual(director.get_screen_state("c"), MOTION.ScreenState.ACTIVE)
        self.assertFalse(director.is_input_locked())

    def test_repeated_modal_request_does_not_restart_enter_or_exit(self):
        motion_asset = asset([
            {"at": 0.0, "action": "display", "target": "$target", "value": "flex"},
            {"at": 0.0, "action": "class", "target": "$target", "name": "enter", "enabled": True},
            {"at": 0.1, "action": "marker", "name": "switch"},
            {"at": 1.0, "action": "marker", "name": "ready"},
        ])
        fake = FakeRml()
        director = MOTION.UIAnimationDirector("doc", motion_asset, fake)
        director.register_screen("a", "screen-a", visible=True)
        director.register_screen("c", "screen-c", modal=True)
        screens = MOTION.UIScreenStack(director)

        ticket = screens.show_modal("c", "go")
        calls_after_first_request = list(fake.calls)
        self.assertGreater(ticket, 0)
        self.assertEqual(screens.show_modal("c", "go"), 0)
        self.assertEqual(fake.calls, calls_after_first_request)
        self.assertEqual(director._active["ticket"], ticket)

        fake.time = 0.1
        director.update()
        self.assertEqual(screens.close_modal("go"), ticket + 1)
        calls_after_first_close = list(fake.calls)
        self.assertEqual(screens.close_modal("go"), 0)
        self.assertEqual(fake.calls, calls_after_first_close)

    def test_modal_enter_has_static_scale_zero_before_display(self):
        motion_asset = json.loads(SURVIVOR_MOTION_PATH.read_text(encoding="utf-8"))
        style = SURVIVOR_STYLE_PATH.read_text(encoding="utf-8")
        self.assertIn(
            ".modal_entering {\n  opacity: 0;\n"
            "  animation: 0.32s screen-zoom-backdrop-enter cubic-out;",
            style,
        )
        self.assertIn(
            ".modal_entering > .modal_card {\n  opacity: 0;\n"
            "  transform: scale(0);",
            style,
        )
        fake = FakeRml()
        director = MOTION.UIAnimationDirector("doc", motion_asset, fake)
        director.register_screen("start", "screen-a", visible=True)
        director.register_screen("settings", "screen-b", modal=True)
        screens = MOTION.UIScreenStack(director)

        screens.show_modal("settings", "modal_enter")
        displayed = ("style", "doc", "screen-b", "display", "flex")
        entering = ("class", "doc", "screen-b", "modal_entering", True)
        self.assertLess(fake.calls.index(entering), fake.calls.index(displayed))

        reduced_fake = FakeRml()
        reduced = MOTION.UIAnimationDirector("doc", motion_asset, reduced_fake)
        reduced.register_screen("start", "screen-a", visible=True)
        reduced.register_screen("settings", "screen-b", modal=True)
        reduced.set_accessibility(True, False)
        MOTION.UIScreenStack(reduced).show_modal("settings", "modal_enter")
        self.assertIn(displayed, reduced_fake.calls)
        self.assertNotIn(entering, reduced_fake.calls)

    def test_clock_failure_and_shutdown_cannot_leak_input_lock(self):
        director, fake = self.director(asset([
            {"at": 0.0, "action": "class", "target": "$source", "name": "exit", "enabled": True},
        ], render_transitions=[{
            "start": 0.0,
            "end": 1.0,
            "from": 1.0,
            "to": 0.0,
            "effect": "diagonal_sticker_wipe",
        }]))

        fake.time = math.nan
        with self.assertRaises(MOTION.UiMotionError):
            director.play_transition("go", "a", "b")
        self.assertFalse(director.is_input_locked())
        self.assertEqual(director.get_screen_state("a"), MOTION.ScreenState.ACTIVE)

        fake.time = 0.0
        director.play_transition("go", "a", "b")
        fake.fail_time = True
        with self.assertRaises(RuntimeError):
            director.update()
        self.assertFalse(director.is_input_locked())

        fake.fail_time = False
        director.play_transition("go", "a", "b")
        director.shutdown()
        self.assertFalse(director.is_transitioning())
        self.assertFalse(director.is_input_locked())
        self.assertEqual(fake.calls[-1], ("render-clear",))


if __name__ == "__main__":
    unittest.main(verbosity=2)
