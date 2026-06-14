#include "board_app.hpp"

#if BOARD_HAS_BMI088

#include "firmware/rmcs_board/app/src/spi/spi.hpp"

#include "firmware/rmcs_board/app/src/spi/bmi088/service.hpp"

namespace librmcs::firmware::board {

void spi_bmi088_irq_handler() {
    spi::spi_bmi088->irq_handler();
    spi::bmi088::service_pending_reads();
}

} // namespace librmcs::firmware::board

#endif
