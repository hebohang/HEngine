"""
ExampleHUD.py  —  Demo HUD driven by the runtime RmlUi pipeline.

Attach a PythonScriptComponent to any entity with ScriptPath = "ExampleHUD".
"""

from HEngineUI import UIScript
import HEngine

RmlUI = HEngine.RmlUI


class Prop:
    pass


class Props:
    pass


class ExampleHUD(UIScript):

    def __init__(self):
        self.health = 80.0
        self.shield = 45.0
        self.score = 1234
        self.document_id = ""

    def Start(self, entity=None):
        self.document_id = RmlUI.Load("Assets/UI/ExampleHUD.hui")
        HEngine.HE_CORE_INFO(f"ExampleHUD Start: document_id='{self.document_id}'")
        if self.document_id:
            RmlUI.Show(self.document_id)
            RmlUI.SetActive(self.document_id)
            self._sync_ui()
        else:
            HEngine.HE_CORE_ERROR("ExampleHUD failed to load Assets/UI/ExampleHUD.hui")

    def OnUI(self):
        if self.document_id:
            self._sync_ui()

    def Stop(self):
        if self.document_id:
            RmlUI.Unload(self.document_id)
            self.document_id = ""

    def _sync_ui(self):
        if not self.document_id:
            return

        RmlUI.SetText(self.document_id, "score_label", f"Score: {self.score}")
        RmlUI.SetStyle(self.document_id, "hp_fill", "width", f"{max(self.health, 0.0) * 2.18:.0f}px")
        RmlUI.SetStyle(self.document_id, "shield_fill", "width", f"{max(self.shield, 0.0) * 2.71:.0f}px")
