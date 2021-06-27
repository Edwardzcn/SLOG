#include "execution/tpcc/new_order.h"

namespace slog {
namespace tpcc {

NewOrderTxn::NewOrderTxn(const StorageAdapterPtr& storage_adapter, int w_id, int d_id, int c_id, int o_id,
                         int64_t datetime, const std::vector<OrderLine>& ol, int w_i_id)
    : warehouse_(storage_adapter),
      district_(storage_adapter),
      customer_(storage_adapter),
      new_order_(storage_adapter),
      order_(storage_adapter),
      order_line_(storage_adapter),
      item_(storage_adapter),
      stock_(storage_adapter) {
  a_w_id_ = MakeInt32Scalar(w_id);
  a_d_id_ = MakeInt8Scalar(d_id);
  a_c_id_ = MakeInt32Scalar(c_id);
  a_o_id_ = MakeInt32Scalar(o_id);
  datetime_ = MakeInt64Scalar(datetime);
  for (const auto& l : ol) {
    a_ol_.push_back(OrderLineScalar{.a_id = MakeInt8Scalar(l.id),
                                    .a_supply_w_id = MakeInt32Scalar(l.supply_w_id),
                                    .a_item_id = MakeInt32Scalar(l.item_id),
                                    .a_quantity = MakeInt8Scalar(l.quantity)});
  }
  w_i_id_ = MakeInt32Scalar(w_i_id);
}

bool NewOrderTxn::Read() {
  bool ok = true;
  if (auto res = warehouse_.Select({a_w_id_}, {WarehouseSchema::Column::TAX}); !res.empty()) {
    w_tax_ = UncheckedCast<Int32Scalar>(res[0]);
  } else {
    ok = false;
  }

  if (auto res = customer_.Select(
          {a_w_id_, a_d_id_, a_c_id_},
          {CustomerSchema::Column::DISCOUNT, CustomerSchema::Column::FULL_NAME, CustomerSchema::Column::CREDIT});
      !res.empty()) {
    c_discount_ = UncheckedCast<Int32Scalar>(res[0]);
    c_last_ = UncheckedCast<FixedTextScalar>(res[1]);
    c_credit_ = UncheckedCast<FixedTextScalar>(res[2]);
  } else {
    ok = false;
  }

  if (auto res = district_.Select({a_w_id_, a_d_id_}, {DistrictSchema::Column::TAX, DistrictSchema::Column::NEXT_O_ID});
      !res.empty()) {
    d_tax_ = UncheckedCast<Int32Scalar>(res[0]);
    d_next_o_id_ = UncheckedCast<Int32Scalar>(res[1]);
  } else {
    ok = false;
  }

  for (auto& l : a_ol_) {
    auto item_res = item_.Select({w_i_id_, l.a_item_id},
                                 {ItemSchema::Column::PRICE, ItemSchema::Column::NAME, ItemSchema::Column::DATA});
    auto stock_res =
        stock_.Select({l.a_supply_w_id, l.a_item_id}, {StockSchema::Column::QUANTITY, StockSchema::Column::ALL_DIST});
    if (item_res.empty() || stock_res.empty()) {
      ok = false;
      continue;
    }
    l.i_price = UncheckedCast<Int32Scalar>(item_res[0]);
    l.s_quantity = UncheckedCast<Int16Scalar>(stock_res[0]);
    std::string dist_info(reinterpret_cast<const char*>(stock_res[1]->data()), 24);
    l.dist_info = MakeFixedTextScalar<24>(dist_info);
  }

  return ok;
}

void NewOrderTxn::Compute() {
  new_d_next_o_id_ = MakeInt32Scalar(d_next_o_id_->value + 1);

  bool all_local = true;
  for (auto& l : a_ol_) {
    if (!(*l.a_supply_w_id == *a_w_id_)) {
      all_local = false;
    }
    l.amount = MakeInt32Scalar(l.a_quantity->value * l.i_price->value);
    if (l.s_quantity->value > l.a_quantity->value) {
      l.s_quantity->value -= l.a_quantity->value;
    } else {
      l.s_quantity->value -= l.a_quantity->value - 91;
    }
  }
  all_local_ = MakeInt8Scalar(all_local);
}

bool NewOrderTxn::Write() {
  bool ok = true;
  if (!district_.Update({a_w_id_, a_d_id_}, {DistrictSchema::Column::NEXT_O_ID}, {new_d_next_o_id_})) {
    ok = false;
  }

  auto null_carrier_id = MakeInt8Scalar(0);
  auto ol_cnt = MakeInt8Scalar(a_ol_.size());
  if (!order_.Insert({a_w_id_, a_d_id_, a_o_id_, a_c_id_, datetime_, null_carrier_id, ol_cnt, all_local_})) {
    ok = false;
  }

  if (!new_order_.Insert({a_w_id_, a_d_id_, a_o_id_, MakeInt8Scalar()})) {
    ok = false;
  }

  auto null_delivery_d = MakeInt64Scalar(0);
  for (const auto& l : a_ol_) {
    ok &= stock_.Update({l.a_supply_w_id, l.a_item_id}, {StockSchema::Column::QUANTITY}, {l.s_quantity});
    ok &= order_line_.Insert({a_w_id_, a_d_id_, a_o_id_, l.a_id, l.a_item_id, l.a_supply_w_id, null_delivery_d,
                              l.a_quantity, l.amount, l.dist_info});
  }

  return ok;
}

}  // namespace tpcc
}  // namespace slog