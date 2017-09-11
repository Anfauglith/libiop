#include <iop/net.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>

#include <iop/buffer.h>
#include <iop/chainparams.h>
#include <iop/cstr.h>
#include <iop/hash.h>
#include <iop/protocol.h>
#include <iop/serialize.h>
#include <iop/utils.h>

#ifdef _WIN32
#include <getopt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <time.h>

#define UNUSED(x) (void)(x)

static const int IOP_PERIODICAL_NODE_TIMER_S = 3;
static const int IOP_PING_INTERVAL_S = 180;
static const int IOP_CONNECT_TIMEOUT_S = 10;

int net_write_log_printf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    printf("DEBUG :");
    vprintf(format, args);
    va_end(args);
    return 1;
}

int net_write_log_null(const char* format, ...)
{
    UNUSED(format);
    return 1;
}

void read_cb(struct bufferevent* bev, void* ctx)
{
    struct evbuffer* input = bufferevent_get_input(bev);
    if (!input)
        return;

    size_t length = evbuffer_get_length(input);

    iop_node* node = (iop_node*)ctx;

    if ((node->state & NODE_CONNECTED) != NODE_CONNECTED) {
        // ignore messages from disconnected peers
        return;
    }
    // expand the cstring buffer if required
    cstr_alloc_minsize(node->recvBuffer, node->recvBuffer->len + length);

    // copy direct to cstring, avoid another heap buffer
    evbuffer_copyout(input, node->recvBuffer->str + node->recvBuffer->len, length);
    node->recvBuffer->len += length;

    // drain the event buffer
    evbuffer_drain(input, length);

    struct const_buffer buf = {node->recvBuffer->str, node->recvBuffer->len};
    iop_p2p_msg_hdr hdr;
    char* read_upto = NULL;

    do {
        //check if message is complete
        if (buf.len < IOP_P2P_HDRSZ) {
            break;
        }

        iop_p2p_deser_msghdr(&hdr, &buf);
        if (hdr.data_len > IOP_MAX_P2P_MSG_SIZE) {
            // check for invalid message lengths
            iop_node_missbehave(node);
            return;
        }
        if (buf.len < hdr.data_len) {
            //if we haven't read the whole message, continue and wait for the next chunk
            break;
        }
        if (buf.len >= hdr.data_len) {
            //at least one message is complete

            struct const_buffer cmd_data_buf = {buf.p, buf.len};
            if ((node->state & NODE_CONNECTED) != NODE_CONNECTED) {
                // ignore messages from disconnected peers
                return;
            }
            iop_node_parse_message(node, &hdr, &cmd_data_buf);

            //skip the size of the whole message
            buf.p = (const unsigned char*)buf.p + hdr.data_len;
            buf.len -= hdr.data_len;

            read_upto = (void*)buf.p;
        }
        if (buf.len == 0) {
            //if we have "consumed" the whole buffer
            node->recvBuffer->len = 0;
            break;
        }
    } while (1);

    if (read_upto != NULL && node->recvBuffer->len != 0 && read_upto != (node->recvBuffer->str + node->recvBuffer->len)) {
        char* end = node->recvBuffer->str + node->recvBuffer->len;
        size_t available_chunk_data = end - read_upto;
        //partial message
        cstring* tmp = cstr_new_buf(read_upto, available_chunk_data);
        cstr_free(node->recvBuffer, true);
        node->recvBuffer = tmp;
    }
}

void write_cb(struct bufferevent* ev, void* ctx)
{
    UNUSED(ev);
    UNUSED(ctx);
}

void node_periodical_timer(int fd, short event, void* ctx)
{
    UNUSED(fd);
    UNUSED(event);
    iop_node* node = (iop_node*)ctx;
    uint64_t now = time(NULL);

    /* pass data to the callback and give it a chance to cancle the call */
    if (node->nodegroup->periodic_timer_cb)
        if (!node->nodegroup->periodic_timer_cb(node, &now))
            return;

    if (node->time_started_con + IOP_CONNECT_TIMEOUT_S < now && ((node->state & NODE_CONNECTING) == NODE_CONNECTING)) {
        node->state = 0;
        node->time_started_con = 0;
        node->state |= NODE_ERRORED;
        node->state |= NODE_TIMEOUT;
        iop_node_connection_state_changed(node);
    }

    if (((node->state & NODE_CONNECTED) == NODE_CONNECTED) && node->lastping + IOP_PING_INTERVAL_S < now) {
        //time for a ping
        uint64_t nonce;
        iop_cheap_random_bytes((uint8_t*)&nonce, sizeof(nonce));
        cstring* pingmsg = iop_p2p_message_new(node->nodegroup->chainparams->netmagic, IOP_MSG_PING, &nonce, sizeof(nonce));
        iop_node_send(node, pingmsg);
        cstr_free(pingmsg, true);
        node->lastping = now;
    }
}

