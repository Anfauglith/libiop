/*

 The MIT License (MIT)

 Copyright (c) 2017 Jonas Schnelli

 Permission is hereby granted, free of charge, to any person obtaining
 a copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 OTHER DEALINGS IN THE SOFTWARE.
 
*/

#include "libiop-config.h"

#include <iop/chainparams.h>
#include <iop/ecc.h>
#include <iop/net.h>
#include <iop/netspv.h>
#include <iop/protocol.h>
#include <iop/random.h>
#include <iop/serialize.h>
#include <iop/tool.h>
#include <iop/tx.h>
#include <iop/utils.h>
#include <iop/wallet.h>

#include <assert.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct option long_options[] =
    {
        {"testnet", no_argument, NULL, 't'},
        {"regtest", no_argument, NULL, 'r'},
        {"ips", no_argument, NULL, 'i'},
        {"debug", no_argument, NULL, 'd'},
        {"maxnodes", no_argument, NULL, 'm'},
        {"dbfile", no_argument, NULL, 'f'},
        {"continuous", no_argument, NULL, 'c'},
        {NULL, 0, NULL, 0}};

static void print_version()
{
    printf("Version: %s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
}

static void print_usage()
{
    print_version();
    printf("Usage: iop-spv (-c|continuous) (-i|-ips <ip,ip,...]>) (-m[--maxpeers] <int>) (-t[--testnet]) (-f <headersfile|0 for in mem only>) (-r[--regtest]) (-d[--debug]) (-s[--timeout] <secs>) <command>\n");
    printf("Supported commands:\n");
    printf("        scan      (scan blocks up to the tip, creates header.db file)\n");
    printf("\nExamples: \n");
    printf("Sync up to the chain tip and stores all headers in headers.db (quit once synced):\n");
    printf("> iop-spv scan\n\n");
    printf("Sync up to the chain tip and give some debug output during that process:\n");
    printf("> iop-spv -d scan\n\n");
    printf("Sync up, show debug info, don't store headers in file (only in memory), wait for new blocks:\n");
    printf("> iop-spv -d -f 0 -c scan\n\n");
}

static bool showError(const char* er)
{
    printf("Error: %s\n", er);
    return 1;
}

iop_bool spv_header_message_processed(struct iop_spv_client_ *client, iop_node *node, iop_blockindex *newtip) {
    UNUSED(client);
    UNUSED(node);
    if (newtip) {
        printf("New headers tip height %d\n", newtip->height);
    }
    return true;
}

static iop_bool quit_when_synced = true;
void spv_sync_completed(iop_spv_client* client) {
    printf("Sync completed, at height %d\n", client->headers_db->getchaintip(client->headers_db_ctx)->height);
    if (quit_when_synced) {
        iop_node_group_shutdown(client->nodegroup);
    }
    else {
        printf("Waiting for new blocks or relevant transactions...\n");
    }
}

int main(int argc, char* argv[])
{
    int ret = 0;
    int long_index = 0;
    int opt = 0;
    char* data = 0;
    char* ips = 0;
    iop_bool debug = false;
    int timeout = 15;
    int maxnodes = 10;
    char* dbfile = 0;
    const iop_chainparams* chain = &iop_chainparams_main;

    if (argc <= 1 || strlen(argv[argc - 1]) == 0 || argv[argc - 1][0] == '-') {
        /* exit if no command was provided */
        print_usage();
        exit(EXIT_FAILURE);
    }
    data = argv[argc - 1];

    /* get arguments */
    while ((opt = getopt_long_only(argc, argv, "i:ctrds:m:f:", long_options, &long_index)) != -1) {
        switch (opt) {
        case 'c':
            quit_when_synced = false;
            break;
        case 't':
            chain = &iop_chainparams_test;
            break;
        case 'r':
            chain = &iop_chainparams_regtest;
            break;
        case 'd':
            debug = true;
            break;
        case 's':
            timeout = (int)strtol(optarg, (char**)NULL, 10);
            break;
        case 'i':
            ips = optarg;
            break;
        case 'm':
            maxnodes = (int)strtol(optarg, (char**)NULL, 10);
            break;
        case 'f':
            dbfile = optarg;
            break;
        case 'v':
            print_version();
            exit(EXIT_SUCCESS);
            break;
        default:
            print_usage();
            exit(EXIT_FAILURE);
        }
    }

    if (strcmp(data, "scan") == 0) {
        iop_ecc_start();
        iop_wallet *wallet = iop_wallet_new(chain);
        int error;
        iop_bool created;
        int res = iop_wallet_load(wallet, "wallet.db", &error, &created);
        if (!res) {
            fprintf(stdout, "Loading wallet failed\n");
            exit(EXIT_FAILURE);
        }
        if (created) {
            // create a new key

            iop_hdnode node;
            uint8_t seed[32];
            assert(iop_random_bytes(seed, sizeof(seed), true));
            iop_hdnode_from_seed(seed, sizeof(seed), &node);
            iop_wallet_set_master_key_copy(wallet, &node);
        }
        else {
            // ensure we have a key
            // TODO
        }

        iop_wallet_hdnode* node = iop_wallet_next_key(wallet);
        size_t strsize = 128;
        char str[strsize];
        iop_hdnode_get_p2pkh_address(node->hdnode, chain, str, strsize);
        printf("Wallet addr: %s (child %d)\n", str, node->hdnode->child_num);

        vector *addrs = vector_new(1, free);
        iop_wallet_get_addresses(wallet, addrs);
        for (unsigned int i = 0; i < addrs->len; i++) {
            char* addr= vector_idx(addrs, i);
            printf("Addr: %s\n", addr);
        }
        vector_free(addrs, true);
        iop_spv_client* client = iop_spv_client_new(chain, debug, (dbfile && (dbfile[0] == '0' || (strlen(dbfile) > 1 && dbfile[0] == 'n' && dbfile[0] == 'o'))) ? true : false);
        client->header_message_processed = spv_header_message_processed;
        client->sync_completed = spv_sync_completed;
        client->sync_transaction = iop_wallet_check_transaction;
        client->sync_transaction_ctx = wallet;
        if (!iop_spv_client_load(client, (dbfile ? dbfile : "headers.db"))) {
            printf("Could not load or create headers database...aborting\n");
            ret = EXIT_FAILURE;
        }
        else {
            printf("Discover peers...");
            iop_spv_client_discover_peers(client, ips);
            printf("done\n");
            printf("Connecting to the p2p network...\n");
            iop_spv_client_runloop(client);
            iop_spv_client_free(client);
            ret = EXIT_SUCCESS;
        }
        iop_ecc_stop();
    }
    else {
        printf("Invalid command (use -?)\n");
        ret = EXIT_FAILURE;
    }
    return ret;
}