#pragma once
// Order-entry wire protocol (TCP). Little-endian, packed, fixed width per
// message type. Every message starts with a 4-byte header carrying its own
// length, so a receiver can frame a byte stream without lookahead.
// See docs/protocol.md for the field tables.
#include <cstdint>
#include <cstring>
#include "types.hpp"

namespace mx {

enum class MsgType : uint8_t {
  // client -> exchange
  Login = 1, NewOrder = 2, Cancel = 3,
  // exchange -> client
  LoginAck = 10, Ack = 11, Reject = 12, Fill = 13, Canceled = 14,
};
enum class Tif : uint8_t { GTC = 0, IOC = 1 };
enum class OrdType : uint8_t { Limit = 0, Market = 1 };
enum class RejectReason : uint8_t {
  None = 0, BadPrice, BadQty, BookFull, UnknownOrder, NotOwner, NotLoggedIn, BadMessage,
};
// Reduced: part of the order was cancelled; the rest is still resting.
enum class CancelReason : uint8_t { User = 0, IocRemainder = 1, Reduced = 2 };

constexpr Qty kMaxQty = 1'000'000;

#pragma pack(push, 1)
struct MsgHeader   { uint16_t len; uint8_t type; uint8_t reserved; };
struct LoginMsg    { MsgHeader h; uint32_t client_id; };
struct NewOrderMsg { MsgHeader h; uint64_t cl_ord_id; int64_t price; uint32_t qty;
                     uint8_t side; uint8_t tif; uint8_t ord_type; uint8_t pad; };
struct CancelMsg   { MsgHeader h; uint64_t order_id; };
struct LoginAckMsg { MsgHeader h; uint32_t client_id; };
struct AckMsg      { MsgHeader h; uint64_t cl_ord_id; uint64_t order_id; };
struct RejectMsg   { MsgHeader h; uint64_t ref; uint8_t reason; uint8_t pad[3]; };
struct FillMsg     { MsgHeader h; uint64_t order_id; int64_t price; uint32_t qty; uint32_t leaves;
                     uint8_t side; uint8_t is_maker; uint8_t pad[2]; };
struct CanceledMsg { MsgHeader h; uint64_t order_id; uint32_t qty; uint8_t reason; uint8_t pad[3]; };
#pragma pack(pop)

static_assert(sizeof(MsgHeader) == 4);
static_assert(sizeof(LoginMsg) == 8);
static_assert(sizeof(NewOrderMsg) == 28);
static_assert(sizeof(CancelMsg) == 12);
static_assert(sizeof(LoginAckMsg) == 8);
static_assert(sizeof(AckMsg) == 20);
static_assert(sizeof(RejectMsg) == 16);
static_assert(sizeof(FillMsg) == 32);
static_assert(sizeof(CanceledMsg) == 20);

constexpr size_t kMaxMsgSize = 32;

// Expected size for a message type, 0 if the type is unknown.
inline uint16_t msg_size(uint8_t type) {
  switch (static_cast<MsgType>(type)) {
    case MsgType::Login:    return sizeof(LoginMsg);
    case MsgType::NewOrder: return sizeof(NewOrderMsg);
    case MsgType::Cancel:   return sizeof(CancelMsg);
    case MsgType::LoginAck: return sizeof(LoginAckMsg);
    case MsgType::Ack:      return sizeof(AckMsg);
    case MsgType::Reject:   return sizeof(RejectMsg);
    case MsgType::Fill:     return sizeof(FillMsg);
    case MsgType::Canceled: return sizeof(CanceledMsg);
  }
  return 0;
}

template <class M>
inline M make_msg(MsgType t) {
  M m;
  std::memset(&m, 0, sizeof(M));
  m.h.len = sizeof(M);
  m.h.type = static_cast<uint8_t>(t);
  return m;
}

}  // namespace mx
