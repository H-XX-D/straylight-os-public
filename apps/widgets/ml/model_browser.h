// apps/widgets/ml/model_browser.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <string>
#include <vector>

namespace straylight::widgets {

struct LayerInfo {
    std::string name;
    std::string type;  // "Linear", "Conv2d", "LayerNorm", etc.
    int64_t param_count = 0;
    std::string shape_desc;
};

struct ModelInfo {
    std::string name;
    std::string path;
    std::string framework; // "pytorch", "onnx", "safetensors"
    int64_t total_params = 0;
    int64_t trainable_params = 0;
    size_t size_bytes = 0;
    std::vector<LayerInfo> layers;
    bool loaded = false;
};

class ModelBrowserWidget : public WidgetBase {
public:
    const char* name() const override { return "Model Browser"; }
    float poll_interval() const override { return 5.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<ModelInfo> models_;
    int selected_model_ = -1;
    std::string error_msg_;
    char filter_buf_[256]{};

    void try_connect();
    void fetch_models();
    static std::string human_params(int64_t count);
    static std::string human_bytes(size_t bytes);
};

} // namespace straylight::widgets
