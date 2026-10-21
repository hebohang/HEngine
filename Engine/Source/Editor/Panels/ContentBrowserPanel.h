#pragma once

#include "BasePanel.h"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <vector>

namespace HEngine
{
	class Event;
	class KeyPressedEvent;
	class ContentBrowserPanel : public BasePanel
	{
		REGISTER_PANEL(ContentBrowserPanel)
	public:
		ContentBrowserPanel(EditorLayer* parent, std::string panelName, bool* pOpen);

		void OnEvent(Event& e) override;
		void OnImGuiRender() override;
	protected:
		void OnImGuiRenderImpl() override;
	private:
		struct DirectoryEntry
		{
			std::filesystem::path path;
			std::string relativePath;
			std::string filenameString;
			std::string extension;
			std::string thumbnailCacheKey;
			bool bIsDirectory;
			bool bIsImage;
		};

		struct DirectoryTreeNode
		{
			std::filesystem::path path;
			std::string displayName;
			std::vector<DirectoryTreeNode> children;
		};

		void DrawTree();
		void DrawTreeNode(const DirectoryTreeNode& node);
		void DrawContent();
		void BeginRename(const DirectoryEntry& entry);
		void DrawRenamePopup();
		bool CommitRename();
		void CancelRename();
		DirectoryTreeNode BuildTreeNode(const std::filesystem::path& currentPath) const;
		bool TryBuildDirectoryEntry(const std::filesystem::directory_entry& filesystemEntry, DirectoryEntry& entry) const;
		void RefreshTreeCache();
		void RefreshDirectoryCache();
		void RefreshSearchCache();
		void InvalidateDirectoryCache() { bDirCacheDirty = true; bTreeCacheDirty = true; bSearchCacheDirty = true; }

		bool OnKeyPressedTree(KeyPressedEvent& e);
		bool OnKeyPressedContent(KeyPressedEvent& e);

		void NewFolder();
	private:
		char renameBuffer[512] = {};
		char mSearchBuffer[256] = {};
		std::filesystem::path mRenamePath;
		std::string mRenameError;
		bool bHandleTreeEvent = false;
		bool bFocusForDirectoryCheck = false;
		bool bHandleContentEvent = false;
		bool bRenamePopupRequested = false;
		bool bRenameFocusRequested = false;
		float mThumbnailSize = 56.0f;
		std::filesystem::path mCurrentDirectory;
		std::optional<std::filesystem::path> mSelectedDirectory;
		std::optional<std::filesystem::path> mSelectedEntry;
		std::vector<DirectoryTreeNode> mTreeRoots;
		std::vector<DirectoryEntry> mCachedEntries;
		std::vector<DirectoryEntry> mCachedSearchEntries;
		std::filesystem::path mCachedDirectory;
		std::filesystem::path mCachedSearchDirectory;
		bool bTreeCacheDirty = true;
		bool bDirCacheDirty = true;
		bool bSearchCacheDirty = true;
		uint64_t mThumbnailDirectoryGeneration = 1;
	};
}
