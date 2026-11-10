#include "plugin.hpp"

#include "illixr/network/topic_config.hpp"
#include "video_encoder.hpp"

#include <cassert>
#include <opencv2/core/mat.hpp>

using namespace ILLIXR;
using namespace ILLIXR::data_format;

#define USE_COMPRESSION

[[maybe_unused]] offload_writer::offload_writer(const std::string& name, phonebook* pb)
    : threadloop{name, pb}
    , switchboard_{phonebook_->lookup_impl<switchboard>()}
    , clock_{phonebook_->lookup_impl<relative_clock>()}
    , stoplight_{phonebook_->lookup_impl<stoplight>()}
    , cam_{switchboard_->get_buffered_reader<binocular_cam_type>("cam")}
    , imu_cam_writer_{switchboard_->get_network_writer<switchboard::event_wrapper<std::string>>(
          "compressed_imu_cam",
          network::topic_config{network::topic_config::SerializationMethod::PROTOBUF,
                                network::parse_transport_method(switchboard_->get_env("ILLIXR_OFFLOAD_VIO_TRANSPORT"))})}
    , log_(spdlogger(switchboard_->get_env_char("OFFLOAD_VIO_LOG_LEVEL"))) {
    std::srand(std::time(0));
    diag_reporter_.start(std::chrono::milliseconds(1000), [this]() {
        log_->info("[DIAG][device_tx] imu={} cam_dequeued={} encoded={} sent={} bytes_sent={} blocked_in_encoder_wait_ms={} "
                   "frame_id={}",
                   diag_imu_.load(), diag_cam_dequeued_.load(), diag_encoded_.load(), diag_sent_.load(),
                   diag_bytes_sent_.load(), diag_waiting_ms_.load(), frame_id_);
    });
}

void offload_writer::start() {
    threadloop::start();

#ifdef USE_COMPRESSION
    encoder_ = std::make_unique<vio_video_encoder>([this](const GstMapInfo& img0, const GstMapInfo& img1) {
        queue_.consume_one([&](uint64_t& timestamp) {
            (void) timestamp;
            uint64_t curr =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
        });
        diag_encoded_++;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            this->img0_ = img0;
            this->img1_ = img1;
            img_ready_  = true;
        }
        condition_var_.notify_one();
    });
    encoder_->init();
#endif // USE_COMPRESSION

    switchboard_->schedule<imu_type>(id_, "imu", [this](const switchboard::ptr<const imu_type>& datum, std::size_t) {
        this->prepare_imu_cam_data(datum);
    });
}

