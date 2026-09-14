// TabManager.h — W10M port of the iOS TabManager. Owns the tab list and the
// selected index, each tab wrapping an engine Session. Deliberately a small
// subset of the iOS TabManagerImplementation (regular tabs + private tabs,
// select/add/close/move), enough to drive the shell.
#pragma once

#include <memory>
#include <string>
#include <vector>
#include "../engine/GeckoEngine.h"

namespace gecko_w10m::client {

struct Tab {
  std::shared_ptr<gecko_w10m::engine::Session> session;
  std::wstring title;
  std::wstring url;
  bool isPrivate = false;
};

class TabManager {
 public:
  explicit TabManager(std::shared_ptr<gecko_w10m::engine::Runtime> runtime);

  int Count() const { return (int)tabs_.size(); }
  int SelectedIndex() const { return selected_; }
  Tab* SelectedTab();
  Tab* At(int index);

  // Returns the new tab's index and selects it.
  int AddTab(bool isPrivate, std::wstring_view initialUrl);
  void SelectTab(int index);
  void CloseTab(int index);
  void MoveTab(int from, int to);

  using ChangedHandler = std::function<void()>;
  void OnChanged(ChangedHandler h) { onChanged_ = std::move(h); }

 private:
  void NotifyChanged() { if (onChanged_) onChanged_(); }

  std::shared_ptr<gecko_w10m::engine::Runtime> runtime_;
  std::vector<Tab> tabs_;
  int selected_ = -1;
  ChangedHandler onChanged_;
};

}  // namespace gecko_w10m::client
