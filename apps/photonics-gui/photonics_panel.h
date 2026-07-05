#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <complex>
#include "photonics_engine.h"

namespace straylight::photonics {

struct MZI {
    int   id;
    float theta;
    float phi;
    float x, y;
    bool  active = true;
    float transmission;
};

struct PhotonicMesh {
    int rows = 4;
    int cols = 4;
    std::vector<MZI> mzis;
    std::string topology = "Clements";
    float wavelength_nm = 1550.0f;
    float insertion_loss_db = 0.5f;
    float crosstalk_db = -40.0f;
};

struct MatrixOp {
    std::string name;
    std::vector<std::vector<float>> matrix;
    float fidelity;
    float error_db;
    float snr_db;
};

struct DetectorState {
    int    id;
    float  power_dbm;
    float  snr_db;
    bool   saturated = false;
    float  history[128];
    int    history_offset;
};

struct PhotonicsState {
    PhotonicMesh mesh;
    std::vector<DetectorState> detectors;
    std::vector<MatrixOp> ops;
    int active_tab = 0;
    std::string synth_msg;        // status from the last Decompose & Program
    bool synth_ok = false;        // last synthesis hit target fidelity
    float heat_map[16];

    // Real simulation engine (MZI unitary mesh). No fabricated data.
    engine::EngineState eng;
    int   input_mode = 0;        // 0 = one-hot e0, 1 = uniform
    int   selected_target = 1;   // index into ops; default Identity
    double last_update_ = -1.0e9;
    bool  dirty_ = true;         // recompute requested (e.g. tuning)

    void init();
    void sync_from_engine();     // copy engine results into GUI struct fields
    void recompute();            // run engine then sync
    void maybe_update();         // called from render loop (health/flux pattern)
    void set_mzi(int grid_id, float theta, float phi);
    void program_target();       // synthesize selected target via the Solver tool
};

class PhotonicsPanel {
public:
    void init() { state_.init(); }
    void render();
private:
    PhotonicsState state_;
    void render_mesh_tab();
    void render_detectors_tab();
    void render_matrix_tab();
    void render_calibrate_tab();
};

} // namespace straylight::photonics
