#include "router.h"
#include <arpa/inet.h>
#include <assert.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>

// #define PRINT_CHECK

static void s2ipv4(const char* addr, uint32_t& ipv4) {
    uint32_t a, b, c, d;
    sscanf(addr, "%u.%u.%u.%u", &a, &b, &c, &d);
    ipv4 = a | (b << 8) | (c << 16) | (d << 24);
}

static void ipv42s(char* addr, uint32_t ipv4) {
    uint8_t a = ipv4 & 0xff;
    uint8_t b = (ipv4 >> 8) & 0xff;
    uint8_t c = (ipv4 >> 16) & 0xff;
    uint8_t d = (ipv4 >> 24) & 0xff;
    sprintf(addr, "%hu.%hu.%hu.%hu", a, b, c, d);
}

static void s2ipv4_with_mask(const char* addr, uint32_t& ipv4, uint32_t& mask) {
    s2ipv4(addr, ipv4);
    uint32_t a, b, c, d, e;
    sscanf(addr, "%u.%u.%u.%u/%u", &a, &b, &c, &d, &e);
    assert(e >= 0 && e <= 32);
    mask = htonl(~((1U << (32 - e)) - 1));
}

// 全局变量
std::map<uint32_t, int> hostip_to_routerid;
std::map<uint32_t, int> extip_to_routerid;

int router_count = 0;

const int DV_INF = 11451419;  // or should be 2147483647

RouterBase* create_router_object() {
    router_count++;
    return new Router(router_count);
}

void Router::router_init(int port_num, int external_port, char* external_addr, char* available_addr) {
    ext_port = external_port;

    if (ext_port != 0) {
        // put all the available ips into the set.
        uint32_t avil_ip, avil_mask;
        s2ipv4_with_mask(available_addr, avil_ip, avil_mask);
        // 把所有的available_ip放到这个set里面。所有以uint32_t形式出现的ip地址都是大端的！
        uint32_t temp_mask = ~ntohl(avil_mask);
        uint32_t temp_ip = ntohl(avil_ip) & (~temp_mask);

        for (int i = 0; i <= temp_mask; i++) {
            available_ip.insert(htonl(temp_ip + i));
        }

        uint32_t ext_ip, ext_mask;
        s2ipv4_with_mask(external_addr, ext_ip, ext_mask);
        temp_mask = ~ntohl(ext_mask);
        temp_ip = ntohl(ext_ip) & (~temp_mask);
        for (int i = 0; i <= temp_mask; i++) {
            extip_to_routerid.insert(std::make_pair(htonl(temp_ip + i), router_id));
        }
    }
    // port2cost init
    for (int p = 1; p <= port_num; p++) {
        port_to_cost[p] = DV_INF;
    }
    // DV init
    for (int i = 1; i <= router_count; i++) {
        if (i != router_id) {
            DV_table[router_id][i] = DV_INF;
        } else {
            DV_table[router_id][i] = 0;
        }
    }

    return;
}

const uint8_t TYPE_DV = 0x00;
const uint8_t TYPE_DATA = 0x01;
const uint8_t TYPE_CONTROL = 0x02;

int Router::router(int in_port, char* packet) {
    // 1 for default, 0 for broadcast, -1 for abandon

    // the below is wrong!
    PackHeader* header = reinterpret_cast<PackHeader*>(packet);

    if (header->type == TYPE_DV) {
        // return handle_DV_message(packet, header->length, in_port);
        return handle_DVP_message(packet, header->length, in_port);
    } else if (header->type == TYPE_DATA) {
        return handle_data_message(header, in_port);
    } else if (header->type == TYPE_CONTROL) {
        return handle_control_message(packet, header->length, true);
    } else {
        throw "error unknown type at packet header";
    }

    return -1;
}

