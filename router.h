#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include "router_prototype.h"

// header
struct PackHeader {
    uint32_t src;
    uint32_t dst;
    uint8_t type;
    uint16_t length;

    PackHeader(uint32_t s, uint32_t d, uint8_t t, uint16_t l) {
        src = s;
        dst = d;
        type = t;
        length = l;
    }
    PackHeader() {
        src = 0;
        dst = 0;
        type = 0;
        length = 0;
    }
};

class Router : public RouterBase {
   public:
    int router_id;                                // router_id 从1开始
    std::map<int, int> routeraim_to_port;         // 转发表
    std::map<int, std::map<int, int> > DV_table;  // 到port1的cost是0
    std::map<int, int> port_to_routernear;
    std::map<int, int> port_to_cost;

    std::map<uint32_t, int> hosts_to_port;
    std::map<uint32_t, uint32_t> NAT_table;  // from host ip to 公网分配的available ip

    int ext_port;

    std::set<uint32_t> available_ip;  // 可用的公网地址范围
    std::set<uint32_t> blocked_ip;

   public:
    Router(int rt)
        : router_id(rt) {
        // std::cout << "Router initialized id " << router_id << std::endl;
        routeraim_to_port.insert(std::make_pair(router_id, 0));
        port_to_cost.insert(std::make_pair(0, 0));
        port_to_cost.insert(std::make_pair(1, 0));
    }

    void router_init(int port_num, int external_port, char* external_addr, char* available_addr);
    int router(int in_port, char* packet);

    int handle_control_message(char* packet, uint16_t length, bool poisoned);

    void make_DV_packet(char* packet, bool force_upload);

    int handle_DV_message(char* packet, uint16_t length, int in_port);

    void make_DVP_packet(char* packet, bool force_upload);

    int handle_DVP_message(char* packet, uint16_t length, int in_port);

    int handle_data_message(PackHeader* header, int in_port);
};
