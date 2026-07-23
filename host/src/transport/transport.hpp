#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "core/src/protocol/constant.hpp"
#include "librmcs/board/common.hpp"
#include "librmcs/data/datas.hpp"

namespace librmcs::host::transport {

/**
 * @brief Buffer interface for transport operations.
 *
 * Buffers are acquired from Transport and must be returned to the same
 * transport instance through either transmit() or release_transmit_buffer().
 *
 * @section Ownership Rules:
 * - Buffers must only be destroyed by the transport that created them
 * - Destroying a buffer externally results in undefined behavior
 * - Passing a buffer to a different transport instance is undefined behavior
 *
 * @section Memory Guarantees:
 * - data() returns a memory region of exactly kProtocolBufferSize bytes
 * - data() always returns the same memory region for a given buffer instance
 */
class TransportBuffer {
public:
    using BufferSpanType = std::span<std::byte, core::protocol::kProtocolBufferSize>;

    TransportBuffer() = default;
    TransportBuffer(const TransportBuffer&) = delete;
    TransportBuffer& operator=(const TransportBuffer&) = delete;
    TransportBuffer(TransportBuffer&&) = delete;
    TransportBuffer& operator=(TransportBuffer&&) = delete;
    virtual ~TransportBuffer() noexcept = default;

    /**
     * @brief Returns a mutable view of the buffer's memory region.
     *
     * The returned span is guaranteed to be exactly kProtocolBufferSize bytes and remains
     * valid for the lifetime of this buffer object. Multiple calls to data()
     * must return a span pointing to the same underlying memory.
     */
    virtual BufferSpanType data() const noexcept = 0;

    // Optional cyclic CAN sideband used by the explicit EtherCAT hybrid API.
    // Ordinary USB/SOEM/stream buffers keep the default `false` behavior, so
    // their protocol bytes and reliable event semantics are unchanged.
    virtual bool try_stage_cyclic_can(
        data::DataId field_id, const data::CanDataView& view) noexcept {
        (void)field_id;
        (void)view;
        return false;
    }
    // Returns true when a cyclic builder is a strict fixed-PDO batch. Such a
    // builder must not spill fields into the reliable stream, because doing so
    // would lose batch ordering and deterministic completion semantics.
    virtual bool begin_cyclic_can_batch() noexcept { return false; }
    virtual void reject_cyclic_can_batch() noexcept {}
    virtual bool has_cyclic_data() const noexcept { return false; }
};

/**
 * @brief Transport interface for bidirectional data transmission.
 *
 * @section Buffer Lifecycle:
 * - Acquire buffers via acquire_transmit_buffer()
 * - Fill buffer with data using TransportBuffer::data()
 * - Either transmit the buffer via transmit() or return it via release_transmit_buffer()
 * - Never destroy buffers externally - let the transport manage their lifecycle
 *
 * @section Receive Semantics:
 * - receive() may only be called once during the transport's lifetime
 * - Once started, reception continues until the transport is destroyed
 * - The callback will be invoked for each received data packet
 *
 * @section Implementation Requirements:
 * - Implementations should detect and log errors when buffers are destroyed externally
 * - Buffer ownership violations should be treated as programming errors
 */
class Transport {
public:
    Transport() = default;
    virtual ~Transport() noexcept = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    /**
     * @brief Acquires a buffer for transmission.
     *
     * The returned buffer is owned by the caller and must be passed back to
     * this transport via either transmit() or release_transmit_buffer().
     *
     * @return A buffer with exactly kProtocolBufferSize bytes of writable memory, or nullptr
     *         if buffer acquisition fails (e.g., resource exhaustion)
     */
    virtual std::unique_ptr<TransportBuffer> acquire_transmit_buffer() noexcept = 0;

    /**
     * @brief Transmits data from the provided buffer.
     *
     * Takes ownership of the buffer and queues it for transmission. The buffer
     * must have been acquired from this transport instance via acquire_transmit_buffer().
     *
     * @param buffer Buffer containing the data to transmit (ownership transferred)
     * @param payload_size Number of bytes to transmit from the buffer (must not exceed buffer size)
     *
     * @section Preconditions:
     *   - buffer must be non-null and acquired from this transport
     *   - payload_size must be <= buffer capacity
     */
    virtual void transmit(std::unique_ptr<TransportBuffer> buffer, size_t payload_size) = 0;

