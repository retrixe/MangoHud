#include <regex>
#include "gpu_fdinfo.h"
namespace fs = ghc::filesystem;

void GPU_fdinfo::find_fd()
{
    auto path = fs::path("/proc/self/fdinfo");

    if (!fs::exists(path)) {
        SPDLOG_DEBUG("{} does not exist", path.string());
        return;
    }

    // Here we store client-ids, if ids match, we dont open this file,
    // because it will have same readings and it becomes a duplicate
    std::set<std::string> client_ids;
    int total = 0;

    for (const auto& entry : fs::directory_iterator(path)) {
        auto fd_path = entry.path().string();
        auto file = std::ifstream(fd_path);

        if (!file.is_open())
            continue;

        std::string driver, pdev, client_id;

        for (std::string line; std::getline(file, line);) {
            auto key = line.substr(0, line.find(":"));
            auto val = line.substr(key.length() + 2);

            if (key == "drm-driver")
                driver = val;
            else if (key == "drm-pdev")
                pdev = val;
            else if (key == "drm-client-id")
                client_id = val;
        }

        if (!driver.empty() && driver == module) {
            total++;
            SPDLOG_DEBUG(
                "driver = \"{}\", pdev = \"{}\", client_id = \"{}\", client_id_exists = \"{}\"",
                driver, pdev, client_id, client_ids.find(client_id) != client_ids.end()
            );
        }

        if (
            driver.empty() || pdev.empty() || client_id.empty() ||
            driver != module || pdev != pci_dev ||
            client_ids.find(client_id) != client_ids.end()
        )
            continue;

        client_ids.insert(client_id);
        open_fdinfo_fd(fd_path);
    }

    SPDLOG_DEBUG("Found {} total fds. Opened {} unique fds.", total, fdinfo.size());
}

void GPU_fdinfo::open_fdinfo_fd(std::string path) {
    fdinfo.push_back(std::ifstream(path));
    fdinfo_data.push_back({});
}

void GPU_fdinfo::gather_fdinfo_data() {
    for (size_t i = 0; i < fdinfo.size(); i++) {
        fdinfo[i].clear();
        fdinfo[i].seekg(0);

        for (std::string line; std::getline(fdinfo[i], line);) {
            auto key = line.substr(0, line.find(":"));
            auto val = line.substr(key.length() + 2);
            fdinfo_data[i][key] = val;
        }
    }
}

uint64_t GPU_fdinfo::get_gpu_time()
{
    uint64_t total = 0;

    for (auto& fd : fdinfo_data) {
        auto time = fd[drm_engine_type];

        if (time.empty())
            continue;

        total += std::stoull(time);
    }

    return total;
}

float GPU_fdinfo::get_memory_used()
{
    uint64_t total = 0;

    for (auto& fd : fdinfo_data) {
        auto mem = fd[drm_memory_type];

        if (mem.empty())
            continue;

        total += std::stoull(mem);
    }

    // TODO: sometimes it's not KB, so add a check for that.
    return (float)total / 1024 / 1024;
}

std::regex voltage_regex("in(\\d+)_input");
std::regex fan_speed_regex("fan(\\d+)_input");
std::regex temp_regex("temp(\\d+)_input");
std::regex power_regex("power(\\d+)_input");
std::regex energy_regex("energy(\\d+)_input");

