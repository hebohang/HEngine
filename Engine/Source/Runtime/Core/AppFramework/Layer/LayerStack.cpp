#include "Hepch.h"
#include "LayerStack.h"

#include "Runtime/Core/Log/Log.h"

#include <exception>

namespace HEngine
{

	LayerStack::LayerStack()
	{
	}

	LayerStack::~LayerStack()
	{
		Shutdown();
	}

	void LayerStack::Shutdown() noexcept
	{
		std::vector<Layer*> layers;
		layers.swap(mLayers);
		mLayerInsertIndex = 0;

		for (Layer* layer : layers)
		{
			if (!layer)
				continue;

			try
			{
				layer->OnDetach();
			}
			catch (const std::exception& exception)
			{
				Log::WriteFallbackError("Layer detach failed during shutdown.", exception.what());
			}
			catch (...)
			{
				Log::WriteFallbackError("Layer detach failed during shutdown.", "unknown exception");
			}
			delete layer;
		}
	}

	void LayerStack::PushLayer(Layer* layer)
	{
		mLayers.emplace(mLayers.begin() + mLayerInsertIndex, layer);
		mLayerInsertIndex++;
	}

	void LayerStack::PushOverlay(Layer* overlay)
	{
		mLayers.emplace_back(overlay);
	}

	void LayerStack::PopLayer(Layer* layer)
	{
		auto it = std::find(mLayers.begin(), mLayers.end(), layer);
		if (it != mLayers.end())
		{
			mLayers.erase(it);
			mLayerInsertIndex--;
		}
	}

	void LayerStack::PopOverlay(Layer* overlay)
	{
		auto it = std::find(mLayers.begin(), mLayers.end(), overlay);
		if (it != mLayers.end())
			mLayers.erase(it);
	}

}
