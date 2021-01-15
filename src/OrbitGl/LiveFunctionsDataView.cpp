// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "LiveFunctionsDataView.h"

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_split.h>
#include <absl/time/time.h>
#include <stddef.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

#include "App.h"
#include "CoreUtils.h"
#include "DataManager.h"
#include "DataViewTypes.h"
#include "FunctionsDataView.h"
#include "LiveFunctionsController.h"
#include "OrbitBase/Logging.h"
#include "OrbitClientData/FunctionUtils.h"
#include "OrbitClientModel/CaptureData.h"
#include "TextBox.h"
#include "TimeGraph.h"
#include "TimerChain.h"
#include "capture_data.pb.h"

using orbit_client_protos::FunctionInfo;
using orbit_client_protos::FunctionStats;

LiveFunctionsDataView::LiveFunctionsDataView(LiveFunctionsController* live_functions, OrbitApp* app)
    : DataView(DataViewType::kLiveFunctions, app), live_functions_(live_functions) {
  update_period_ms_ = 300;
  OnDataChanged();
}

const std::vector<DataView::Column>& LiveFunctionsDataView::GetColumns() {
  static const std::vector<Column> columns = [] {
    std::vector<Column> columns;
    columns.resize(kNumColumns);
    columns[kColumnSelected] = {"Hooked", .0f, SortingOrder::kDescending};
    columns[kColumnName] = {"Function", .4f, SortingOrder::kAscending};
    columns[kColumnCount] = {"Count", .0f, SortingOrder::kDescending};
    columns[kColumnTimeTotal] = {"Total", .075f, SortingOrder::kDescending};
    columns[kColumnTimeAvg] = {"Avg", .075f, SortingOrder::kDescending};
    columns[kColumnTimeMin] = {"Min", .075f, SortingOrder::kDescending};
    columns[kColumnTimeMax] = {"Max", .075f, SortingOrder::kDescending};
    columns[kColumnModule] = {"Module", .1f, SortingOrder::kAscending};
    columns[kColumnAddress] = {"Address", .0f, SortingOrder::kAscending};
    return columns;
  }();
  return columns;
}

std::string LiveFunctionsDataView::GetValue(int row, int column) {
  if (!app_->HasCaptureData()) {
    return "";
  }
  if (row >= static_cast<int>(GetNumElements())) {
    return "";
  }

  const FunctionInfo& function = *GetSelectedFunction(row);
  const FunctionStats& stats = app_->GetCaptureData().GetFunctionStatsOrDefault(function);

  switch (column) {
    case kColumnSelected:
      return FunctionsDataView::BuildSelectedColumnsString(app_, function);
    case kColumnName:
      return function_utils::GetDisplayName(function);
    case kColumnCount:
      return absl::StrFormat("%lu", stats.count());
    case kColumnTimeTotal:
      return GetPrettyTime(absl::Nanoseconds(stats.total_time_ns()));
    case kColumnTimeAvg:
      return GetPrettyTime(absl::Nanoseconds(stats.average_time_ns()));
    case kColumnTimeMin:
      return GetPrettyTime(absl::Nanoseconds(stats.min_ns()));
    case kColumnTimeMax:
      return GetPrettyTime(absl::Nanoseconds(stats.max_ns()));
    case kColumnModule:
      return function.loaded_module_path();
    case kColumnAddress: {
      const CaptureData& capture_data = app_->GetCaptureData();
      return absl::StrFormat("0x%llx", capture_data.GetAbsoluteAddress(function));
    }
    default:
      return "";
  }
}

void LiveFunctionsDataView::OnSelect(std::optional<int> row) {
  app_->DeselectTextBox();

  if (!row.has_value()) {
    app_->set_highlighted_function(DataManager::kInvalidFunctionAddress);
  } else {
    const CaptureData& capture_data = app_->GetCaptureData();
    app_->set_highlighted_function(
        capture_data.GetAbsoluteAddress(*GetSelectedFunction(row.value())));
  }
}

#define ORBIT_FUNC_SORT(Member)                                                      \
  [&](int a, int b) {                                                                \
    return orbit_core::Compare(functions[a].Member, functions[b].Member, ascending); \
  }
#define ORBIT_STAT_SORT(Member)                                                                    \
  [&](int a, int b) {                                                                              \
    const FunctionStats& stats_a = app_->GetCaptureData().GetFunctionStatsOrDefault(functions[a]); \
    const FunctionStats& stats_b = app_->GetCaptureData().GetFunctionStatsOrDefault(functions[b]); \
    return orbit_core::Compare(stats_a.Member, stats_b.Member, ascending);                         \
  }
