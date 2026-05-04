#ifndef COR_H
#define COR_H

#include <netinet/in.h>
#include <stdbool.h>

// Definições e macros
#define BUFFER_SIZE 300
#define MAX_NODES 16
#define DEFAULT_REG_IP "193.136.138.142"
#define DEFAULT_REG_UDP_PORT 59000
#define ID_RANGE 100
#define MAX_PATH_LENGTH 45
#define MAX_CORDS 13
#define MAX_NEIGHBORS 15

// Estrutura de nó
typedef struct _node {
    char ID[3];
    char IP[INET_ADDRSTRLEN];
    int port;
} Node;

// Estrutura de cordas
typedef struct {
    int fd;              // File descriptor da corda
    char nodeID[3];      // ID do nó
    bool is_active;      // Indica se a corda está ativa
} Cord;

// Linha de encaminhamento para um destino
typedef struct {
    char nodeID[3];                        // ID do nó destino
    char paths[MAX_NEIGHBORS][100];        // Caminhos para o destino através de cada vizinho
} RoutingEntry;

// Estrutura para a tabela de encaminhamento
typedef struct {
    RoutingEntry entries[MAX_NODES];       // Entradas de encaminhamento
    int nodeCount;                         // Número de nós na tabela
    char neighborIDs[MAX_NEIGHBORS][3];    // IDs dos vizinhos
    int neighborCount;                     // Número de vizinhos
} RoutingTable;

// Estrutura para armazenar o ID do nó destino e o melhor caminho
typedef struct {
    char nodeID[3];                        // ID do nó destino
    char shortestPath[MAX_PATH_LENGTH];    // Melhor caminho para o destino
} ShortestPathEntry;

// Estrutura para a tabela de melhores caminhos
typedef struct {
    ShortestPathEntry entries[MAX_NODES];  // Entradas dos melhores caminhos
    int nodeCount;                         // Número de entradas na tabela
} ShortestPathTable;

// Funções do main
void user_input(int listen_fd, int *joined, int *chorded, int node_TCP, char *nodeIP);
void process_succ_fd(int listen_fd, int *chorded);
void process_pred_fd(int listen_fd, int *chorded);

// Funções comandos stdin
int join_command(char *regPortStr, char *ringID, int node_TCP, char *nodeIP);
int d_join_command(char *succIP, int succPort, char *succID, int node_TCP, char *nodeIP);
int chord_command(char *regPortStr, char *ringID, int node_TCP, char *nodeIP);
void remove_chord_command(int fd);
void show_topology();
void show_routing_command(RoutingTable *table, char *destinationID);
void show_path_command(ShortestPathTable *table, char *destinationID);
void show_forwarding_command();
void message_command(char *buffer, char *origin, char *dest, int *fd);
int leave_command(char *regPortStr, char *ringID, int node_TCP, char *nodeIP);
int clean_ring_DEBUG(char *regPortStr, char *ringID, char *clean_id, int node_TCP, char *nodeIP);

// Funções sockets UDP
int setup_udp_connection(char *regPortStr, struct addrinfo **servinfo);
int send_udp_message(int sock_UDP, struct addrinfo *servinfo, char *message);
int receive_udp_message(int sock_UDP, char *buffer, size_t bufferSize);
int udp_communication(char *regPortStr, char *message, char *response, size_t responseSize);

// Funções sockets TCP
void setup_tcp_listener(int *listen_fd, char *port_str, char *ip_str);
void accept_tcp_connection(int listen_fd, int *joined, int node_TCP, char *nodeIP);
void handle_tcp_command(char *buffer, int *joined, int client_fd);

// Funções protocolos de mensagem
int send_entry_message(Node *target);
void forward_entry_message(int sock_fd, Node *shared);
void send_succ_message(int sock_fd, Node *shared);
void send_pred_message(Node *target);
int send_chord_message(Node *target);

// Funções de cordas
void init_chords();
void add_chord(int fd, char* nodeID);
void print_active_chords();
int is_existing_chord(Node randomNode, Cord chords[], int max_cords);
void close_all_chords();
void close_invalid_chords(int *chorded);

// Funções routing
void handle_routing(char *buffer, int fd);
void send_route_to_neighbours(int exclude_fd, char *buffer);
void send_all_routes_to_neighbor(int target_fd);
void init_routing_table(RoutingTable *table);
void init_shortest_path_table(ShortestPathTable *table);
void add_routing_entry(RoutingTable *table, char *neighborID, char *destID, char *path);
void remove_routing_neighbor(RoutingTable *table, char *neighborID);
void remove_routing_destinations(char *destID);
int remove_neighbor_path(RoutingTable *table, char *neighborID, char *destID);
int add_shortest_path_entry(ShortestPathTable *table, char *nodeID, char *path);
void remove_shortest_path_destination(ShortestPathTable *table, char *destID);
void remove_destination_if_no_paths(RoutingTable *routingTable, ShortestPathTable *shortestPathTable, char *destID);
int count_hops(char *path);
int find_shortest_path(RoutingTable *routingTable, ShortestPathTable *shortestPathTable, char *changed_paths);
void add_changed_route(char *changed_paths, char *sourceID, char *destID, char *path);
void check_neighbours(RoutingTable *routingTable);

// Funções auxiliares
int parse_node_list(char *buffer, Node *nodes, int max_nodes);
void print_node_info(char* node_name, Node* node);
bool free_id(Node nodes[], int node_count, char *id);
void get_new_id(Node nodes[], int node_count, char *new_ID);
Node choose_node(Node nodes[], int node_count);
void clear_node(Node *node);
void update_max_fd(int listen_fd);
int choose_next(char *destID);

#endif // COR_H
