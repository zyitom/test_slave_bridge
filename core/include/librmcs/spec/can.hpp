#pragma once

#include <librmcs/data/datas.hpp>

namespace librmcs::spec {

struct CanDescriptor {
    constexpr explicit CanDescriptor(data::DataId data_id) noexcept
        : data_id(data_id) {}

    data::DataId data_id;
};

} // namespace librmcs::spec
