#include "hepch.h"
#include "LayerStack.h"

namespace HEngine
{

	LayerStack::LayerStack()
	{
	}

	LayerStack::~LayerStack()
	{
		for (Layer* layer : mLayers)
		{
			layer->OnDetach();
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