void event_cb(struct bufferevent* ev, short type, void* ctx)
{
    UNUSED(ev);
    iop_node* node = (iop_node*)ctx;
    node->nodegroup->log_write_cb("Event callback on node %d\n", node->nodeid);

    if (((type & BEV_EVENT_TIMEOUT) != 0) && ((node->state & NODE_CONNECTING) == NODE_CONNECTING)) {
        node->nodegroup->log_write_cb("Timout connecting to node %d.\n", node->nodeid);
        node->state = 0;
        node->state |= NODE_ERRORED;
        node->state |= NODE_TIMEOUT;
        iop_node_connection_state_changed(node);
    } else if (((type & BEV_EVENT_EOF) != 0) ||
               ((type & BEV_EVENT_ERROR) != 0)) {
        node->state = 0;
        node->state |= NODE_ERRORED;
        node->state |= NODE_DISCONNECTED;
        if ((type & BEV_EVENT_EOF) != 0) {
            node->nodegroup->log_write_cb("Disconnected from the remote peer %d.\n", node->nodeid);
            node->state |= NODE_DISCONNECTED_FROM_REMOTE_PEER;
        }
        else {
            node->nodegroup->log_write_cb("Error connecting to node %d.\n", node->nodeid);
        }
        iop_node_connection_state_changed(node);
    } else if (type & BEV_EVENT_CONNECTED) {
        node->nodegroup->log_write_cb("Successfull connected to node %d.\n", node->nodeid);
        node->state |= NODE_CONNECTED;
        node->state &= ~NODE_CONNECTING;
        node->state &= ~NODE_ERRORED;
        iop_node_connection_state_changed(node);
        /* if callback is set, fire */
    }
    node->nodegroup->log_write_cb("Connected nodes: %d\n", iop_node_group_amount_of_connected_nodes(node->nodegroup, NODE_CONNECTED));
}

iop_node* iop_node_new()
{
    iop_node* node;
    node = iop_calloc(1, sizeof(*node));
    node->version_handshake = false;
    node->state = 0;
    node->nonce = 0;
    node->services = 0;
    node->lastping = 0;
    node->time_started_con = 0;
    node->time_last_request = 0;
    iop_hash_clear(node->last_requested_inv);

    node->recvBuffer = cstr_new_sz(IOP_P2P_MESSAGE_CHUNK_SIZE);
    node->hints = 0;
    return node;
}

iop_bool iop_node_set_ipport(iop_node* node, const char* ipport)
{
    int outlen = (int)sizeof(node->addr);

    //return true in case of success (0 == no error)
    return (evutil_parse_sockaddr_port(ipport, &node->addr, &outlen) == 0);
}

void iop_node_release_events(iop_node* node)
{
    if (node->event_bev) {
        bufferevent_free(node->event_bev);
        node->event_bev = NULL;
    }

    if (node->timer_event) {
        event_del(node->timer_event);
        event_free(node->timer_event);
        node->timer_event = NULL;
    }
}

iop_bool iop_node_missbehave(iop_node* node)
{
    node->nodegroup->log_write_cb("Mark node %d as missbehaved\n", node->nodeid);
    node->state |= NODE_MISSBEHAVED;
    iop_node_connection_state_changed(node);
    return 0;
}

void iop_node_disconnect(iop_node* node)
{
    if ((node->state & NODE_CONNECTED) == NODE_CONNECTED || (node->state & NODE_CONNECTING) == NODE_CONNECTING) {
        node->nodegroup->log_write_cb("Disconnect node %d\n", node->nodeid);
    }
    /* release buffer and timer event */
    iop_node_release_events(node);

    node->state &= ~NODE_CONNECTING;
    node->state &= ~NODE_CONNECTED;
    node->state |= NODE_DISCONNECTED;

    node->time_started_con = 0;
}

void iop_node_free(iop_node* node)
{
    iop_node_disconnect(node);
    cstr_free(node->recvBuffer, true);
    iop_free(node);
}

