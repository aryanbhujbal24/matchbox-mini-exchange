# Wire protocol

All integers little-endian. Structs are packed. Prices are integer ticks.

## Order entry (TCP)

Every message starts with a 4-byte header.

| Field | Type | Notes |
|---|---|---|
| len | u16 | total message length including the header |
| type | u8 | message type |
| reserved | u8 | 0 |

A receiver reads the header, checks `len` equals the fixed size for `type`,
and waits for that many bytes. A wrong length or unknown type ends the session.

### Client to exchange

| Type | Name | Size | Body |
|---|---|---|---|
| 1 | Login | 8 | `client_id u32` (non-zero, unique among connected sessions) |
| 2 | NewOrder | 28 | `cl_ord_id u64`, `price i64`, `qty u32`, `side u8` (0 buy, 1 sell), `tif u8` (0 GTC, 1 IOC), `ord_type u8` (0 limit, 1 market), `pad u8` |
| 3 | Cancel | 12 | `order_id u64` (the exchange id from the Ack) |

### Exchange to client

| Type | Name | Size | Body |
|---|---|---|---|
| 10 | LoginAck | 8 | `client_id u32` |
| 11 | Ack | 20 | `cl_ord_id u64`, `order_id u64` |
| 12 | Reject | 16 | `ref u64` (cl_ord_id for an order, order_id for a cancel), `reason u8`, `pad[3]` |
| 13 | Fill | 32 | `order_id u64`, `price i64`, `qty u32`, `leaves u32`, `side u8`, `is_maker u8`, `pad[2]` |
| 14 | Canceled | 20 | `order_id u64`, `qty u32`, `reason u8` (0 requested, 1 IOC remainder), `pad[3]` |

Reject reasons: 1 bad price, 2 bad quantity, 3 book full, 4 unknown order,
5 not owner, 6 not logged in, 7 bad message.

### Session rules

- Every request gets exactly one primary response, in request order:
  NewOrder -> Ack or Reject; Cancel -> Canceled (reason 0) or Reject.
- Fills for an order always follow its Ack. An IOC or market order that does
  not fully fill ends with Canceled (reason 1).
- Ownership is by `client_id`, not by connection. A client that reconnects
  with the same id can cancel orders it placed earlier, including after an
  exchange restart.
- `order_id` = generation (high 32 bits) | slot (low 32 bits). Ids are never
  reused, so a late cancel for a finished order is always rejected.

## Market data (UDP)

One datagram = a 16-byte header followed by `count` 16-byte messages
(at most 64, so 1040 bytes, inside a 1500-byte MTU).

| Header field | Type | Notes |
|---|---|---|
| first_seq | u64 | sequence number of the first message in this datagram |
| count | u16 | messages in this datagram |
| pad | u16 + u32 | 0 |

| Message field | Type | Notes |
|---|---|---|
| type | u8 | 1 Level, 2 Trade |
| side | u8 | Level: book side. Trade: aggressor side |
| pad | u16 | 0 |
| qty | u32 | Level: total resting quantity at the price, 0 means the level is gone. Trade: quantity traded |
| price | i64 | ticks |

A receiver keeps `expected = first_seq + count` from the last datagram. If
the next `first_seq` is higher, messages were lost; if lower, the datagram is
a duplicate or arrived out of order and is ignored. Level updates carry
absolute quantities, so a lost update is corrected the next time that level
changes.