void GPU_fdinfo::find_hwmon()
{
    std::string device = "/sys/bus/pci/devices/";
    device += pci_dev;
    device += "/hwmon";

    if (!fs::exists(device)) {
        SPDLOG_DEBUG("hwmon directory {} doesn't exist.", device);
        return;
    }

    auto dir_iterator = fs::directory_iterator(device);
    auto hwmon = dir_iterator->path().string();

    if (hwmon.empty()) {
        SPDLOG_DEBUG("hwmon directory is empty.");
        return;
    }

    std::string voltage, fan_speed, temp, power, energy;
    uint64_t voltage_id = 0, fan_speed_id = 0, temp_id = 0, power_id = 0, energy_id = 0;

    for (const auto &entry : fs::directory_iterator(hwmon)) {
        auto filename = entry.path().filename().string();

        std::smatch matches;
        if (std::regex_match(filename, matches, voltage_regex) && matches.size() == 2) {
            auto id = std::stoull(matches[1].str());
            if (voltage.empty() || id < voltage_id) {
                voltage = entry.path().string();
                voltage_id = id;
            }
        } else if (std::regex_match(filename, matches, fan_speed_regex) && matches.size() == 2) {
            auto id = std::stoull(matches[1].str());
            if (fan_speed.empty() || id < fan_speed_id) {
                fan_speed = entry.path().string();
                fan_speed_id = id;
            }
        } else if (std::regex_match(filename, matches, temp_regex) && matches.size() == 2) {
            auto id = std::stoull(matches[1].str());
            if (temp.empty() || id < temp_id) {
                temp = entry.path().string();
                temp_id = id;
            }
        } else if (std::regex_match(filename, matches, power_regex) && matches.size() == 2) {
            auto id = std::stoull(matches[1].str());
            if (power.empty() || id < power_id) {
                power = entry.path().string();
                power_id = id;
            }
        } else if (std::regex_match(filename, matches, energy_regex) && matches.size() == 2) {
            auto id = std::stoull(matches[1].str());
            if (energy.empty() || id < energy_id) {
                energy = entry.path().string();
                energy_id = id;
            }
        }
    }

    if (voltage.empty()) {
        SPDLOG_DEBUG("hwmon: Voltage reading not found at {}", hwmon);
    } else {
        SPDLOG_DEBUG("hwmon: Voltage reading found at {}", voltage);
        voltage_stream.open(voltage);

        if (!voltage_stream.good())
            SPDLOG_DEBUG("hwmon: failed to open voltage reading {}", voltage);
    }

    if (fan_speed.empty()) {
        SPDLOG_DEBUG("hwmon: Fan RPM reading not found at {}", hwmon);
    } else {
        SPDLOG_DEBUG("hwmon: Fan RPM reading found at {}", fan_speed);
        fan_speed_stream.open(fan_speed);

        if (!fan_speed_stream.good())
            SPDLOG_DEBUG("hwmon: failed to open fan RPM reading {}", fan_speed);
    }

    if (temp.empty()) {
        SPDLOG_DEBUG("hwmon: Temperature reading not found at {}", hwmon);
    } else {
        SPDLOG_DEBUG("hwmon: Temperature reading found at {}", temp);
        temp_stream.open(temp);

        if (!temp_stream.good())
            SPDLOG_DEBUG("hwmon: failed to open temperature reading {}", temp);
    }

    if (!power.empty()) {
        SPDLOG_DEBUG("hwmon: Power reading found at {}", power);
        power_stream.open(power);

        if (!power_stream.good())
            SPDLOG_DEBUG("hwmon: failed to open power reading {}", power);
    } else if (energy.empty()) {
        SPDLOG_DEBUG("hwmon: Energy reading not found at {}", hwmon);
    } else {
        SPDLOG_DEBUG("hwmon: Energy reading found at {}", energy);
        power_stream.open(energy);

        if (!power_stream.good())
            SPDLOG_DEBUG("hwmon: failed to open energy reading {}", energy);

        is_power_energy = true;
        // Initialize value for the first time, otherwise delta will be very large
        // and your gpu power usage will be like 1 million watts for a second.
        this->last_power = get_current_power();
    }
}

int GPU_fdinfo::get_current_voltage()
{
    if (!voltage_stream.is_open())
        return 0;

    std::string voltage_input_str;
    voltage_stream.seekg(0);
    std::getline(voltage_stream, voltage_input_str);

    if (voltage_input_str.empty())
        return 0;

    return std::stoull(voltage_input_str);
}

int GPU_fdinfo::get_current_fan_speed()
{
    if (!fan_speed_stream.is_open())
        return 0;

    std::string fan_speed_input_str;
    fan_speed_stream.seekg(0);
    std::getline(fan_speed_stream, fan_speed_input_str);

    if (fan_speed_input_str.empty())
        return 0;

    return std::stoull(fan_speed_input_str);
}

int GPU_fdinfo::get_current_temp()
{
    if (!temp_stream.is_open())
        return 0.f;

    std::string temp_input_str;
    uint64_t temp_input;

    temp_stream.seekg(0);
    std::getline(temp_stream, temp_input_str);

    if (temp_input_str.empty())
        return 0.f;

    temp_input = std::stoull(temp_input_str);

    return temp_input / 1000;
}

float GPU_fdinfo::get_current_power()
{
    if (!power_stream.is_open())
        return 0.f;

    std::string power_input_str;
    uint64_t power_input;

    power_stream.seekg(0);

    std::getline(power_stream, power_input_str);

    if (power_input_str.empty())
        return 0.f;

    power_input = std::stoull(power_input_str);

    return (float)power_input / 1'000'000;
}

float GPU_fdinfo::get_power_usage()
{
    if (!is_power_energy)
        return get_current_power();

    float now = get_current_power();
    float delta = now - this->last_power;
    delta /= (float)METRICS_UPDATE_PERIOD_MS / 1000;

    this->last_power = now;

    return delta;
}

