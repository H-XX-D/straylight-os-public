// services/nerve/irq_monitor.cpp
#include "irq_monitor.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace straylight {

IrqMonitor::IrqMonitor()
    : prev_time_(std::chrono::steady_clock::now()) {}

Result<void, std::string> IrqMonitor::sample() {
    std::ifstream proc_ints("/proc/interrupts");
    if (!proc_ints.is_open()) {
        return Result<void, std::string>::error("Cannot open /proc/interrupts");
    }

    auto now = std::chrono::steady_clock::now();

    // Parse header for CPU count
    std::string header;
    std::getline(proc_ints, header);

    uint32_t num_cpus = 0;
    {
        std::istringstream hss(header);
        std::string tok;
        while (hss >> tok) {
            if (tok.rfind("CPU", 0) == 0) ++num_cpus;
        }
    }

    if (num_cpus == 0) {
        num_cpus = static_cast<uint32_t>(::sysconf(_SC_NPROCESSORS_ONLN));
    }

    // Temporary CPU totals for this sample
    std::map<uint32_t, uint64_t> new_cpu_totals;
    for (uint32_t i = 0; i < num_cpus; ++i) {
        new_cpu_totals[i] = 0;
    }

    std::map<uint32_t, uint64_t> new_counts;
    std::map<uint32_t, std::string> new_device_names;

    std::string line;
    while (std::getline(proc_ints, line)) {
        std::istringstream iss(line);
        std::string irq_str;
        iss >> irq_str;

        // Remove trailing ':'
        if (!irq_str.empty() && irq_str.back() == ':') irq_str.pop_back();

        // Skip non-numeric
        bool is_num = true;
        for (char c : irq_str) {
            if (!std::isdigit(c)) { is_num = false; break; }
        }
        if (!is_num || irq_str.empty()) continue;

        uint32_t irq_num = static_cast<uint32_t>(std::stoul(irq_str));
        uint64_t total = 0;

        for (uint32_t cpu = 0; cpu < num_cpus; ++cpu) {
            uint64_t count = 0;
            if (iss >> count) {
                total += count;
                new_cpu_totals[cpu] += count;
            }
        }

        new_counts[irq_num] = total;

        // Parse device name from remaining tokens
        std::string token;
        std::vector<std::string> remaining;
        while (iss >> token) {
            remaining.push_back(token);
        }
        if (!remaining.empty()) {
            // Last token is typically the device name
            std::string dev;
            for (size_t i = 0; i < remaining.size(); ++i) {
                const auto& tok = remaining[i];
                if (tok.find("PCI") != std::string::npos ||
                    tok.find("APIC") != std::string::npos ||
                    tok.find("IR-") == 0 ||
                    tok.find("IO-") == 0 ||
                    tok.find("edge") != std::string::npos ||
                    tok.find("fasteoi") != std::string::npos) {
                    continue;
                }
                if (!dev.empty()) dev += " ";
                dev += tok;
            }
            if (!dev.empty()) {
                new_device_names[irq_num] = dev;
            }
        }
    }

    std::lock_guard lock(mu_);

    cpu_totals_ = new_cpu_totals;

    if (!initialized_) {
        prev_counts_ = new_counts;
        device_names_ = new_device_names;
        prev_time_ = now;
        initialized_ = true;
        return Result<void, std::string>::ok();
    }

    // Compute rates
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - prev_time_);
    double seconds = static_cast<double>(elapsed.count()) / 1000000.0;
    if (seconds <= 0.0) seconds = 1.0;

    current_rates_.clear();

    for (const auto& [irq, count] : new_counts) {
        uint64_t prev = 0;
        auto it = prev_counts_.find(irq);
        if (it != prev_counts_.end()) {
            prev = it->second;
        }

        uint64_t delta = (count >= prev) ? (count - prev) : count;
        double rate = static_cast<double>(delta) / seconds;
        current_rates_[irq] = rate;

        // Store in history
        IrqSample sample;
        sample.timestamp = now;
        sample.count = count;
        sample.rate_per_second = rate;

        auto& hist = history_[irq];
        hist.push_back(sample);
        if (hist.size() > max_history_) {
            hist.erase(hist.begin());
        }
    }

    prev_counts_ = new_counts;
    device_names_.insert(new_device_names.begin(), new_device_names.end());
    prev_time_ = now;

    detect_anomalies();

    return Result<void, std::string>::ok();
}