#define ORBIT_CUSTOM_FUNC_SORT(Func)                                               \
  [&](int a, int b) {                                                              \
    return orbit_core::Compare(Func(functions[a]), Func(functions[b]), ascending); \
  }

void LiveFunctionsDataView::DoSort() {
  if (!app_->HasCaptureData()) {
    CHECK(functions_.size() == 0);
    return;
  }
  bool ascending = sorting_orders_[sorting_column_] == SortingOrder::kAscending;
  std::function<bool(int a, int b)> sorter = nullptr;

  const std::vector<FunctionInfo>& functions = functions_;

  switch (sorting_column_) {
    case kColumnSelected:
      sorter = ORBIT_CUSTOM_FUNC_SORT(app_->IsFunctionSelected);
      break;
    case kColumnName:
      sorter = ORBIT_CUSTOM_FUNC_SORT(function_utils::GetDisplayName);
      break;
    case kColumnCount:
      sorter = ORBIT_STAT_SORT(count());
      break;
    case kColumnTimeTotal:
      sorter = ORBIT_STAT_SORT(total_time_ns());
      break;
    case kColumnTimeAvg:
      sorter = ORBIT_STAT_SORT(average_time_ns());
      break;
    case kColumnTimeMin:
      sorter = ORBIT_STAT_SORT(min_ns());
      break;
    case kColumnTimeMax:
      sorter = ORBIT_STAT_SORT(max_ns());
      break;
    case kColumnModule:
      sorter = ORBIT_CUSTOM_FUNC_SORT(function_utils::GetLoadedModuleName);
      break;
    case kColumnAddress:
      sorter = ORBIT_FUNC_SORT(address());
      break;
    default:
      break;
  }

  if (sorter) {
    std::stable_sort(indices_.begin(), indices_.end(), sorter);
  }
}

const std::string LiveFunctionsDataView::kMenuActionSelect = "Hook";
const std::string LiveFunctionsDataView::kMenuActionUnselect = "Unhook";
const std::string LiveFunctionsDataView::kMenuActionJumpToFirst = "Jump to first";
const std::string LiveFunctionsDataView::kMenuActionJumpToLast = "Jump to last";
const std::string LiveFunctionsDataView::kMenuActionJumpToMin = "Jump to min";
const std::string LiveFunctionsDataView::kMenuActionJumpToMax = "Jump to max";
const std::string LiveFunctionsDataView::kMenuActionDisassembly = "Go to Disassembly";
const std::string LiveFunctionsDataView::kMenuActionIterate = "Add iterator(s)";
const std::string LiveFunctionsDataView::kMenuActionEnableFrameTrack = "Enable frame track(s)";
const std::string LiveFunctionsDataView::kMenuActionDisableFrameTrack = "Disable frame track(s)";

std::vector<std::string> LiveFunctionsDataView::GetContextMenu(
    int clicked_index, const std::vector<int>& selected_indices) {
  bool enable_select = false;
  bool enable_unselect = false;
  bool enable_disassembly = false;
  bool enable_iterator = false;
  bool enable_enable_frame_track = false;
  bool enable_disable_frame_track = false;

  const CaptureData& capture_data = app_->GetCaptureData();
  for (int index : selected_indices) {
    const FunctionInfo& selected_function = *GetSelectedFunction(index);

    if (app_->IsCaptureConnected(capture_data)) {
      enable_select |= !app_->IsFunctionSelected(selected_function);
      enable_unselect |= app_->IsFunctionSelected(selected_function);
      enable_disassembly = true;
    }

    const FunctionStats& stats = capture_data.GetFunctionStatsOrDefault(selected_function);
    // We need at least one function call to a function so that adding iterators makes sense.
    enable_iterator |= stats.count() > 0;

    if (app_->IsCaptureConnected(capture_data)) {
      enable_enable_frame_track |= !app_->IsFrameTrackEnabled(selected_function);
      enable_disable_frame_track |= app_->IsFrameTrackEnabled(selected_function);
    } else {
      enable_enable_frame_track |= !app_->HasFrameTrackInCaptureData(selected_function);
      enable_disable_frame_track |= app_->HasFrameTrackInCaptureData(selected_function);
    }
  }

  std::vector<std::string> menu;
  if (enable_select) menu.emplace_back(kMenuActionSelect);
  if (enable_unselect) menu.emplace_back(kMenuActionUnselect);
  if (enable_disassembly) menu.emplace_back(kMenuActionDisassembly);

  if (enable_iterator) {
    menu.emplace_back(kMenuActionIterate);
  }
  if (enable_enable_frame_track) {
    menu.emplace_back(kMenuActionEnableFrameTrack);
  }
  if (enable_disable_frame_track) {
    menu.emplace_back(kMenuActionDisableFrameTrack);
  }

  // For now, these actions only make sense when one function is selected,
  // so we don't show them otherwise.
  if (selected_indices.size() == 1) {
    const FunctionInfo& function = *GetSelectedFunction(selected_indices[0]);
    const FunctionStats& stats = capture_data.GetFunctionStatsOrDefault(function);
    if (stats.count() > 0) {
      menu.insert(menu.end(), {kMenuActionJumpToFirst, kMenuActionJumpToLast, kMenuActionJumpToMin,
                               kMenuActionJumpToMax});
    }
  }
  Append(menu, DataView::GetContextMenu(clicked_index, selected_indices));
  return menu;
}