int GPU_fdinfo::get_xe_load()
{
    double load = 0;

    for (auto& fd : fdinfo_data) {
        std::string client_id = fd["drm-client-id"];
        std::string cur_cycles_str = fd["drm-cycles-rcs"];
        std::string cur_total_cycles_str = fd["drm-total-cycles-rcs"];

        if (
            client_id.empty() || cur_cycles_str.empty() ||
            cur_total_cycles_str.empty()
        )
            continue;

        auto cur_cycles = std::stoull(cur_cycles_str);
        auto cur_total_cycles = std::stoull(cur_total_cycles_str);

        if (prev_xe_cycles.find(client_id) == prev_xe_cycles.end()) {
            prev_xe_cycles[client_id] = { cur_cycles, cur_total_cycles };
            continue;
        }

        auto prev_cycles = prev_xe_cycles[client_id].first;
        auto prev_total_cycles = prev_xe_cycles[client_id].second;

        auto delta_cycles = cur_cycles - prev_cycles;
        auto delta_total_cycles = cur_total_cycles - prev_total_cycles;

        prev_xe_cycles[client_id] = { cur_cycles, cur_total_cycles };

        if (delta_cycles <= 0 || delta_total_cycles <= 0)
            continue;

        auto fd_load = (double)delta_cycles / delta_total_cycles * 100;
        load += fd_load;
    }

    if (load > 100.f)
        load = 100.f;

    return std::lround(load);
}

int GPU_fdinfo::get_gpu_load()
{
    if (module == "xe")
        return get_xe_load();

    uint64_t now = os_time_get_nano();
    uint64_t gpu_time_now = get_gpu_time();

    float delta_time = now - previous_time;
    float delta_gpu_time = gpu_time_now - previous_gpu_time;

    int result = delta_gpu_time / delta_time * 100;

    if (result > 100)
        result = 100;

    previous_gpu_time = gpu_time_now;
    previous_time = now;

    return result;
}

void GPU_fdinfo::find_i915_gt_dir()
{
    std::string device = "/sys/bus/pci/devices/" + pci_dev + "/drm";

    // Find first dir which starts with name "card"
    for (const auto& entry : fs::directory_iterator(device)) {
        auto path = entry.path().string();

        if (path.substr(device.size() + 1, 4) == "card") {
            device = path;
            break;
        }
    }

    auto gpu_clock_path = device + "/gt_act_freq_mhz";
    gpu_clock_stream.open(gpu_clock_path);

    if (!gpu_clock_stream.good())
        SPDLOG_WARN("Intel i915 gt dir: failed to open {}", device);

    // Assuming gt0 since all recent GPUs have the RCS engine on gt0, and latest GPUs need Xe anyway
    auto throttle_folder = device + "/gt/gt0/throttle_";
    auto throttle_status_path = throttle_folder + "reason_status";

    throttle_status_stream.open(throttle_status_path);
    if (!throttle_status_stream.good()) {
       SPDLOG_WARN("Intel i915 gt dir: failed to open {}", throttle_status_path);
    } else {
        load_xe_i915_throttle_reasons(throttle_folder, intel_throttle_power, throttle_power_streams);
        load_xe_i915_throttle_reasons(throttle_folder, intel_throttle_current, throttle_current_streams);
        load_xe_i915_throttle_reasons(throttle_folder, intel_throttle_temp, throttle_temp_streams);
    }
}

void GPU_fdinfo::find_xe_gt_dir()
{
    std::string device = "/sys/bus/pci/devices/" + pci_dev + "/tile0";

    if (!fs::exists(device)) {
        SPDLOG_WARN(
            "\"{}\" doesn't exist. GPU clock will be unavailable.",
            device
        );
        return;
    }

    bool has_rcs = true;

    // Check every "gt" dir if it has "engines/rcs" inside
    for (const auto& entry : fs::directory_iterator(device)) {
        auto path = entry.path().string();

        if (path.substr(device.size() + 1, 2) != "gt")
            continue;

        SPDLOG_DEBUG("Checking \"{}\" for rcs.", path);

        if (!fs::exists(path + "/engines/rcs")) {
            SPDLOG_DEBUG("Skipping \"{}\" because rcs doesn't exist.", path);
            continue;
        }

        SPDLOG_DEBUG("Found rcs in \"{}\"", path);
        has_rcs = true;
        device = path;
        break;

    }

    if (!has_rcs) {
        SPDLOG_WARN("rcs not found inside \"{}\". GPU clock will not be available.", device);
        return;
    }

    auto gpu_clock_path = device + "/freq0/act_freq";
    gpu_clock_stream.open(gpu_clock_path);

    if (!gpu_clock_stream.good())
        SPDLOG_WARN("Intel xe gt dir: failed to open {}", gpu_clock_path);

    auto throttle_folder = device + "/freq0/throttle/";
    auto throttle_status_path = throttle_folder + "status";

    throttle_status_stream.open(throttle_status_path);
    if (!throttle_status_stream.good()) {
       SPDLOG_WARN("Intel xe gt dir: failed to open {}", throttle_status_path);
    } else {
        load_xe_i915_throttle_reasons(throttle_folder, intel_throttle_power, throttle_power_streams);
        load_xe_i915_throttle_reasons(throttle_folder, intel_throttle_current, throttle_current_streams);
        load_xe_i915_throttle_reasons(throttle_folder, intel_throttle_temp, throttle_temp_streams);
    }
}

