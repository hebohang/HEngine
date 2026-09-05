import HEngine, HEngine.Input, HEngine.KeyCode

class Props():
    step = 5.0
    ignore__ = 111
    handsome = "hbh"

class MoveRight:
    def __init__(self) -> None:
        pass

    def Start(self, gameObject:HEngine.Entity):
        HEngine.HE_INFO("Start")
        self.gameObject = gameObject
        self.moveSpeed = 0.1

    def Update(self, ts:HEngine.Timestep):
        # HEngine.HE_CORE_TRACE("FPS:  %f" % (1 / ts.GetSeconds()))
        trans = self.gameObject.GetComponent("Transform")
        if (HEngine.Input.IsKeyPressed(HEngine.KeyCode.W)):
            self.gameObject.Move(0.0, 0.0, -5.0)
            HEngine.HE_CORE_TRACE("Press W")
        if (HEngine.Input.IsKeyPressed(HEngine.KeyCode.S)):
            self.gameObject.Move(0.0, 0.0, 5.0)
            HEngine.HE_CORE_TRACE("Press S")
        if (HEngine.Input.IsKeyPressed(HEngine.KeyCode.A)):
            self.gameObject.Move(-5.0, 0.0, 0.0)
            HEngine.HE_CORE_TRACE("Press A")
        if (HEngine.Input.IsKeyPressed(HEngine.KeyCode.D)):
            self.gameObject.Move(5.0, 0.0, 0.0)
            HEngine.HE_CORE_TRACE("Press D")
        if (HEngine.Input.IsKeyPressed(HEngine.KeyCode.E)):
            self.gameObject.Move(0.0, 5.0, 0.0)
            HEngine.HE_CORE_TRACE("Press E")
        if (HEngine.Input.IsKeyPressed(HEngine.KeyCode.Q)):
            self.gameObject.Move(0.0, -5.0, 0.0)
            HEngine.HE_CORE_TRACE("Press Q")
        tagComp = self.gameObject.GetComponent("Tag")
        if (HEngine.Input.IsKeyPressed(HEngine.KeyCode.Space)):
            tagComp.Tag = "hbh"
        return


    def Stop(self):
        HEngine.HE_CORE_INFO("Stop!")

    def OnUpdateEditor(self, transformComponent, ts):
        transformComponent.SetTranslation(transformComponent.GetTranslation().x + ts.GetSeconds(), transformComponent.GetTranslation().y, transformComponent.GetTranslation().z)
        return HEngine.vec3(1, 2, 3)