void LiveFunctionsDataView::OnContextMenu(const std::string& action, int menu_index,
                                          const std::vector<int>& item_indices) {
  const CaptureData& capture_data = app_->GetCaptureData();
  if (action == kMenuActionSelect || action == kMenuActionUnselect ||
      action == kMenuActionDisassembly) {
    for (int i : item_indices) {
      FunctionInfo* selected_function = GetSelectedFunction(i);
      if (action == kMenuActionSelect) {
        app_->SelectFunction(*selected_function);
      } else if (action == kMenuActionUnselect) {
        app_->DeselectFunction(*selected_function);
        // Unhooking a function implies disabling (and removing) the frame
        // track for this function. While it would be possible to keep the
        // current frame track in the capture data, this would lead to a
        // somewhat inconsistent state where the frame track for this function
        // is enabled for the current capture but disabled for the next one.
        app_->DisableFrameTrack(*selected_function);
        app_->RemoveFrameTrack(*selected_function);
      } else if (action == kMenuActionDisassembly) {
        int32_t pid = capture_data.process_id();
        app_->Disassemble(pid, *selected_function);
      }
    }
  } else if (action == kMenuActionJumpToFirst) {
    CHECK(item_indices.size() == 1);
    auto function_address = capture_data.GetAbsoluteAddress(*GetSelectedFunction(item_indices[0]));
    auto first_box = GCurrentTimeGraph->FindNextFunctionCall(
        function_address, std::numeric_limits<uint64_t>::lowest());
    if (first_box != nullptr) {
      GCurrentTimeGraph->SelectAndZoom(first_box);
    }
  } else if (action == kMenuActionJumpToLast) {
    CHECK(item_indices.size() == 1);
    auto function_address = capture_data.GetAbsoluteAddress(*GetSelectedFunction(item_indices[0]));
    auto last_box = GCurrentTimeGraph->FindPreviousFunctionCall(
        function_address, std::numeric_limits<uint64_t>::max());
    if (last_box != nullptr) {
      GCurrentTimeGraph->SelectAndZoom(last_box);
    }
  } else if (action == kMenuActionJumpToMin) {
    CHECK(item_indices.size() == 1);
    const FunctionInfo& function = *GetSelectedFunction(item_indices[0]);
    auto [min_box, _] = GetMinMax(function);
    if (min_box != nullptr) {
      GCurrentTimeGraph->SelectAndZoom(min_box);
    }
  } else if (action == kMenuActionJumpToMax) {
    CHECK(item_indices.size() == 1);
    const FunctionInfo& function = *GetSelectedFunction(item_indices[0]);
    auto [_, max_box] = GetMinMax(function);
    if (max_box != nullptr) {
      GCurrentTimeGraph->SelectAndZoom(max_box);
    }
  } else if (action == kMenuActionIterate) {
    for (int i : item_indices) {
      FunctionInfo* selected_function = GetSelectedFunction(i);
      const FunctionStats& stats =
          app_->GetCaptureData().GetFunctionStatsOrDefault(*selected_function);
      if (stats.count() > 0) {
        live_functions_->AddIterator(selected_function);
      }
    }
  } else if (action == kMenuActionEnableFrameTrack) {
    for (int i : item_indices) {
      FunctionInfo* function = GetSelectedFunction(i);
      if (app_->IsCaptureConnected(capture_data)) {
        app_->SelectFunction(*function);
      }
      app_->EnableFrameTrack(*function);
      app_->AddFrameTrack(*function);
    }
  } else if (action == kMenuActionDisableFrameTrack) {
    for (int i : item_indices) {
      FunctionInfo* function = GetSelectedFunction(i);
      app_->DisableFrameTrack(*function);
      app_->RemoveFrameTrack(*function);
    }
  } else {
    DataView::OnContextMenu(action, menu_index, item_indices);
  }
}

