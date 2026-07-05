// apps/widgets/ml/tensor_inspector.cpp
#include "tensor_inspector.h"
#include "widget_registry.h"

#include <imgui.h>

REGISTER_WIDGET(straylight::widgets::TensorInspectorWidget, "tensor_inspector", "Tensor Inspector", straylight::widgets::WidgetCategory::ML);
#include <algorithm>
#include <cstdio>
#include <numeric>

namespace straylight::widgets {

const char* TensorInspectorWidget::dtype_str(DType dt) {
    switch (dt) {
        case DType::Float16:    return "float16";
        case DType::Float32:    return "float32";
        case DType::Float64:    return "float64";
        case DType::Int8:       return "int8";
        case DType::Int16:      return "int16";
        case DType::Int32:      return "int32";
        case DType::Int64:      return "int64";
        case DType::UInt8:      return "uint8";
        case DType::BFloat16:   return "bfloat16";
        case DType::Float8E4M3: return "fp8e4m3";
        case DType::Float8E5M2: return "fp8e5m2";
    }
    return "unknown";
}

const char* TensorInspectorWidget::device_str(DeviceType dt) {
    switch (dt) {
        case DeviceType::CPU:   return "cpu";
        case DeviceType::CUDA:  return "cuda";
        case DeviceType::ROCm:  return "rocm";
        case DeviceType::OneAPI:return "oneapi";
        case DeviceType::Metal: return "metal";
    }
    return "unknown";
}

std::string TensorInspectorWidget::shape_str(const std::vector<int64_t>& s) {
    std::string out = "[";
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0) out += ", ";
        out += std::to_string(s[i]);
    }
    out += "]";
    return out;
}

std::string TensorInspectorWidget::human_bytes(size_t bytes) {
    char buf[64];
    if (bytes >= 1ULL << 30) {
        std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(bytes) / (1ULL << 30));
    } else if (bytes >= 1ULL << 20) {
        std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(bytes) / (1ULL << 20));
    } else if (bytes >= 1ULL << 10) {
        std::snprintf(buf, sizeof(buf), "%.2f KiB", static_cast<double>(bytes) / (1ULL << 10));
    } else {
        std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    }
    return buf;
}

void TensorInspectorWidget::try_connect() {
    auto res = ipc_.connect("/run/straylight/bus.sock");
    connected_ = res.has_value();
    if (!connected_) error_msg_ = res.error();
}

void TensorInspectorWidget::fetch_tensors() {
    if (!connected_) return;

    auto res = ipc_.command("tensor.list");
    if (!res.has_value()) {
        error_msg_ = res.error();
        connected_ = false;
        return;
    }

    auto& j = res.value();
    if (!j.contains("tensors") || !j["tensors"].is_array()) return;

    tensors_.clear();
    total_bytes_ = 0;
    for (auto& tj : j["tensors"]) {
        TensorEntry t;
        t.name = tj.value("name", "");
        t.dtype = static_cast<DType>(tj.value("dtype", 1));
        t.device = static_cast<DeviceType>(tj.value("device", 0));
        t.device_id = tj.value("device_id", 0);
        t.requires_grad = tj.value("requires_grad", false);
        t.layout = tj.value("layout", "contiguous");

        if (tj.contains("shape") && tj["shape"].is_array()) {
            for (auto& v : tj["shape"]) t.shape.push_back(v.get<int64_t>());
        }
        if (tj.contains("strides") && tj["strides"].is_array()) {
            for (auto& v : tj["strides"]) t.strides.push_back(v.get<int64_t>());
        }

        int64_t numel = 1;
        for (auto s : t.shape) numel *= s;
        t.nbytes = static_cast<size_t>(numel) * dtype_size(t.dtype);
        total_bytes_ += t.nbytes;

        tensors_.push_back(std::move(t));
    }
}

void TensorInspectorWidget::update() {
    if (!should_update()) return;
    if (!connected_) try_connect();
    if (connected_) fetch_tensors();
}

void TensorInspectorWidget::render(bool* p_open) {
    if (!ImGui::Begin("Tensor Inspector", p_open)) {
        ImGui::End();
        return;
    }

    if (!connected_) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disconnected from straylight-bus");
        if (!error_msg_.empty()) ImGui::TextWrapped("Error: %s", error_msg_.c_str());
        if (ImGui::Button("Retry")) try_connect();
        ImGui::End();
        return;
    }

    // Summary
    ImGui::Text("Tensors: %zu | Total Memory: %s",
                tensors_.size(), human_bytes(total_bytes_).c_str());

    // Filter
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Filter by name...", filter_buf_, sizeof(filter_buf_));
    filter_ = filter_buf_;

    ImGui::Separator();

    // Tensor table
    if (ImGui::BeginTable("##tensor_table", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
            ImVec2(0, ImGui::GetContentRegionAvail().y - 120))) {

        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Shape");
        ImGui::TableSetupColumn("DType");
        ImGui::TableSetupColumn("Device");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Grad");
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(tensors_.size()); ++i) {
            auto& t = tensors_[i];
            // Apply filter
            if (!filter_.empty() && t.name.find(filter_) == std::string::npos) continue;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool sel = (selected_ == i);
            if (ImGui::Selectable(t.name.c_str(), sel,
                    ImGuiSelectableFlags_SpanAllColumns)) {
                selected_ = i;
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(shape_str(t.shape).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(dtype_str(t.dtype));
            ImGui::TableNextColumn();
            ImGui::Text("%s:%d", device_str(t.device), t.device_id);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(human_bytes(t.nbytes).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(t.requires_grad ? "yes" : "no");
        }
        ImGui::EndTable();
    }

    // Detail panel for selected tensor
    if (selected_ >= 0 && selected_ < static_cast<int>(tensors_.size())) {
        ImGui::Separator();
        auto& t = tensors_[selected_];
        ImGui::Text("Detail: %s", t.name.c_str());
        ImGui::Text("  Layout: %s", t.layout.c_str());
        if (!t.strides.empty()) {
            ImGui::Text("  Strides: %s", shape_str(t.strides).c_str());
        }
        int64_t numel = 1;
        for (auto s : t.shape) numel *= s;
        ImGui::Text("  Elements: %lld", static_cast<long long>(numel));
    }

    ImGui::End();
}

} // namespace straylight::widgets
