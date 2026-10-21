#pragma once

namespace HEngine
{
    class Layer;

    // Each project supplies its own game layer; the engine owns host startup.
    Layer* CreateProjectGameLayer();
}
