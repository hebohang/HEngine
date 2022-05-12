import Cpp

def OnUpdateRuntime(transformComponent, ts):
    transformComponent.SetTranslation(transformComponent.GetTranslation().x + ts.GetSeconds(), transformComponent.GetTranslation().y, transformComponent.GetTranslation().z)
    return transformComponent;

def OnUpdateEditor(transformComponent):
    return transformComponent.GetTranslation().x