int Router::handle_control_message(char* packet, uint16_t length, bool poisoned) {
    uint32_t type;
    sscanf(packet + sizeof(PackHeader), "%u", &type);

    switch (type) {
        case 0: {
            // trigger DV send
            if (poisoned) {
                make_DVP_packet(packet, false);
            } else {
                make_DV_packet(packet, false);
            }
            return 0;
        }
        case 1: {
            // release NAT item
            uint32_t a, b, c, d;
            sscanf(packet + sizeof(PackHeader), "%u %u.%u.%u.%u", &type, &a, &b, &c, &d);
            uint32_t ipv4 = a | (b << 8) | (c << 16) | (d << 24);
            if (NAT_table.find(ipv4) != NAT_table.end()) {
                uint32_t avil_ip = NAT_table.at(ipv4);
                NAT_table.erase(ipv4);
                available_ip.insert(avil_ip);
            } else {
                std::cerr << "erasing NAT_ip unfound" << std::endl;
            }

            return -1;
        }

        case 2: {
            // port value change
            int port, value;
            sscanf(packet + sizeof(PackHeader), "%u %d %d", &type, &port, &value);

            if (value == -1) {
                // 这意味着port断开
                // 此时port不仅可能是指向router的port,也有可能是指向host的port
                if (port == ext_port) {
                    // 删去网络port。这似乎不会出现
                    return -1;
                } else {
                    // 删去host port
                    bool is_hostport = false;
                    for (auto it = hosts_to_port.begin(); it != hosts_to_port.end();) {
                        if (it->second == port) {
                            is_hostport = true;
                            hostip_to_routerid.erase(it->first);
                            it = hosts_to_port.erase(it);
                            break;
                        } else {
                            ++it;
                        }
                    }
                    if (is_hostport) {
                        return -1;
                    } else {
                        // 更新port cost值
                        port_to_cost[port] = DV_INF;
                        port_to_routernear.erase(port);

                        // 进行完全的更新
                        for (int i = 1; i <= router_count; i++) {
                            if (i == router_id) {
                                continue;
                            }
                            DV_table[router_id][i] = DV_INF;
                            for (auto itp = port_to_routernear.begin(); itp != port_to_routernear.end(); ++itp) {
                                // itp->first是正在遍历的所有port
                                // itp->second是正在遍历的所有neighbor router
                                if (DV_table[router_id][i] > port_to_cost[itp->first] + DV_table[itp->second][i]) {
                                    DV_table[router_id][i] = port_to_cost[itp->first] + DV_table[itp->second][i];
                                    // 更新转发表
                                    routeraim_to_port[i] = itp->first;
                                }
                            }
                        }

                        // 摆烂等它慢慢收敛
                        if (poisoned) {
                            make_DVP_packet(packet, false);
                            return 0;
                        } else {
                            return -1;
                        }
                    }
                }
            }

            else {
                port_to_cost[port] = value;

                // 进行完全的更新
                for (int i = 1; i <= router_count; i++) {
                    if (i == router_id) {
                        continue;
                    }
                    DV_table[router_id][i] = DV_INF;
                    for (auto itp = port_to_routernear.begin(); itp != port_to_routernear.end(); ++itp) {
                        // itp->first是正在遍历的所有port
                        // itp->second是正在遍历的所有neighbor router
                        if (DV_table.find(itp->second) == DV_table.end()) {
                            continue;
                        }
                        if (DV_table[itp->second].find(i) == DV_table[itp->second].end()) {
                            DV_table[itp->second][i] = DV_INF;
                        }
                        if (DV_table[router_id][i] > port_to_cost[itp->first] + DV_table[itp->second][i]) {
                            DV_table[router_id][i] = port_to_cost[itp->first] + DV_table[itp->second][i];
                            // 更新转发表
                            routeraim_to_port[i] = itp->first;
                        }
                    }
                }

                // trigger DV exchange
                if (poisoned) {
                    make_DVP_packet(packet, false);
                } else {
                    make_DV_packet(packet, false);
                }
                return 0;
            }
            return -1;
        }
        case 3: {
            // add host

            int port;
            uint32_t a, b, c, d;
            sscanf(packet + sizeof(PackHeader), "%u %d %u.%u.%u.%u", &type, &port, &a, &b, &c, &d);
            uint32_t ipv4 = a | (b << 8) | (c << 16) | (d << 24);

            hostip_to_routerid.insert(std::make_pair(ipv4, router_id));
            hosts_to_port.insert(std::make_pair(ipv4, port));

            return -1;
        }

        case 5: {
            // block addr
            uint32_t a, b, c, d;
            sscanf(packet + sizeof(PackHeader), "%u %u.%u.%u.%u", &type, &a, &b, &c, &d);
            uint32_t ipv4 = a | (b << 8) | (c << 16) | (d << 24);
            blocked_ip.insert(ipv4);
            return -1;
        }

        case 6: {
            // unblock addr
            uint32_t a, b, c, d;
            sscanf(packet + sizeof(PackHeader), "%u %u.%u.%u.%u", &type, &a, &b, &c, &d);
            uint32_t ipv4 = a | (b << 8) | (c << 16) | (d << 24);
            if (blocked_ip.erase(ipv4) == 0) {
                // the addr to be unblocked wasn't blocked at first
                std::cerr << "erasing blocked_ip unfound" << std::endl;
            }
            return -1;
        }

        default: {
            throw "error unknown control type";
        }
    }
    return -1;
}

