#!/usr/bin/env node

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


const config    = require('config');
const fetch     = require('node-fetch');

const { Api, JsonRpc, RpcError } = require('eosjs');
const { JsSignatureProvider } = require('eosjs/dist/eosjs-jssig');
const { TextEncoder, TextDecoder } = require('util');



const url        = config.get('url');
const posacc  = config.get('posacc');
const oracleacc   = config.get('oracleacc');
const oraclekey   = config.get('oraclekey');

const timer_keepalive = config.get('timer.keepalive');
const timer_recheck = config.get('timer.recheck');
const timer_irrev_variance = config.get('timer.irrev_variance');


const sigProvider = new JsSignatureProvider([oraclekey]);
const rpc = new JsonRpc(url, { fetch });
const api = new Api({rpc: rpc, signatureProvider: sigProvider,
                     textDecoder: new TextDecoder(), textEncoder: new TextEncoder()});

console.log("Started POS oracle using " + url);

var known_last_sale = 0;

setInterval(keepaalive, timer_keepalive);
setInterval(recheck, timer_recheck);
recheck();


async function keepaalive() {
    console.log('keepalive');
}


async function recheck() {
    let last_irrev = await get_last_irrev();
    let last_sale = await get_last_sale();
    console.log("last_irrev: " + last_irrev + ", last_sale: " + last_sale);
    
    if( last_sale > known_last_sale && last_sale > last_irrev ) {
        let data = await retrieve_irrev();
        if( data.irrev_timestamp > last_irrev ) {
            send_orairrev(data.irrev_block, data.irrev_timestamp);
            if( last_sale > data.irrev_timestamp ) {
                setTimeout(recheck, last_sale - data.irrev_timestamp + timer_irrev_variance);
            }
        }
    }

    known_last_sale = last_sale;
}


async function get_last_sale() {
    let response = await fetch(url + '/v1/chain/get_table_rows', {
        method: 'post',
        body:    JSON.stringify({
            json: 'true',
            code: posacc,
            scope: '0',
            table: 'props',
            index_position: '1',
            key_type: 'name',
            lower_bound: 'lastsale',
            upper_bound: 'lastsale',
            limit: 1
        }),
        headers: { 'Content-Type': 'application/json' },
    });

    let data = await response.json();
    return (data.rows.length > 0) ? data.rows[0].val_uint/1000 : 0;
}


async function get_last_irrev() {
    let response = await fetch(url + '/v1/chain/get_table_rows', {
        method: 'post',
        body:    JSON.stringify({
            json: 'true',
            code: posacc,
            scope: '0',
            table: 'props',
            index_position: '1',
            key_type: 'name',
            lower_bound: 'irrevtime',
            upper_bound: 'irrevtime',
            limit: 1
        }),
        headers: { 'Content-Type': 'application/json' },
    });

    let data = await response.json();
    return (data.rows.length > 0) ? data.rows[0].val_uint/1000 : 0;
}


async function retrieve_irrev() {
    let response = await fetch(url + '/v1/chain/get_info', {
        method: 'post',
        headers: { 'Content-Type': 'application/json' },
    });

    let data = await response.json();
    let irrev_block = data.last_irreversible_block_num;
    console.log('irrev_block: ' + irrev_block);

    response = await fetch(url + '/v1/chain/get_block', {
        method: 'post',
        body:    JSON.stringify({
            block_num_or_id: irrev_block
        }),
        headers: { 'Content-Type': 'application/json' },
    });

    data = await response.json();
    let irrev_timestamp_string = data.timestamp;
    let irrev_timestamp = Date.parse(irrev_timestamp_string + 'Z');
    console.log("irrev_timestamp: " + irrev_timestamp + " " + irrev_timestamp_string);

    return {irrev_block: irrev_block,
            irrev_timestamp_string: irrev_timestamp_string,
            irrev_timestamp: irrev_timestamp};
}
            


async function send_orairrev(irrev_block, irrev_timestamp) {
    try {

        let ts = new Date(irrev_timestamp);
        let tsstring = ts.toISOString().slice(0, -1); // cut the trailing Z from ISO string
        
        const result = await api.transact(
            {
                actions:
                [
                    {
                        account: posacc,
                        name: 'orairrev',
                        authorization: [{
                            actor: oracleacc,
                            permission: 'active'} ],
                        data: {
                            irrev_block: irrev_block,
                            irrev_timestamp: tsstring
                        },
                    }
                ]
            },
            {
                blocksBehind: 10,
                expireSeconds: 60
            }
        );
        console.info('orairrev transaction ID: ', result.transaction_id);
    } catch (e) {
        console.error('ERROR: ' + e);
    }

}




/*
 Local Variables:
 mode: javascript
 indent-tabs-mode: nil
 End:
*/
