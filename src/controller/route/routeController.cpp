//
// Created by LEGION on 2024/6/14.
//
#include "../../common/common.h"
#include "routeController.h"
#include "../../model/LSDB/LSDB.h"

//
// RoutingTableItem
//
RoutingTableItem::RoutingTableItem() {

}

RoutingTableItem::RoutingTableItem(uint32_t destination, uint32_t next_hop, uint32_t metric) {
    this->destination = destination;
    this->address_mask = 0xffffff00;
    this->type = 1;
    this->metric = metric;
    this->next_hop = next_hop;
}

void RoutingTableItem::print() {
    ip_addr.s_addr = htonl(destination);
    printf("| %-15s ", inet_ntoa(ip_addr));
    ip_addr.s_addr = htonl(address_mask);
    printf("| %-15s ", inet_ntoa(ip_addr));
    ip_addr.s_addr = htonl(next_hop);
    printf("| %-15s ", inet_ntoa(ip_addr));
    printf("|   %02d   | \n", metric);
}

//
// Edge
//
Edge::Edge() {

}

Edge::Edge(uint32_t source_ip, uint32_t metric, uint32_t target_id) {
    this->source_ip = source_ip;
    this->metric = metric;
    this->target_id = target_id;
}

//
//  Vertex
//
Vertex::Vertex() {

}

Vertex::Vertex(uint32_t router_id) {
    this->router_id = router_id;
}

void Vertex::print() {
    ip_addr.s_addr = htonl(router_id);
    printf("Vertex: [%-15s]\n", inet_ntoa(ip_addr));
    for (auto& elem : adjacencies) {
        Edge edge = elem.second;
        ip_addr.s_addr = htonl(edge.source_ip);
        printf("\t[%-15s]----%02d---->",inet_ntoa(ip_addr), edge.metric);
        ip_addr.s_addr = htonl(edge.target_id);
        printf("[%-15s]\n", inet_ntoa(ip_addr));
    }
}

//
// toTargetVertex
//
toTargetVertex::toTargetVertex() {

}

toTargetVertex::toTargetVertex(uint32_t target_vertex_id, uint32_t total_metric) {
    this->target_vertex_id = target_vertex_id;
    this->total_metric = total_metric;
}

void toTargetVertex::print() {
    ip_addr.s_addr = htonl(target_vertex_id);
    printf("to [%-15s]: %02d ", inet_ntoa(ip_addr), total_metric);
    ip_addr.s_addr = htonl(next_hop);
    printf("%-15s\n", inet_ntoa(ip_addr));
}

//
// RoutingTable
//

RoutingTable::RoutingTable() {
    router_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (router_fd < 0) {
        perror("Failed to init router_fd");
        return;
    }
}

RoutingTable::~RoutingTable() {
    printf("Destruct router_fd.\n");
    resetRoute();
    close(router_fd);
}

void RoutingTable::buildTopo() {
    //首先清空现有的拓扑
    vertexes.clear();

    pthread_mutex_lock(&lsdb.router_lock);
    pthread_mutex_lock(&lsdb.network_lock);
    //暂存当前的LSDB
    LSDB lsdb_copy = lsdb;
    pthread_mutex_unlock(&lsdb.network_lock);
    pthread_mutex_unlock(&lsdb.router_lock);

    //遍历所有的lsa_router
    for (LSARouter* lsa_router : lsdb_copy.lsa_routers) {
        //找到LSA的始发路由器标识，初始化顶点
        uint32_t source_id = lsa_router->lsaHeader.advertising_router;
        if (vertexes.find(source_id) == vertexes.end()) {
            vertexes[source_id] = Vertex(source_id);
        }
        //(在该始发路由器视角)构建与之相连的边
        for (LSARouterLink link : lsa_router->LSARouterLinks) {
            //始发路由器发出此LSA的接口的IP
            uint32_t source_ip = link.link_data;
            //路由器连接的距离
            uint32_t metric = link.metric;
            uint32_t target_id;
            //链路类型为TRANSIT：链接到广播或非广播多点可达网络
            if (link.type == TRANSIT) {
                //通过link_id(DR生成)获取网络LSA
                LSANetwork* lsa_network = lsdb_copy.getNetworkLSA(link.link_id);
                for (uint32_t target_router_id : lsa_network->attached_routers) {
                    if (target_router_id == source_id) {
                        continue;
                    }
                    vertexes[source_id].adjacencies[target_router_id] = Edge(source_ip, metric, target_router_id);
                }
            }
            else if (link.type == P2P) {
                //P2P网络中link_id为邻居的路由器标识
                target_id = link.link_id;
                vertexes[source_id].adjacencies[target_id] = Edge(source_ip, metric, target_id);
            }
            else {
                continue;
            }
        }
    }
}

