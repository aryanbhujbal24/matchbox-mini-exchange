#pragma once
#include <cstdint>

namespace mx {
using OrderId = uint64_t;
using Price   = int64_t;   // integer ticks, never floating point
using Qty     = uint32_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

struct Fill {
  OrderId maker_id;
  OrderId taker_id;
  Price   price;
  Qty     qty;
  Qty     maker_left;  // quantity still resting on the maker order after this fill
};
}  // namespace mx
