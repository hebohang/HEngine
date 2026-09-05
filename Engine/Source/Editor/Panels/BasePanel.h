#pragma once 

#include <string>

namespace HEngine
{
#define REGISTER_PANEL(class_name) \
    public: static std::string GetTypeName() {return #class_name;}

    class EditorLayer;
    class Event;
    class IEditorUndoClient;

    class BasePanel
    {
    public:
        BasePanel(EditorLayer* parent, std::string panelName, bool* pOpen);
        BasePanel(BasePanel&&) = default;
        virtual ~BasePanel() {}

        void Open() { *pOpen = true; }
        [[nodiscard]] bool IsOpen() const { return pOpen && *pOpen; }
        [[nodiscard]] EditorLayer* GetParent() const { return mParent; }

        virtual void OnImGuiRender();
        virtual void OnEvent(Event& e) {}
        virtual IEditorUndoClient* GetUndoClient() { return nullptr; }
    protected:
        virtual void OnImGuiRenderImpl() = 0;
        EditorLayer* mParent;
        std::string mName;
        bool* pOpen;
    };
}
