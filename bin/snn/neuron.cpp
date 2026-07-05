// bin/snn/neuron.cpp
#include "neuron.h"

namespace straylight::snn {

LIFNeuron::LIFNeuron(NeuronParams params)
    : params_(params), v_(params.v_rest), spiked_(false), ref_remaining_(0.0f) {}

void LIFNeuron::step(float dt, float i_ext) {
    spiked_ = false;

    // If in refractory period, count down and hold at reset voltage
    if (ref_remaining_ > 0.0f) {
        ref_remaining_ -= dt;
        v_ = params_.v_reset;
        return;
    }

    // Euler integration of LIF equation:
    // dV/dt = (-(V - V_rest) + R_m * I_ext) / tau_m
    float dv = (-(v_ - params_.v_rest) + params_.r_m * i_ext) / params_.tau_m;
    v_ += dv * dt;

    // Check for spike
    if (v_ >= params_.v_threshold) {
        spiked_ = true;
        v_ = params_.v_reset;
        ref_remaining_ = params_.tau_ref;
    }
}

bool LIFNeuron::spiked() const {
    return spiked_;
}

float LIFNeuron::voltage() const {
    return v_;
}

void LIFNeuron::reset() {
    v_ = params_.v_rest;
    spiked_ = false;
    ref_remaining_ = 0.0f;
}

const NeuronParams& LIFNeuron::params() const {
    return params_;
}

} // namespace straylight::snn
