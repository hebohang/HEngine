"""
HEngineUI — In-game ImGui UI framework for HEngine Python scripts.

Usage example
-------------
    from HEngineUI import UIScript
    import HEngine

    class MyHUD(UIScript):
        def __init__(self):
            self.health = 100.0
            self.score = 0

        def Start(self):
            pass

        def OnUI(self):
            import HEngine
            UI = HEngine.UI

            UI.SetNextWindowPosScaled(10.0, 10.0)
            UI.SetNextWindowSizeScaled(220.0, 120.0)
            UI.Begin("HUD", UI.WindowFlags_NoDecoration | UI.WindowFlags_NoMove
                            | UI.WindowFlags_NoSavedSettings | UI.WindowFlags_NoBackground)

            UI.TextColored(0.2, 1.0, 0.4, 1.0, f"Score: {self.score}")
            UI.Spacing()
            UI.Text("Health")
            UI.ProgressBarScaled(self.health / 100.0, -1.0, 0.0)

            UI.End()
"""

from HEngineUI.layout import Column, Group, Window, S
from HEngineUI.motion import ScreenState, UIAnimationDirector, UIScreenStack, UiMotionError


class UIScript:
    """Base class for all Python-driven in-game UI scripts.

    Sub-class this and override ``OnUI`` (and optionally ``Start``).
    The ``OnUI`` method is called every frame inside a valid ImGui context.
    """

    def Start(self, entity=None):
        """Called once when the scene starts (optional)."""
        pass

    def OnUI(self):
        """Called every frame — place all ImGui calls here."""
        pass


__all__ = [
    "UIScript", "Column", "Group", "Window", "S",
    "ScreenState", "UIAnimationDirector", "UIScreenStack", "UiMotionError",
]