//根据拓扑信息计算路由
void RoutingTable::calRouting() {
    //判断拓扑是否为空
    if (vertexes.empty()) {
        printf("run buildTopo first to get topo.\n");
        return;
    }
    //清空原有路径
    paths.clear();

    std::map<uint32_t, toTargetVertex> pending_vertexes;
    for (auto& elem : vertexes) {
        //初始化：将除本路由器以外的所有顶点加入并将初始开销设为无穷大
        if (elem.first != WHYConfig::router_id) {
            pending_vertexes[elem.first] = toTargetVertex(elem.first, INFINITY);
        }
    }
    //初始化：将本路由器加入paths并将其初始开销设为0
    paths[WHYConfig::router_id] = toTargetVertex(WHYConfig::router_id, 0);

    //run dijkstra
    uint32_t now_id = WHYConfig::router_id;
    while (pending_vertexes.size() != 0) {
        //当前顶点
        Vertex now_vertex = vertexes[now_id];
        toTargetVertex now_toTargetVertex = paths[now_id];
        if (now_vertex.adjacencies.size() == 0) {
            break;
        }
        //遍历当前顶点的相邻顶点和边
        for (auto& elem : now_vertex.adjacencies) {
            //相邻顶点
            Vertex other_vertex = vertexes[elem.first];
            //两者之间的边
            Edge edge = elem.second;
            //检查临接点是否已被操作
            if (pending_vertexes.find(edge.target_id) == pending_vertexes.end()) {
                continue;
            }
            //松弛(比较pending中的最短距离和当前路径)
            toTargetVertex temp = pending_vertexes[edge.target_id];
            if (temp.total_metric > now_toTargetVertex.total_metric + edge.metric) {
                temp.total_metric = now_toTargetVertex.total_metric + edge.metric;
                //更新下一跳，根据是否为本路由器讨论
                if (now_id == WHYConfig::router_id) {
                    temp.next_hop = other_vertex.adjacencies[now_id].source_ip;
                    temp.out_interface = WHYConfig::iptointerface[edge.source_ip];
                }
                else {
                    temp.next_hop = now_toTargetVertex.next_hop;
                    temp.out_interface = now_toTargetVertex.out_interface;
                }
            }
        }
        //找到当前距离最短的顶点
        toTargetVertex min_toTargetVertex = toTargetVertex(0, INFINITY);
        for (auto& elem : pending_vertexes) {
            if (elem.second.total_metric < min_toTargetVertex.total_metric) {
                min_toTargetVertex = elem.second;
            }
        }
        //更新结点
        now_id = min_toTargetVertex.target_vertex_id;
        paths[now_id] = min_toTargetVertex;
        pending_vertexes.erase(now_id);
    }
}

