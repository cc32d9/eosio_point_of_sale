/*
  Copyright 202` cc32d9@gmail.com

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
#include <eosio/string.hpp>


using namespace eosio;
using eosio::string;
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

    checksum256 hash = sha256(sku.cbegin(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    check( hashidx.find(hash) == hashidx.end(), "An SKU with this name already exists");

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
                                     row.c_skus = 1;
                                   });
    }
    else {
      _sellercntrs.modify( *cntrs_itr, seller, [&]( auto& row ) {
                                                 row.c_skus++;
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
    checksum256 hash = sha256(sku.cbegin(), sku.size());
    skus _skus(_self, 0);
    auto hashidx = _skus.get_index<name("skuhash")>();
    auto hashitr = hashidx.find(hash);
    check( hashitr != hashidx.end(), "Cannot find an SKU with such a name");

    require_auth(hashitr->seller);
    _add_stock(hashitr->seller, hashitr->id, count);
  }









  // incoming payment. Memo must match an SKU

  [[eosio::on_notify("*::transfer")]] void on_payment (name from, name to, asset quantity, string memo) {
    if(to == _self) {
      checksum256 hash = sha256(memo.cbegin(), memo.size());
      skus _skus(_self, 0);
      auto hashidx = _skus.get_index<name("skuhash")>();
      auto hashitr = hashidx.find(hash);
      check(hashitr != hashidx.end(), "Cannot find an SKU with such a name");
      check(hashitr->tkcontract == get_first_receiver(),
            "Wrong token contract, expected: " + hashitr->tkcontract.to_string());
      check(hashitr->price == quantity,
            "Wrong amount or currency, expected: " + hashitr->price.to_string());

      uint64_t skuid = hashitr->id;

      stockitems _stockitems(_self, hashitr->seller.value);
      auto itemidx = _stockitems.get_index<name("skuid")>();
      auto item_itr = itemidx.lower_bound(skuid);
      check(item_itr != itemidx.end(), "This SKU is sold out");

      // mark the item as sold
      _stockitems.modify(*item_itr, same_payer, [&]( auto& row ) {
                                                  row.sold_on = current_time_point();
                                                  row.trxid = get_trxid();
                                                });

      // update statistics
      sellercntrs _sellercntrs(_self, 0);
      auto ctr_itr = _sellercntrs.find(hashitr->seller.value);
      check( ctr_itr != _sellercntrs.end(), "This should never happen 3");
      _sellercntrs.modify(*ctr_itr, same_payer, [&]( auto& row ) {
                                                  row.c_items_onsale--;
                                                });

      stockrows _stockrows(_self, 0);
      auto stock_itr = _stockrows.find(skuid);
      check(stock_itr != _stockrows.end(), "This should never happen 4");
      _stockrows.modify(*stock_itr, same_payer, [&]( auto& row ) {
                                                  row.c_items_onsale--;
                                                });
    }
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
    uint64_t       c_items_onsale = 0; // number of items available on sale
    auto primary_key()const { return skuid; }
  };

  typedef eosio::multi_index<name("stock"), stockrow> stockrows;


  // Seller counters; scope=0
  struct [[eosio::table("sellercntrs")]] sellercntr {
    name       seller;
    uint64_t   next_item_id = 1;
    uint64_t   c_skus;  // number of SKUs on sale
    uint64_t   c_items_onsale = 0; // number of items on sale
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
    checksum256     trxid;
    auto primary_key()const { return id; }
    uint64_t get_skuid()const { return (sold_on.elapsed.count() == 0) ? skuid : 0; }
    uint64_t get_sold_on()const { return sold_on.elapsed.count(); }
  };

  typedef eosio::multi_index<
    name("stockitems"), stockitem,
    indexed_by<name("skuid"), const_mem_fun<stockitem, uint64_t, &stockitem::get_skuid>>,
    indexed_by<name("soldon"), const_mem_fun<stockitem, uint64_t, &stockitem::get_sold_on>>
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
                                                row.c_items_onsale += count;
                                                row.next_item_id = itemid;
                                              });

    stockrows _stockrows(_self, 0);
    auto stock_itr = _stockrows.find(skuid);
    check(stock_itr != _stockrows.end(), "This should never happen 2");
    _stockrows.modify(*stock_itr, same_payer, [&]( auto& row ) {
                                                row.c_items_onsale += count;
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
            transfer_args  {
            .from=_self, .to=recipient,
                              .quantity=x.quantity, .memo=memo
                              }
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