void LiveFunctionsDataView::DoFilter() {
  if (!app_->HasCaptureData()) {
    CHECK(functions_.size() == 0);
    return;
  }
  std::vector<uint32_t> indices;

  std::vector<std::string> tokens = absl::StrSplit(ToLower(filter_), ' ');

  for (size_t i = 0; i < functions_.size(); ++i) {
    const FunctionInfo& function = functions_[i];
    std::string name = ToLower(function_utils::GetDisplayName(function));

    bool match = true;

    for (std::string& filter_token : tokens) {
      if (name.find(filter_token) == std::string::npos) {
        match = false;
        break;
      }
    }

    if (match) {
      indices.push_back(i);
    }
  }

  indices_ = std::move(indices);

  // Filter drawn textboxes
  absl::flat_hash_set<uint64_t> visible_functions;
  const CaptureData& capture_data = app_->GetCaptureData();
  for (size_t i = 0; i < indices_.size(); ++i) {
    FunctionInfo* func = GetSelectedFunction(i);
    visible_functions.insert(capture_data.GetAbsoluteAddress(*func));
  }
  app_->SetVisibleFunctions(std::move(visible_functions));
}

void LiveFunctionsDataView::OnDataChanged() {
  functions_.clear();
  indices_.clear();

  if (!app_->HasCaptureData()) {
    DataView::OnDataChanged();
    return;
  }

  const absl::flat_hash_map<uint64_t, orbit_client_protos::FunctionInfo>& selected_functions =
      app_->GetCaptureData().selected_functions();
  size_t functions_count = selected_functions.size();
  indices_.resize(functions_count);
  size_t i = 0;
  for (const auto& pair : selected_functions) {
    if (function_utils::IsOrbitFunc(pair.second)) {
      continue;
    }
    functions_.push_back(pair.second);
    indices_[i] = i;
    ++i;
  }

  DataView::OnDataChanged();
}

void LiveFunctionsDataView::OnTimer() {
  if (app_->IsCapturing()) {
    OnSort(sorting_column_, {});
  }
}

FunctionInfo* LiveFunctionsDataView::GetSelectedFunction(unsigned int row) {
  CHECK(row < functions_.size());
  return &(functions_[indices_[row]]);
}

std::pair<TextBox*, TextBox*> LiveFunctionsDataView::GetMinMax(const FunctionInfo& function) const {
  const CaptureData& capture_data = app_->GetCaptureData();
  auto function_address = capture_data.GetAbsoluteAddress(function);
  TextBox* min_box = nullptr;
  TextBox* max_box = nullptr;
  std::vector<std::shared_ptr<TimerChain>> chains =
      GCurrentTimeGraph->GetAllThreadTrackTimerChains();
  for (auto& chain : chains) {
    if (!chain) continue;
    for (auto& block : *chain) {
      for (size_t i = 0; i < block.size(); i++) {
        TextBox& box = block[i];
        if (box.GetTimerInfo().function_address() == function_address) {
          uint64_t elapsed_nanos = box.GetTimerInfo().end() - box.GetTimerInfo().start();
          if (min_box == nullptr ||
              elapsed_nanos < (min_box->GetTimerInfo().end() - min_box->GetTimerInfo().start())) {
            min_box = &box;
          }
          if (max_box == nullptr ||
              elapsed_nanos > (max_box->GetTimerInfo().end() - max_box->GetTimerInfo().start())) {
            max_box = &box;
          }
        }
      }
    }
  }
  return std::make_pair(min_box, max_box);
}

std::optional<int> LiveFunctionsDataView::GetRowFromFunctionAddress(uint64_t function_address) {
  for (int function_row = 0; function_row < static_cast<int>(GetNumElements()); function_row++) {
    if (absl::StrFormat("0x%llx", function_address) == GetValue(function_row, kColumnAddress)) {
      return function_row;
    }
  }
  return std::nullopt;
}