const char* SPACE = " ";

void Router::make_DV_packet(char* packet, bool force_upload) {
    uint16_t my_payload_size = sizeof(int) + router_count * sizeof(int) + router_count;

    char my_packet[sizeof(PackHeader) + my_payload_size];

    PackHeader my_header(0, 0, TYPE_DV, my_payload_size);

    std::memcpy(my_packet, &my_header, sizeof(PackHeader));

    char* my_packet_pos = my_packet + sizeof(PackHeader);

    int temp_router_id;
    if (force_upload) {
        temp_router_id = -1;
    } else {
        temp_router_id = router_id;
    }
    std::memcpy(my_packet_pos, &temp_router_id, sizeof(int));

    my_packet_pos = my_packet_pos + sizeof(int);

    for (int i = 1; i <= router_count; i++) {
        std::memcpy(my_packet_pos, SPACE, 1);
        my_packet_pos = my_packet_pos + 1;

        if (DV_table[router_id].find(i) == DV_table[router_id].end()) {
            DV_table[router_id].insert(std::make_pair(i, DV_INF));
        }
        int dv_val = DV_table[router_id][i];
        std::memcpy(my_packet_pos, &dv_val, sizeof(int));
        my_packet_pos = my_packet_pos + sizeof(int);
    }

    // 不能够像下面这样做！因为指针是值传递的。
    // packet = my_packet;

    std::memcpy(packet, my_packet, sizeof(PackHeader) + my_payload_size);

    return;
}

int Router::handle_DV_message(char* packet, uint16_t length, int in_port) {
    // read DV package
    if (port_to_cost[in_port] <= 0) {
        throw "error DV_message from non-router device";
    }

    char* packet_pos = packet + sizeof(PackHeader);
    // sscanf(packet_pos, "%d", &from_router_id);
    int from_router_id = *(reinterpret_cast<int*>(packet_pos));

    if (from_router_id == -1) {
        make_DV_packet(packet, false);
        return 0;
    }

    if (port_to_routernear.find(in_port) == port_to_routernear.end()) {
        port_to_routernear.insert(std::make_pair(in_port, from_router_id));
    }
    packet_pos = packet_pos + sizeof(int) + 1;
    for (int i = 1; i <= router_count; i++) {
        int temp_DV_val;
        // sscanf(packet_pos, " %d", &temp_DV_val);
        temp_DV_val = *(reinterpret_cast<int*>(packet_pos));

        DV_table[from_router_id][i] = temp_DV_val;

        packet_pos = packet_pos + 1 + sizeof(int);
    }

    // Bellman-Ford, no poison
    // 下面这个Bellman-Ford是完全错误的！
    bool renewed = false;
    for (int i = 1; i <= router_count; i++) {
        if (DV_table[router_id].find(i) == DV_table[router_id].end()) {
            DV_table[router_id].insert(std::make_pair(i, DV_INF));
        }
        if (DV_table[router_id][i] > DV_table[from_router_id][i] + port_to_cost[in_port]) {
            renewed = true;
            DV_table[router_id][i] = DV_table[from_router_id][i] + port_to_cost[in_port];
            routeraim_to_port[i] = in_port;
        }
    }

    // decide whether to send more
    if (renewed) {
        make_DV_packet(packet, false);
        return 0;
    } else {
        return -1;
    }
}