void iop_node_free_cb(void* obj)
{
    iop_node* node = (iop_node*)obj;
    iop_node_free(node);
}

iop_node_group* iop_node_group_new(const iop_chainparams* chainparams)
{
    iop_node_group* node_group;
    node_group = iop_calloc(1, sizeof(*node_group));
    node_group->event_base = event_base_new();
    if (!node_group->event_base) {
        return NULL;
    };

    node_group->nodes = vector_new(1, iop_node_free_cb);
    node_group->chainparams = (chainparams ? chainparams : &iop_chainparams_main);
    node_group->parse_cmd_cb = NULL;
    strcpy(node_group->clientstr, "libiop 0.1");

    /* nullify callbacks */
    node_group->postcmd_cb = NULL;
    node_group->node_connection_state_changed_cb = NULL;
    node_group->should_connect_to_more_nodes_cb = NULL;
    node_group->handshake_done_cb = NULL;
    node_group->log_write_cb = net_write_log_null;
    node_group->desired_amount_connected_nodes = 3;

    return node_group;
}

void iop_node_group_shutdown(iop_node_group *group) {
    for (size_t i = 0; i < group->nodes->len; i++) {
        iop_node* node = vector_idx(group->nodes, i);
        iop_node_disconnect(node);
    }
}

void iop_node_group_free(iop_node_group* group)
{
    if (!group)
        return;

    if (group->event_base) {
        event_base_free(group->event_base);
    }

    if (group->nodes) {
        vector_free(group->nodes, true);
    }
    iop_free(group);
}

void iop_node_group_event_loop(iop_node_group* group)
{
    event_base_dispatch(group->event_base);
}

void iop_node_group_add_node(iop_node_group* group, iop_node* node)
{
    vector_add(group->nodes, node);
    node->nodegroup = group;
    node->nodeid = group->nodes->len;
}

int iop_node_group_amount_of_connected_nodes(iop_node_group* group, enum NODE_STATE state)
{
    int cnt = 0;
    for (size_t i = 0; i < group->nodes->len; i++) {
        iop_node* node = vector_idx(group->nodes, i);
        if ((node->state & state) == state)
            cnt++;
    }
    return cnt;
}

iop_bool iop_node_group_connect_next_nodes(iop_node_group* group)
{
    iop_bool connected_at_least_to_one_node = false;
    int connect_amount = group->desired_amount_connected_nodes - iop_node_group_amount_of_connected_nodes(group, NODE_CONNECTED);
    if (connect_amount <= 0)
        return true;

    connect_amount = connect_amount*3;
    /* search for a potential node that has not errored and is not connected or in connecting state */
    for (size_t i = 0; i < group->nodes->len; i++) {
        iop_node* node = vector_idx(group->nodes, i);
        if (
            !((node->state & NODE_CONNECTED) == NODE_CONNECTED) &&
            !((node->state & NODE_CONNECTING) == NODE_CONNECTING) &&
            !((node->state & NODE_DISCONNECTED) == NODE_DISCONNECTED) &&
            !((node->state & NODE_ERRORED) == NODE_ERRORED)) {
            /* setup buffer event */
            node->event_bev = bufferevent_socket_new(group->event_base, -1, BEV_OPT_CLOSE_ON_FREE);
            bufferevent_setcb(node->event_bev, read_cb, write_cb, event_cb, node);
            bufferevent_enable(node->event_bev, EV_READ | EV_WRITE);
            if (bufferevent_socket_connect(node->event_bev, (struct sockaddr*)&node->addr, sizeof(node->addr)) < 0) {
                /* Error starting connection */
                if (node->event_bev)
                    bufferevent_free(node->event_bev);
                return false;
            }

            /* setup periodic timer */
            node->time_started_con = time(NULL);
            struct timeval tv;
            tv.tv_sec = IOP_PERIODICAL_NODE_TIMER_S;
            tv.tv_usec = 0;
            node->timer_event = event_new(group->event_base, 0, EV_TIMEOUT | EV_PERSIST, node_periodical_timer,
                                          (void*)node);
            event_add(node->timer_event, &tv);
            node->state |= NODE_CONNECTING;
            connected_at_least_to_one_node = true;

            node->nodegroup->log_write_cb("Trying to connect to %d...\n", node->nodeid);

            connect_amount--;
            if (connect_amount <= 0)
                return true;
        }
    }
    /* node group misses a node to connect to */
    return connected_at_least_to_one_node;
}

