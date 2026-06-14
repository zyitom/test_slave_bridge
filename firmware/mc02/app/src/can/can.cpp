#include "firmware/mc02/app/src/can/can.hpp"

#include <fdcan.h>

#include "core/include/librmcs/data/datas.hpp"
#include "firmware/mc02/app/src/usb/helper.hpp"

namespace librmcs::firmware::can {

extern "C" void HAL_FDCAN_RxFifo0Callback(
    FDCAN_HandleTypeDef* hfdcan, uint32_t rx_fifo0_its) {
    (void)rx_fifo0_its;

    Can* can;
    data::DataId field_id;

    if (hfdcan == &hfdcan1) {
        can = can1.get();
        field_id = data::DataId::kCan1;
    } else if (hfdcan == &hfdcan2) {
        can = can2.get();
        field_id = data::DataId::kCan2;
    } else if (hfdcan == &hfdcan3) {
        can = can3.get();
        field_id = data::DataId::kCan3;
    } else {
        return;
    }

    can->handle_uplink(field_id, usb::get_serializer());
}

} // namespace librmcs::firmware::can
