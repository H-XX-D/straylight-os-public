// tests/unit/subsystems/test_core.cpp
#include <gtest/gtest.h>
#include "pipeline.h"
#include "doctor.h"
#include "core_daemon.h"

using namespace straylight;

TEST(Pipeline, SubsystemRegistration) {
    Pipeline pipeline;
    pipeline.register_subsystem("straylight-bus",      SubsystemPriority::Critical);
    pipeline.register_subsystem("straylight-registry", SubsystemPriority::Critical);
    pipeline.register_subsystem("straylight-entropy",  SubsystemPriority::Critical);
    pipeline.register_subsystem("straylight-agent",    SubsystemPriority::Normal);

    EXPECT_EQ(pipeline.subsystem_count(), 4u);
    EXPECT_EQ(pipeline.critical_count(), 3u);
}

TEST(Doctor, ReportsHealthyWhenAllUp) {
    Doctor doctor;
    doctor.record_health("straylight-bus",      HealthStatus::Healthy);
    doctor.record_health("straylight-registry", HealthStatus::Healthy);
    EXPECT_TRUE(doctor.all_healthy());
    EXPECT_EQ(doctor.unhealthy_count(), 0u);
}

TEST(Doctor, DetectsUnhealthy) {
    Doctor doctor;
    doctor.record_health("straylight-bus",      HealthStatus::Healthy);
    doctor.record_health("straylight-registry", HealthStatus::Degraded);
    EXPECT_FALSE(doctor.all_healthy());
    EXPECT_EQ(doctor.unhealthy_count(), 1u);
}

TEST(Doctor, RestartThresholdTracking) {
    Doctor doctor;
    for (int i = 0; i < 3; ++i)
        doctor.record_health("straylight-entropy", HealthStatus::Failed);
    EXPECT_TRUE(doctor.needs_restart("straylight-entropy"));
}

TEST(CoreDaemon, StartupOrderIsEnforced) {
    CoreDaemon core;
    core.register_subsystem("straylight-bus",      SubsystemPriority::Critical);
    core.register_subsystem("straylight-registry", SubsystemPriority::Critical);

    EXPECT_FALSE(core.is_ready());

    core.on_health_update("straylight-bus",      HealthStatus::Healthy);
    EXPECT_FALSE(core.is_ready());

    core.on_health_update("straylight-registry", HealthStatus::Healthy);
    EXPECT_TRUE(core.is_ready());
}