    /**
     * @brief Returns an unused buffer back to the transport.
     *
     * Use this to release a buffer that was acquired but will not be transmitted.
     * Takes ownership of the buffer and returns it to the transport's buffer pool.
     *
     * @param buffer Buffer to release (ownership transferred)
     *
     * @section Preconditions:
     *   - buffer must be non-null and acquired from this transport
     *   - buffer must not have been previously transmitted or released
     */
    virtual void release_transmit_buffer(std::unique_ptr<TransportBuffer> buffer) = 0;

    /**
     * @brief Starts receiving data with the provided callback.
     *
     * This function may only be called once during the transport's lifetime.
     * Once called, the transport will invoke the callback for each received
     * data packet until the transport is destroyed.
     *
     * @param callback Function invoked for each received data packet.
     *                 The span is valid only for the duration of the callback.
     *
     * @section Preconditions:
     *   - Must be called at most once per transport instance
     *   - callback must be a valid callable object
     *
     * @section Thread Safety:
     *   The callback will be invoked from at most one thread at any given time.
     *   Concurrent invocations are guaranteed not to occur. However, the callback
     *   may be invoked from any thread, and different invocations may use different
     *   threads. Callers are responsible for thread synchronization if state is
     *   shared with other threads.
     */
    virtual void receive(std::function<void(std::span<const std::byte>)> callback) = 0;

    // Optional direct receive path paired with try_stage_cyclic_can(). The
    // callback is invoked on the same transport cycle thread as receive(), so a
    // hybrid transport never calls the Handler concurrently from two threads.
    virtual void receive_cyclic_can(
        std::function<void(data::DataId, const data::CanDataView&)> callback) {
        (void)callback;
    }

    // Optional transport-link restart notification. The callback runs on the
    // same receive thread and must not block it; Handler uses it to invalidate
    // the protocol session and lets its keepalive thread establish a new one.
    virtual void on_link_restart(std::function<void()> callback) { (void)callback; }
};

namespace usb {

using ConnectionOptions = board::AdvancedOptions;

std::unique_ptr<Transport> create_transport(
    uint16_t usb_vid, int32_t usb_pid, std::string_view serial_filter,
    const ConnectionOptions& options);

} // namespace usb

namespace soem {

using ConnectionOptions = board::AdvancedOptions;

/**
 * @brief EtherCAT transport over the rmcs_board stream bridge, using SOEM.
 *
 * Only available when the SDK is built with -DLIBRMCS_ENABLE_SOEM=ON (the
 * symbol is absent otherwise). interface_name is the raw network interface
 * connected to the slave (e.g. "enp2s0" on an i226 port); opening it requires
 * CAP_NET_RAW. The transport busy-polls process data on a dedicated thread;
 * use options.thread_setup to pin/prioritize that thread for the lowest
 * latency.
 */
std::unique_ptr<Transport>
    create_transport(std::string_view interface_name, const ConnectionOptions& options);

} // namespace soem

namespace igh {

using ConnectionOptions = board::AdvancedOptions;

/**
 * @brief EtherCAT transport over the rmcs_board stream bridge, using the IgH
 *        EtherCAT Master native driver stack.
 *
 * Only available when the SDK is built with -DLIBRMCS_ENABLE_IGH=ON (the symbol
 * is absent otherwise). Unlike the SOEM backend, the target device is selected
 * by the installed IgH master (ethercat.conf / master index 0), not by binding
 * an interface: interface_name is advisory (logged for parity). The IgH master
 * kernel module must own the NIC first (sudo ethercatctl start); the process
 * needs write access to /dev/EtherCAT0 (root by default). This backend bypasses
 * the socket/AF_XDP/BPF layer entirely and measures ~4x lower round-trip
 * latency than SOEM on the same hardware (see host/src/transport/igh/). The
 * transport busy-polls process data on a dedicated thread; use
 * options.thread_setup to pin/prioritize that thread for the lowest latency.
 */
std::unique_ptr<Transport>
    create_transport(std::string_view interface_name, const ConnectionOptions& options);

} // namespace igh

} // namespace librmcs::host::transport