void GPU_fdinfo::load_xe_i915_throttle_reasons(
    std::string throttle_folder,
    std::vector<std::string> throttle_reasons,
    std::vector<std::ifstream>& throttle_reason_streams
) {
    for (const auto& throttle_reason : throttle_reasons) {
        std::string throttle_path = throttle_folder + throttle_reason;
        if (!fs::exists(throttle_path)) {
            SPDLOG_WARN("Intel xe/i915 gt dir: Throttle file {} not found", throttle_path);
            continue;
        }
        auto throttle_stream = std::ifstream(throttle_path);
        if (!throttle_stream.good()) {
            SPDLOG_WARN("Intel xe/i915 gt dir: failed to open {}", throttle_path);
            continue;
        }
        throttle_reason_streams.push_back(std::move(throttle_stream));
    }
}

int GPU_fdinfo::get_gpu_clock()
{
    if (!gpu_clock_stream.is_open())
        return 0;

    std::string clock_str;

    gpu_clock_stream.seekg(0);

    std::getline(gpu_clock_stream, clock_str);

    if (clock_str.empty())
        return 0;

    return std::stoi(clock_str);
}

bool GPU_fdinfo::check_throttle_reasons(std::vector<std::ifstream>& throttle_reason_streams)
{
    for (auto& throttle_reason_stream : throttle_reason_streams) {
        std::string throttle_reason_str;
        throttle_reason_stream.seekg(0);
        std::getline(throttle_reason_stream, throttle_reason_str);

        if (throttle_reason_str == "1")
            return true;
    }

    return false;
}

GPU_throttle_status GPU_fdinfo::get_throttling_status()
{
    if (!throttle_status_stream.is_open())
        return GPU_throttle_status::NONE;

    std::string throttle_status_str;
    throttle_status_stream.seekg(0);
    std::getline(throttle_status_stream, throttle_status_str);

    if (throttle_status_str != "1")
        return GPU_throttle_status::NONE;
    else if (check_throttle_reasons(throttle_current_streams))
        return GPU_throttle_status::CURRENT; // Overcurrent is probably the worst possible outcome
    else if (check_throttle_reasons(throttle_temp_streams))
        return GPU_throttle_status::TEMP; // Temp is a more important throttle indicator than power
    else if (check_throttle_reasons(throttle_power_streams))
        return GPU_throttle_status::POWER;

    return GPU_throttle_status::OTHER;
}

void GPU_fdinfo::main_thread()
{
    while (!stop_thread) {
        std::unique_lock<std::mutex> lock(metrics_mutex);
        cond_var.wait(lock, [this]() { return !paused || stop_thread; });

        gather_fdinfo_data();

        metrics.load = get_gpu_load();
        metrics.temp = get_current_temp();
        metrics.memoryUsed = get_memory_used();
        metrics.powerUsage = get_power_usage();
        metrics.CoreClock = get_gpu_clock();
        auto throttling = get_throttling_status();
        metrics.is_power_throttled = throttling == GPU_throttle_status::POWER;
        metrics.is_current_throttled = throttling == GPU_throttle_status::CURRENT;
        metrics.is_temp_throttled = throttling == GPU_throttle_status::TEMP;
        metrics.is_other_throttled = throttling == GPU_throttle_status::OTHER;
        metrics.fan_speed = get_current_fan_speed();
        metrics.voltage = get_current_voltage();
        metrics.fan_rpm = true; // Fan data is pulled from hwmon

        SPDLOG_DEBUG(
            "pci_dev = {}, pid = {}, module = {}, load = {}, mem = {}, power = {}",
            pci_dev, pid, module, metrics.load, metrics.memoryUsed, metrics.powerUsage
        );

        std::this_thread::sleep_for(
            std::chrono::milliseconds(METRICS_UPDATE_PERIOD_MS)
        );
    }
}
