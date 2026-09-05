"""Deterministic, interruptible screen animation orchestration for RmlUi."""

import math


class ScreenState:
    HIDDEN = "Hidden"
    PRE_ENTERING = "PreEntering"
    ENTERING = "Entering"
    ACTIVE = "Active"
    EXITING = "Exiting"
    SUSPENDED = "Suspended"


class UiMotionError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise UiMotionError(message)


class UIAnimationDirector:
    """Executes validated UiMotion v1 tracks using the presentation clock.

    Tracks are discrete DOM mutations. RCSS owns interpolation, keeping Python
    work bounded and independent of refresh rate.
    """

    _ACTIONS = {"class", "display", "marker", "sound", "event"}
    _INTERRUPTS = {"cancel", "replace", "reverse", "safe_complete"}
    _RENDER_EFFECTS = {
        "diagonal_sticker_wipe",
        "star_petal_reveal",
        "halftone_dot_reveal",
    }

    def __init__(self, document_id, motion_asset, rml_ui=None):
        if rml_ui is None:
            from HEngine import RmlUI as rml_ui
        self.document_id = document_id
        self._rml = rml_ui
        self._timelines = self._validate(motion_asset)
        self._screens = {}
        self._stack = []
        self._active = None
        self._ticket = 0
        self._input_locks = set()
        self._reduce_motion = False
        self._reduce_flash = False
        self._dispatch_depth = 0
        self._draining_transitions = False
        self._pending_transitions = []

    @staticmethod
    def _validate(asset):
        _require(isinstance(asset, dict), "UiMotion root must be an object")
        _require(asset.get("format") == "UiMotion", "UiMotion format must be 'UiMotion'")
        _require(type(asset.get("version")) is int and asset.get("version") == 1,
                 "Only UiMotion version 1 is supported")
        _require(set(asset).issubset({"format", "version", "timelines", "budgets"}),
                 "UiMotion root contains unknown fields")
        budgets = asset.get("budgets", {})
        _require(isinstance(budgets, dict), "UiMotion budgets must be an object")
        _require(set(budgets).issubset({"maxActiveTimelines", "maxTracksPerTimeline"}),
                 "UiMotion budgets contains unknown fields")
        _require(type(budgets.get("maxActiveTimelines", 1)) is int and
                 budgets.get("maxActiveTimelines", 1) == 1,
                 "UiMotion v1 supports exactly one active timeline")
        max_tracks = budgets.get("maxTracksPerTimeline", 256)
        _require(type(max_tracks) is int and 1 <= max_tracks <= 256,
                 "UiMotion maxTracksPerTimeline must be between 1 and 256")
        timelines = asset.get("timelines")
        _require(isinstance(timelines, dict) and timelines, "UiMotion timelines must be a non-empty object")
        result = {}
        for name, value in timelines.items():
            _require(isinstance(name, str) and name, "Timeline names must be non-empty strings")
            _require(isinstance(value, dict), f"Timeline '{name}' must be an object")
            _require(set(value).issubset({
                         "duration", "interrupt", "unlockMarker", "tracks", "renderTransitions"
                     }),
                     f"Timeline '{name}' contains unknown fields")
            duration = value.get("duration")
            _require(type(duration) in (int, float) and 0.0 <= duration <= 10.0,
                     f"Timeline '{name}' duration must be between 0 and 10 seconds")
            interrupt = value.get("interrupt", "replace")
            _require(interrupt in UIAnimationDirector._INTERRUPTS,
                     f"Timeline '{name}' has invalid interrupt policy")
            tracks = value.get("tracks")
            _require(isinstance(tracks, list) and len(tracks) <= max_tracks,
                     f"Timeline '{name}' tracks exceed the configured budget")
            unlock_marker = value.get("unlockMarker", "complete")
            _require(isinstance(unlock_marker, str) and unlock_marker,
                     f"Timeline '{name}' unlockMarker must be a non-empty string")
            checked = []
            for index, track in enumerate(tracks):
                _require(isinstance(track, dict), f"Timeline '{name}' track {index} must be an object")
                _require(set(track).issubset({"at", "action", "target", "name", "enabled", "value", "reduceMotion", "reduceFlash"}),
                         f"Timeline '{name}' track {index} contains unknown fields")
                at = track.get("at")
                action = track.get("action")
                _require(type(at) in (int, float) and 0.0 <= at <= duration,
                         f"Timeline '{name}' track {index} time is outside its duration")
                _require(action in UIAnimationDirector._ACTIONS,
                         f"Timeline '{name}' track {index} has an invalid action")
                if action in ("class", "display"):
                    _require(isinstance(track.get("target"), str) and track["target"],
                             f"Timeline '{name}' track {index} requires a target")
                if action in ("class", "marker", "sound", "event"):
                    _require(isinstance(track.get("name"), str) and track["name"],
                             f"Timeline '{name}' track {index} requires a name")
                if action == "display":
                    _require(track.get("value") in ("none", "block", "flex"),
                             f"Timeline '{name}' track {index} has an invalid display value")
                checked.append(dict(track))
            render_transitions = value.get("renderTransitions", [])
            _require(isinstance(render_transitions, list) and len(render_transitions) <= 8,
                     f"Timeline '{name}' renderTransitions must contain at most 8 segments")
            checked_render_transitions = []
            previous_end = 0.0
            for index, segment in enumerate(render_transitions):
                _require(isinstance(segment, dict),
                         f"Timeline '{name}' render transition {index} must be an object")
                _require(set(segment).issubset({"start", "end", "from", "to", "effect"}),
                         f"Timeline '{name}' render transition {index} contains unknown fields")
                start = segment.get("start")
                end = segment.get("end")
                from_value = segment.get("from")
                to_value = segment.get("to")
                effect = segment.get("effect")
                _require(type(start) in (int, float) and type(end) in (int, float) and
                         0.0 <= start < end <= duration,
                         f"Timeline '{name}' render transition {index} has invalid times")
                _require(float(start) >= previous_end,
                         f"Timeline '{name}' render transitions must not overlap")
                _require(type(from_value) in (int, float) and
                         type(to_value) in (int, float) and
                         0.0 <= from_value <= 1.0 and 0.0 <= to_value <= 1.0,
                         f"Timeline '{name}' render transition {index} values must be in [0, 1]")
                _require(effect in UIAnimationDirector._RENDER_EFFECTS,
                         f"Timeline '{name}' render transition {index} has an invalid effect")
                checked_render_transitions.append({
                    "start": float(start),
                    "end": float(end),
                    "from": float(from_value),
                    "to": float(to_value),
                    "effect": effect,
                })
                previous_end = float(end)
            result[name] = {
                "duration": float(duration),
                "interrupt": interrupt,
                "unlockMarker": unlock_marker,
                "tracks": sorted(checked, key=lambda item: float(item["at"])),
                "renderTransitions": checked_render_transitions,
            }
        return result

    def register_screen(self, name, element_id, visible=False, modal=False):
        _require(isinstance(name, str) and name and name not in self._screens,
                 "Screen name must be unique and non-empty")
        _require(isinstance(element_id, str) and element_id,
                 "Screen element ID must be non-empty")
        self._screens[name] = {
            "element": element_id,
            "state": ScreenState.ACTIVE if visible else ScreenState.HIDDEN,
            "modal": bool(modal),
        }
        if visible:
            self._stack.append(name)

    def set_accessibility(self, reduce_motion, reduce_flash):
        self._reduce_motion = bool(reduce_motion)
        self._reduce_flash = bool(reduce_flash)

    def play_transition(self, name, source="", target="", operation="replace", callback=None):
        _require(name in self._timelines, f"Unknown UiMotion timeline '{name}'")
        if source:
            _require(source in self._screens, f"Unknown source screen '{source}'")
        if target:
            _require(target in self._screens, f"Unknown target screen '{target}'")

        if self._dispatch_depth:
            if self._active and self._timelines[name]["interrupt"] == "cancel":
                return 0
            self._ticket += 1
            ticket = self._ticket
            self._input_locks.add(ticket)
            self._pending_transitions.append({
                "ticket": ticket,
                "name": name,
                "source": source,
                "target": target,
                "operation": operation,
                "callback": callback,
            })
            return ticket

        return self._play_transition_now(
            name, source, target, operation, callback, reserved_ticket=0)

    def _play_transition_now(
            self, name, source, target, operation, callback, reserved_ticket):
        try:
            start_time = float(self._rml.GetPresentationTime())
        except Exception:
            if reserved_ticket:
                self._input_locks.discard(reserved_ticket)
            raise
        if not math.isfinite(start_time):
            if reserved_ticket:
                self._input_locks.discard(reserved_ticket)
            raise UiMotionError("Presentation time must be finite")

        if self._active:
            policy = self._timelines[name]["interrupt"]
            if policy == "cancel":
                if reserved_ticket:
                    self._input_locks.discard(reserved_ticket)
                return 0
            if policy == "safe_complete":
                self.skip_transition()
            elif policy == "reverse" and self._active["source"] == target and self._active["target"] == source:
                self._interrupt_active()
            else:
                self._interrupt_active()

        if reserved_ticket:
            ticket = reserved_ticket
        else:
            self._ticket += 1
            ticket = self._ticket
        self._input_locks.add(ticket)
        previous_states = {
            screen_name: screen["state"]
            for screen_name, screen in self._screens.items()
        }
        previous_stack = list(self._stack)
        if source:
            self._screens[source]["state"] = ScreenState.EXITING
        if target:
            self._screens[target]["state"] = ScreenState.PRE_ENTERING
        self._active = {
            "ticket": ticket,
            "name": name,
            "timeline": self._timelines[name],
            "source": source,
            "target": target,
            "operation": operation,
            "callback": callback,
            "start": start_time,
            "next": 0,
            "undo": [],
            "previous_states": previous_states,
            "previous_stack": previous_stack,
            "committed": False,
        }
        self._advance(0.0)
        return ticket

    def _interrupt_active(self):
        active = self._active
        if not active:
            return
        if active["committed"]:
            self._complete_committed_active(active)
        else:
            self._fail_safe_finish(active["ticket"])

    def _complete_committed_active(self, active):
        """Apply old DOM cleanup without dispatching stale callbacks.

        A marker callback may request the next timeline after the old stack has
        already committed. Class/display cleanup still belongs to the old
        screen, but later marker/event callbacks must not recursively mutate the
        replacement timeline.
        """
        tracks = active["timeline"]["tracks"]
        while active["next"] < len(tracks):
            track = tracks[active["next"]]
            active["next"] += 1
            if self._reduce_motion and track.get("reduceMotion") == "skip":
                continue
            if self._reduce_flash and track.get("reduceFlash") == "skip":
                continue
            if track["action"] in ("class", "display"):
                self._execute(track, active)
            elif (track["action"] == "marker" and
                  track["name"] == active["timeline"]["unlockMarker"]):
                self._input_locks.discard(active["ticket"])
        self._finish(active["ticket"])

    def _discard_pending_transitions(self):
        for request in self._pending_transitions:
            self._input_locks.discard(request["ticket"])
        self._pending_transitions.clear()

    def _drain_pending_transitions(self):
        if self._draining_transitions or self._dispatch_depth:
            return
        self._draining_transitions = True
        try:
            while self._pending_transitions:
                request = self._pending_transitions.pop(0)
                try:
                    self._play_transition_now(
                        request["name"],
                        request["source"],
                        request["target"],
                        request["operation"],
                        request["callback"],
                        reserved_ticket=request["ticket"],
                    )
                except Exception:
                    self._input_locks.discard(request["ticket"])
                    self._discard_pending_transitions()
                    raise
        finally:
            self._draining_transitions = False

    def update(self):
        if not self._active:
            return
        active = self._active
        try:
            now = float(self._rml.GetPresentationTime())
            if not math.isfinite(now):
                raise UiMotionError("Presentation time must be finite")
        except Exception:
            if self._active is active:
                self._fail_safe_finish(active["ticket"])
            self._discard_pending_transitions()
            raise
        elapsed = max(0.0, now - active["start"])
        if self._reduce_motion:
            elapsed /= 0.18
        self._advance(elapsed)

    def _resolve_target(self, target, active=None):
        active = active or self._active
        if target == "$source":
            target = active["source"]
        elif target == "$target":
            target = active["target"]
        if target in self._screens:
            return self._screens[target]["element"]
        return target

    def _screen_for_track_target(self, target, active=None):
        active = active or self._active
        if target == "$source":
            target = active["source"]
        elif target == "$target":
            target = active["target"]
        return self._screens.get(target)

    def _advance(self, elapsed):
        active = self._active
        if not active:
            return
        failed = False
        self._dispatch_depth += 1
        try:
            self._update_render_transition(elapsed, active)
            if self._active is not active:
                return
            tracks = active["timeline"]["tracks"]
            while active["next"] < len(tracks) and float(tracks[active["next"]]["at"]) <= elapsed:
                track = tracks[active["next"]]
                active["next"] += 1
                if self._reduce_motion and track.get("reduceMotion") == "skip":
                    continue
                if self._reduce_flash and track.get("reduceFlash") == "skip":
                    continue
                self._execute(track, active)
                if self._active is not active or self._pending_transitions:
                    return
            if self._active is active and elapsed >= active["timeline"]["duration"]:
                self._finish(active["ticket"])
        except Exception:
            failed = True
            if self._active is active:
                self._fail_safe_finish(active["ticket"])
            self._discard_pending_transitions()
            raise
        finally:
            self._dispatch_depth -= 1
            if self._dispatch_depth == 0 and not failed:
                self._drain_pending_transitions()

    def _update_render_transition(self, elapsed, active):
        segments = active["timeline"]["renderTransitions"]
        if not segments:
            return
        segment = segments[0]
        progress = segment["from"]
        for candidate in segments:
            if elapsed < candidate["start"]:
                break
            segment = candidate
            if elapsed <= candidate["end"]:
                span = candidate["end"] - candidate["start"]
                amount = max(0.0, min(1.0, (elapsed - candidate["start"]) / span))
                progress = candidate["from"] + (
                    candidate["to"] - candidate["from"]
                ) * amount
                break
            progress = candidate["to"]
        self._rml.SetRenderTransition(
            segment["effect"],
            progress,
            self._reduce_motion,
            self._reduce_flash,
        )

    def _execute(self, track, active):
        action = track["action"]
        if action == "class":
            element = self._resolve_target(track["target"], active)
            enabled = bool(track.get("enabled", True))
            previous_enabled = self._rml.GetClass(
                self.document_id, element, track["name"])
            if previous_enabled is None:
                raise UiMotionError(
                    f"UiMotion target element '{element}' no longer exists")
            self._rml.SetClass(self.document_id, element, track["name"], enabled)
            active["undo"].append(
                ("class", element, track["name"], bool(previous_enabled)))
        elif action == "display":
            element = self._resolve_target(track["target"], active)
            previous_display = self._rml.GetStyle(
                self.document_id, element, "display")
            if previous_display is None:
                raise UiMotionError(
                    f"UiMotion target element '{element}' has no display property")
            self._rml.SetStyle(self.document_id, element, "display", track["value"])
            active["undo"].append(("display", element, previous_display))
        else:
            name = track["name"]
            if action == "marker" and name == "switch":
                self._commit_stack(active)
            if name == active["timeline"]["unlockMarker"]:
                self._input_locks.discard(active["ticket"])
            callback = active["callback"]
            if callback:
                callback(action, name, active["ticket"])

    def _commit_stack(self, active):
        source = active["source"]
        target = active["target"]
        operation = active["operation"]
        if source:
            self._screens[source]["state"] = ScreenState.HIDDEN
        if target:
            self._screens[target]["state"] = ScreenState.ENTERING
        if operation == "push" and target:
            if source:
                self._screens[source]["state"] = ScreenState.SUSPENDED
            self._stack.append(target)
        elif operation == "pop":
            if self._stack and self._stack[-1] == source:
                self._stack.pop()
            if target:
                self._screens[target]["state"] = ScreenState.ENTERING
                if not self._stack or self._stack[-1] != target:
                    self._stack.append(target)
        elif operation == "modal" and target:
            if source:
                self._screens[source]["state"] = ScreenState.SUSPENDED
            self._stack.append(target)
        elif target:
            if self._stack:
                self._stack[-1] = target
            else:
                self._stack.append(target)
        active["committed"] = True

    def _finish(self, ticket):
        active = self._active
        if not active or active["ticket"] != ticket:
            return
        if active["source"] and self._screens[active["source"]]["state"] == ScreenState.EXITING:
            self._screens[active["source"]]["state"] = ScreenState.HIDDEN
        if active["target"]:
            self._screens[active["target"]]["state"] = ScreenState.ACTIVE
        self._input_locks.discard(ticket)
        if active["timeline"]["renderTransitions"]:
            self._rml.ClearRenderTransition()
        self._active = None

    def _fail_safe_finish(self, ticket):
        active = self._active
        if active and active["ticket"] == ticket:
            try:
                for undo in reversed(active["undo"]):
                    try:
                        if undo[0] == "class":
                            self._rml.SetClass(self.document_id, undo[1], undo[2], undo[3])
                        elif undo[0] == "display":
                            self._rml.SetStyle(self.document_id, undo[1], "display", undo[2])
                    except Exception:
                        # A destroyed or reloaded document cannot be rolled back,
                        # but it must never retain an input ticket.
                        pass
            finally:
                try:
                    if active["timeline"]["renderTransitions"]:
                        self._rml.ClearRenderTransition()
                except Exception:
                    pass
                for screen_name, state in active["previous_states"].items():
                    if screen_name in self._screens:
                        self._screens[screen_name]["state"] = state
                self._stack = list(active["previous_stack"])
                self._input_locks.discard(ticket)
                self._active = None

    def cancel_transition(self):
        if self._active:
            self._fail_safe_finish(self._active["ticket"])
        self._discard_pending_transitions()

    def skip_transition(self):
        if self._active:
            self._advance(self._active["timeline"]["duration"])

    def is_transitioning(self):
        return self._active is not None

    def is_input_locked(self):
        return bool(self._input_locks)

    def get_screen_state(self, name):
        _require(name in self._screens, f"Unknown screen '{name}'")
        return self._screens[name]["state"]

    def reset_to_screen(self, name):
        _require(name in self._screens, f"Unknown screen '{name}'")
        self.cancel_transition()
        for screen_name, screen in self._screens.items():
            screen["state"] = (
                ScreenState.ACTIVE if screen_name == name else ScreenState.HIDDEN
            )
        self._stack = [name]

    def shutdown(self):
        try:
            if self._active:
                self._fail_safe_finish(self._active["ticket"])
        except Exception:
            # Shutdown and document unload must never retain an input ticket.
            pass
        finally:
            self._discard_pending_transitions()
            self._active = None
            self._input_locks.clear()
            try:
                self._rml.ClearRenderTransition()
            except Exception:
                pass

    PlayTransition = play_transition
    CancelTransition = cancel_transition
    SkipTransition = skip_transition
    IsTransitioning = is_transitioning
    GetScreenState = get_screen_state
    ResetToScreen = reset_to_screen
    Shutdown = shutdown
    Update = update


