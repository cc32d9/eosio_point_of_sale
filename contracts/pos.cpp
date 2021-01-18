/*
  Copyright 2021 cc32d9@gmail.com

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include <eosio/eosio.hpp>
#include <eosio/multi_index.hpp>
#include <eosio/action.hpp>
#include <eosio/transaction.hpp>
#include <eosio/asset.hpp>
#include <eosio/crypto.hpp>
#include <eosio/time.hpp>


using namespace eosio;
using std::string;
using std::vector;

CONTRACT pos : public eosio::contract {
 public:

  pos( name self, name code, datastream<const char*> ds ):
    contract(self, code, ds)
    {}

  // sets the admin account
  ACTION setoracle(name oracle)
  {
    require_auth(_self);
    check(is_account(oracle), "oracle account does not exist");
    set_name_prop(name("oracleacc"), oracle);
  }

  // the account will receive the fee from every claim
  ACTION setfee(name feeacc, uint16_t permille)
  {
    require_auth(_self);
    check(is_account(feeacc), "fee account does not exist");
    check(permille < 1000, "permille must be less than 1000");
    set_name_prop(name("feeacc"), feeacc);
    set_uint_prop(name("feepermille"), permille);
  }


  ACTION newsku(name seller, string sku, string description, name tkcontract, asset price, uint32_t count)
  {
    require_auth(seller);

    checksum256 hash = sha256(sku.data(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    check( hashidx.find(hash) == hashidx.end(), "An SKU with this name already exists");

    // validate that such token exists
    {
      stats_table statstbl(tkcontract, price.symbol.code().raw());
      auto statsitr = statstbl.find(price.symbol.code().raw());
      check(statsitr != statstbl.end(), "This currency symbol does not exist");
      check(statsitr->supply.symbol.precision() == price.symbol.precision(), "Wrong currency precision");
    }

    inc_uint_prop(name("lastskuid"));
    uint64_t skuid = get_uint_prop(name("lastskuid"));

    _skus.emplace(seller, [&]( auto& row ) {
                            row.id = skuid;
                            row.seller = seller;
                            row.sku = sku;
                            row.description = description;
                            row.skuhash = hash;
                            row.tkcontract = tkcontract;
                            row.price = price;
                          });

    stockrows _stockrows(_self, 0);
    _stockrows.emplace(seller, [&]( auto& row ) {
                                 row.skuid = skuid;
                               });

    sellercntrs _sellercntrs(_self, 0);
    auto cntrs_itr = _sellercntrs.find(seller.value);
    if( cntrs_itr == _sellercntrs.end() ) {
      _sellercntrs.emplace(seller, [&]( auto& row ) {
                                     row.seller = seller;
                                     row.skus = 1;
                                   });
    }
    else {
      _sellercntrs.modify( *cntrs_itr, seller, [&]( auto& row ) {
                                                 row.skus++;
                                               });
    }

    sellerinforows _sellerinforows(_self, 0);
    auto info_itr = _sellerinforows.find(seller.value);
    if( info_itr == _sellerinforows.end() ) {
      _sellerinforows.emplace(seller, [&]( auto& row ) {
                                        row.seller = seller;
                                      });
    }

    if( count > 0 ) {
      _add_stock(seller, skuid, count);
    }
  }


  ACTION updseller(name seller, string company, string website)
  {
    require_auth(seller);
    sellerinforows _sellerinforows(_self, 0);
    auto info_itr = _sellerinforows.find(seller.value);
    if( info_itr == _sellerinforows.end() ) {
      _sellerinforows.emplace(seller, [&]( auto& row ) {
                                        row.seller = seller;
                                        row.company = company;
                                        row.website = website;
                                      });
    }
    else {
      _sellerinforows.modify( *info_itr, seller, [&]( auto& row ) {
                                                   row.company = company;
                                                   row.website = website;
                                                 });
    }
  }


  ACTION addstock(string sku, uint32_t count)
  {
    checksum256 hash = sha256(sku.data(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    auto hashitr = hashidx.find(hash);
    check( hashitr != hashidx.end(), "Cannot find an SKU with such a name");

    require_auth(hashitr->seller);
    _add_stock(hashitr->seller, hashitr->id, count);
  }


  // Oracle delivers last irreversible block and its timestamp.
  ACTION orairrev(uint32_t irrev_block, time_point irrev_timestamp)
  {
    name oracle = get_name_prop(name("oracleacc"));
    check(oracle.value != 0, "oracle account is not set");
    require_auth(oracle);

    set_uint_prop(name("irrevblock"), irrev_block);
    set_uint_prop(name("irrevtime"), irrev_timestamp.elapsed.count());
  }


  // seller claims the payments after they become irreversible
  ACTION claim(name seller, uint32_t count)
  {
    require_auth(seller);
    sellercntrs _sellercntrs(_self, 0);
    check(_sellercntrs.find(seller.value) != _sellercntrs.end(), "Unknown seller");

    uint64_t feepermille = get_uint_prop(name("feepermille"));
    name feeacc = get_name_prop(name("feeacc"));

    uint64_t irrev_time = get_uint_prop(name("irrevtime"));
    bool done_something = false;

    skus _skus(_self, 0);

    stockitems _stockitems(_self, seller.value);
    auto itemidx = _stockitems.get_index<name("soldon")>();
    auto item_itr = itemidx.lower_bound(1); // sold_on is zero if the item is not sold

    while(count-- > 0 && item_itr != itemidx.end() && item_itr->get_sold_on() <= irrev_time) {
      auto& sku = _skus.get(item_itr->skuid, "This should never happen 5");
      asset quantity = sku.price;

      if( feepermille > 0 && feeacc != name("") ) {
        asset fee = quantity * feepermille / 1000;
        quantity -= fee;
        extended_asset xfee(fee, sku.tkcontract);
        send_payment(feeacc, xfee, "fees");
      }

      send_payment(seller, extended_asset{quantity, sku.tkcontract},
                   string("{\"sku\":\"") + sku.sku + "\",\"item_id\":\"" +
                   std::to_string(item_itr->id) +
                   "\",\"buyer\":\"" + item_itr->buyer.to_string() + "\"}");
      action {
        permission_level{_self, name("active")},
          _self,
            name("finalreceipt"),
            receipt_abi {.item_id=item_itr->id, .seller=seller, .sku=sku.sku, .buyer=item_itr->buyer}
      }.send();

      item_itr = itemidx.erase(item_itr);
    }
    check(done_something, "No sold items available for claims");
  }


  // incoming payment. Memo must match an SKU

  [[eosio::on_notify("*::transfer")]] void on_payment (name from, name to, asset quantity, string memo) {
    if(to == _self) {
      checksum256 hash = sha256(memo.data(), memo.size());
      skus _skus(_self, 0);
      auto hashidx = _skus.get_index<name("skuhash")>();
      auto hashitr = hashidx.find(hash);
      check(hashitr != hashidx.end(), "Cannot find an SKU with such a name");
      check(hashitr->tkcontract == get_first_receiver(),
            "Wrong token contract, expected: " + hashitr->tkcontract.to_string());
      check(hashitr->price == quantity,
            "Wrong amount or currency, expected: " + hashitr->price.to_string());

      uint64_t skuid = hashitr->id;
      name seller = hashitr->seller;

      stockitems _stockitems(_self, seller.value);
      auto itemidx = _stockitems.get_index<name("skuid")>();
      auto item_itr = itemidx.lower_bound(skuid);
      check(item_itr != itemidx.end(), "This SKU is sold out");

      auto buyeridx = _stockitems.get_index<name("buyersku")>();
      check(buyeridx.find(((uint128_t)from.value << 64)|(uint128_t)skuid) == buyeridx.end(),
            "This buyer has purchased already this SKU. Wait for the seller to process the purchase");

      uint64_t item_id = item_itr->id;
      time_point now = current_time_point();

      // mark the item as sold
      _stockitems.modify(*item_itr, same_payer, [&]( auto& row ) {
                                                  row.sold_on = now;
                                                  row.buyer = from;
                                                  row.trxid = get_trxid();
                                                });

      // update statistics
      sellercntrs _sellercntrs(_self, 0);
      auto ctr_itr = _sellercntrs.find(seller.value);
      check( ctr_itr != _sellercntrs.end(), "This should never happen 3");
      _sellercntrs.modify(*ctr_itr, same_payer, [&]( auto& row ) {
                                                  row.items_onsale--;
                                                  row.last_sale = now;
                                                });

      stockrows _stockrows(_self, 0);
      auto stock_itr = _stockrows.find(skuid);
      check(stock_itr != _stockrows.end(), "This should never happen 4");
      _stockrows.modify(*stock_itr, same_payer, [&]( auto& row ) {
                                                  row.items_onsale--;
                                                  row.last_sale = now;
                                                });

      action {
        permission_level{_self, name("active")},
          _self,
            name("payreceipt"),
            receipt_abi {.item_id=item_id, .seller=seller, .sku=hashitr->sku, .buyer=from }
      }.send();

      inc_uint_prop(name("purchases"));
    }
  }


  // inline notifications
  struct receipt_abi {
    uint64_t       item_id;
    name           seller;
    string         sku;
    name           buyer;
  };

  ACTION payreceipt(receipt_abi)
  {
    require_auth(_self);
  }

  ACTION finalreceipt(receipt_abi x)
  {
    require_auth(_self);
    require_recipient(x.seller);
  }


 private:

  // Stock keeping unit; scope=0
  struct [[eosio::table("skus")]] sku {
    uint64_t       id;
    name           seller;
    string         sku;
    string         description;
    checksum256    skuhash;
    name           tkcontract;
    asset          price;
    auto primary_key()const { return id; }
    checksum256 get_skuhash() const { return skuhash; }
  };

  typedef eosio::multi_index<
    name("skus"), sku,
    indexed_by<name("skuhash"), const_mem_fun<sku, checksum256, &sku::get_skuhash>>
    > skus;


  // Stock count; scope=0
  struct [[eosio::table("stock")]] stockrow {
    uint64_t       skuid;
    uint64_t       items_onsale = 0; // number of items available on sale
    time_point     last_sale;
    auto primary_key()const { return skuid; }
  };

  typedef eosio::multi_index<name("stock"), stockrow> stockrows;


  // Seller counters; scope=0
  struct [[eosio::table("sellercntrs")]] sellercntr {
    name           seller;
    uint64_t       next_item_id = 1;
    uint64_t       skus;  // number of SKUs on sale
    uint64_t       items_onsale = 0; // number of items on sale
    time_point     last_sale;
    auto primary_key()const { return seller.value; }
  };

  typedef eosio::multi_index<name("sellercntrs"), sellercntr> sellercntrs;


  // Seller information; scope=0
  struct [[eosio::table("sellerinfo")]] sellerinforow {
    name       seller;
    string     company;
    string     website;
    auto primary_key()const { return seller.value; }
  };

  typedef eosio::multi_index<name("sellerinfo"), sellerinforow> sellerinforows;



  // Stock items; scope=seller
  struct [[eosio::table("stockitems")]] stockitem {
    uint64_t        id;
    uint64_t        skuid;
    time_point      sold_on;
    name            buyer;
    checksum256     trxid;
    auto primary_key()const { return id; }
    uint64_t get_skuid()const { return (sold_on.elapsed.count() == 0) ? skuid : 0; }
    uint64_t get_sold_on()const { return sold_on.elapsed.count(); }
    uint128_t get_buyer_sku()const { return ((uint128_t)buyer.value << 64)|(uint128_t)skuid; }
  };

  typedef eosio::multi_index<
    name("stockitems"), stockitem,
    indexed_by<name("skuid"), const_mem_fun<stockitem, uint64_t, &stockitem::get_skuid>>,
    indexed_by<name("soldon"), const_mem_fun<stockitem, uint64_t, &stockitem::get_sold_on>>,
    indexed_by<name("buyersku"), const_mem_fun<stockitem, uint128_t, &stockitem::get_buyer_sku>>
    > stockitems;



  void _add_stock(name seller, uint64_t skuid, uint32_t count)
  {
    sellercntrs _sellercntrs(_self, 0);
    auto ctr_itr = _sellercntrs.find(seller.value);
    check( ctr_itr != _sellercntrs.end(), "This should never happen 1");
    uint64_t itemid = ctr_itr->next_item_id;

    stockitems _stockitems(_self, seller.value);
    while( count > 0 ) {
      _stockitems.emplace(seller, [&]( auto& row ) {
                                    row.id = itemid;
                                    row.skuid = skuid;
                                  });
      itemid++;
    }

    _sellercntrs.modify(*ctr_itr, same_payer, [&]( auto& row ) {
                                                row.items_onsale += count;
                                                row.next_item_id = itemid;
                                              });

    stockrows _stockrows(_self, 0);
    auto stock_itr = _stockrows.find(skuid);
    check(stock_itr != _stockrows.end(), "This should never happen 2");
    _stockrows.modify(*stock_itr, same_payer, [&]( auto& row ) {
                                                row.items_onsale += count;
                                              });
  }




  // properties table for keeping contract settings; scope=0
  struct [[eosio::table("props")]] prop {
    name           key;
    name           val_name;
    uint64_t       val_uint = 0;
    auto primary_key()const { return key.value; }
  };

  typedef eosio::multi_index<
    name("props"), prop> props;

  void set_name_prop(name key, name value)
  {
    props p(_self, 0);
    auto itr = p.find(key.value);
    if( itr != p.end() ) {
      p.modify( *itr, same_payer, [&]( auto& row ) {
                                    row.val_name = value;
                                  });
    }
    else {
      p.emplace(_self, [&]( auto& row ) {
                         row.key = key;
                         row.val_name = value;
                       });
    }
  }

  name get_name_prop(name key)
  {
    props p(_self, 0);
    auto itr = p.find(key.value);
    if( itr != p.end() ) {
      return itr->val_name;
    }
    else {
      p.emplace(_self, [&]( auto& row ) {
                         row.key = key;
                       });
      return name();
    }
  }


  void set_uint_prop(name key, uint64_t value)
  {
    props p(_self, 0);
    auto itr = p.find(key.value);
    if( itr != p.end() ) {
      p.modify( *itr, same_payer, [&]( auto& row ) {
                                    row.val_uint = value;
                                  });
    }
    else {
      p.emplace(_self, [&]( auto& row ) {
                         row.key = key;
                         row.val_uint = value;
                       });
    }
  }

  uint64_t get_uint_prop(name key)
  {
    props p(_self, 0);
    auto itr = p.find(key.value);
    if( itr != p.end() ) {
      return itr->val_uint;
    }
    else {
      p.emplace(_self, [&]( auto& row ) {
                         row.key = key;
                       });
      return 0;
    }
  }

  void inc_uint_prop(name key)
  {
    props p(_self, 0);
    auto itr = p.find(key.value);
    if( itr != p.end() ) {
      p.modify( *itr, same_payer, [&]( auto& row ) {
                                    row.val_uint++;
                                  });
    }
    else {
      p.emplace(_self, [&]( auto& row ) {
                         row.key = key;
                         row.val_uint = 1;
                       });
    }
  }

  void dec_uint_prop(name key)
  {
    props p(_self, 0);
    auto itr = p.find(key.value);
    check(itr != p.end(), "Cannot find prroperty: " + key.to_string());
    check(itr->val_uint > 0, "Trying to decrement a zero property: " + key.to_string());

    p.modify( *itr, same_payer, [&]( auto& row ) {
                                  row.val_uint--;
                                });
  }






  inline checksum256 get_trxid()
  {
    auto trxsize = transaction_size();
    char trxbuf[trxsize];
    uint32_t trxread = read_transaction( trxbuf, trxsize );
    check( trxsize == trxread, "read_transaction failed");
    return sha256(trxbuf, trxsize);
  }

  struct transfer_args
  {
    name         from;
    name         to;
    asset        quantity;
    string       memo;
  };

  void send_payment(name recipient, const extended_asset& x, const string memo)
  {
    action
      {
        permission_level{_self, name("active")},
          x.contract,
            name("transfer"),
            transfer_args {.from=_self, .to=recipient, .quantity=x.quantity, .memo=memo}
      }.send();
  }



  // eosio.token structure
  struct currency_stats {
    asset  supply;
    asset  max_supply;
    name issuer;

    uint64_t primary_key()const { return supply.symbol.code().raw(); }
  };

  typedef eosio::multi_index<"stat"_n, currency_stats> stats_table;
};
