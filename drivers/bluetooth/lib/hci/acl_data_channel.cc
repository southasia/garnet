// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "acl_data_channel.h"

#include <endian.h>
#include <magenta/status.h>

#include "lib/ftl/functional/make_copyable.h"
#include "lib/ftl/logging.h"
#include "lib/ftl/strings/string_printf.h"

#include "slab_allocators.h"
#include "transport.h"

namespace bluetooth {
namespace hci {

DataBufferInfo::DataBufferInfo(size_t max_data_length, size_t max_num_packets)
    : max_data_length_(max_data_length), max_num_packets_(max_num_packets) {
}

DataBufferInfo::DataBufferInfo() : max_data_length_(0u), max_num_packets_(0u) {}

bool DataBufferInfo::operator==(const DataBufferInfo& other) const {
  return max_data_length_ == other.max_data_length_ && max_num_packets_ == other.max_num_packets_;
}

ACLDataChannel::ACLDataChannel(Transport* transport, mx::channel hci_acl_channel)
    : transport_(transport),
      channel_(std::move(hci_acl_channel)),
      is_initialized_(false),
      event_handler_id_(0u),
      io_handler_key_(0u),
      num_sent_packets_(0u),
      le_num_sent_packets_(0u) {
  FTL_DCHECK(transport_), FTL_DCHECK(channel_.is_valid());
}

ACLDataChannel::~ACLDataChannel() {
  ShutDown();
}

void ACLDataChannel::Initialize(const DataBufferInfo& bredr_buffer_info,
                                const DataBufferInfo& le_buffer_info) {
  FTL_DCHECK(thread_checker_.IsCreationThreadCurrent());
  FTL_DCHECK(!is_initialized_);
  FTL_DCHECK(bredr_buffer_info.IsAvailable() || le_buffer_info.IsAvailable());

  bredr_buffer_info_ = bredr_buffer_info;
  le_buffer_info_ = le_buffer_info;

  // We make sure that this method blocks until the I/O handler registration task has run.
  std::mutex init_mutex;
  std::condition_variable init_cv;
  bool ready = false;

  io_task_runner_ = transport_->io_task_runner();
  io_task_runner_->PostTask([&] {
    // TODO(armansito): We'll need to pay attention to MX_CHANNEL_WRITABLE as well or perhaps not if
    // we switch to mx fifo.
    io_handler_key_ =
        mtl::MessageLoop::GetCurrent()->AddHandler(this, channel_.get(), MX_CHANNEL_READABLE);
    FTL_LOG(INFO) << "hci: ACLDataChannel: I/O handler registered";
    {
      std::lock_guard<std::mutex> lock(init_mutex);
      ready = true;
    }
    init_cv.notify_one();
  });

  std::unique_lock<std::mutex> lock(init_mutex);
  init_cv.wait(lock, [&ready] { return ready; });

  event_handler_id_ = transport_->command_channel()->AddEventHandler(
      kNumberOfCompletedPacketsEventCode,
      std::bind(&ACLDataChannel::NumberOfCompletedPacketsCallback, this, std::placeholders::_1),
      io_task_runner_);
  FTL_DCHECK(event_handler_id_);

  is_initialized_ = true;

  FTL_LOG(INFO) << "hci: ACLDataChannel: initialized";
}

void ACLDataChannel::ShutDown() {
  FTL_DCHECK(thread_checker_.IsCreationThreadCurrent());
  if (!is_initialized_) return;

  FTL_LOG(INFO) << "hci: ACLDataChannel: shutting down";

  io_task_runner_->PostTask([handler_key = io_handler_key_] {
    FTL_DCHECK(mtl::MessageLoop::GetCurrent());
    FTL_LOG(INFO) << "hci: ACLDataChannel Removing I/O handler";
    mtl::MessageLoop::GetCurrent()->RemoveHandler(handler_key);
  });

  transport_->command_channel()->RemoveEventHandler(event_handler_id_);

  is_initialized_ = false;

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_queue_.clear();
  }