//根据网络拓扑和计算结果生成路由表
void RoutingTable::generateRouting() {
    routings.clear();
    //当前路由器不需要为自己生成路由
    for (auto& elem : paths) {
        uint32_t router_id = elem.first;
        if (router_id == WHYConfig::router_id) {
            continue;
        }
        //获取顶点信息和到顶点信息
        Vertex vertex = vertexes[router_id];
        toTargetVertex toVertex = elem.second;
        //遍历当前路由器的临接点
        for (auto& item : vertex.adjacencies) {
            Edge edge = item.second;
            uint32_t dest_ip = edge.source_ip & 0xffffff00;
            //如果路由表中没有目标网络，直接插入
            if (routings.find(dest_ip) == routings.end()) {
                routings[dest_ip] = RoutingTableItem(dest_ip, toVertex.next_hop, toVertex.total_metric);
            }
            else {
                //有目标网络，维护较小值
                RoutingTableItem route_item = routings[dest_ip];
                if (route_item.metric > toVertex.total_metric) {
                    routings[dest_ip] = RoutingTableItem(dest_ip, toVertex.next_hop, toVertex.total_metric);
                }
            }
        }
    }
}

void RoutingTable::printTopo() {
    printf("============Topo Information============\n");
    for (auto& elem : vertexes) {
        elem.second.print();
    }
    printf("========================================\n");
}

void RoutingTable::printPaths() {
    printf("============Path Information============\n");
    for (auto& elem : paths) {
        elem.second.print();
    }
    printf("========================================\n");
}

void RoutingTable::printRoutingTable() {
    printf("============Table Information============\n");
    printf("| dest_ip | net_mask | next_hop | metric |\n");
    for (auto& elem : routings) {
        elem.second.print();
    }
    printf("=========================================\n");
}

void RoutingTable::update() {
    printf("Update OSPF Routing.\n");

    //(1)构建当前路由器拓扑信息
    buildTopo();
    printTopo();

    //(2)计算路由信息
    calRouting();
    printPaths();

    //(3)生成路由表
    generateRouting();
    printRoutingTable();

    //(4)写入内核态
    writeKernelRoute();
}

void RoutingTable::resetRoute() {
    //不知道这样写会不会出错
    for (auto& route : rtentries) {
        if (ioctl(router_fd, SIOCDELRT, &route) < 0) {
            struct sockaddr_in dest = *((struct sockaddr_in*)&route.rt_dst);
            printf("Deleted route %s.\n", inet_ntoa(dest.sin_addr));
            perror(":");
        }
    }
    rtentries.clear();
    printf("Successfully reset kernel route.\n");
}

void RoutingTable::writeKernelRoute() {
    //首先清空之前的配置
    resetRoute();

    //循环遍历路由条目
    for (auto& elem : routings) {
        RoutingTableItem item = elem.second;
        //定义并初始化
        struct rtentry rtentry;
        memset(&rtentry, 0, sizeof(rtentry));
        //设置目的地址
        rtentry.rt_dst.sa_family = AF_INET;
        ((struct sockaddr_in*)&rtentry.rt_dst)->sin_addr.s_addr = htonl(item.destination);
        //设置子网掩码
        rtentry.rt_genmask.sa_family = AF_INET;
        ((struct sockaddr_in*)&rtentry.rt_genmask)->sin_addr.s_addr = htonl(item.address_mask);
        //设置网关地址
        rtentry.rt_gateway.sa_family = AF_INET;
        ((struct sockaddr_in*)&rtentry.rt_gateway)->sin_addr.s_addr = htonl(item.next_hop);
        //设置路由度量值
        rtentry.rt_metric = htons((uint16_t)item.metric);
        //设置路由标志
        rtentry.rt_flags = RTF_UP | RTF_GATEWAY;
        //复制rtentry
        struct rtentry copy = rtentry;
        //添加路由条目
        if (ioctl(router_fd, SIOCADDRT, &rtentry) < 0) {
            perror("Failed to add route");
        }
        else {
            printf("Successfully added a route item.\n");
            rtentries.push_back(copy);
        }
    }
    printf("Successfully wrote kernel route.\n");
}   