class UIScreenStack:
    def __init__(self, director):
        self.director = director

    def push_screen(self, target, timeline, callback=None):
        source = self.director._stack[-1] if self.director._stack else ""
        return self.director.play_transition(timeline, source, target, "push", callback)

    def replace_screen(self, target, timeline, callback=None):
        source = self.director._stack[-1] if self.director._stack else ""
        return self.director.play_transition(timeline, source, target, "replace", callback)

    def pop_screen(self, timeline, callback=None):
        _require(len(self.director._stack) >= 2, "Cannot pop the root UI screen")
        return self.director.play_transition(
            timeline, self.director._stack[-1], self.director._stack[-2], "pop", callback)

    def show_modal(self, target, timeline, callback=None):
        _require(target in self.director._screens, f"Unknown target screen '{target}'")
        if self.director._screens[target]["state"] in (
                ScreenState.PRE_ENTERING, ScreenState.ENTERING, ScreenState.ACTIVE):
            return 0
        source = self.director._stack[-1] if self.director._stack else ""
        return self.director.play_transition(timeline, source, target, "modal", callback)

    def close_modal(self, timeline, callback=None):
        if (not self.director._stack or
                self.director._screens[self.director._stack[-1]]["state"] == ScreenState.EXITING):
            return 0
        return self.pop_screen(timeline, callback)

    PushScreen = push_screen
    ReplaceScreen = replace_screen
    PopScreen = pop_screen
    ShowModal = show_modal
    CloseModal = close_modal
