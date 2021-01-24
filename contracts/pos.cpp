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


  ACTION updseller(name seller, string company, string website, bool tracking)
  {
    require_auth(seller);
    sellerinforows _sellerinforows(_self, 0);
    sellercntrs _sellercntrs(_self, 0);

    auto info_itr = _sellerinforows.find(seller.value);
    if( info_itr == _sellerinforows.end() ) {
      _sellerinforows.emplace(seller, [&]( auto& row ) {
                                        row.seller = seller;
                                        row.company = company;
                                        row.website = website;
                                        row.tracking = tracking;
                                      });
      _sellercntrs.emplace(seller, [&]( auto& row ) {
                                     row.seller = seller;
                                     row.skus = 0;
                                   });
      inc_uint_prop(name("sellers"));
    }
    else {
      _sellerinforows.modify( *info_itr, seller, [&]( auto& row ) {
                                                   row.company = company;
                                                   row.website = website;
                                                   row.tracking = tracking;
                                                 });
    }
  }


  ACTION newsku(name seller, string sku, string description, name tkcontract, asset price, uint32_t count)
  {
    require_auth(seller);

    checksum256 hash = sha256(sku.data(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    check( hashidx.find(hash) == hashidx.end(), "An SKU with this name already exists");

    check(price.amount > 0, "Price must be above zero");

    // validate that such token exists
    {
      stats_table statstbl(tkcontract, price.symbol.code().raw());
      auto statsitr = statstbl.find(price.symbol.code().raw());
      check(statsitr != statstbl.end(), "This currency symbol does not exist");
      check(statsitr->supply.symbol.precision() == price.symbol.precision(), "Wrong currency precision");
    }

    sellercntrs _sellercntrs(_self, 0);
    auto cntrs_itr = _sellercntrs.find(seller.value);
    check(cntrs_itr != _sellercntrs.end(), "Unknown seller");
    _sellercntrs.modify( *cntrs_itr, seller, [&]( auto& row ) {
                                               row.skus++;
                                             });

    inc_uint_prop(name("lastskuid"));
    uint64_t skuid = get_uint_prop(name("lastskuid"));

    _skus.emplace(seller, [&]( auto& row ) {
                            row.id = skuid;
                            row.seller = seller;
                            row.sku = sku;
                            row.description = description;
                            row.tkcontract = tkcontract;
                            row.price = price;
                          });

    stockrows _stockrows(_self, 0);
    _stockrows.emplace(seller, [&]( auto& row ) {
                                 row.skuid = skuid;
                               });

    if( count > 0 ) {
      _add_stock(seller, skuid, count);
    }

    inc_uint_prop(name("skus"));
  }




  ACTION delseller(name seller)
  {
    require_auth(seller);
    sellercntrs _sellercntrs(_self, 0);
    auto ctr_itr = _sellercntrs.find(seller.value);
    check(ctr_itr != _sellercntrs.end(), "Unknown seller");
    check(ctr_itr->skus == 0, "Cannot delete a seller with registered SKUs");
    _sellercntrs.erase(ctr_itr);

    sellerinforows _sellerinforows(_self, 0);
    auto info_itr = _sellerinforows.find(seller.value);
    _sellerinforows.erase(info_itr);

    dec_uint_prop(name("sellers"));
  }


  ACTION updskuprice(string sku, asset price)
  {
    checksum256 hash = sha256(sku.data(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    auto hashitr = hashidx.find(hash);
    check( hashitr != hashidx.end(), "Cannot find an SKU with such a name");
    require_auth(hashitr->seller);

    check(price.amount > 0, "Price must be above zero");
    check(price.symbol == hashitr->price.symbol, "Cannot change the currency");
    check(price != hashitr->price, "SKU has this price already");

    _skus.modify(*hashitr, hashitr->seller, [&]( auto& row ) {
        row.price = price;
      });
  }


  ACTION updskudescr(string sku, string description)
  {
    checksum256 hash = sha256(sku.data(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    auto hashitr = hashidx.find(hash);
    check( hashitr != hashidx.end(), "Cannot find an SKU with such a name");
    require_auth(hashitr->seller);
    check(description != hashitr->description, "SKU has already this description");

    _skus.modify(*hashitr, hashitr->seller, [&]( auto& row ) {
        row.description = description;
      });
  }


  ACTION delsku(string sku)
  {
    checksum256 hash = sha256(sku.data(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    auto hashitr = hashidx.find(hash);
    check( hashitr != hashidx.end(), "Cannot find an SKU with such a name");
    require_auth(hashitr->seller);

    stockrows _stockrows(_self, 0);
    auto stock_itr = _stockrows.find(hashitr->id);
    check(stock_itr != _stockrows.end(), "Exception 9");
    check(stock_itr->items_onsale == 0, "Cannot delete the SKU while items are on sale");

    stockitems _stockitems(_self, hashitr->seller.value);
    auto itemidx = _stockitems.get_index<name("sku")>();
    auto item_itr = itemidx.find(hashitr->id);
    check(item_itr == itemidx.end(), "Cannot delete the SKU: there are sold items that were not claimed");

    trackingrows _trackingrows(_self, hashitr->seller.value);
    auto trackidx = _trackingrows.get_index<name("sku")>();
    check(trackidx.find(hashitr->id) == trackidx.end(),
          "Cannot delete the SKU: there are items in tracking table");

    sellercntrs _sellercntrs(_self, 0);
    auto cntrs_itr = _sellercntrs.find(hashitr->seller.value);
    _sellercntrs.modify( *cntrs_itr, hashitr->seller, [&]( auto& row ) {
        row.skus--;
      });

    hashidx.erase(hashitr);
    _stockrows.erase(stock_itr);
    dec_uint_prop(name("skus"));
  }



  ACTION addstock(string sku, uint32_t count)
  {
    checksum256 hash = sha256(sku.data(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    auto hashitr = hashidx.find(hash);
    check(hashitr != hashidx.end(), "Cannot find an SKU with such a name");
    require_auth(hashitr->seller);
    _add_stock(hashitr->seller, hashitr->id, count);
  }


  ACTION delstock(string sku, uint32_t count)
  {
    checksum256 hash = sha256(sku.data(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    auto hashitr = hashidx.find(hash);
    check(hashitr != hashidx.end(), "Cannot find an SKU with such a name");
    require_auth(hashitr->seller);

    uint64_t skuid = hashitr->id;
    stockitems _stockitems(_self, hashitr->seller.value);
    auto itemidx = _stockitems.get_index<name("skustock")>();
    auto item_itr = itemidx.lower_bound(skuid);

    for( uint32_t i=0; i<count; i++ ) {
      check(item_itr != itemidx.end() && item_itr->skuid == skuid,
            "There are only " + std::to_string(i) + " unsold items on stock");
      item_itr = itemidx.erase(item_itr);
    }

    sellercntrs _sellercntrs(_self, 0);
    auto ctr_itr = _sellercntrs.find(hashitr->seller.value);
    check(ctr_itr != _sellercntrs.end(), "Exception 10");
    check(ctr_itr->items_onsale >= count, "Exception 11");

    _sellercntrs.modify(*ctr_itr, same_payer, [&]( auto& row ) {
                                                row.items_onsale -= count;
                                              });

    stockrows _stockrows(_self, 0);
    auto stock_itr = _stockrows.find(hashitr->id);
    check(stock_itr != _stockrows.end(), "Exception 12");
    check(stock_itr->items_onsale >= count, "Exception 13");

    _stockrows.modify(*stock_itr, same_payer, [&]( auto& row ) {
                                                row.items_onsale -= count;
                                              });

  }


  // Oracle delivers last irreversible block and its timestamp.
  ACTION orairrev(uint32_t irrev_block, time_point irrev_timestamp)
  {
    name oracle = get_name_prop(name("oracleacc"));
    check(oracle.value != 0, "oracle account is not set");
    require_auth(oracle);

    uint64_t old_irrev = get_uint_prop(name("irrevtime"));
    uint64_t new_irrev = irrev_timestamp.elapsed.count();

    check(get_uint_prop(name("lastsale")) > old_irrev, "No new sales");
    check(new_irrev > old_irrev, "already registered this timestamp");

    set_uint_prop(name("irrevblock"), irrev_block);
    set_uint_prop(name("irrevtime"), new_irrev);
  }




  // incoming payment. Memo must match an SKU
  [[eosio::on_notify("*::transfer")]] void on_payment (name from, name to, asset quantity, string memo) {
    if(to == _self) {
      checksum256 hash = sha256(memo.data(), memo.size());
      skus _skus(_self, 0);
      auto hashidx = _skus.get_index<name("skuhash")>();
      auto hashitr = hashidx.find(hash);
      check(hashitr != hashidx.end(),
            "Cannot find an SKU with such a name. Transfer memo must match exactly an SKU name.");
      check(hashitr->tkcontract == get_first_receiver(),
            "Wrong token contract, expected: " + hashitr->tkcontract.to_string());
      check(hashitr->price == quantity,
            "Wrong amount or currency, expected: " + hashitr->price.to_string());

      uint64_t skuid = hashitr->id;
      name seller = hashitr->seller;

      stockitems _stockitems(_self, seller.value);
      auto itemidx = _stockitems.get_index<name("skustock")>();
      auto item_itr = itemidx.lower_bound(skuid);
      check(item_itr != itemidx.end() && item_itr->skuid == skuid, "This SKU is sold out");

      auto buyeridx = _stockitems.get_index<name("buyersku")>();
      check(buyeridx.find(((uint128_t)from.value << 64)|(uint128_t)skuid) == buyeridx.end(),
            "This buyer has purchased already this SKU. Wait for the seller to process the purchase");

      uint64_t item_id = item_itr->id;
      time_point now = current_time_point();

      // mark the item as sold
      _stockitems.modify(*item_itr, same_payer, [&]( auto& row ) {
                                                  row.sold_on = now;
                                                  row.price = quantity;
                                                  row.buyer = from;
                                                  row.trxid = get_trxid();
                                                });

      // update statistics
      sellercntrs _sellercntrs(_self, 0);
      auto ctr_itr = _sellercntrs.find(seller.value);
      check( ctr_itr != _sellercntrs.end(), "Exception 3");
      _sellercntrs.modify(*ctr_itr, same_payer, [&]( auto& row ) {
                                                  row.items_onsale--;
                                                  row.last_sale = now;
                                                });

      stockrows _stockrows(_self, 0);
      auto stock_itr = _stockrows.find(skuid);
      check(stock_itr != _stockrows.end(), "Exception 4");
      _stockrows.modify(*stock_itr, same_payer, [&]( auto& row ) {
                                                  row.items_onsale--;
                                                  row.last_sale = now;
                                                });

      receipt rcpt(*hashitr, *item_itr);
      action {
        permission_level{_self, name("active")},
          _self, name("payreceipt"), rcpt
            }.send();

      inc_uint_prop(name("purchases"));
      set_uint_prop(name("lastsale"), now.elapsed.count());
    }
  }


  // seller claims the payments after they become irreversible
  ACTION claim(name seller, uint32_t count)
  {
    require_auth(seller);
    sellerinforows _sellerinforows(_self, 0);
    auto info_itr = _sellerinforows.find(seller.value);
    check(info_itr != _sellerinforows.end(), "Unknown seller");

    trackingrows _trackingrows(_self, seller.value);

    uint64_t feepermille = get_uint_prop(name("feepermille"));
    name feeacc = get_name_prop(name("feeacc"));

    uint64_t irrev_time = get_uint_prop(name("irrevtime"));
    bool done_something = false;

    skus _skus(_self, 0);

    stockitems _stockitems(_self, seller.value);
    auto itemidx = _stockitems.get_index<name("soldon")>();
    auto item_itr = itemidx.lower_bound(1); // sold_on is zero if the item is not sold

    time_point now;
    checksum256 trxid;
    if( info_itr->tracking ) {
      now = current_time_point();
      trxid = get_trxid();
    }

    while(count-- > 0 && item_itr != itemidx.end() && item_itr->get_sold_on() <= irrev_time) {
      auto& sku = _skus.get(item_itr->skuid, "Exception 5");
      asset quantity = item_itr->price;

      if( feepermille > 0 && feeacc != name("") ) {
        asset fee = quantity * feepermille / 1000;
        quantity -= fee;
        extended_asset xfee(fee, sku.tkcontract);
        send_payment(feeacc, xfee, "fees");
      }

      receipt rcpt(sku, *item_itr);

      send_payment(seller, extended_asset{quantity, sku.tkcontract},
                   string("{\"sku\":\"") + sku.sku + "\",\"item_id\":\"" +
                   std::to_string(item_itr->id) +
                   "\",\"buyer\":\"" + item_itr->buyer.to_string() + "\"}");
      action {
        permission_level{_self, name("active")},
          _self, name("finalreceipt"), rcpt
            }.send();

      if( info_itr->tracking ) {
        _trackingrows.emplace(seller, [&]( auto& row ) {
                                        row.itemid = item_itr->id;
                                        row.skuid  = item_itr->skuid;
                                        row.sold_on = item_itr->sold_on;
                                        row.price = item_itr->price;
                                        row.buyer = item_itr->buyer;
                                        row.tracking_state = name("paymntrcvd");
                                        row.memo = "Payment received";
                                        row.updated_on = now;
                                        row.update_trxid = trxid;
                                      });
      }

      item_itr = itemidx.erase(item_itr);
      done_something = true;
    }
    check(done_something, "No sold items available for claims");
  }


  ACTION updtracking(name seller, name newstate, string memo, vector<uint64_t> itemids)
  {
    require_auth(seller);
    checksum256 trxid = get_trxid();
    time_point now = current_time_point();
    trackingrows _trackingrows(_self, seller.value);

    for( uint64_t itemid: itemids ) {
      auto track_iter = _trackingrows.find(itemid);
      check(track_iter != _trackingrows.end(),
            "Cannot find tracking item id: " + std::to_string(itemid));

      _trackingrows.modify( *track_iter, seller, [&]( auto& row ) {
                                                   row.tracking_state = newstate;
                                                   row.memo = memo;
                                                   row.updated_on = now;
                                                   row.update_trxid = trxid;
                                                 });
    }
  }


  ACTION deltracking(name seller, vector<uint64_t> itemids)
  {
    require_auth(seller);
    trackingrows _trackingrows(_self, seller.value);

    for( uint64_t itemid: itemids ) {
      auto track_iter = _trackingrows.find(itemid);
      check(track_iter != _trackingrows.end(),
            "Cannot find tracking item id: " + std::to_string(itemid));
      _trackingrows.erase(track_iter);
    }
  }


  ACTION wipeall(uint32_t count)
  {
    require_auth(_self);
    bool done_something = false;

    sellerinforows _sellerinforows(_self, 0);
    auto selleritr = _sellerinforows.begin();
    while( count-- > 0 && selleritr != _sellerinforows.end() ) {

      stockitems _stockitems(_self, selleritr->seller.value);
      auto items_itr = _stockitems.begin();
      while( count-- > 0 && items_itr != _stockitems.end() ) {
        items_itr = _stockitems.erase(items_itr);
        done_something = true;
      }

      trackingrows _trackingrows(_self, selleritr->seller.value);
      auto track_iter = _trackingrows.begin();
      while( count-- > 0 && track_iter != _trackingrows.end() ) {
        track_iter = _trackingrows.erase(track_iter);
        done_something = true;
      }


      if( items_itr == _stockitems.end() && track_iter == _trackingrows.end() ) {
        selleritr = _sellerinforows.erase(selleritr);
        done_something = true;
      }
    }

    skus _skus(_self, 0);
    auto skuitr = _skus.begin();
    while( count-- > 0 && skuitr != _skus.end() ) {
      skuitr = _skus.erase(skuitr);
      done_something = true;
    }

    stockrows _stockrows(_self, 0);
    auto stockitr = _stockrows.begin();
    while( count-- > 0 && stockitr != _stockrows.end() ) {
      stockitr = _stockrows.erase(stockitr);
      done_something = true;
    }

    sellercntrs _sellercntrs(_self, 0);
    auto ctr_itr = _sellercntrs.begin();
    while( count-- > 0 && ctr_itr != _sellercntrs.end() ) {
      ctr_itr = _sellercntrs.erase(ctr_itr);
      done_something = true;
    }

    props _props(_self, 0);
    auto pitr = _props.begin();
    while( count-- > 0 && pitr != _props.end() ) {
      pitr = _props.erase(pitr);
      done_something = true;
    }

    check(done_something, "Nothing to do");
  }



 private:

  // Stock keeping unit; scope=0
  struct [[eosio::table("skus")]] sku {
    uint64_t       id;
    name           seller;
    string         sku;
    string         description;
    name           tkcontract;
    asset          price;
    auto primary_key()const { return id; }
    checksum256 get_skuhash() const { return sha256(sku.data(), sku.size()); }
    uint128_t get_selleridx() const { return ((uint128_t)seller.value << 64)|(uint128_t)id; }
  };

  typedef eosio::multi_index<
    name("skus"), sku,
    indexed_by<name("skuhash"), const_mem_fun<sku, checksum256, &sku::get_skuhash>>,
    indexed_by<name("byseller"), const_mem_fun<sku, uint128_t, &sku::get_selleridx>>
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
    bool       tracking;
    auto primary_key()const { return seller.value; }
  };

  typedef eosio::multi_index<name("sellerinfo"), sellerinforow> sellerinforows;



  // Stock items; scope=seller
  struct [[eosio::table("stockitems")]] stockitem {
    uint64_t        id;
    uint64_t        skuid;
    time_point      sold_on;
    asset           price;
    name            buyer;
    checksum256     trxid;
    auto primary_key()const { return id; }
    uint64_t get_sku()const { return skuid; }
    uint64_t get_skustock()const { return (sold_on.elapsed.count() == 0) ? skuid : 0; }
    uint64_t get_sold_on()const { return sold_on.elapsed.count(); }
    uint128_t get_buyer_sku()const { return ((uint128_t)buyer.value << 64)|(uint128_t)skuid; }
  };

  typedef eosio::multi_index<
    name("stockitems"), stockitem,
    indexed_by<name("sku"), const_mem_fun<stockitem, uint64_t, &stockitem::get_sku>>,
    indexed_by<name("skustock"), const_mem_fun<stockitem, uint64_t, &stockitem::get_skustock>>,
    indexed_by<name("soldon"), const_mem_fun<stockitem, uint64_t, &stockitem::get_sold_on>>,
    indexed_by<name("buyersku"), const_mem_fun<stockitem, uint128_t, &stockitem::get_buyer_sku>>
    > stockitems;



  void _add_stock(name seller, uint64_t skuid, uint32_t count)
  {
    sellercntrs _sellercntrs(_self, 0);
    auto ctr_itr = _sellercntrs.find(seller.value);
    check( ctr_itr != _sellercntrs.end(), "Exception 1");
    uint64_t itemid = ctr_itr->next_item_id;

    stockitems _stockitems(_self, seller.value);
    for( uint32_t i=0; i<count; i++ ) {
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
    check(stock_itr != _stockrows.end(), "Exception 2");
    _stockrows.modify(*stock_itr, same_payer, [&]( auto& row ) {
                                                row.items_onsale += count;
                                              });
  }


  // Tracking the items; scope=seller
  struct [[eosio::table("tracking")]] trackingrow {
    uint64_t        itemid;
    uint64_t        skuid;
    time_point      sold_on;
    asset           price;
    name            buyer;
    name            tracking_state;
    string          memo;
    time_point      updated_on;
    checksum256     update_trxid;
    auto primary_key()const { return itemid; }
    uint64_t get_sku()const { return skuid; }
    uint64_t get_updated_on()const { return updated_on.elapsed.count(); }
    uint128_t get_by_state()const { return ((uint128_t)tracking_state.value << 64)|(uint128_t)itemid; }
  };

  typedef eosio::multi_index<
    name("tracking"), trackingrow,
    indexed_by<name("sku"), const_mem_fun<trackingrow, uint64_t, &trackingrow::get_sku>>,
    indexed_by<name("updates"), const_mem_fun<trackingrow, uint64_t, &trackingrow::get_updated_on>>,
    indexed_by<name("state"), const_mem_fun<trackingrow, uint128_t, &trackingrow::get_by_state>>
    > trackingrows;



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



  checksum256 get_trxid()
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

 public:

  // inline notifications
  struct receipt {
    uint64_t       item_id;
    name           seller;
    string         sku;
    asset          price;
    name           buyer;
    checksum256    payment_trxid;

    receipt(const pos::sku& skuobj, const pos::stockitem& item) {
      item_id = item.id;
      seller = skuobj.seller;
      sku = skuobj.sku;
      price = item.price;
      buyer = item.buyer;
      payment_trxid = item.trxid;
    }

    receipt() {}
  };
  EOSLIB_SERIALIZE(receipt, (item_id)(seller)(sku)(price)(buyer)(payment_trxid));

  ACTION payreceipt(receipt& receipt)
  {
    require_auth(_self);
  }

  ACTION finalreceipt(receipt& receipt)
  {
    require_auth(_self);
    require_recipient(receipt.seller);
  }

};
