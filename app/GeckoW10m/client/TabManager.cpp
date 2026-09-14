// TabManager.cpp
#include "pch.h"
#include "TabManager.h"

#include <algorithm>

namespace gecko_w10m::client {

TabManager::TabManager(std::shared_ptr<gecko_w10m::engine::Runtime> runtime)
    : runtime_(std::move(runtime)) {}

Tab* TabManager::SelectedTab() {
  if (selected_ < 0 || selected_ >= (int)tabs_.size()) return nullptr;
  return &tabs_[selected_];
}

Tab* TabManager::At(int index) {
  if (index < 0 || index >= (int)tabs_.size()) return nullptr;
  return &tabs_[index];
}

int TabManager::AddTab(bool isPrivate, std::wstring_view initialUrl) {
  Tab tab;
  tab.isPrivate = isPrivate;
  tab.session = runtime_ ? runtime_->CreateSession(isPrivate) : nullptr;
  tab.url = std::wstring(initialUrl);
  tabs_.push_back(std::move(tab));
  int index = (int)tabs_.size() - 1;
  selected_ = index;
  if (!initialUrl.empty() && tabs_[index].session) {
    tabs_[index].session->LoadUri(initialUrl);
  }
  NotifyChanged();
  return index;
}

void TabManager::SelectTab(int index) {
  if (index < 0 || index >= (int)tabs_.size()) return;
  selected_ = index;
  NotifyChanged();
}

void TabManager::CloseTab(int index) {
  if (index < 0 || index >= (int)tabs_.size()) return;
  tabs_.erase(tabs_.begin() + index);
  if (tabs_.empty()) {
    selected_ = -1;
  } else if (selected_ >= (int)tabs_.size()) {
    selected_ = (int)tabs_.size() - 1;
  }
  NotifyChanged();
}

void TabManager::MoveTab(int from, int to) {
  if (from < 0 || from >= (int)tabs_.size()) return;
  to = std::clamp(to, 0, (int)tabs_.size() - 1);
  if (from == to) return;
  Tab moved = std::move(tabs_[from]);
  tabs_.erase(tabs_.begin() + from);
  tabs_.insert(tabs_.begin() + to, std::move(moved));
  selected_ = to;
  NotifyChanged();
}

}  // namespace gecko_w10m::client