  io_task_runner_ = nullptr;
  io_handler_key_ = 0u;
  event_handler_id_ = 0u;
  SetDataRxHandler(nullptr);
}

void ACLDataChannel::SetDataRxHandler(const DataReceivedCallback& rx_callback) {
  std::lock_guard<std::mutex> lock(rx_mutex_);
  rx_callback_ = rx_callback;
}

bool ACLDataChannel::SendPacket(std::unique_ptr<ACLDataPacket> data_packet,
                                Connection::LinkType ll_type) {
  if (!is_initialized_) {
    FTL_VLOG(1) << "hci: ACLDataChannel: Cannot send packets while uninitialized";
    return false;
  }

  FTL_DCHECK(data_packet);

  if (data_packet->view().payload_size() > GetBufferMTU(ll_type)) {
    FTL_LOG(ERROR) << "ACL data packet too large!";
    return false;
  }

  std::lock_guard<std::mutex> lock(send_mutex_);

  send_queue_.push_back(QueuedDataPacket(ll_type, std::move(data_packet)));

  TrySendNextQueuedPacketsLocked();

  return true;
}

const DataBufferInfo& ACLDataChannel::GetBufferInfo() const {
  return bredr_buffer_info_;
}

const DataBufferInfo& ACLDataChannel::GetLEBufferInfo() const {
  return le_buffer_info_.IsAvailable() ? le_buffer_info_ : bredr_buffer_info_;
}

size_t ACLDataChannel::GetBufferMTU(Connection::LinkType ll_type) const {
  if (ll_type == Connection::LinkType::kACL) return bredr_buffer_info_.max_data_length();
  return GetLEBufferInfo().max_data_length();
}

void ACLDataChannel::NumberOfCompletedPacketsCallback(const EventPacket& event) {
  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(event.event_code() == kNumberOfCompletedPacketsEventCode);

  const auto& payload = event.view().payload<NumberOfCompletedPacketsEventParams>();
  size_t total_comp_packets = 0;
  size_t le_total_comp_packets = 0;

  std::lock_guard<std::mutex> lock(send_mutex_);

  for (uint8_t i = 0; i < payload.number_of_handles; ++i) {
    const NumberOfCompletedPacketsEventData* data = payload.data + i;

    auto iter = pending_links_.find(le16toh(data->connection_handle));
    if (iter == pending_links_.end()) {
      FTL_LOG(WARNING) << "Controller reported sent packets on unknown connection handle!";
      continue;
    }

    uint16_t comp_packets = le16toh(data->hc_num_of_completed_packets);

    FTL_DCHECK(iter->second.count);
    if (iter->second.count < comp_packets) {
      FTL_LOG(WARNING) << ftl::StringPrintf(
          "Packet tx count mismatch! (handle: 0x%04x, expected: %zu, actual : %u)",
          le16toh(data->connection_handle), iter->second.count, comp_packets);
      iter->second.count = 0u;

      // On debug builds it's better to assert and crash so that we can catch controller bugs. On
      // release builds we log the warning message above and continue.
      FTL_NOTREACHED() << "Controller reported incorrect packet count!";
    } else {
      iter->second.count -= comp_packets;
    }

    if (iter->second.ll_type == Connection::LinkType::kACL) {
      total_comp_packets += comp_packets;
    } else {
      le_total_comp_packets += comp_packets;
    }

    if (!iter->second.count) {
      pending_links_.erase(iter);
    }
  }

  DecrementTotalNumPacketsLocked(total_comp_packets);
  DecrementLETotalNumPacketsLocked(le_total_comp_packets);
  TrySendNextQueuedPacketsLocked();
}

void ACLDataChannel::TrySendNextQueuedPacketsLocked() {
  if (!is_initialized_) return;

  size_t avail_bredr_packets = GetNumFreeBREDRPacketsLocked();
  size_t avail_le_packets = GetNumFreeLEPacketsLocked();

  // Based on what we know about controller buffer availability, we process packets that are
  // currently in |send_queue_|. The packets that can be sent are added to |to_send|. Packets that
  // cannot be sent remain in |send_queue_|.
  DataPacketQueue to_send;
  for (auto iter = send_queue_.begin(); iter != send_queue_.end();) {
    if (!avail_bredr_packets && !avail_le_packets) break;

    if (send_queue_.front().ll_type == Connection::LinkType::kACL && avail_bredr_packets) {
      --avail_bredr_packets;
    } else if (send_queue_.front().ll_type == Connection::LinkType::kLE && avail_le_packets) {
      --avail_le_packets;
    } else {
      // Cannot send packet yet, so skip it.
      ++iter;
      continue;
    }

    to_send.push_back(std::move(*iter));
    send_queue_.erase(iter++);
  }

  if (to_send.empty()) return;

  size_t bredr_packets_sent = 0;
  size_t le_packets_sent = 0;
  while (!to_send.empty()) {
    const QueuedDataPacket& packet = to_send.front();

    auto packet_bytes = packet.packet->view().data();
    mx_status_t status = channel_.write(0, packet_bytes.data(), packet_bytes.size(), nullptr, 0);
    if (status < 0) {
      FTL_LOG(ERROR) << "hci: ACLDataChannel: Failed to send data packet to HCI driver ("
                     << mx_status_get_string(status) << ") - dropping packet";
      to_send.pop_front();
      continue;
    }

    if (packet.ll_type == Connection::LinkType::kACL) {
      ++bredr_packets_sent;
    } else {
      ++le_packets_sent;
    }

    auto iter = pending_links_.find(packet.packet->connection_handle());
    if (iter == pending_links_.end()) {
      pending_links_[packet.packet->connection_handle()] = PendingPacketData(packet.ll_type);
    } else {
      iter->second.count++;
    }

    to_send.pop_front();
  }

  IncrementTotalNumPacketsLocked(bredr_packets_sent);
  IncrementLETotalNumPacketsLocked(le_packets_sent);
}

size_t ACLDataChannel::GetNumFreeBREDRPacketsLocked() const {
  FTL_DCHECK(bredr_buffer_info_.max_num_packets() >= num_sent_packets_);
  return bredr_buffer_info_.max_num_packets() - num_sent_packets_;
}

size_t ACLDataChannel::GetNumFreeLEPacketsLocked() const {
  if (!le_buffer_info_.IsAvailable()) return GetNumFreeBREDRPacketsLocked();

  FTL_DCHECK(le_buffer_info_.max_num_packets() >= le_num_sent_packets_);
  return le_buffer_info_.max_num_packets() - le_num_sent_packets_;
}

void ACLDataChannel::DecrementTotalNumPacketsLocked(size_t count) {
  FTL_DCHECK(num_sent_packets_ >= count);
  num_sent_packets_ -= count;
}

void ACLDataChannel::DecrementLETotalNumPacketsLocked(size_t count) {
  if (!le_buffer_info_.IsAvailable()) {
    DecrementTotalNumPacketsLocked(count);
    return;
  }

  FTL_DCHECK(le_num_sent_packets_ >= count);
  le_num_sent_packets_ -= count;
}

void ACLDataChannel::IncrementTotalNumPacketsLocked(size_t count) {
  FTL_DCHECK(num_sent_packets_ + count <= bredr_buffer_info_.max_num_packets());
  num_sent_packets_ += count;
}

void ACLDataChannel::IncrementLETotalNumPacketsLocked(size_t count) {
  if (!le_buffer_info_.IsAvailable()) {
    IncrementTotalNumPacketsLocked(count);
    return;
  }

  FTL_DCHECK(le_num_sent_packets_ + count <= le_buffer_info_.max_num_packets());
  le_num_sent_packets_ += count;
}

void ACLDataChannel::OnHandleReady(mx_handle_t handle, mx_signals_t pending, uint64_t count) {
  if (!is_initialized_) return;

  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(handle == channel_.get());
  FTL_DCHECK(pending & MX_CHANNEL_READABLE);

  std::lock_guard<std::mutex> lock(rx_mutex_);
  if (!rx_callback_) return;

  // Allocate a buffer for the event. Since we don't know the size beforehand we allocate the
  // largest possible buffer.
  auto packet = ACLDataPacket::New(slab_allocators::kLargeACLDataPayloadSize);
  if (!packet) {
    FTL_LOG(ERROR) << "Failed to allocate buffer received ACL data packet!";
    return;
  }

  uint32_t read_size;
  auto packet_bytes = packet->mutable_view()->mutable_data();
  mx_status_t status = channel_.read(0u, packet_bytes.mutable_data(), packet_bytes.size(),
                                     &read_size, nullptr, 0, nullptr);
  if (status < 0) {
    FTL_VLOG(1) << "hci: ACLDataChannel: Failed to read RX bytes: " << mx_status_get_string(status);
    // Clear the handler so that we stop receiving events from it.
    mtl::MessageLoop::GetCurrent()->RemoveHandler(io_handler_key_);
    return;
  }

  if (read_size < sizeof(ACLDataHeader)) {
    FTL_LOG(ERROR) << "hci: ACLDataChannel: Malformed data packet - "
                   << "expected at least " << sizeof(ACLDataHeader) << " bytes, "
                   << "got " << read_size;
    return;
  }

  const size_t rx_payload_size = read_size - sizeof(ACLDataHeader);
  const size_t size_from_header = le16toh(packet->view().header().data_total_length);
  if (size_from_header != rx_payload_size) {
    FTL_LOG(ERROR) << "hci: ACLDataChannel: Malformed packet - "
                   << "payload size from header (" << size_from_header << ")"
                   << " does not match received payload size: " << rx_payload_size;
    return;
  }

  packet->InitializeFromBuffer();

  rx_callback_(std::move(packet));
}

void ACLDataChannel::OnHandleError(mx_handle_t handle, mx_status_t error) {
  FTL_DCHECK(io_task_runner_->RunsTasksOnCurrentThread());
  FTL_DCHECK(handle == channel_.get());

  FTL_LOG(ERROR) << "hci: ACLDataChannel: channel error: " << mx_status_get_string(error);

  // Clear the handler so that we stop receiving events from it.
  mtl::MessageLoop::GetCurrent()->RemoveHandler(io_handler_key_);
}

}  // namespace hci
}  // namespace bluetooth
