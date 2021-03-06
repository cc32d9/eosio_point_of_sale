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
const timer_poll = config.get('timer.poll');
const timer_followup = config.get('timer.followup');
const timer_irrev_variance = config.get('timer.irrev_variance');


const sigProvider = new JsSignatureProvider([oraclekey]);
const rpc = new JsonRpc(url, { fetch });
const api = new Api({rpc: rpc, signatureProvider: sigProvider,
                     textDecoder: new TextDecoder(), textEncoder: new TextEncoder()});

console.log("Started POS oracle using " + url);

var known_last_sale = 0;

setInterval(keepaalive, timer_keepalive);
setInterval(poll_changes, timer_poll);
poll_changes();


async function keepaalive() {
    console.log('keepalive');
}

var last_poll_msg = '';

async function poll_changes() {
    let last_sale = await get_last_sale();
    let new_poll_msg = 'known_last_sale: ' + known_last_sale + ', last_sale: ' + last_sale;
    if( new_poll_msg != last_poll_msg ) {
        console.log(new_poll_msg);
        last_poll_msg = new_poll_msg;
    }
    if( last_sale > known_last_sale ) {
        updoracle(last_sale);
        known_last_sale = last_sale;
    }
}


async function updoracle(last_sale) {
    if( ! last_sale ) {
        last_sale = await get_last_sale();
    }

    let last_irrev = await get_last_irrev();
    console.log("last_irrev: " + last_irrev + ", last_sale: " + last_sale);
    let data = await retrieve_irrev();

    if( data.irrev_timestamp > last_irrev && last_sale > last_irrev ) {
        send_orairrev(data.irrev_block, data.irrev_timestamp);

        if( last_sale > data.irrev_timestamp ) {
            let newtimeout = last_sale - data.irrev_timestamp + timer_irrev_variance;
            console.log('setting updoracle timeout: ' + newtimeout);
            setTimeout(updoracle, newtimeout);
        }
        else {
            console.log('scheduling updoracle follow-up in: ' + timer_followup);
            setTimeout(updoracle, timer_followup);
        }
    }
}



async function get_last_sale() {
    let ret = 0;
    try {
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

        if( response.ok ) {
            let data = await response.json();
            if( data.rows.length > 0 ) {
                ret = data.rows[0].val_uint/1000;
            }
        }
        else {
            console.error('HTTP error: ' + await response.text());
        }        
    } catch (e) {
        console.error('ERROR: ' + e);
    }
    return ret;
}


async function get_last_irrev() {
    let ret = 0;
    try {
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

        if( response.ok ) {
            let data = await response.json();
            if( data.rows.length > 0 ) {
                ret = data.rows[0].val_uint/1000;
            }
        }
        else {
            console.error('HTTP error: ' + await response.text());
        }        
    } catch (e) {
        console.error('ERROR: ' + e);
    }
    return ret;
}


async function retrieve_irrev() {
    let irrev_block = 0;
    let irrev_timestamp_string;
    let irrev_timestamp = 0;
    try {
        let response = await fetch(url + '/v1/chain/get_info', {
            method: 'post',
            headers: { 'Content-Type': 'application/json' },
        });

        if( response.ok ) {
            let data = await response.json();
            irrev_block = data.last_irreversible_block_num;
            console.log('irrev_block: ' + irrev_block);

            response = await fetch(url + '/v1/chain/get_block', {
                method: 'post',
                body:    JSON.stringify({
                    block_num_or_id: irrev_block
                }),
                headers: { 'Content-Type': 'application/json' },
            });

            if( response.ok ) {
                data = await response.json();
                irrev_timestamp_string = data.timestamp;
                irrev_timestamp = Date.parse(irrev_timestamp_string + 'Z');
                console.log("irrev_timestamp: " + irrev_timestamp + " " + irrev_timestamp_string);
            }
            else {
            console.error('HTTP error: ' + await response.text());
            }        
        }
        else {
            console.error('HTTP error: ' + await response.text());
        }        
    } catch (e) {
        console.error('ERROR: ' + e);
    }
    
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