void offload_writer::_p_one_iteration() {
    while (!stoplight_->check_should_stop()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void offload_writer::send_imu_cam_data(std::optional<time_point>& cam_time) {
    data_buffer_->set_real_timestamp(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    data_buffer_->set_frame_id(frame_id_);

    std::string data_to_be_sent = data_buffer_->SerializeAsString();
    std::string delimiter       = "EEND!";

    log_->info("{},{}", cam_time.value().time_since_epoch().count(),
               (double) (clock_->now().time_since_epoch().count() - cam_time.value().time_since_epoch().count()) / 1e6);
    // socket.write(data_to_be_sent + delimitter);
    diag_sent_++;
    diag_bytes_sent_ += data_to_be_sent.size() + delimiter.size();
    imu_cam_writer_.put(std::make_shared<switchboard::event_wrapper<std::string>>(data_to_be_sent + delimiter));

    frame_id_++;
    delete data_buffer_;
    data_buffer_ = new vio_input_proto::IMUCamVec();
    cam_time.reset();
}

void offload_writer::prepare_imu_cam_data(switchboard::ptr<const imu_type> datum) {
    // Ensures that slam doesnt start before valid IMU readings come in
    if (datum == nullptr) {
        assert(!latest_imu_time_);
        return;
    }

    diag_imu_++;

    // Ensure that IMU data is received in the time order
    assert(datum->time > latest_imu_time_);
    latest_imu_time_ = datum->time;

    vio_input_proto::IMUData* imu_data = data_buffer_->add_imu_data();
    imu_data->set_timestamp(datum->time.time_since_epoch().count());

    auto* angular_vel = new vio_input_proto::Vec3();
    angular_vel->set_x(datum->angular_v.x());
    angular_vel->set_y(datum->angular_v.y());
    angular_vel->set_z(datum->angular_v.z());
    imu_data->set_allocated_angular_vel(angular_vel);

    auto* linear_accel = new vio_input_proto::Vec3();
    linear_accel->set_x(datum->linear_a.x());
    linear_accel->set_y(datum->linear_a.y());
    linear_accel->set_z(datum->linear_a.z());
    imu_data->set_allocated_linear_accel(linear_accel);

    if (latest_cam_time_ && latest_imu_time_ > latest_cam_time_) {
        send_imu_cam_data(latest_cam_time_);
    }

    switchboard::ptr<const binocular_cam_type> cam;

    if (cam_.size() != 0 && !latest_cam_time_) {
        cam = cam_.dequeue();
        diag_cam_dequeued_++;

        cv::Mat cam_img0      = (cam->at(image::LEFT_EYE)).clone();
        cv::Mat cam_img1      = (cam->at(image::RIGHT_EYE)).clone();
        int     cam_img0_size = cam_img0.rows * cam_img0.cols;

        auto* cam_data = new vio_input_proto::CamData();
        cam_data->set_timestamp(cam->time.time_since_epoch().count());
        cam_data->set_rows(cam_img0.rows);
        cam_data->set_cols(cam_img0.cols);

#ifdef USE_COMPRESSION
        /** WITH COMPRESSION **/
        uint64_t curr =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        queue_.push(curr);
        std::unique_lock<std::mutex> lock{mutex_};
        encoder_->enqueue(cam_img0, cam_img1);
        // DIAG: same wait as before, but report every 2 s while blocked.
        {
            auto wait_start = std::chrono::steady_clock::now();
            while (!condition_var_.wait_for(lock, std::chrono::seconds(2), [this]() {
                return img_ready_;
            })) {
                auto waited =
                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start)
                        .count();
                diag_waiting_ms_ = static_cast<uint64_t>(waited);
                log_->error("[DIAG][device_tx] still waiting for encoder: {} ms, frame_id={} cam_dequeued={} encoded={}",
                            waited, frame_id_, diag_cam_dequeued_.load(), diag_encoded_.load());
            }
            diag_waiting_ms_ = 0;
        }
        img_ready_ = false;

        sizes_.push_back((int) this->img0_.size);

        // calculate average sizes
        // if (sizes_.size() > 100) {
        //    int32_t sum = 0;
        //    for (auto& s : sizes_) {
        //        sum += s;
        //    }
        // For debugging, prints out average image size after compression and compression ratio
        // std::cout << "compression ratio: " << img0_size / (sum / sizes_.size()) << " average size after compression "
        // << sum / sizes_.size() << std::endl;
        //}

        cam_data->set_img0_data((void*) this->img0_.data, this->img0_.size);
        cam_data->set_img1_data((void*) this->img1_.data, this->img1_.size);

        lock.unlock();
        /** WITH COMPRESSION END **/
#else
        /** NO COMPRESSION **/
        cam_data->set_img0_data((void*) cam_img0.data, cam_img0_size);
        cam_data->set_img1_data((void*) cam_img1.data, cam_img0_size);
        /** NO COMPRESSION END **/
#endif
        data_buffer_->set_allocated_cam_data(cam_data);
        latest_cam_time_ = cam->time;
        if (latest_imu_time_ <= latest_cam_time_) {
            return;
        } else {
            send_imu_cam_data(latest_cam_time_);
        }
    }
}

PLUGIN_MAIN(offload_writer)
