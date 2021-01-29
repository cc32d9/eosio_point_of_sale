# EOSIO Point of Sale contract

The goal of this project is to provide a base layer for all kinds of
e-commerce and marketplaces. Anyone can list their products, and
define the prices in their preferred currency.

The contract provides all on-chain means for the seller to be
independent from any history services. The seller can track the status
of their items and payments by querying the contract tables directly.

The contract insists on exact amounts and memo text in incoming
payments. Once the payment becomes irreversible, the contract allows
the buyer to claim the tokens. The seller also receives a detailed
receipt about every sold item, so the receipt action can trigger
additional activity, such as sending an NFT token.

The contract also prevents the buyer from paying twice by accident:
the same SKU can only be bought by the same buyer after the first
payment becomes irreversible and is claimed by the seller.

The contract deducts 0.5% fee from the sales, to compensate for the
infrastructure and development costs.

In addition, the contract provides two functions for the the seller's
convenience: a possibility to track the item status after the payment
is received; and a possibility to challenge the user and verify that
he or she controls a specific EOSIO account.


It is deployed under account name `saleterminal` on the following
EOSIO networks:

* Telos Testnet

* WAX Testnet

* Jungle Testnet

Deployment on production chains is coming soon.


## Terminology and objects

### Stock Keeping Unit (SKU)

SKU defines a product on sale. It is characterized by the following
attributes:

* `sku`: a unique string, normally a combination of letters and
  numbers, identifying the product in seller's inventory. It makes
  sense to prefix the string with the seller abbreviation, for better
  clarity. The buyer will need to specify this value exactly in the
  payment memo, so it should be easy to type in. Example:
  `aliexpress:4000712168245`.

* `description`: a free text describing the product. It can be a plain
  text or a JSON object. It is solely up to the marketplace to
  standardize the format for better displaying.

* `seller`: an EOSIO account that has listed this SKU. The seller
  account will also receive the payments for sold items.

* `tkcontract`: a token contract account, such as `eosio.token` or
  `vigortoken11`.

* `price`: an asset specifying the unit price and token symbol. For
  example, `1000.0000 TLOS` or `1000.0000 VIG`. The seller can modify
  the price for unsold items.

The SKU objects are stored in `skus` table, in Scope 0.



### Stock Item

Stock item is an instance of an SKU on sale. In some cases, an SKU
would only have one item, in others an SKU can have thousands of
items.

The seller specifies the number of items in `newsku` action, and he or
she can add or remove items with `addstock` and `delstock` actions.

The life-cycle of an item is as follows:

1. It's created in contract RAM by `newsku` or `addstock` actions (the
seller provides the RAM);

2. The buyer pays the price, and the item turns into "sold" state. In
this state, it cannot be removed by `delstock` action or sold again.

3. After the purchasing transaction becomes irreversible, the seller
can claim the payment by calling the `claim` action, and the item is
removed from the contract RAM.

The following are the attributes of an item:

* `id`: a unique integer identifier among all items from a given
  seller. The identifiers never repeat.

* `skuid`: an identifier for the SKU object in `skus` table.

* `sold_on`: a timestamp of purchasing transaction, or zero for unsold items.

* `price`: purchase price. The seller may change the price of an SKU,
  but he or she can never change the price of a sold item.

* `buyer`: EOSIO account of the payer.

* `trxid`: transaction ID of the purchase transfer.

Items are stored in `stockitems` table with seller account as scope.


### Seller

Seller is an EOSIO account that creates SKU and item objects, and
receives the payments from sold items.

The seller information is stored in two tables, both using Scope 0.

`sellerinfo` table contains a static information about the seller:

* `company`: a string describing the seller company;

* `website`: a URL to the seller's web site.

* `tracking`: a Boolean flag that defines if post-sale item tracking
  is needed.

`sellercntrs` table contains variable information related to the
seller's items:

* `next_item_id`: next identifier for a stock item;

* `skus`: total number of SKUs that the seller defined;

* `items_onsale`: total number of unsold items;

* `last_sale`: time stamp of the latest purchase.


### Tracking table

The tracking table is an auxiliary tool helping the seller keep track
of their items.

When the seller calls `claim` action and if `tracking` parameter is
set to true for the seller, their items are automatically added to the
tracking table with the state set to `paymntrcvd` and memo text set to
"Payment received".

Other states are up to the seller to define to match their accounting
and shipping process.

The `tracking` table stores the information in scope set to seller
account, and primary key is the item ID from stock items table. Row
attributes:

* `itemid`: unique item identifier, copied from `stockitems` table;

* `skuid`: SKU identifier in `skus` table;

* `sold_on`: time point of payment;

* `price`: selling price;

* `buyer`: the buyer account;

* `tracking_state`: state of the item;

* `memo`: human readable description of the state or additional
  information, such as shipment tracking number;

