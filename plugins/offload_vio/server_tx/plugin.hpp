#pragma once

#include "illixr/data_format/imu.hpp"
#include "illixr/data_format/misc.hpp"
#include "illixr/data_format/poses/head_pose.hpp"
#include "illixr/diag_stats.hpp"
#include "illixr/network/tcpsocket.hpp"
#include "illixr/phonebook.hpp"
#include "illixr/plugin.hpp"
#include "illixr/switchboard.hpp"

#include <atomic>
#include <mutex>

namespace ILLIXR {
class server_writer : public plugin {
public:
    [[maybe_unused]] server_writer(const std::string& name, phonebook* pb);
    void start() override;
    void send_vio_output(const switchboard::ptr<const data_format::pose::head_pose_type>& datum);

private:
    const std::shared_ptr<switchboard>                                   switchboard_;
    switchboard::reader<data_format::imu_integrator_input>               imu_int_input_;
    switchboard::network_writer<switchboard::event_wrapper<std::string>> vio_pose_writer_;

    // ---- Diagnostics ----
    std::shared_ptr<spdlog::logger> diag_log_;
    std::atomic<uint64_t>           diag_slow_pose_{0}; // slow_pose callbacks (OpenVINS output)
    std::atomic<uint64_t>           diag_sent_{0};      // vio_pose messages put on the network writer
    std::atomic<uint64_t>           diag_no_imu_int_input_{0};
    std::mutex                      diag_mutex_;
    Eigen::Vector3f                 diag_last_pos_{0, 0, 0};
    diag::periodic_reporter         diag_reporter_;
};
} // namespace ILLIXR