int Router::handle_data_message(PackHeader* header, int in_port) {
    // check src
    if (blocked_ip.find(header->src) != blocked_ip.end()) {
        // blocked src
        return -1;
    }
    if (extip_to_routerid.find(header->src) != extip_to_routerid.end()) {
        // src是一个网络ip
        if (extip_to_routerid.at(header->src) == router_id) {
            // 自己是网关路由
            assert(in_port == ext_port);
            // 那么它的dst只能是NAT表项里面的val。否则丢弃
            uint32_t dst_host;
            bool found_dst_in_NAT = false;
            for (auto it = NAT_table.begin(); it != NAT_table.end(); ++it) {
                if (it->second == header->dst) {
                    found_dst_in_NAT = true;
                    dst_host = it->first;
                    break;
                }
            }
            if (found_dst_in_NAT) {
                // 找到了。接下来改一改dst,往内网传。
                // 暂时不急着return
                header->dst = dst_host;
            } else {
                // dst不合法。丢弃。
                return -1;
            }
        }
    }

    // check dst
    if (hostip_to_routerid.find(header->dst) != hostip_to_routerid.end()) {
        // dst是一个host ip.现在我们检查
        if (hostip_to_routerid.at(header->dst) == router_id) {
            // 这个host跟自己相连
            // std::cout << "last forwarding towards host on router " << router_id << std::endl;
            return hosts_to_port.at(header->dst);
        } else {
            // 这个host跟自己没连着, 查路由表
            // std::cout << "@router " << router_id << " forwarding aim host not on me, check routeraim_to_port" << std::endl;
            // 路由表上面有
            int aimrouter_id = hostip_to_routerid.at(header->dst);
            if (routeraim_to_port.find(aimrouter_id) != routeraim_to_port.end()) {
                return routeraim_to_port.at(aimrouter_id);
            } else {
                // 路由表上面没有
                return 1;
            }
        }
    } else if (extip_to_routerid.find(header->dst) != extip_to_routerid.end()) {
        // dst是一个external ip
        if (extip_to_routerid.at(header->dst) == router_id) {
            // 自己是网关路由器。需要进行NAT
            // 查下是否已有NAT表项
            if (NAT_table.find(header->src) != NAT_table.end()) {
                // 已经有NAT映射了
                // 改一下src发出去就可以了
                header->src = NAT_table.at(header->src);
                return ext_port;
            } else {
                if (available_ip.empty()) {
                    return -1;
                } else {
                    // 新分配一个NAT表项
                    auto it = available_ip.begin();
                    uint32_t avil_ip = *it;
                    available_ip.erase(it);
                    NAT_table.insert(std::make_pair(header->src, avil_ip));
                    // 改一下src发出去
                    header->src = NAT_table.at(header->src);
                    return ext_port;
                }
            }

        } else {
            // 查路由表
            // 路由表上面有
            int aimrouter_id = extip_to_routerid.at(header->dst);
            if (routeraim_to_port.find(aimrouter_id) != routeraim_to_port.end()) {
                return routeraim_to_port.at(aimrouter_id);
            } else {
                // 路由表上面没有
                return 1;
            }
        }
    } else {
        std::cout << "dstination is not a host ip or ext ip. what is it?" << std::endl;
        // 路由表上面没有
        return 1;
    }
}

void Router::make_DVP_packet(char* packet, bool force_upload) {
    // std::cout << "@make_DVP_packet at router " << router_id << " content: ";

    uint16_t my_payload_size = sizeof(int) + 2 * router_count * (sizeof(int) + 1);

    char my_packet[sizeof(PackHeader) + my_payload_size];

    PackHeader my_header(0, 0, TYPE_DV, my_payload_size);

    std::memcpy(my_packet, &my_header, sizeof(PackHeader));

    char* my_packet_pos = my_packet + sizeof(PackHeader);

    int temp_router_id;
    if (force_upload) {
        temp_router_id = -1;
    } else {
        temp_router_id = router_id;
    }
    std::memcpy(my_packet_pos, &temp_router_id, sizeof(int));

    my_packet_pos = my_packet_pos + sizeof(int);

    for (int i = 1; i <= router_count; i++) {
        std::memcpy(my_packet_pos, SPACE, 1);
        my_packet_pos = my_packet_pos + 1;

        if (DV_table[router_id].find(i) == DV_table[router_id].end()) {
            DV_table[router_id].insert(std::make_pair(i, DV_INF));
        }
        int dv_val = DV_table[router_id][i];
        std::memcpy(my_packet_pos, &dv_val, sizeof(int));
        my_packet_pos = my_packet_pos + sizeof(int);

        // 下一个位置是next_router
        // 如果无从得知，就把它设置为-1
        int next_router_id = -1;
        if (routeraim_to_port.find(i) != routeraim_to_port.end()) {
            int out_port = routeraim_to_port.at(i);
            if (port_to_routernear.find(out_port) != port_to_routernear.end()) {
                next_router_id = port_to_routernear.at(out_port);
            }
        }

        std::memcpy(my_packet_pos, SPACE, 1);
        my_packet_pos = my_packet_pos + 1;
        std::memcpy(my_packet_pos, &next_router_id, sizeof(int));
        my_packet_pos = my_packet_pos + sizeof(int);
    }

    std::memcpy(packet, my_packet, sizeof(PackHeader) + my_payload_size);

    return;
}