void IrqMonitor::detect_anomalies() {
    // Must be called with mu_ held

    // Storm detection
    for (const auto& [irq, rate] : current_rates_) {
        if (rate > static_cast<double>(storm_threshold_)) {
            std::string dev = "unknown";
            auto it = device_names_.find(irq);
            if (it != device_names_.end()) dev = it->second;

            std::ostringstream msg;
            msg << "IRQ storm detected: IRQ " << irq << " (" << dev << ") at "
                << static_cast<uint64_t>(rate) << " ints/sec (threshold: "
                << storm_threshold_ << ")";

            add_alert(irq, dev, msg.str(), IrqAlert::Severity::Critical);
        }
    }

    // Imbalance detection
    if (cpu_totals_.size() > 1) {
        uint64_t max_total = 0;
        uint64_t min_total = UINT64_MAX;
        for (const auto& [cpu, total] : cpu_totals_) {
            max_total = std::max(max_total, total);
            min_total = std::min(min_total, total);
        }

        if (min_total > 0) {
            double ratio = static_cast<double>(max_total) / static_cast<double>(min_total);
            if (ratio > imbalance_threshold_) {
                std::ostringstream msg;
                msg << "Severe interrupt imbalance: ratio " << ratio
                    << "x (threshold: " << imbalance_threshold_ << "x)";
                add_alert(0, "system", msg.str(), IrqAlert::Severity::Warning);
            }
        }
    }
}

void IrqMonitor::add_alert(uint32_t irq, const std::string& device,
                            const std::string& message, IrqAlert::Severity severity) {
    const auto now = std::chrono::system_clock::now();
    const auto suppress_window = std::chrono::seconds(30);
    const std::string alert_key = std::to_string(irq) + "|" + device + "|" +
        std::to_string(static_cast<int>(severity));

    auto last_it = last_alert_times_.find(alert_key);
    if (last_it != last_alert_times_.end() &&
        now - last_it->second < suppress_window) {
        return;
    }
    last_alert_times_[alert_key] = now;

    if (last_alert_times_.size() > 2048) {
        for (auto it = last_alert_times_.begin(); it != last_alert_times_.end();) {
            if (now - it->second > std::chrono::minutes(5)) {
                it = last_alert_times_.erase(it);
            } else {
                ++it;
            }
        }
    }

    IrqAlert alert;
    alert.timestamp = now;
    alert.irq_number = irq;
    alert.device_name = device;
    alert.message = message;
    alert.severity = severity;

    // Deduplicate: don't add if same IRQ alerted in last 10 seconds
    for (auto it = alerts_.rbegin(); it != alerts_.rend(); ++it) {
        if (it->irq_number == irq) {
            auto age = now - it->timestamp;
            if (age < std::chrono::seconds(10)) {
                return; // Suppress duplicate
            }
            break;
        }
    }

    alerts_.push_back(std::move(alert));

    // Cap alerts at 1000
    if (alerts_.size() > 1000) {
        alerts_.erase(alerts_.begin(), alerts_.begin() + 500);
    }
}

Result<double, std::string> IrqMonitor::get_rate(uint32_t irq) const {
    std::lock_guard lock(mu_);
    auto it = current_rates_.find(irq);
    if (it == current_rates_.end()) {
        return Result<double, std::string>::error(
            "No rate data for IRQ " + std::to_string(irq));
    }
    return Result<double, std::string>::ok(it->second);
}

std::vector<std::pair<uint32_t, double>> IrqMonitor::get_rates_sorted() const {
    std::lock_guard lock(mu_);
    std::vector<std::pair<uint32_t, double>> rates(current_rates_.begin(), current_rates_.end());
    std::sort(rates.begin(), rates.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    return rates;
}

std::vector<IrqSample> IrqMonitor::get_history(uint32_t irq, size_t max_samples) const {
    std::lock_guard lock(mu_);
    auto it = history_.find(irq);
    if (it == history_.end()) return {};

    const auto& hist = it->second;
    if (hist.size() <= max_samples) return hist;

    return std::vector<IrqSample>(hist.end() - static_cast<ptrdiff_t>(max_samples), hist.end());
}

std::vector<IrqAlert> IrqMonitor::get_and_clear_alerts() {
    std::lock_guard lock(mu_);
    std::vector<IrqAlert> result;
    result.swap(alerts_);
    return result;
}

std::vector<IrqAlert> IrqMonitor::get_alerts() const {
    std::lock_guard lock(mu_);
    return alerts_;
}

bool IrqMonitor::is_storm_detected() const {
    std::lock_guard lock(mu_);
    for (const auto& [irq, rate] : current_rates_) {
        if (rate > static_cast<double>(storm_threshold_)) return true;
    }
    return false;
}

std::map<uint32_t, double> IrqMonitor::get_cpu_distribution() const {
    std::lock_guard lock(mu_);
    std::map<uint32_t, double> result;

    uint64_t total = 0;
    for (const auto& [cpu, count] : cpu_totals_) {
        total += count;
    }

    if (total == 0) return result;

    for (const auto& [cpu, count] : cpu_totals_) {
        result[cpu] = static_cast<double>(count) / static_cast<double>(total) * 100.0;
    }

    return result;
}

void IrqMonitor::set_storm_threshold(uint64_t threshold) {
    std::lock_guard lock(mu_);
    storm_threshold_ = threshold;
}

void IrqMonitor::set_imbalance_threshold(double ratio) {
    std::lock_guard lock(mu_);
    imbalance_threshold_ = ratio;
}

void IrqMonitor::set_max_history(size_t max) {
    std::lock_guard lock(mu_);
    max_history_ = max;
}

} // namespace straylight