void iop_node_connection_state_changed(iop_node* node)
{
    if (node->nodegroup->node_connection_state_changed_cb)
        node->nodegroup->node_connection_state_changed_cb(node);

    if ((node->state & NODE_ERRORED) == NODE_ERRORED) {
        iop_node_release_events(node);

        /* connect to more nodes are required */
        iop_bool should_connect_to_more_nodes = true;
        if (node->nodegroup->should_connect_to_more_nodes_cb)
            should_connect_to_more_nodes = node->nodegroup->should_connect_to_more_nodes_cb(node);

        if (should_connect_to_more_nodes && (iop_node_group_amount_of_connected_nodes(node->nodegroup, NODE_CONNECTED) + iop_node_group_amount_of_connected_nodes(node->nodegroup, NODE_CONNECTING) < node->nodegroup->desired_amount_connected_nodes))
            iop_node_group_connect_next_nodes(node->nodegroup);
    }
    if ((node->state & NODE_MISSBEHAVED) == NODE_MISSBEHAVED) {
        if ((node->state & NODE_CONNECTED) == NODE_CONNECTED || (node->state & NODE_CONNECTING) == NODE_CONNECTING) {
            iop_node_disconnect(node);
        }
    } else
        iop_node_send_version(node);
}

void iop_node_send(iop_node* node, cstring* data)
{
    if ((node->state & NODE_CONNECTED) != NODE_CONNECTED)
        return;

    bufferevent_write(node->event_bev, data->str, data->len);
    char* dummy = data->str + 4;
    node->nodegroup->log_write_cb("sending message to node %d: %s\n", node->nodeid, dummy);
}

void iop_node_send_version(iop_node* node)
{
    if (!node)
        return;

    /* get new string buffer */
    cstring* version_msg_cstr = cstr_new_sz(256);

    /* copy socket_addr to p2p addr */
    iop_p2p_address fromAddr;
    iop_p2p_address_init(&fromAddr);
    iop_p2p_address toAddr;
    iop_p2p_address_init(&toAddr);
    iop_addr_to_p2paddr(&node->addr, &toAddr);

    /* create a version message struct */
    iop_p2p_version_msg version_msg;
    memset(&version_msg, 0, sizeof(version_msg));

    /* create a serialized version message */
    iop_p2p_msg_version_init(&version_msg, &fromAddr, &toAddr, node->nodegroup->clientstr, true);
    iop_p2p_msg_version_ser(&version_msg, version_msg_cstr);

    /* create p2p message */
    cstring* p2p_msg = iop_p2p_message_new(node->nodegroup->chainparams->netmagic, IOP_MSG_VERSION, version_msg_cstr->str, version_msg_cstr->len);

    /* send message */
    iop_node_send(node, p2p_msg);

    /* cleanup */
    cstr_free(version_msg_cstr, true);
    cstr_free(p2p_msg, true);
}

int iop_node_parse_message(iop_node* node, iop_p2p_msg_hdr* hdr, struct const_buffer* buf)
{
    node->nodegroup->log_write_cb("received command from node %d: %s\n", node->nodeid, hdr->command);
    if (memcmp(hdr->netmagic, node->nodegroup->chainparams->netmagic, sizeof(node->nodegroup->chainparams->netmagic)) != 0) {
        return iop_node_missbehave(node);
    }

    /* send the header and buffer to the possible callback */
    /* callback can decide to run the internal base message logic */
    if (!node->nodegroup->parse_cmd_cb || node->nodegroup->parse_cmd_cb(node, hdr, buf)) {
        if (strcmp(hdr->command, IOP_MSG_VERSION) == 0) {
            iop_p2p_version_msg v_msg_check;
            if (!iop_p2p_msg_version_deser(&v_msg_check, buf)) {
                return iop_node_missbehave(node);
            }
            if ((v_msg_check.services & IOP_NODE_NETWORK) != IOP_NODE_NETWORK) {
                iop_node_disconnect(node);
            }
            node->bestknownheight = v_msg_check.start_height;
            node->nodegroup->log_write_cb("Connected to node %d: %s (%d)\n", node->nodeid, v_msg_check.useragent, v_msg_check.start_height);
            /* confirm version via verack */
            cstring* verack = iop_p2p_message_new(node->nodegroup->chainparams->netmagic, IOP_MSG_VERACK, NULL, 0);
            iop_node_send(node, verack);
            cstr_free(verack, true);
        } else if (strcmp(hdr->command, IOP_MSG_VERACK) == 0) {
            /* complete handshake if verack has been received */
            node->version_handshake = true;

            /* execute callback and inform that the node is ready for custom message logic */
            if (node->nodegroup->handshake_done_cb)
                node->nodegroup->handshake_done_cb(node);
        } else if (strcmp(hdr->command, IOP_MSG_PING) == 0) {
            /* response pings */
            uint64_t nonce = 0;
            if (!deser_u64(&nonce, buf)) {
                return iop_node_missbehave(node);
            }
            cstring* pongmsg = iop_p2p_message_new(node->nodegroup->chainparams->netmagic, IOP_MSG_PONG, &nonce, 8);
            iop_node_send(node, pongmsg);
            cstr_free(pongmsg, true);
        }
    }

    /* pass data to the "post command" callback */
    if (node->nodegroup->postcmd_cb)
        node->nodegroup->postcmd_cb(node, hdr, buf);

    return true;
}

