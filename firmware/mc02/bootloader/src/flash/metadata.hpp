#pragma once

#include <cstdint>
#include <cstring>

#include <main.h>

#include "firmware/mc02/bootloader/src/flash/layout.hpp"
#include "firmware/mc02/bootloader/src/flash/unlock_guard.hpp"
#include "firmware/mc02/bootloader/src/utility/assert.hpp"

namespace librmcs::firmware::flash {

// STM32H7 flash word = 256 bits = 32 bytes; each DataSlot occupies exactly one flash word.
class Metadata {
public:
    static Metadata& get_instance() {
        static Metadata image_metadata;
        return image_metadata;
    }

    bool is_ready()    const { return latest_valid_slot_state_ == DataSlotState::kReady; }
    bool is_flashing() const { return latest_valid_slot_state_ == DataSlotState::kFlashing; }

    uint32_t image_size()  const { return latest_valid_slot_->image_size; }
    uint32_t image_crc32() const { return latest_valid_slot_->image_crc32; }

    void begin_flashing() {
        if (!latest_valid_slot_ || latest_valid_slot_state_ == DataSlotState::kFatal) {
            erase_and_rescan();
        } else if (latest_valid_slot_state_ == DataSlotState::kEmpty) {
            // Use current empty slot — already positioned
        } else {
            // kFlashing or kReady: advance to next slot.
            // Need room for 2 more slots (flashing marker + ready record).
            auto next_addr =
                reinterpret_cast<uintptr_t>(latest_valid_slot_) + sizeof(DataSlot);
            if (next_addr + sizeof(DataSlot) > kMetadataEndAddress)
                erase_and_rescan();
            else
                latest_valid_slot_ = reinterpret_cast<DataSlot*>(next_addr);
        }

        latest_valid_slot_->enter_flashing_state();
        latest_valid_slot_state_ = DataSlotState::kFlashing;
    }

    // On H7 we cannot modify a written flash word, so the ready record goes in
    // the NEXT slot after the flashing-marker slot.
    void finish_flashing(uint32_t size, uint32_t crc32) {
        utility::assert_debug(latest_valid_slot_state_ == DataSlotState::kFlashing);

        auto next_addr =
            reinterpret_cast<uintptr_t>(latest_valid_slot_) + sizeof(DataSlot);
        utility::assert_debug(next_addr < kMetadataEndAddress);

        auto* ready_slot = reinterpret_cast<DataSlot*>(next_addr);
        ready_slot->enter_ready_state(size, crc32);
        latest_valid_slot_ = ready_slot;
        latest_valid_slot_state_ = DataSlotState::kReady;
    }

private:
    Metadata() { scan_latest_valid_slot(); }

    static constexpr uintptr_t kMetadataStartAddress = 0x08020000U; // Sector 1
    static constexpr uintptr_t kMetadataEndAddress   = 0x08040000U;

    static constexpr uint32_t kFlashWordErased    = 0xFFFFFFFF;
    static constexpr uint32_t kImageMetadataMagic = 0x524D4353; // "RMCS"
    static constexpr uint32_t kImageStateReady    = 0x494D5244; // "IMRD"

    enum class DataSlotState : uint8_t { kFatal, kEmpty, kFlashing, kReady };

    // Exactly 32 bytes — one STM32H7 flash word.
    struct [[gnu::aligned(32)]] DataSlot {
        uint32_t magic;
        uint32_t image_state;
        uint32_t image_size;
        uint32_t image_crc32;
        uint8_t  pad[16];

        DataSlotState read_state() const {
            switch (magic) {
            case kFlashWordErased:
                if (image_state == kFlashWordErased && image_size == kFlashWordErased
                    && image_crc32 == kFlashWordErased)
                    return DataSlotState::kEmpty;
                return DataSlotState::kFatal;
            case kImageMetadataMagic:
                if (image_state == kFlashWordErased)
                    return DataSlotState::kFlashing;
                if (image_state == kImageStateReady && image_size <= kAppMaxImageSize)
                    return DataSlotState::kReady;
                return DataSlotState::kFatal;
            default:
                return DataSlotState::kFatal;
            }
        }