* `updated_on`: timestamp of the latest state update;

* `update_trxid`: transaction ID of the latest state update.






## Seller's contract actions

The following actions are available for sellers. Anyone can register
as a seller and start offering their goods. There are no actions for
buyers, and they only need to transfer the exact amount of token and
use the SKU identifier in memo.

All needed RAM in these actions is allocated from the seller's quota.

### action: updseller

Before the seller can list their items, they need to register in the
contact. Later on, the parameters can be updated by calling this action
again.

```
  ACTION updseller(name seller, string company, string website, bool tracking)
```



### action: newsku

The action creates a new SKU in contract memory. If `count` is
positive, the action creates the stock items too.


```
  ACTION newsku(name seller, string sku, string description,
                name tkcontract, asset price, uint32_t count)
```

### action: delseller

The seller can delete itself from the contract, but only if all items
are sold or deleted, and all SKUs too.

```
  ACTION delseller(name seller)
```

### action: updskuprice

The seller can alter the price of an SKU, and it will affect all new
purchases. The sold items are not affected by this action. Only the
amount is editable, and the new price must be in the same currency
as before.

```
  ACTION updskuprice(string sku, asset price)
```


### action: updskudescr

This action modifies the description of an SKU.

```
  ACTION updskudescr(string sku, string description)
```


### action: delsku

This action deletes an SKU from contract memory, but only if all stock
items are sold or deleted.

```
  ACTION delsku(string sku)
```


### action: addstock

This action creates new items for sale.

```
  ACTION addstock(string sku, uint32_t count)
```


### action: delstock

This action removes unsold stock items from contract memory. The count
cannot exceed the number of unsold items.

```
  ACTION delstock(string sku, uint32_t count)
```


### action: claim

The seller calls this action to claim the payment for sold items. Up
to the specified count of items would be processed. If there are more
sold items in irreversible transactions, the seller can call the
`claim` action again.

The action generates separate transfers for every sold item. 0.95% of
the sell price is transferred to the seller, and 0.5% is transferred
to the fee collection account. The transfers to the seller contain a
JSON object in the memo, describing the item that has been sold.

In addition to transfers, the action sends a `finalreceipt`
notification to the seller for each sold item, supplying all the
details about the purchase. The seller can implement a smart contract
that reacts on this notification and executes some additional actions,
such as fungible or non-fungible token transfer.

```
  ACTION claim(name seller, uint32_t count)
```


### action: updtracking

The action updates the tracking state and memo for the specified sold
items:

```
  ACTION updtracking(name seller, name newstate, string memo, vector<uint64_t> itemids)
```

### action: deltracking

The action deletes the specified items from tracking table and releases RAM back to the seller.

```
  ACTION deltracking(name seller, vector<uint64_t> itemids)
```


## Challenge and response

Two actions, `challenge` and `respond` are designed for e-commerce
website operators to verify that the logged-in user controls a
specified EOSIO account.

The website backend generates a random challenge string and a random
64-bit identifier. It calls the `challenge` action and specifies
SHA256 hash of the challenge string and the account name that needs to
be verified. It also specifies the expiration time in seconds.

Once the challenge is placed on chain, the website shows the challenge
string to the logged-in user, and the user needs to send an `respond`
action with a copy of the challenge string. The smart contract
verifies that the response string matches the challenge hash.

Once the verification is successful, the contract updates the entry in
`challenges` table by setting `responded` to true, and extending the
expiration time by 10 minutes. The website can check the status of the
challenge by its ID.

Both `challenge` and `respond` actions delete up to 50 expired
challenges that were inserted previously.

Actions and the table:

```
  ACTION challenge(name challenger, uint64_t id, name account, checksum256 hash, uint32_t expire_seconds);

  ACTION respond(uint64_t id, string response);

  struct [[eosio::table("challenges")]] challenge_row {
    uint64_t        id;
    name            account;
    checksum256     challenge;
    bool            responded;
    time_point      responded_on;
    checksum256     response_trxid;
    time_point      expires_on;
    auto primary_key()const { return id; }
    uint64_t get_expires_on()const { return expires_on.elapsed.count(); }
  };

  typedef eosio::multi_index<
    name("challenges"), challenge_row,
    indexed_by<name("expires"), const_mem_fun<challenge_row, uint64_t, &challenge_row::get_expires_on>>
    > challenges;
```







## Irreversibility oracle

The oracle process runs on several independent servers, supplying the
blockchain irreversibility timestamps to the contract.


## Copyright and License

Copyright 2021 cc32d9@gmail.com

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
implied.  See the License for the specific language governing
permissions and limitations under the License.


## Donations and paid service

ETH address: `0x7137bfe007B15F05d3BF7819d28419EAFCD6501E`

EOS account: `cc32dninexxx`
