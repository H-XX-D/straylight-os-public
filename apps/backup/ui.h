// apps/backup/ui.h
// BackupApp — ImGui panels for profile CRUD, schedules, and history
#pragma once

#include "engine.h"
#include "scheduler.h"

#include <future>
#include <string>

namespace straylight::backup {

class BackupApp {
public:
    int run(int argc, char* argv[]);

private:
    Engine                                  engine_;
    Scheduler                               scheduler_;
    std::vector<BackupProfile>              profiles_;
    int                                     sel_           = -1;
    float                                   progress_      = 0.f;
    std::string                             status_;
    std::future<Result<BackupRun, SLError>> active_;

    // Profile editing buffers
    char prof_name_[128]  = {};
    char prof_src_[1024]  = {};
    char prof_dst_[1024]  = {};
    char prof_excl_[512]  = {};

    // Schedule editing state
    int  sched_interval_  = 1;  // index into combo
    int  sched_hour_      = 2;
    int  sched_weekday_   = 0;
    int  sched_custom_s_  = 3600;
    bool sched_enabled_   = true;

    // Restore state
    int  restore_snap_idx_ = -1;
    char restore_dst_[1024] = {};

    void render_profiles();
    void render_schedule();
    void render_history();
    void load_profiles();
    void save_profiles() const;
    void apply_style();
};

} // namespace straylight::backup
