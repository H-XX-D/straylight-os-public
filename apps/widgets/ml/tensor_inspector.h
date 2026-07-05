// apps/widgets/ml/tensor_inspector.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <straylight/types.h>
#include <string>
#include <vector>

namespace straylight::widgets {

struct TensorEntry {
    std::string name;
    std::vector<int64_t> shape;
    DType dtype = DType::Float32;
    DeviceType device = DeviceType::CPU;
    int device_id = 0;
    size_t nbytes = 0;
    bool requires_grad = false;
    std::string layout; // "contiguous", "strided", "sparse_coo", etc.
    std::vector<int64_t> strides;
};

class TensorInspectorWidget : public WidgetBase {
public:
    const char* name() const override { return "Tensor Inspector"; }
    float poll_interval() const override { return 1.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<TensorEntry> tensors_;
    std::string filter_;
    char filter_buf_[256]{};
    int selected_ = -1;
    std::string error_msg_;
    size_t total_bytes_ = 0;

    void try_connect();
    void fetch_tensors();
    static const char* dtype_str(DType dt);
    static const char* device_str(DeviceType dt);
    static std::string shape_str(const std::vector<int64_t>& s);
    static std::string human_bytes(size_t bytes);
};

} // namespace straylight::widgets
