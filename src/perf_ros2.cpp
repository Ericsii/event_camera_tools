// -*-c++-*--------------------------------------------------------------------
// Copyright 2022 Bernd Pfrommer <bernd.pfrommer@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <event_array_codecs/decoder_factory.h>
#include <event_array_codecs/event_processor.h>
#include <unistd.h>

#include <chrono>
#include <event_array_msgs/msg/event_array.hpp>
#include <fstream>
#include <rclcpp/rclcpp.hpp>
#include <unordered_map>

void usage()
{
  std::cout << "usage:" << std::endl;
  std::cout << "perf <ros_topic>" << std::endl;
}

using event_array_codecs::Decoder;
using event_array_msgs::msg::EventArray;

class Perf : public rclcpp::Node, public event_array_codecs::EventProcessor
{
public:
  explicit Perf(const std::string & topic, const rclcpp::NodeOptions & options)
  : Node("perf", options)
  {
    const int qsize = 1000;
    auto qos = rclcpp::QoS(rclcpp::KeepLast(qsize)).best_effort().durability_volatile();
    RCLCPP_INFO_STREAM(this->get_logger(), "subscribing to " << topic);
    sub_ = this->create_subscription<EventArray>(
      topic, qos, std::bind(&Perf::eventMsg, this, std::placeholders::_1));
    timer_ = rclcpp::create_timer(
      this, this->get_clock(), rclcpp::Duration::from_seconds(2.0),
      [=]() { this->timerExpired(); });
    lastTime_ = this->get_clock()->now();
  }
  // ---------- from the EventProcessor interface:
  void eventCD(uint64_t sensor_time, uint16_t, uint16_t, uint8_t p) override
  {
    lastSensorTime_ = sensor_time;
    cdEvents_[std::min(uint8_t(1), p)]++;
  }
  void eventExtTrigger(uint64_t sensor_time, uint8_t edge, uint8_t /*id*/) override
  {
    lastSensorTime_ = sensor_time;
    trEvents_[std::min(uint8_t(1), edge)]++;
  }
  void finished() override {}
  void rawData(const char *, size_t) override {}
  // -------- end of inherited
  void eventMsg(EventArray::ConstSharedPtr msg)
  {
    auto decoder = decoderFactory_.getInstance(msg->encoding);
    if (!decoder) {
      printf("unsupported encoding: %s\n", msg->encoding.c_str());
      return;
    }
    decoder->setTimeBase(msg->time_base);
    decoder->decode(&msg->events[0], msg->events.size(), this);
    numMsgs_++;
    if (lastSeq_ == 0) {
      lastSeq_ = msg->seq - 1;
    }
    numSeqDropped_ += (msg->seq - 1 - lastSeq_);
    lastSeq_ = msg->seq;
    const auto t = this->get_clock()->now();
    const auto t_stamp = rclcpp::Time(msg->header.stamp);
    delay_ += (t - t_stamp).nanoseconds();
    if (previousSensorTime_ != 0) {
      const int64_t dd = (int64_t(lastSensorTime_) - int64_t(previousSensorTime_)) -
                         (int64_t(t_stamp.nanoseconds()) - int64_t(previousStamp_));
      drift_ += dd;
    }
    previousSensorTime_ = lastSensorTime_;
    previousStamp_ = t_stamp.nanoseconds();
  }

  void timerExpired()
  {
    const auto t = this->get_clock()->now();
    const double dt = (t - lastTime_).nanoseconds() * 1e-9;  // in milliseconds
    if (numMsgs_ != 0) {
      printf(
        "msgs: %8.2f/s drp: %2zu del: %5.2fms drft: %6.4fs", numMsgs_ / dt, numSeqDropped_,
        delay_ / (1e6 * numMsgs_), drift_ * 1e-9);
      const size_t cdTot = cdEvents_[0] + cdEvents_[1];
      if (cdTot > 0) {
        printf(" ev: %8.4f M/s %%ON: %3zu", cdTot / dt * 1e-6, (cdEvents_[1] * 100) / cdTot);
      }
      const size_t trTot = trEvents_[0] + trEvents_[1];
      if (trTot > 0) {
        printf(" tr: %8.2f 1/s %%UP: %3zu\n", trTot / dt, (trEvents_[1] * 100) / trTot);
      } else {
        printf("\n");
      }
    } else {
      printf("no messages received ...\n");
    }
    cdEvents_[0] = cdEvents_[1] = 0;
    trEvents_[0] = trEvents_[1] = 0;
    numMsgs_ = 0;
    numSeqDropped_ = 0;
    delay_ = 0;
    lastTime_ = t;
  }
  // ---------- variables
  rclcpp::Subscription<EventArray>::SharedPtr sub_;
  event_array_codecs::DecoderFactory<Perf> decoderFactory_;
  rclcpp::TimerBase::SharedPtr timer_;
  size_t numMsgs_{0};
  size_t cdEvents_[2]{0, 0};  // contrast change detected
  size_t trEvents_[2]{0, 0};  // trigger

  rclcpp::Time lastTime_;
  uint64_t lastSeq_{0};
  uint64_t numSeqDropped_{0};
  uint64_t lastSensorTime_{0};
  uint64_t previousSensorTime_{0};
  uint64_t previousStamp_{0};
  int64_t delay_{0};
  int64_t drift_{0};
};

int main(int argc, char ** argv)
{
  std::string topic;
  rclcpp::init(argc, argv);
  switch (argc) {
    case 2:
      topic = argv[1];
      break;
    default:
      usage();
      exit(-1);
  }
  auto node = std::make_shared<Perf>(topic, rclcpp::NodeOptions());
  rclcpp::spin(node);  // should not return
  rclcpp::shutdown();
  return 0;
}