/* utility function to get peers (ips/port as char*) from a seed */
int iop_get_peers_from_dns(const char* seed, vector* ips_out, int port, int family)
{
    if (!seed || !ips_out || (family != AF_INET && family != AF_INET6) || port > 99999) {
        return 0;
    }
    char def_port[6] = {0};
    sprintf(def_port, "%d", port);
    struct evutil_addrinfo hints, *aiTrav = NULL, *aiRes = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    int err = evutil_getaddrinfo(seed, NULL, &hints, &aiRes);
    if (err)
        return 0;

    aiTrav = aiRes;
    while (aiTrav != NULL) {
        int maxlen = 256;
        char* ipaddr = iop_calloc(1, maxlen);
        if (aiTrav->ai_family == AF_INET) {
            assert(aiTrav->ai_addrlen >= sizeof(struct sockaddr_in));
            evutil_inet_ntop(aiTrav->ai_family, &((struct sockaddr_in*)(aiTrav->ai_addr))->sin_addr, ipaddr, maxlen);
        }

        if (aiTrav->ai_family == AF_INET6) {
            assert(aiTrav->ai_addrlen >= sizeof(struct sockaddr_in6));
            evutil_inet_ntop(aiTrav->ai_family, &((struct sockaddr_in6*)(aiTrav->ai_addr))->sin6_addr, ipaddr, maxlen);
        }

        memcpy(ipaddr + strlen(ipaddr), ":", 1);
        memcpy(ipaddr + strlen(ipaddr), def_port, strlen(def_port));

        vector_add(ips_out, ipaddr);

        aiTrav = aiTrav->ai_next;
    }
    evutil_freeaddrinfo(aiRes);
    return ips_out->len;
}

iop_bool iop_node_group_add_peers_by_ip_or_seed(iop_node_group *group, const char *ips) {
    if (ips == NULL) {
        /* === DNS QUERY === */
        /* get a couple of peers from a seed */
        vector* ips_dns = vector_new(10, free);
        const iop_dns_seed seed = group->chainparams->dnsseeds[0];
        if (strlen(seed.domain) == 0) {
            return false;
        }
        /* todo: make sure we have enought peers, eventually */
        /* call another seeder */
        iop_get_peers_from_dns(seed.domain, ips_dns, group->chainparams->default_port, AF_INET);
        for (unsigned int i = 0; i < ips_dns->len; i++) {
            char* ip = (char*)vector_idx(ips_dns, i);

            /* create a node */
            iop_node* node = iop_node_new();
            if (iop_node_set_ipport(node, ip) > 0) {
                /* add the node to the group */
                iop_node_group_add_node(group, node);
            }
        }
        vector_free(ips_dns, true);
    } else {
        // add comma seperated ips (nodes)
        char working_str[64];
        memset(working_str, 0, sizeof(working_str));
        size_t offset = 0;
        for (unsigned int i = 0; i <= strlen(ips); i++) {
            if (i == strlen(ips) || ips[i] == ',') {
                iop_node* node = iop_node_new();
                if (iop_node_set_ipport(node, working_str) > 0) {
                    iop_node_group_add_node(group, node);
                }
                offset = 0;
                memset(working_str, 0, sizeof(working_str));
            } else if (ips[i] != ' ' && offset < sizeof(working_str)) {
                working_str[offset] = ips[i];
                offset++;
            }
        }
    }
    return true;
}