int Router::handle_DVP_message(char* packet, uint16_t length, int in_port) {
    // read DV package
    if (port_to_cost[in_port] <= 0) {
        throw "error DV_message from non-router device";
    }

    char* packet_pos = packet + sizeof(PackHeader);
    // sscanf(packet_pos, "%d", &from_router_id);
    int from_router_id = *(reinterpret_cast<int*>(packet_pos));

    if (from_router_id == -1) {
        make_DVP_packet(packet, false);
        return 0;
    }

    if (port_to_routernear.find(in_port) == port_to_routernear.end()) {
        port_to_routernear.insert(std::make_pair(in_port, from_router_id));
    }

    packet_pos = packet_pos + sizeof(int) + 1;
    for (int i = 1; i <= router_count; i++) {
        int temp_DV_val = *(reinterpret_cast<int*>(packet_pos));

        packet_pos = packet_pos + 1 + sizeof(int);
        int temp_DVP_router = *(reinterpret_cast<int*>(packet_pos));

        if (temp_DVP_router == router_id) {
            // poison
            DV_table[from_router_id][i] = DV_INF;
        } else {
            DV_table[from_router_id][i] = temp_DV_val;
        }

        packet_pos = packet_pos + 1 + sizeof(int);
    }

    // Bellman-Ford
    // 偷懒是不可以的！这里必须进行完全的更新，也就是说，使用不仅仅来自in_port的结果
    // 如果仅仅使用in_port的结果，并且每次更新之前不设置DV_table[router_id][i] = DV_INF
    // 那么将无法处理更新为“比真实值更大”的情况
    // 如果仅仅使用in_port的结果，并且每次更新之前设置DV_table[router_id][i] = DV_INF
    // 那么将进入无限循环。因为它会重复地设置有限的val和不可达的DV_INF
    bool renewed = false;

    for (int i = 1; i <= router_count; i++) {
        if (i == router_id) {
            continue;
        }
        int ori_dv_val = DV_table[router_id][i];
        int ori_rt_port = -1;
        if (routeraim_to_port.find(i) != routeraim_to_port.end()) {
            ori_rt_port = routeraim_to_port.at(i);
        }
        DV_table[router_id][i] = DV_INF;

        for (auto itp = port_to_routernear.begin(); itp != port_to_routernear.end(); ++itp) {
            // itp->first是正在遍历的所有port
            // itp->second是正在遍历的所有neighbor router
            if (DV_table.find(itp->second) == DV_table.end()) {
                continue;
            }
            if (DV_table[itp->second].find(i) == DV_table[itp->second].end()) {
                DV_table[itp->second][i] = DV_INF;
            }
            if (DV_table[router_id][i] > port_to_cost[itp->first] + DV_table[itp->second][i]) {
                DV_table[router_id][i] = port_to_cost[itp->first] + DV_table[itp->second][i];
                // 更新转发表
                routeraim_to_port[i] = itp->first;
            }
        }
        int aft_rt_port = -1;
        if (routeraim_to_port.find(i) != routeraim_to_port.end()) {
            aft_rt_port = routeraim_to_port.at(i);
        }

        if (DV_table[router_id][i] != ori_dv_val || ori_rt_port != aft_rt_port) {
            renewed = true;
        }
    }

    // decide whether to send more
    if (renewed) {
        make_DVP_packet(packet, false);
        return 0;
    } else {
        return -1;
    }
}
