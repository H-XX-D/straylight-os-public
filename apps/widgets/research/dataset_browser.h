// apps/widgets/research/dataset_browser.h
#pragma once

#include <straylight/widget.h>
#include <straylight/ipc_client.h>
#include <string>
#include <vector>

namespace straylight::widgets {

struct ColumnStats {
    std::string name;
    std::string dtype;
    float mean = 0.0f;
    float std_dev = 0.0f;
    float min_val = 0.0f;
    float max_val = 0.0f;
    int null_count = 0;
    int unique_count = 0;
    std::vector<float> histogram; // bucket counts
};

struct DatasetInfo {
    std::string name;
    std::string path;
    std::string format; // "csv", "parquet", "arrow", "jsonl"
    int64_t num_rows = 0;
    int64_t num_cols = 0;
    size_t size_bytes = 0;
    std::vector<ColumnStats> columns;
    std::vector<std::vector<std::string>> sample_rows; // first N rows as strings
};

class DatasetBrowserWidget : public WidgetBase {
public:
    const char* name() const override { return "Dataset Browser"; }
    float poll_interval() const override { return 10.0f; }
    void update() override;
    void render(bool* p_open) override;

private:
    IpcJsonClient ipc_;
    bool connected_ = false;
    std::vector<DatasetInfo> datasets_;
    int selected_ds_ = -1;
    int selected_col_ = -1;
    std::string error_msg_;
    char filter_buf_[128]{};
    int view_tab_ = 0; // 0=overview, 1=stats, 2=sample

    void try_connect();
    void fetch_datasets();
    static std::string human_bytes(size_t bytes);
};

} // namespace straylight::widgets
