import HEngine
from StubGen.Src.Pybind11Stubgen import ModuleStubsGenerator

class GenerateStubs():
    def Start(self, gameObject):
        module = ModuleStubsGenerator(HEngine)
        module.parse()
        module.write_setup_py = False
        module.write()