        void enter_flashing_state() {
            utility::assert_debug(read_state() == DataSlotState::kEmpty);
            write_flash_word(kImageMetadataMagic, kFlashWordErased, kFlashWordErased, kFlashWordErased);
            utility::assert_always(read_state() == DataSlotState::kFlashing);
        }

        void enter_ready_state(uint32_t size, uint32_t crc32) {
            utility::assert_debug(read_state() == DataSlotState::kEmpty);
            write_flash_word(kImageMetadataMagic, kImageStateReady, size, crc32);
            utility::assert_always(read_state() == DataSlotState::kReady);
        }

    private:
        void write_flash_word(
            uint32_t magic_val, uint32_t state_val, uint32_t size_val, uint32_t crc_val) {
            alignas(32) DataSlot buf{};
            buf.magic       = magic_val;
            buf.image_state = state_val;
            buf.image_size  = size_val;
            buf.image_crc32 = crc_val;
            std::memset(buf.pad, 0xFF, sizeof(buf.pad));

            const auto guard = UnlockGuard();
            utility::assert_always(
                HAL_FLASH_Program(
                    FLASH_TYPEPROGRAM_FLASHWORD,
                    reinterpret_cast<uintptr_t>(this),
                    reinterpret_cast<uint32_t>(&buf))
                == HAL_OK);
            // Bootloader runs cache-less (see main.cpp): flash reads are always
            // coherent, and issuing D-cache ops with the cache disabled faults.
        }
    };
    static_assert(sizeof(DataSlot) == 32);

    void scan_latest_valid_slot() {
        latest_valid_slot_ =
            reinterpret_cast<DataSlot*>(kMetadataStartAddress);
        latest_valid_slot_state_ = DataSlotState::kEmpty;

        for (uintptr_t addr = kMetadataStartAddress; addr < kMetadataEndAddress;
             addr += sizeof(DataSlot)) {
            auto* slot = reinterpret_cast<DataSlot*>(addr);
            const auto state = slot->read_state();

            if (state == DataSlotState::kFatal) {
                latest_valid_slot_ = nullptr;
                latest_valid_slot_state_ = DataSlotState::kFatal;
                return;
            }
            if (state == DataSlotState::kEmpty) {
                // First gap: everything after here is unwritten
                if (latest_valid_slot_state_ == DataSlotState::kEmpty)
                    latest_valid_slot_ = slot; // point at first empty
                return;
            }
            latest_valid_slot_ = slot;
            latest_valid_slot_state_ = state;
        }

        // Metadata sector is completely full — treat as fatal to force erase
        latest_valid_slot_ = nullptr;
        latest_valid_slot_state_ = DataSlotState::kFatal;
    }

    void erase_and_rescan() {
        FLASH_EraseInitTypeDef erase{};
        erase.TypeErase = FLASH_TYPEERASE_SECTORS;
        erase.Banks     = FLASH_BANK_1;
        erase.Sector    = FLASH_SECTOR_1;
        erase.NbSectors = 1;

        uint32_t sector_error = 0U;
        {
            const auto guard = UnlockGuard();
            utility::assert_always(HAL_FLASHEx_Erase(&erase, &sector_error) == HAL_OK);
        }
        // Bootloader runs cache-less (see main.cpp): no D-cache maintenance needed.

        scan_latest_valid_slot();
        utility::assert_always(
            latest_valid_slot_ != nullptr
            && latest_valid_slot_state_ == DataSlotState::kEmpty);
    }

    DataSlot*     latest_valid_slot_       = nullptr;
    DataSlotState latest_valid_slot_state_ = DataSlotState::kFatal;
};

} // namespace librmcs::firmware::flash
