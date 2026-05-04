#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <stdbool.h>
#include <limits.h>
#include "cor.h"

// Variáveis globais :/
Node pred, succ, succ2, me;
Cord chords[MAX_CORDS];
RoutingTable routingTable;
ShortestPathTable shortestPathTable;

int pred_fd = -1, succ_fd = -1, my_chord_fd = -1;
int joining = 0, reg_flag = 0;
int regUDP;
int max_fd;

char currentRing[4];
char *regIP;

// Main
int main(int argc, char *argv[]) {

    // Tratamento de argumentos
    if(argc < 3) {
        printf("Uso: %s IP TCP [regIP] [regUDP]\n", argv[0]);
        return 1;
    }

    char *nodeIP = argv[1]; // IP local
    int node_TCP = atoi(argv[2]); // Porta TCP local

    regIP = argc > 4 ? argv[3] : DEFAULT_REG_IP; // IP do servidor de nós
    regUDP = argc > 5 ? atoi(argv[4]) : DEFAULT_REG_UDP_PORT; // Porta UDP do servidor de nós

    // Validação
    if(node_TCP <= 0 || regUDP <= 0) {
        fprintf(stderr, "Portas TCP e UDP devem ser números positivos.\n");
        return 1;
    }
    struct in_addr addr;
    if(!inet_aton(nodeIP, &addr)) {
        fprintf(stderr, "Endereço IP inválido: %s\n", nodeIP);
        return 1;
    }

    // Guardar informação do nosso nó
    strncpy(me.IP, nodeIP, INET_ADDRSTRLEN);
    me.port = node_TCP;
    
    printf("\n");
    printf("IP do Nó-------------------------%s\n", nodeIP);
    printf("Porta TCP do Nó------------------%d\n", node_TCP);
    printf("IP do Servidor de Nós------------%s\n", regIP);
    printf("Porta UDP do Servidor de Nós-----%d\n", regUDP);
    printf("\n");
    
    init_chords();
    
    int joined = 0, chorded = 0;;
    int listen_fd;

    unsigned short tcp_port = node_TCP;
    char port_str[7];
    snprintf(port_str, sizeof(port_str), "%d", tcp_port);

    setup_tcp_listener(&listen_fd, port_str, me.IP);

    // Preparar select()
    fd_set read_fds;

    // Loop de entradas
    while(1) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds); // STDIN
        FD_SET(listen_fd, &read_fds); // Socket de escuta

        if(pred_fd >= 0) FD_SET(pred_fd, &read_fds);
        if(succ_fd >= 0) FD_SET(succ_fd, &read_fds);

        // Adiciona as "cordas" ativas ao set de leitura
        for (int i = 0; i < MAX_CORDS; i++) {
            if(chords[i].is_active) {
                FD_SET(chords[i].fd, &read_fds);
            }
        }

        update_max_fd(listen_fd);

        int select_result = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if(select_result < 0) {
            if(errno == EINTR) 
            perror("select error");
            exit(EXIT_FAILURE);
        }
        // STDIN
        if(FD_ISSET(0, &read_fds)) {
            user_input(listen_fd, &joined, &chorded, node_TCP, nodeIP);
        }

        // TCP ESCUTA
        if(FD_ISSET(listen_fd, &read_fds)) {
            accept_tcp_connection(listen_fd, &joined, node_TCP, nodeIP);
        }

        // TCP do predecessor
        if(FD_ISSET(pred_fd, &read_fds)) {
            process_pred_fd(listen_fd, &chorded);
        }
        // TCP do sucessor
        if(FD_ISSET(succ_fd, &read_fds)) {
            process_succ_fd(listen_fd, &chorded);
        }
        // Verificação das cordas ativas
        for (int i = 0; i < MAX_CORDS; i++) {
            if(chords[i].is_active && FD_ISSET(chords[i].fd, &read_fds)) {
                char buffer[BUFFER_SIZE];

                ssize_t bytesRead = read(chords[i].fd, buffer, sizeof(buffer) - 1);
                if(bytesRead > 0) {
                    
                    buffer[bytesRead] = '\0';

                    char *line = strtok(buffer, "\n");
                    char routesBuffer[BUFFER_SIZE] = "";

                    while (line != NULL) {
                        char chatDest[BUFFER_SIZE], chatOrig[BUFFER_SIZE];

                        // Processa 'ROUTE'
                        if(strncmp(line, "ROUTE", 5) == 0) {
                            strcat(routesBuffer, line);
                            strcat(routesBuffer, "\n");
                        }
                        // Processa 'CHAT'
                        else if(sscanf(line, "CHAT %s %s", chatOrig, chatDest) == 2) {
                            // A mensagem é para o meu nó
                            if (strcmp(chatDest, me.ID) == 0) {
                                // Encontra o início da mensagem
                                char* messageStart = strstr(line, " ") + 1;
                                messageStart = strstr(messageStart, " ") + 1;
                                messageStart = strstr(messageStart, " ") + 1;

                                if (messageStart) {
                                    printf("[%s] -> %s\n", chatOrig, messageStart);
                                }
                            } 
                            // Não é para o meu nó
                            else {
                                int next_fd = choose_next(chatDest);
                                if(next_fd != -1) {
                                    if(write(next_fd, line, strlen(line) + 1) == -1)
                                        perror("write CHAT");
                                }
                            }
                        }

                        line = strtok(NULL, "\n");
                    }
                    // Processa todas as mensagens 'ROUTE' agrupadas
                    if(strlen(routesBuffer) > 0) {
                        ////printf("-> %s\n", routesBuffer);
                        handle_routing(routesBuffer, chords[i].fd);
                    }
                    
                } else {
                    // A ligação foi fechada pelo outro lado
                    printf("Corda %s fechou a ligação.\n", chords[i].nodeID);

                    char changed_paths[BUFFER_SIZE], routes_to_send[BUFFER_SIZE];

                    // Remover corda vizinha da tabela de encaminhamento
                    remove_routing_neighbor(&routingTable, chords[i].nodeID);
                    snprintf(routes_to_send, BUFFER_SIZE, "ROUTE %s %s\n", me.ID, chords[i].nodeID);

                    close(chords[i].fd);
                    chords[i].fd = -1;
                    strcpy(chords[i].nodeID, "");
                    chords[i].is_active = false; 
                    close_invalid_chords(&chorded);

                    // Mudar e enviar informações de caminhos
                    find_shortest_path(&routingTable, &shortestPathTable, changed_paths);
                    strcat(routes_to_send, changed_paths);
                    send_route_to_neighbours(-2, routes_to_send);
                }
            }
        }
    }
    return 0;
} //main()


// Tratamento do stdin
void user_input(int listen_fd, int *joined, int *chorded, int node_TCP, char *nodeIP) {
    char command[BUFFER_SIZE];
    char ringID[4];
    char regPortStr[6];

    char succID[3], succIP[INET_ADDRSTRLEN];
    char dest[3];
    char message[BUFFER_SIZE];
    int succPort;

    memset(command, 0, sizeof command);
    memset(message, 0, sizeof message);

    if(fgets(command, sizeof(command), stdin) != NULL) {

        // JOIN ///////////////////////////////////////////////////////////
        if(strncmp(command, "join", 4) == 0  && *joined == 0) {
            sscanf(command, "join %s %s", ringID, me.ID);
            snprintf(regPortStr, sizeof(regPortStr), "%d", regUDP);
            strncpy(currentRing, ringID, strlen(ringID));
            if(join_command(regPortStr, ringID, node_TCP, nodeIP) == 0) {
                *joined = 1;
            }

        } 
        else if(strncmp(command, "j", 1) == 0 && *joined == 0) {
            sscanf(command, "j %s %s", ringID, me.ID);
            snprintf(regPortStr, sizeof(regPortStr), "%d", regUDP);
            strncpy(currentRing, ringID, strlen(ringID));
            if(join_command(regPortStr, ringID, node_TCP, nodeIP) == 0) {
                *joined = 1;
            }
        } 

        // DIRECT JOIN //////////////////////////////////////////////////////
        else if(strncmp(command, "direct join", 11) == 0 && *joined == 0) {
            if(sscanf(command, "direct join %s %s %s %d", me.ID, succID, succIP, &succPort) == 4) { 
                if(d_join_command(succIP, succPort, succID, node_TCP, nodeIP) == 0)
                    *joined = 1;
            }
        }
        else if(strncmp(command, "dj", 2) == 0 && *joined == 0) {
            if(sscanf(command, "dj %s %s %s %d", me.ID, succID, succIP, &succPort) == 4) { 
                if(d_join_command(succIP, succPort, succID, node_TCP, nodeIP) == 0)
                    *joined = 1;
            }
        } 

        // CHORD /////////////////////////////////////////////////////////////
        else if((strncmp(command, "chord", 5) == 0 || strncmp(command, "c", 1) == 0) && *joined == 1) {
            if(*chorded == 0) {
                snprintf(regPortStr, sizeof(regPortStr), "%d", regUDP);
                if(chord_command(regPortStr, currentRing, node_TCP, nodeIP) == 0)
                    *chorded = 1;
            }
            else
                printf("Já há uma corda formada!\n");
        }

        // REMOVE CHORD //////////////////////////////////////////////////////
        else if((strncmp(command, "remove chord", 12) == 0 || strncmp(command, "rc", 2) == 0) && *joined == 1) {
            if(*chorded == 1) {
                snprintf(regPortStr, sizeof(regPortStr), "%d", regUDP);
                remove_chord_command(my_chord_fd);
                *chorded = 0;
            }
            else
                printf("Não há nenhuma corda formada!\n");
        } 
 
        // SHOW TOPOLOGY //////////////////////////////////////////////////////
        else if(strncmp(command, "show topology", 13) == 0 || strncmp(command, "st", 2) == 0) {
            show_topology();
        }

        // SHOW ROUTING //////////////////////////////////////////////////////
        else if(strncmp(command, "show routing", 12) == 0) {
            if(sscanf(command, "show routing %s", dest) == 1) { 
                
                show_routing_command(&routingTable, dest);
            }
        }
        else if(strncmp(command, "sr", 2) == 0) {
            if(sscanf(command, "sr %s", dest) == 1) { 
                
                show_routing_command(&routingTable, dest);
            }
        } 

        // SHOW PATH ////////////////////////////////////////////////////////
        else if(strncmp(command, "show path", 9) == 0) {
            if(sscanf(command, "show path %s", dest) == 1) { 
                
                show_path_command(&shortestPathTable, dest);
            }
        }
        else if(strncmp(command, "sp", 2) == 0) {
            if(sscanf(command, "sp %s", dest) == 1) { 
                
                show_path_command(&shortestPathTable, dest);
            }
        }

        // SHOW FORWARDING //////////////////////////////////////////////////
        else if(strncmp(command, "show forwarding", 15) == 0 || strncmp(command, "sf", 2) == 0) {  

            show_forwarding_command();
        }

        // MESSAGE ///////////////////////////////////////////////////////////
        else if(strncmp(command, "message", 7) == 0 || strncmp(command, "m", 1) == 0) {
            char *destToken = strtok(command, " "); // "message" ou "m"
            destToken = strtok(NULL, " "); // O próximo token tem de ser o destinatário

            // Verifica se há destinatário
            if(destToken != NULL) {
                // Guardar nó destino
                strncpy(dest, destToken, 2);
                dest[2] = '\0';

                // Verifica se o detinatário é o nosso nó
                if(strcmp(me.ID, dest) != 0) {
                    char *messageStart = destToken + strlen(destToken) + 1;

                    // Verifica se há mensagem
                    if(messageStart != NULL && *messageStart != '\0') { 
                        strncpy(message, messageStart, BUFFER_SIZE - 1);
                        message[BUFFER_SIZE - 1] = '\0';
                    } else {
                        printf("Mensagem vazia!\n");
                        return;
                    }
                }
                else {
                    printf("Envia para outro destinatário!\n");
                }
            } 
            else {
                printf("Destinatário inexistente!\n");
                return;
            }
            // Escolher o vizinho de caminho mais rápido
            int next_fd = choose_next(dest);
            if(next_fd != -1)
                message_command(message, me.ID, dest, &succ_fd);
        }

        // LEAVE /////////////////////////////////////////////////////////////
        else if((strncmp(command, "leave", 5) == 0 || strncmp(command, "l", 1) == 0) && *joined == 1) {
            snprintf(regPortStr, sizeof(regPortStr), "%d", regUDP);
            if(leave_command(regPortStr, currentRing, node_TCP, nodeIP) == 0) {
                *joined = 0;
            }
        }
        
        // EXIT //////////////////////////////////////////////////////////////
        else if(strncmp(command, "exit", 4) == 0 || strncmp(command, "x", 1) == 0) {
            snprintf(regPortStr, sizeof(regPortStr), "%d", regUDP);
            if(*joined == 1) {
                leave_command(regPortStr, currentRing, node_TCP, nodeIP);    
            }
            close_all_chords();
            close(listen_fd);
            exit(0);
        }
        
        // limpar anel
        else if(strncmp(command, "clean", 5) == 0) {
            char clean_node[3];

            sscanf(command, "clean %s %s", ringID, clean_node);

            // Converter regPort de int para string
            snprintf(regPortStr, sizeof(regPortStr), "%d", regUDP);
            if(clean_ring_DEBUG(regPortStr, ringID, clean_node, node_TCP, nodeIP) == 0) {
                *joined = 0;
            }
        }
        // Comando inexistente
         else {
            printf("Comando Inválido\n");
        }
    }
} //user_input()

// Tratamento da socket do sucessor
void process_succ_fd(int listen_fd, int *chorded) {
    char buffer[BUFFER_SIZE];
    ssize_t bytesRead = read(succ_fd, buffer, sizeof(buffer) - 1);
    if(bytesRead > 0) {
        buffer[bytesRead] = '\0';

        char *line = strtok(buffer, "\n");
        char routesBuffer[BUFFER_SIZE] = "";

        while (line != NULL) {
            Node entryNode;
            char chatOrig[3], chatDest[3];

            // Processa 'ENTRY'
            if(sscanf(line, "ENTRY %s %s %d", entryNode.ID, entryNode.IP, &entryNode.port) == 3) {

                succ2 = succ;
                succ = entryNode;

                send_pred_message(&entryNode);
                send_succ_message(pred_fd, &entryNode);
            }
            // Processa 'SUCC'
            else if(sscanf(line, "SUCC %s %s %d", entryNode.ID, entryNode.IP, &entryNode.port) == 3) {

                succ2 = entryNode;
            }
            // Processa 'ROUTE'
            else if(strncmp(line, "ROUTE", 5) == 0) {
                strcat(routesBuffer, line);
                strcat(routesBuffer, "\n");
            }
            // Processa 'CHAT'
            else if(sscanf(line, "CHAT %s %s", chatOrig, chatDest) == 2) {

                // A mensagem é para o meu nó
                if(strcmp(chatDest, me.ID) == 0) {

                    // Encontra o início da mensagem
                    char* messageStart = strstr(line, " ") + 1; // Salta o 'CHAT'
                    messageStart = strstr(messageStart, " ") + 1; // Salta o origID
                    messageStart = strstr(messageStart, " ") + 1; // Salta o destID

                    if (messageStart) {
                        printf("[%s] -> %s\n", chatOrig, messageStart);
                    }
                } 
                // Não é para o meu nó
                else {
                    int next_fd = choose_next(chatDest);
                    if(next_fd != -1) {
                        if(write(next_fd, line, strlen(line) + 1) == -1)
                            perror("write CHAT");
                    }
                    // Não há caminho
                    else {
                        printf("Destinatário não existe!\n");
                    }
                }
            }

            line = strtok(NULL, "\n"); // Vai para a próxima linha no buffer
        }
        // Processa todas as mensagens 'ROUTE' agrupadas
        if(strlen(routesBuffer) > 0) {
            handle_routing(routesBuffer, succ_fd);
        }
    } 
    // ligação graciosamente fechada pelo outro lado
    else {
        // Ficaram 2+ nó no anel
        if(strcmp(me.ID, succ2.ID) != 0) {

            // Fazer alterações na tabela(sucessor saiu)
            remove_routing_neighbor(&routingTable, succ.ID);
            remove_routing_destinations(succ.ID);
            remove_shortest_path_destination(&shortestPathTable, succ.ID);

            char changed_paths[BUFFER_SIZE], routes_to_send[BUFFER_SIZE];
            snprintf(routes_to_send, BUFFER_SIZE, "ROUTE %s %s\n", me.ID, succ.ID);

            // Atualizar sucessor
            succ = succ2;
            // Informar novo sucessor
            send_pred_message(&succ);
            // Informar predecessor
            send_succ_message(pred_fd, &succ);

            // Mudar e enviar informações de caminhos
            find_shortest_path(&routingTable, &shortestPathTable, changed_paths);
            strcat(routes_to_send, changed_paths);

            // Enviar mudanças 
            send_all_routes_to_neighbor(succ_fd);
            send_route_to_neighbours(succ_fd, routes_to_send);
        } 
        // Ficámos sozinhos no anel
        else {
            // Fazer Reset nas tabelas
            init_routing_table(&routingTable);
            init_shortest_path_table(&shortestPathTable);

            // Atualizar vizinhos para serem iguais a nós
            succ = me;
            succ2 = me;
            strncpy(pred.ID, me.ID, strlen(me.ID));

            // Não há ligações
            close(succ_fd);
            succ_fd = -1;
            close(pred_fd);
            pred_fd = -1;
            update_max_fd(listen_fd);
        }
    } 

    close_invalid_chords(chorded);
}

// Tratamento da socket do predecessor
void process_pred_fd(int listen_fd, int *chorded) {
    char buffer[BUFFER_SIZE];
    ssize_t bytesRead = read(pred_fd, buffer, sizeof(buffer) - 1); // Garantir espaço para o terminador null
    if(bytesRead > 0) {
        buffer[bytesRead] = '\0';

        char *line = strtok(buffer, "\n");
        char routesBuffer[BUFFER_SIZE] = ""; // Buffer para acumular mensagens ROUTE

        while (line != NULL) {
            char chatOrig[3], chatDest[3];

            // Processa 'CHAT'
            if(sscanf(line, "CHAT %s %s", chatOrig, chatDest) == 2) {

                // A mensagem é para o meu nó
                if(strcmp(chatDest, me.ID) == 0) {

                    // Encontra o início da mensagem
                    char* messageStart = strstr(line, " ") + 1; // Salta o 'CHAT'
                    messageStart = strstr(messageStart, " ") + 1; // Salta o origID
                    messageStart = strstr(messageStart, " ") + 1; // Salta o destID

                    if(messageStart) {
                        printf("[%s] -> %s\n", chatOrig, messageStart);
                    }
                } 
                // Não é para o meu nó
                else {
                    int next_fd = choose_next(chatDest);
                    if(next_fd != -1) {
                        write(next_fd, line, strlen(line) + 1);
                    }
                    // Não há caminho
                    else {
                        printf("Destinatário não existe!\n");
                    }
                }
            }
            // Processa 'ROUTE'
            else if(strncmp(line, "ROUTE", 5) == 0) {
                strcat(routesBuffer, line);
                strcat(routesBuffer, "\n");
            }

            line = strtok(NULL, "\n"); // Próxima linha no buffer
        }
        // Processa todas as mensagens 'ROUTE' agrupadas
        if(strlen(routesBuffer) > 0) {
            handle_routing(routesBuffer, pred_fd);
        }
    }
    close_invalid_chords(chorded);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Comando join
int join_command(char *regPortStr, char *ringID, int node_TCP, char *nodeIP) {
    Node node_list[MAX_NODES];
    char buffer[BUFFER_SIZE];

    // Enviar o pedido ao servidor - "NODES"
    sprintf(buffer, "NODES %s", ringID);
    if(udp_communication(regPortStr, buffer, buffer, BUFFER_SIZE) == -1) {
        printf("O pedido ao servidor falhou.\n");
        return 1;
    }
    
    // Já existem nós no anel
    if(strlen(buffer) > 14) {
        int node_count = parse_node_list(buffer, node_list, MAX_NODES);
        if(node_count >= 16) {
            printf("O anel escolhido está cheio!\n");
            return 1;
        }

        // Verificar se o ID escolhido já existe
        while(free_id(node_list, node_count, me.ID)) {
            char new_ID[3];
            get_new_id(node_list, node_count, new_ID);
            printf("O ID escolhido já existe. Foi escolhido outro ID: %s\n\n", new_ID);
            strncpy(me.ID, new_ID, strlen(new_ID));
        }

        // Escolha random de sucessor
        Node randomNode = choose_node(node_list, node_count);

        // Informar nós da minha entrada
        if(d_join_command(randomNode.IP, randomNode.port, randomNode.ID, me.port, me.IP) == -1) {
            return 1;
        }
        reg_flag = 1;
        return 0;
    } 

    // Somos o primeiro nó a entrar
    else {
        d_join_command(me.IP, me.port, me.ID, me.port, me.IP);

        joining = 0;

        // Enviar o pedido ao servidor - "REG"
        sprintf(buffer, "REG %s %s %s %d", ringID, me.ID, nodeIP, node_TCP);
        if(udp_communication(regPortStr, buffer, buffer, BUFFER_SIZE) == -1) {
            printf("O pedido ao servidor falhou.\n");
            return 1;
        } else if(strncmp(buffer, "OKREG", 5) != 0) {
            printf("Registo não aceite pelo servidor.\n");
            return 1;
        }
        reg_flag = 1;
        printf("Entrou no Anel %s\n", currentRing);

        return 0;
    }
}//join_command()

// Comando direct join
int d_join_command(char *succIP, int succPort, char *succID, int node_TCP, char *nodeIP) {

    init_routing_table(&routingTable);
    init_shortest_path_table(&shortestPathTable);

    // Se o nó se liga a si próprio
    if(strcmp(succID, me.ID) == 0) {
        succ = me;
        succ2 = me;
        strncpy(pred.ID, me.ID, strlen(me.ID));
    }
    // Se o nó se liga a outro nó
    else {
        // Guarda informações
        strncpy(succ.ID, succID, strlen(succID));
        strncpy(succ.IP, succIP, INET_ADDRSTRLEN);
        succ.port = succPort;
        
        // Envia 'ENTRY'
        int result = send_entry_message(&succ);
        if(result != 0) {
            clear_node(&succ);
            return -1;
        }
    }
    joining = 1;
    return 0;
} //d_join_command()

// Comando para criar corda
int chord_command(char *regPortStr, char *ringID, int node_TCP, char *nodeIP) {
    Node node_list[MAX_NODES];
    char buffer[BUFFER_SIZE];

    // Pedir lista de nós ao servidor
    sprintf(buffer, "NODES %s", ringID);
    if(udp_communication(regPortStr, buffer, buffer, BUFFER_SIZE) == -1) {
        printf("O pedido ao servidor falhou.\n");
        return 1;
    }
    
    int node_count = parse_node_list(buffer, node_list, MAX_NODES);
    if(node_count > 3) {
        // Escolha random de destino da corda
        Node randomNode = choose_node(node_list, node_count);
        if (is_existing_chord(randomNode, chords, MAX_CORDS)) {
            // Já é uma das cordas
            printf("Todas as cordas possíveis já estão formadas!\n");
            return 1;
        }
        printf("Nó escolhido para estabelecer corda: %s\n", randomNode.ID);

        // Informar nó da corda
        if(send_chord_message(&randomNode) == -1) {
            return 1;
        }
        send_all_routes_to_neighbor(my_chord_fd);

    } else {
        printf("Não existem nós suficientes para estabelecer uma corda!\n");
        return 1;
    }
    return 0;
    
}//chord_command()

// Comando para remover corda
void remove_chord_command(int fd) {
    for (int i = 0; i < MAX_CORDS; i++) {
        if(chords[i].fd == fd && chords[i].is_active) {
            chords[i].fd = -1;
            char changed_paths[BUFFER_SIZE], routes_to_send[BUFFER_SIZE];

            printf("Corda para %s fechada.\n", chords[i].nodeID);

            // Remover corda vizinha da tabela de encaminhamento
            remove_routing_neighbor(&routingTable, chords[i].nodeID);
            snprintf(routes_to_send, BUFFER_SIZE, "ROUTE %s %s\n", me.ID, chords[i].nodeID);

            strcpy(chords[i].nodeID, "");
            chords[i].is_active = false;
            close(my_chord_fd);

            // Mudar e enviar informações de caminhos
            find_shortest_path(&routingTable, &shortestPathTable, changed_paths);
            strcat(routes_to_send, changed_paths);
            send_route_to_neighbours(-2, routes_to_send);       
            break;
        }
    }


} //remove_chord_command()

// Comando show topology
void show_topology() {
    print_node_info("PRED ", &pred);
    print_node_info("ME   ", &me);
    print_node_info("SUCC ", &succ);
    print_node_info("SUCC2", &succ2);
    print_active_chords();
    printf("\n");
} //show_topology()

// Comando para imprimir tabela de encaminhamento
void show_routing_command(RoutingTable *table, char *destinationID) {
    bool found = false;

    for (int i = 0; i < table->nodeCount; ++i) {
        if(strncmp(table->entries[i].nodeID, destinationID, 2) == 0) {
            found = true;
            // Itera sobre todos os vizinhos
            for (int j = 0; j < table->neighborCount; ++j) {
                if(table->entries[i].paths[j][0] != '\0') { // Se houver um caminho pelo vizinho j
                    printf("Caminhos para o destino %s:\n", destinationID);
                    printf("  Via Vizinho %s: %s\n", table->neighborIDs[j], table->entries[i].paths[j]);
                }
            }
            break;
        }
    }
    if(!found) {
        printf("Não existe caminho para %s.\n", destinationID);
    }
}

// Comando para ver o caminho mais curto para um destino
void show_path_command(ShortestPathTable *table, char *destinationID) {
    for (int i = 0; i < table->nodeCount; i++) {
        if(strcmp(table->entries[i].nodeID, destinationID) == 0) {
            printf("Menor caminho para o destino %s:\n", destinationID);
            printf("%7s | %s\n", table->entries[i].nodeID, table->entries[i].shortestPath);
            return;
        }
    }
    printf("Nenhum caminho encontrado para o destino %s.\n", destinationID);
}

// Comando para ver tabela de expedição
void show_forwarding_command() {
    printf("Tabela de Expedição:\n");
    printf("Destino | Próximo Nó\n");
    
    for (int i = 0; i < shortestPathTable.nodeCount; i++) {
        char* path = shortestPathTable.entries[i].shortestPath;
        char nextNode[3] = {'\0'};
        char* current = path;

        while (current != NULL && *current != '\0') {
            if(strncmp(current, me.ID, 2) == 0) {
                current = strchr(current, '-');
                if(current != NULL) current++;
            } else {
                if(sscanf(current, "%2s", nextNode) == 1) {
                    break;
                }
            }
        }

        // Imprime o destino e o próximo nó
        printf("%7s | %s\n", shortestPathTable.entries[i].nodeID, nextNode);
    }
    printf("%7s | --\n", me.ID);
}

// Comando message
void message_command(char *buffer, char *origin, char *dest, int *fd) {
    char Message[BUFFER_SIZE];

    sprintf(Message, "CHAT %s %s %s\n", origin, dest, buffer);
    if(write(*fd, Message, sizeof(Message)) == -1)
        perror("write CHAT");
} //message()

// Comando leave
int leave_command(char *regPortStr, char *ringID, int node_TCP, char *nodeIP) {
    char buffer[BUFFER_SIZE];

    // Enviar o pedido ao servidor - "UNREG"
    if(reg_flag) {
        sprintf(buffer, "UNREG %s %s", ringID, me.ID);
        if(udp_communication(regPortStr, buffer, buffer, BUFFER_SIZE) == -1) {
            printf("Não foi possível completar a comunicação UDP.\n");
            return 1;
        } else {
            reg_flag = 0;
        }
    }
    printf("Saiu do Anel %s\n", currentRing);

    // Limpeza
    init_routing_table(&routingTable);
    init_shortest_path_table(&shortestPathTable);

    clear_node(&pred);
    clear_node(&succ);
    clear_node(&succ2);
    close_all_chords();

    close(succ_fd);
    succ_fd = -1;
    close(pred_fd);
    pred_fd = -1;

    return 0;
} //leave_command()

// DEBUG para retirar nós específicos do servidor
int clean_ring_DEBUG(char *regPortStr, char *ringID, char *clean_id, int node_TCP, char *nodeIP) {
    struct addrinfo *servinfo;
    char buffer[BUFFER_SIZE];

    // Início ligação UDP com o servidor de nós
    int sock_UDP = setup_udp_connection(regPortStr, &servinfo);
    if(sock_UDP == -1) {
        return -1;
    }

    // Enviar o pedido ao servidor - "NODES ringID"
    sprintf(buffer, "UNREG %s %s", ringID, clean_id);
    if(send_udp_message(sock_UDP, servinfo, buffer) == -1 || receive_udp_message(sock_UDP, buffer, BUFFER_SIZE) == -1) {
        freeaddrinfo(servinfo);
        close(sock_UDP);
        return -1;
    }

    // Manutenção
    close(sock_UDP);
    freeaddrinfo(servinfo);

    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup de ligação UDP
int setup_udp_connection(char *regPortStr, struct addrinfo **servinfo) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int status;
    if((status = getaddrinfo(regIP, regPortStr, &hints, servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return -1;
    }

    int sock_UDP = socket((*servinfo)->ai_family, (*servinfo)->ai_socktype, (*servinfo)->ai_protocol);
    if(sock_UDP == -1) {
        perror("Erro ao criar o socket UDP");
        return -1;
    }
    return sock_UDP;
} //setup_udp_connection()

// Envio de mensagem UDP
int send_udp_message(int sock_UDP, struct addrinfo *servinfo, char *message) {
    //printf("<- %s\n\n", message);
    ssize_t numbytes = sendto(sock_UDP, message, strlen(message), 0, servinfo->ai_addr, servinfo->ai_addrlen);
    if(numbytes == -1) {
        perror("Erro ao enviar mensagem ao servidor");
        return -1;
    }
    return 0;
} //send_udp_message()

// Receção de mensagem UDP
int receive_udp_message(int sock_UDP, char *buffer, size_t bufferSize) {
    struct sockaddr_storage their_addr;
    socklen_t addr_len = sizeof their_addr;
    memset(buffer, 0, bufferSize);
    ssize_t numbytes = recvfrom(sock_UDP, buffer, bufferSize - 1, 0, (struct sockaddr *)&their_addr, &addr_len);
    if(numbytes == -1) {
        perror("Erro ao receber resposta do servidor");
        return -1;
    }
    buffer[numbytes] = '\0';
    //printf("-> %s\n\n", buffer);
    return 0;
} //receive_udp_message()

// Comunicação UDP com temporizador
int udp_communication(char *regPortStr, char *message, char *response, size_t responseSize) {
    struct addrinfo *servinfo;
    int sock_UDP = setup_udp_connection(regPortStr, &servinfo);
    if(sock_UDP == -1) {
        return -1;
    }

    if(send_udp_message(sock_UDP, servinfo, message) == -1) {
        close(sock_UDP);
        freeaddrinfo(servinfo);
        return -1;
    }

    // Configura o temporizador
    struct timeval tv;
    tv.tv_sec = 5; // Tempo de espera de
    tv.tv_usec = 0;

    // Configura fd_set para o socket UDP
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock_UDP, &readfds);

    // Espera pela resposta ou pelo timeout
    int retval = select(sock_UDP + 1, &readfds, NULL, NULL, &tv);

    if(retval == -1) {
        perror("select()");
        close(sock_UDP);
        freeaddrinfo(servinfo);
        return -1;
    } else if(retval) {
        // Dados disponíveis para leitura
        if(receive_udp_message(sock_UDP, response, responseSize) == -1) {
            close(sock_UDP);
            freeaddrinfo(servinfo);
            return -1;
        }
    } else {
        printf("Timeout na espera pela resposta\n");
        close(sock_UDP);
        freeaddrinfo(servinfo);
        return -1;
    }

    close(sock_UDP);
    freeaddrinfo(servinfo);
    return 0; // Sucesso
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Setup do server TCP
void setup_tcp_listener(int *listen_fd, char *port_str, char *ip_str) {
    struct addrinfo hints, *res;
    
    *listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(*listen_fd == -1) {
        perror("Não foi possível criar a socket TCP");
        exit(EXIT_FAILURE);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    
    int errcode = getaddrinfo(NULL, port_str, &hints, &res);
    if(errcode != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(errcode));
        close(*listen_fd);
        exit(EXIT_FAILURE);
    }
    
    if(bind(*listen_fd, res->ai_addr, res->ai_addrlen) == -1) {
        perror("Bind falhou");
        close(*listen_fd);
        freeaddrinfo(res);
        exit(EXIT_FAILURE);
    }

    if(listen(*listen_fd, 5) == -1) {
        perror("Listen falhou");
        close(*listen_fd);
        freeaddrinfo(res);
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(res);
} //setup_tcp_listener()

// Aceitar ligações no port TCP
void accept_tcp_connection(int listen_fd, int *joined, int node_TCP, char *nodeIP) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_size = sizeof(client_addr);

    int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_addr_size);

    if(new_fd == -1) {
        perror("accept");
        return;
    }

    // Receber comando externo
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received = read(new_fd, buffer, sizeof(buffer) - 1);

    if(bytes_received > 0) {
        buffer[bytes_received] = '\0';

        // Interpretar comando recebido
        handle_tcp_command(buffer, joined, new_fd);

    } else if(bytes_received == 0) {
        printf("ligação fechada pelo cliente\n");
    } else {
        perror("read");
    }
} //accept_tcp_connection()

// Guardar informaçoes recebidas por TCP
void handle_tcp_command(char *buffer, int *joined, int client_fd) {
    char *line = strtok(buffer, "\n");
    char routesBuffer[BUFFER_SIZE] = "";

    while (line != NULL) {
        Node entryNode;

        // Caso receba 'ENTRY'
        if(sscanf(line, "ENTRY %s %s %d", entryNode.ID, entryNode.IP, &entryNode.port) == 3) {

            // Apenas 1 nó no anel
            if(strcmp(succ.ID, me.ID) == 0) {

                // Armazenar as informações no nó predecessor
                strncpy(pred.ID, entryNode.ID, strlen(entryNode.ID));

                // Sucessor vai ser igual ao predecessor
                succ = entryNode;

                // Segundo sucessor vai ser o nosso próprio nó
                succ2 = me;

                // Atualizar file descriptor do predecessor
                pred_fd = client_fd;

                // Enviar mensagens ao nó que entrou(predecessor)
                send_succ_message(pred_fd, &succ);
                send_pred_message(&succ);
            }
            // Mais nós no anel
            else {
                // Enviar mensagens ao nó que entrou(predecessor)
                send_succ_message(client_fd, &succ);

                // envia ENTRY ao antigo predecessor
                forward_entry_message(pred_fd, &entryNode);

                // Guarda novo predecessor
                strncpy(pred.ID, entryNode.ID, strlen(entryNode.ID));

                // Atualizar file descriptor do predecessor
                close(pred_fd);
                pred_fd = client_fd;
            }
        } 

        // Caso receba 'PRED'
        else if(sscanf(line, "PRED %s", entryNode.ID) == 1) {

            // Predecessor entrou
            if(joining == 1) {
                // Guarda novo predecessor
                strcpy(pred.ID, entryNode.ID);

                // Atualizar file descriptor do predecessor
                close(pred_fd);
                pred_fd = client_fd;

                // Registar no servidor de nós caso seja 'join' e não 'direct join'
                if(reg_flag) {
                    char regPortStr[6];
                    snprintf(regPortStr, sizeof(regPortStr), "%d", regUDP);

                    sprintf(buffer, "REG %s %s %s %d", currentRing, me.ID, me.IP, me.port);
                    if(udp_communication(regPortStr, buffer, buffer, BUFFER_SIZE) == -1) {
                        printf("Não foi possível completar a comunicação UDP.\n");
                        return;
                    } else {
                        if(strncmp(buffer, "OKREG", 5) != 0) {
                            printf("Registo não aceite pelo servidor.\n");
                            return;
                        }
                    }
                }
                joining = 0;

                printf("Entrou no Anel %s\n", currentRing);

                // Encaminhamentos
                send_all_routes_to_neighbor(pred_fd);
                if(strcmp(me.ID, succ2.ID) != 0)
                    send_all_routes_to_neighbor(succ_fd);  

            } 
            // Predecessor saiu
            else {
                // Atualizar file descriptor do predecessor
                close(pred_fd);
                pred_fd = client_fd;

                // Enviar mensagens ao nó que entrou(predecessor)
                send_succ_message(pred_fd, &succ);

                // Fazer alterações na tabela(predecessor saiu)
                remove_routing_destinations(pred.ID);
                remove_routing_neighbor(&routingTable, pred.ID);
                remove_shortest_path_destination(&shortestPathTable, pred.ID);

                char changed_paths[BUFFER_SIZE], routes_to_send[BUFFER_SIZE];

                snprintf(routes_to_send, BUFFER_SIZE, "ROUTE %s %s\n", me.ID, pred.ID);

                // Guarda novo predecessor
                strcpy(pred.ID, entryNode.ID); 

                find_shortest_path(&routingTable, &shortestPathTable, changed_paths);
                strcat(routes_to_send, changed_paths);

                send_all_routes_to_neighbor(pred_fd);
                send_route_to_neighbours(pred_fd, routes_to_send); 
            }
    
        }
        // Caso receba 'CHORD'
        if(sscanf(line, "CHORD %s", entryNode.ID) == 1) {

            add_chord(client_fd, entryNode.ID);
            send_all_routes_to_neighbor(client_fd);
        }
        // Caso receba 'ROUTE'
        if(strncmp(line, "ROUTE", 5) == 0) {
                strcat(routesBuffer, line);
                strcat(routesBuffer, "\n");
        }

        line = strtok(NULL, "\n");
    }
    // Processa todas as mensagens 'ROUTE' agrupadas
    if(strlen(routesBuffer) > 0) {
        handle_routing(routesBuffer, client_fd);
    }
} //handle_tcp_command()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Enviar protocolo 'ENTRY'
int send_entry_message(Node *target) {
    int sock_fd;
    struct addrinfo hints, *res;
    char portStr[6];

    snprintf(portStr, sizeof(portStr), "%d", target->port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    // Criar ligação com novo sucessor
    if(getaddrinfo(target->IP, portStr, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    // Criar um socket
    sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if(sock_fd == -1) {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }
    
    // Estabelecer ligação
    if(connect(sock_fd, res->ai_addr, res->ai_addrlen) == -1) {
        perror("connect");
        close(sock_fd);
        freeaddrinfo(res);
        return -1;
    }

    // Enviar ENTRY
    char entryMessage[BUFFER_SIZE];
    sprintf(entryMessage, "ENTRY %s %s %d\n", me.ID, me.IP, me.port);
    if(write(sock_fd, entryMessage, strlen(entryMessage)) == -1) {
        perror("Erro ao enviar ENTRY");
        close(sock_fd);
        freeaddrinfo(res);
        return -1;
    }
    // Atualizar fd para o sucessor
    succ_fd = sock_fd;
    ////printf("<- %s\n", entryMessage);

    return 0;
} //send_entry_message()

// Enviar protocolo 'ENTRY' para predecessor
void forward_entry_message(int sock_fd, Node *shared) {
    char entryMsg[BUFFER_SIZE];

    snprintf(entryMsg, BUFFER_SIZE, "ENTRY %s %s %d\n", shared->ID, shared->IP, shared->port);

    // Enviar ENTRY
    if(write(sock_fd, entryMsg, strlen(entryMsg)) == -1) {
        perror("write ENTRY");
    } else {
        ////printf("<- %s\n", entryMsg);
    }
} //forward_entry_message()

// Enviar protocolo 'SUCC'
void send_succ_message(int sock_fd, Node *shared) {
    char succMsg[BUFFER_SIZE];

    snprintf(succMsg, BUFFER_SIZE, "SUCC %s %s %d\n", shared->ID, shared->IP, shared->port);

    // Enviar SUCC
    if(write(sock_fd, succMsg, strlen(succMsg)) == -1) {
        perror("write SUCC");
    } else {
        ////printf("<- %s\n", succMsg);
    }
} //send_succ_message()

// Enviar protocolo 'PRED'
void send_pred_message(Node *target) {
    int sock_fd;
    struct addrinfo hints, *res;
    char portStr[6];

    snprintf(portStr, sizeof(portStr), "%d", target->port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    // Criar ligação com novo sucessor
    if(getaddrinfo(target->IP, portStr, &hints, &res) != 0) {
        perror("getaddrinfo for PRED");
        return;
    }

    // Criar um socket
    sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if(sock_fd == -1) {
        perror("socket for PRED");
        freeaddrinfo(res);
        return;
    }

    // Estabelecer ligação
    if(connect(sock_fd, res->ai_addr, res->ai_addrlen) == -1) {
        perror("connect for PRED");
        close(sock_fd);
        freeaddrinfo(res);
        return;
    }

    char predMsg[BUFFER_SIZE];
    snprintf(predMsg, BUFFER_SIZE, "PRED %s\n", me.ID);

    // Enviar PRED
    if(write(sock_fd, predMsg, strlen(predMsg)) == -1) {
        perror("send PRED");
    }
    ////printf("<- %s\n", predMsg);

    close(succ_fd);
    succ_fd = sock_fd;

} //send_pred_message()

// Enviar protocolo 'CHORD'
int send_chord_message(Node *target) {
    int sock_fd;
    struct addrinfo hints, *res;
    char portStr[6];

    snprintf(portStr, sizeof(portStr), "%d", target->port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    // Criar ligação com novo vizinho
    if(getaddrinfo(target->IP, portStr, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    // Criar um socket
    sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if(sock_fd == -1) {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }
    
    // Estabelecer ligação
    if(connect(sock_fd, res->ai_addr, res->ai_addrlen) == -1) {
        perror("connect");
        close(sock_fd);
        freeaddrinfo(res);
        return -1;
    }

    // Enviar CHORD
    char chordMessage[BUFFER_SIZE];
    sprintf(chordMessage, "CHORD %s\n", me.ID);
    if(write(sock_fd, chordMessage, strlen(chordMessage)) == -1) {
        perror("Erro ao enviar mensagem CHORD");
        close(sock_fd);
        freeaddrinfo(res);
        return -1;
    }
    // Atualizar fd da corda
    my_chord_fd = sock_fd;
    add_chord(sock_fd, target->ID);
    //printf("<- %s", chordMessage);

    return 0;
} //send_chord_message()

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Função para inicializar as "cordas"
void init_chords() {
    for (int i = 0; i < MAX_CORDS; i++) {
        chords[i].fd = -1;
        strcpy(chords[i].nodeID, "");
        chords[i].is_active = false;
    }
}

// Função para adicionar uma nova "corda"
void add_chord(int fd, char* nodeID) {
    for (int i = 0; i < MAX_CORDS; i++) {
        if(!chords[i].is_active) {
            chords[i].fd = fd;
            strncpy(chords[i].nodeID, nodeID, 2);
            chords[i].nodeID[2] = '\0';
            chords[i].is_active = true;
            break;
        }
    }
}

// Imprimir cordas
void print_active_chords() {
    printf("Cordas Ativas:\n");
    for (int i = 0; i < MAX_CORDS; i++) {
        if(chords[i].is_active) {
            printf("ME <-> %s\n", chords[i].nodeID);
        }
    }
}

// Descobrir se certo nó já uma corda nossa
int is_existing_chord(Node randomNode, Cord chords[], int max_cords) {
    for (int i = 0; i < max_cords; i++) {
        if (chords[i].is_active && strcmp(chords[i].nodeID, randomNode.ID) == 0) {
            // Node ID já é uma das cordas ativas
            return 1;
        }
    }
    // Node ID não é uma corda ativa
    return 0;
}

// Fechar todas as cordas
void close_all_chords() {
    for (int i = 0; i < MAX_CORDS; i++) {
        if(chords[i].is_active) {
            close(chords[i].fd);
            chords[i].fd = -1;
            chords[i].is_active = false;
            strcpy(chords[i].nodeID, "");
        }
    }
}

// Fechar as cordas que se tornam vizinhos/vê se ainda tenho a minha corda criada
void close_invalid_chords(int *chorded) {
    int activeChords = 0; // Contador de cordas ativas

    for (int i = 0; i < MAX_CORDS; i++) {
        if(chords[i].is_active) {
            if(strcmp(chords[i].nodeID, pred.ID) == 0 || strcmp(chords[i].nodeID, succ.ID) == 0) {
                close(chords[i].fd);
                chords[i].fd = -1;
                chords[i].is_active = false;
                strcpy(chords[i].nodeID, "");
            } else {
                activeChords++;
            }
        }
    }

    // Se após verificar todas as cordas não houver nenhuma ativa
    if(activeChords == 0) {
        *chorded = 0;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Decifrar informação de ROUTE
void handle_routing(char *buffer, int fd) {
    char new_path[BUFFER_SIZE];
    char routes_to_send[BUFFER_SIZE * MAX_NODES];
    memset(routes_to_send, 0, sizeof(routes_to_send));

    // Verificar mudanças nos vizinhos
    check_neighbours(&routingTable);

    char *line = strtok(buffer, "\n");
    while (line != NULL) {
        char neighborID[3], destID[3], path[MAX_PATH_LENGTH];

        // Caso geral
        if(sscanf(line, "ROUTE %s %s %s", neighborID, destID, path) == 3) {

            // Verifica se o meu ID está no caminho
            memset(new_path, 0, sizeof new_path);
            char *found = strstr(path, me.ID);
            
            // Se eu for o destino, passar à frente
            if(strcmp(me.ID, destID) == 0) {
                // Passa para a próxima linha
                line = strtok(NULL, "\n");
                continue;
            }

            if(found) {
                // Passa para a próxima linha
                line = strtok(NULL, "\n");
                continue;
            } else {
                // Se não encontrou, concatena o meu ID no início
                snprintf(new_path, sizeof(new_path), "%s-%s", me.ID, path);
            }

            // Adicionar entrada
            add_routing_entry(&routingTable, neighborID, destID, new_path);
        
            char changed_paths[BUFFER_SIZE];
            find_shortest_path(&routingTable, &shortestPathTable, changed_paths);
            strcat(routes_to_send, changed_paths); 
        }
        // Saída de um nó (ROUTE i j\n)
        else {
            // Deixa de haver caminho para destino j pelo vizinho i
            char changed_paths[BUFFER_SIZE], deleted_path[BUFFER_SIZE];

            if(remove_neighbor_path(&routingTable, neighborID, destID) == 0) {

                snprintf(deleted_path, BUFFER_SIZE, "ROUTE %s %s\n", me.ID, destID);
                strcat(routes_to_send, deleted_path);
            }
            remove_destination_if_no_paths(&routingTable, &shortestPathTable, destID);
            find_shortest_path(&routingTable, &shortestPathTable, changed_paths);
            strcat(routes_to_send, changed_paths); 
        }
        // Passa para a próxima linha
        line = strtok(NULL, "\n");
    }

    // Caso seja ROUTE de 'introdução' (ROUTE i i i\n)
    if(strlen(buffer) == 14) {
        // Enviar todos os meus caminhos mais curtos
        send_all_routes_to_neighbor(fd);
        if(pred.ID != succ.ID)
            send_route_to_neighbours(fd, routes_to_send);
    }
    // Caso geral
    else {
        // Caso tenham sido feitas mudanças
        if(strlen(routes_to_send) != 0) {
            send_route_to_neighbours(fd, routes_to_send);
        }
    }
}

// Envia routes alterados para os seus vizinhos
void send_route_to_neighbours(int exclude_fd, char *buffer) {
    // Enviar para o predecessor, se ativo e não for o excluído
    if(pred_fd > 0 && pred_fd != exclude_fd) {
        write(pred_fd, buffer, strlen(buffer));
        //printf("<- %s", buffer);
    }

    // Enviar para o sucessor, se ativo e não for o excluído
    if(succ_fd > 0 && succ_fd != exclude_fd) {
        write(succ_fd, buffer, strlen(buffer));
        //printf("<- %s", buffer);
    }

    // Enviar para todas as cordas ativas, exceto a excluída
    for (int i = 0; i < MAX_CORDS; i++) {
        if(chords[i].is_active && chords[i].fd != exclude_fd) {
            write(chords[i].fd, buffer, strlen(buffer));
            //printf("<- %s", buffer);
        }
    }
}

// Envia as todos os nossos melhores caminhos para um vizinho
void send_all_routes_to_neighbor(int target_fd) {
    if(target_fd <= 0) {
        return;
    }

    char all_routes_msg[BUFFER_SIZE * MAX_NODES];
    memset(all_routes_msg, 0, sizeof(all_routes_msg));

    // Adiciona todos os melhores caminhos
    for (int i = 0; i < shortestPathTable.nodeCount; i++) {
        char route_msg[BUFFER_SIZE];
        snprintf(route_msg, sizeof(route_msg), "ROUTE %s %s %s\n", me.ID, shortestPathTable.entries[i].nodeID, shortestPathTable.entries[i].shortestPath);
        if(strlen(all_routes_msg) + strlen(route_msg) < sizeof(all_routes_msg) - BUFFER_SIZE) { 
            strcat(all_routes_msg, route_msg);
        } else {
            fprintf(stderr, "Buffer overflow.\n");
            break;
        }
    }

    // Adiciona o próprio caminho
    char self_route_msg[BUFFER_SIZE];
    snprintf(self_route_msg, sizeof(self_route_msg), "ROUTE %s %s %s\n", me.ID, me.ID, me.ID);
    strcat(all_routes_msg, self_route_msg);

    // Envia a mensagem completa com todos os melhores caminhos
    write(target_fd, all_routes_msg, strlen(all_routes_msg));
    //printf("<- %s", all_routes_msg);
}
//---------------------------------------------------------------------------------------------------------

// Inicializa uma tabela de encaminhamento vazia
void init_routing_table(RoutingTable *table) {
    table->nodeCount = 0;
    table->neighborCount = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        table->entries[i].nodeID[0] = '\0';
        for (int j = 0; j < MAX_NEIGHBORS; j++) {
            table->entries[i].paths[j][0] = '\0';
        }
    }
    for (int i = 0; i < MAX_NEIGHBORS; i++) {
        table->neighborIDs[i][0] = '\0';
    }
}

// Inicializa uma tabela de caminhos mais curtos vazia
void init_shortest_path_table(ShortestPathTable *table) {
    table->nodeCount = 0;
    for (int i = 0; i < MAX_NODES; i++) {
        table->entries[i].nodeID[0] = '\0';
        table->entries[i].shortestPath[0] = '\0';
    }
}
//---------------------------------------------------------------------------------------------------------

// Adiciona uma entrada à tabela de encaminhamento
void add_routing_entry(RoutingTable *table, char *neighborID, char *destID, char *path) {
    int entryIndex = -1;
    // Procura se o destino já existe na tabela
    for (int i = 0; i < table->nodeCount; i++) {
        if(strcmp(table->entries[i].nodeID, destID) == 0) {
            entryIndex = i;
            break;
        }
    }

    // Se o destino não existe adiciona como uma nova entrada
    if(entryIndex == -1 && table->nodeCount < MAX_NODES) {
        entryIndex = table->nodeCount;
        strncpy(table->entries[entryIndex].nodeID, destID, sizeof(table->entries[entryIndex].nodeID) - 1);
        table->entries[entryIndex].nodeID[sizeof(table->entries[entryIndex].nodeID) - 1] = '\0';
        table->nodeCount++;
    }

    // Encontra o índice do vizinho correspondente ao neighborID
    int neighborIndex = -1;
    for (int i = 0; i < table->neighborCount; i++) {
        if(strcmp(table->neighborIDs[i], neighborID) == 0) {
            neighborIndex = i;
            break;
        }
    }

    // Se o neighborID não for encontrado adiciona-o à lista de vizinhos
    if(neighborIndex == -1 && table->neighborCount < MAX_NEIGHBORS) {
        strncpy(table->neighborIDs[table->neighborCount], neighborID, sizeof(table->neighborIDs[table->neighborCount]) - 1);
        table->neighborIDs[table->neighborCount][sizeof(table->neighborIDs[table->neighborCount]) - 1] = '\0';
        neighborIndex = table->neighborCount;
        table->neighborCount++;
    }

    // Se encontra o vizinho atualiza o caminho
    if(neighborIndex != -1) {
        strncpy(table->entries[entryIndex].paths[neighborIndex], path, sizeof(table->entries[entryIndex].paths[neighborIndex]) - 1);
        table->entries[entryIndex].paths[neighborIndex][sizeof(table->entries[entryIndex].paths[neighborIndex]) - 1] = '\0';
    }
}

// Remove uma entrada à tabela de encaminhamento
void remove_routing_neighbor(RoutingTable *table, char *neighborID) {
    int neighborIndex = -1;

    // Encontra o índice do vizinho a ser removido
    for (int i = 0; i < table->neighborCount; i++) {
        if(strcmp(table->neighborIDs[i], neighborID) == 0) {
            neighborIndex = i;
            break;
        }
    }

    if(neighborIndex == -1) {
        // Vizinho não encontrado
        return;
    }

    // Remove o ID do vizinho da lista de IDs dos vizinhos
    for (int i = neighborIndex; i < table->neighborCount - 1; i++) {
        strcpy(table->neighborIDs[i], table->neighborIDs[i + 1]);
    }
    table->neighborIDs[table->neighborCount - 1][0] = '\0';
    table->neighborCount--;

    // Para cada entrada na tabela de encaminhamento remove o caminho associado a esse vizinho
    for (int i = 0; i < table->nodeCount; i++) {
        for (int j = neighborIndex; j < table->neighborCount; j++) {
            strcpy(table->entries[i].paths[j], table->entries[i].paths[j + 1]);
        }
        table->entries[i].paths[table->neighborCount][0] = '\0';
    }
}

// Remove destinos inexistentes
void remove_routing_destinations(char *destID) {
    int i = 0;
    while (i < routingTable.nodeCount) {
        if(strcmp(routingTable.entries[i].nodeID, destID) == 0) {
            
            // Se não é o último elemento desloca entradas
            if(i < routingTable.nodeCount - 1) {
                memmove(&routingTable.entries[i], &routingTable.entries[i + 1], (routingTable.nodeCount - i - 1) * sizeof(RoutingEntry));
            }

            routingTable.nodeCount--;

        } else {
            i++;
        }
    }
}

// Remove caminho inválido para destino
int remove_neighbor_path(RoutingTable *table, char *neighborID, char *destID) {
    int neighborIndex = -1;
    int destIndex = -1;

    // Encontra o índice do vizinho
    for (int i = 0; i < table->neighborCount; i++) {
        if(strcmp(table->neighborIDs[i], neighborID) == 0) {
            neighborIndex = i;
            break;
        }
    }

    // Encontrar o índice do destino
    for (int i = 0; i < table->nodeCount; i++) {
        if(strcmp(table->entries[i].nodeID, destID) == 0) {
            destIndex = i;
            break;
        }
    }

    // Destino não encontrado
    if(destIndex == -1) {
        return 1;
    }

    // Remove o caminho
    table->entries[destIndex].paths[neighborIndex][0] = '\0';
    return 0;
}

// Adiciona ou atualiza uma entrada à tabela de caminhos mais curtos
int add_shortest_path_entry(ShortestPathTable *table, char *nodeID, char *path) {
    // Procura se já existe um caminho para o nodeID
    for (int i = 0; i < table->nodeCount; i++) {
        if(strcmp(table->entries[i].nodeID, nodeID) == 0) {
            // Novo caminho é diferente
            if(strcmp(table->entries[i].shortestPath, path) != 0) {
                strncpy(table->entries[i].shortestPath, path, MAX_PATH_LENGTH - 1);
                table->entries[i].shortestPath[MAX_PATH_LENGTH - 1] = '\0';
                return 1; // Uma atualização
            }
            return 0; // Nenhuma atualização
        }
    }

    // Se o nodeID não é encontrado adiciona entrada
    if(table->nodeCount < MAX_NODES) {
        strncpy(table->entries[table->nodeCount].nodeID, nodeID, 2);
        table->entries[table->nodeCount].nodeID[2] = '\0';
        strncpy(table->entries[table->nodeCount].shortestPath, path, MAX_PATH_LENGTH - 1);
        table->entries[table->nodeCount].shortestPath[MAX_PATH_LENGTH - 1] = '\0';
        table->nodeCount++;
        return 1; // Uma nova entrada
    } else {
        return 0; // Nenhuma nova entrada
    }
}

// Remove destinos inexistentes
void remove_shortest_path_destination(ShortestPathTable *table, char *destID) {
    for (int i = 0; i < table->nodeCount; ++i) {
        if(strcmp(table->entries[i].nodeID, destID) == 0) {
            if(i < table->nodeCount - 1) {
                memmove(&table->entries[i], &table->entries[i + 1], (table->nodeCount - i - 1) * sizeof(ShortestPathEntry));
            }
            table->nodeCount--;
            return;
        }
    }
}

// Remove destinos com caminhos vazios
void remove_destination_if_no_paths(RoutingTable *routingTable, ShortestPathTable *shortestPathTable, char *destID) {
    int i, j;
    int hasPaths = 0;

    // Verifica na tabela de encaminhamento se existem caminhos para o destino
    for (i = 0; i < routingTable->nodeCount; i++) {
        if (strcmp(routingTable->entries[i].nodeID, destID) == 0) {
            for (j = 0; j < MAX_NEIGHBORS; j++) {
                if (routingTable->entries[i].paths[j][0] != '\0') {
                    hasPaths = 1;  // Encontrou pelo menos um caminho
                    break;
                }
            }
            if (!hasPaths) {
                // Remover destino da tabela de encaminhamento
                for (j = i; j < routingTable->nodeCount - 1; j++) {
                    routingTable->entries[j] = routingTable->entries[j + 1];
                }
                routingTable->nodeCount--;
            }
            break;
        }
    }

    // Verifica se o destino está na tabela de caminhos mais curtos e remove
    if (!hasPaths) {
        for (i = 0; i < shortestPathTable->nodeCount; i++) {
            if (strcmp(shortestPathTable->entries[i].nodeID, destID) == 0) {
                for (j = i; j < shortestPathTable->nodeCount - 1; j++) {
                    shortestPathTable->entries[j] = shortestPathTable->entries[j + 1];
                }
                shortestPathTable->nodeCount--;
                break;
            }
        }
    }
}
//---------------------------------------------------------------------------------------------------------

// Função auxiliar para contar os - num caminho
int count_hops(char *path) {
    int hops = 0;
    char *p = path;

    while (*p != '\0') {
        if(*p == '-') {
            hops++;
        }
        p++;
    }
    return hops;
}

// Encontra o caminho mais curto para cada destino e adiciona à tabela de caminhos mais curtos
int find_shortest_path(RoutingTable *routingTable, ShortestPathTable *shortestPathTable, char *changed_paths) {
    int changesMade = 0;

    if(changed_paths) {
        changed_paths[0] = '\0';
    }

    // Itera sobre todos os nós conhecidos na tabela de encaminhamento
    for (int i = 0; i < routingTable->nodeCount; i++) {
        int minHops = INT_MAX;
        char *shortestPath = NULL;
        int allPathsEmpty = 1;

        // Itera sobre todos os caminhos para o nó i a partir de cada vizinho
        for (int j = 0; j < routingTable->neighborCount; ++j) {
            if(routingTable->entries[i].paths[j][0] != '\0') {
                allPathsEmpty = 0;
                int hops = count_hops(routingTable->entries[i].paths[j]);
                if(hops < minHops) {
                    minHops = hops;
                    shortestPath = routingTable->entries[i].paths[j];
                }
            }
        }

        if(!allPathsEmpty && shortestPath) {
            int changed = add_shortest_path_entry(shortestPathTable, routingTable->entries[i].nodeID, shortestPath);
            if(changed && changed_paths) {
                
                add_changed_route(changed_paths, me.ID, routingTable->entries[i].nodeID, shortestPath);
                changesMade = 1;
            }
        }
    }
    return changesMade;
}

// Criação de mensagem ROUTE alterada
void add_changed_route(char *changed_paths, char *sourceID, char *destID, char *path) {
    if(sourceID == NULL || destID == NULL || path == NULL || changed_paths == NULL) {
        return;
    }
    if(strlen(changed_paths) + strlen(sourceID) + strlen(destID) + strlen(path) + 10 < BUFFER_SIZE) {
        char route[MAX_PATH_LENGTH];
        snprintf(route, sizeof(route), "ROUTE %s %s %s\n", sourceID, destID, path);
        strcat(changed_paths, route);
    }
}

// Descobre se houve uma mudança nos vizinhos
void check_neighbours(RoutingTable *routingTable) {
    for (int i = 0; i < routingTable->neighborCount; ) {
        bool isCurrentNeighbour = false;
        // Verifica se o vizinho está entre os vizinhos conhecidos
        if(strcmp(routingTable->neighborIDs[i], pred.ID) == 0 ||
            strcmp(routingTable->neighborIDs[i], succ.ID) == 0) {
            isCurrentNeighbour = true;
        } else {
            for (int c = 0; c < MAX_CORDS; c++) {
                if(chords[c].is_active && strcmp(chords[c].nodeID, routingTable->neighborIDs[i]) == 0) {
                    isCurrentNeighbour = true;
                    break;
                }
            }
        }

        // Se não é um vizinho atual
        if(!isCurrentNeighbour) {
            for (int j = i; j < routingTable->neighborCount - 1; j++) {
                strcpy(routingTable->neighborIDs[j], routingTable->neighborIDs[j + 1]);
                for (int k = 0; k < routingTable->nodeCount; k++) {
                    strcpy(routingTable->entries[k].paths[j], routingTable->entries[k].paths[j + 1]);
                }
            }
            strcpy(routingTable->neighborIDs[routingTable->neighborCount - 1], "");
            for (int k = 0; k < routingTable->nodeCount; k++) {
                routingTable->entries[k].paths[routingTable->neighborCount - 1][0] = '\0';
            }
            routingTable->neighborCount--;
        } else {
            i++;
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Função auxiliar para guardar lista de nós
int parse_node_list(char *buffer, Node *nodes, int max_nodes) {
    int nodeCount = 0;
    char *line = strtok(buffer, "\n");

    // Ignorar a primeira linha que é "NODESLIST r"
    line = strtok(NULL, "\n");

    while (line != NULL && nodeCount < max_nodes) {
        if(sscanf(line, "%s %s %d", nodes[nodeCount].ID, nodes[nodeCount].IP, &nodes[nodeCount].port) == 3) {
            nodeCount++;
        }
        line = strtok(NULL, "\n");
    }
    return nodeCount;
} //parse_node_list()

// Função auxiliar para imprimir informações de um Nó
void print_node_info(char *node_name, Node *node) {
    if(strcmp(node->ID, "\0") != 0) {
        if(node->port == 0) {
            printf("%s - ID: %2s\n", node_name, node->ID);
        }
        else {
            printf("%s - ID: %2s, IP: %s, Port: %u\n", node_name, node->ID, node->IP, node->port);
        }
    } else {
        printf("%s - (vazio)\n", node_name);
    }
} //print_node_info()

// Função auxiliar para verificar se o ID já existe no vetor de nós
bool free_id(Node nodes[], int node_count, char *id) {
    for (int i = 0; i < node_count; i++) {
        if(strcmp(nodes[i].ID, id) == 0) {
            return true;
        }
    }
    return false;
} //free_id()

// Função auxiliar para gerar um novo ID que não esteja no vetor de nós
void get_new_id(Node nodes[], int node_count, char *new_ID) {
    bool idFound;
    int randomID;

    do {
        idFound = false;
        randomID = rand() % ID_RANGE;
        sprintf(new_ID, "%d", randomID);

        if(free_id(nodes, node_count, new_ID)) {
            idFound = true;
        }
    } while (idFound);
} //get_new_id()

// Função auxiliar para escolher um nó ao calhas da lista de nós
Node choose_node(Node nodes[], int node_count) {
    int index;

    while(1) {
        index = rand() % node_count;
        // Verifica se o nó escolhido é diferente de mim, do meu predecessor e do meu sucessor
        if(strcmp(nodes[index].ID, me.ID) != 0 && strcmp(nodes[index].ID, pred.ID) != 0 && strcmp(nodes[index].ID, succ.ID) != 0) {
            break; // Se verdadeiro, sai do loop
        }
    }
    return nodes[index];
}//choose_node()

// Função auxiliar para dar reset num Nó
void clear_node(Node *node) {
    memset(node->ID, 0, sizeof(node->ID));
    memset(node->IP, 0, sizeof(node->IP));
    node->port = 0;
} //clear_node()

// Função auxiliar para atualizar o número de file descriptors
void update_max_fd(int listen_fd) {
    max_fd = listen_fd; // listen_fd deve ser configurado antes
    if(pred_fd > max_fd) max_fd = pred_fd;
    if(succ_fd > max_fd) max_fd = succ_fd;

    // Verifica todos os fds das cordas ativas para atualizar o max_fd
    for (int i = 0; i < MAX_CORDS; i++) {
        if(chords[i].is_active && chords[i].fd > max_fd) {
            max_fd = chords[i].fd;
        }
    }
}

// Função auxiliar para escolher o primeiro nó do caminho mais rápido para um certo nó destino
int choose_next(char *destID) {
    char shortestPathCopy[MAX_PATH_LENGTH];
    char *shortestPath = NULL;
    
    // Encontrar o caminho mais curto para o destID
    for (int i = 0; i < shortestPathTable.nodeCount; i++) {
        if(strcmp(shortestPathTable.entries[i].nodeID, destID) == 0) {
            shortestPath = shortestPathTable.entries[i].shortestPath;
            break;
        }
    }

    if(shortestPath == NULL) {
        return -1;
    }

    strncpy(shortestPathCopy, shortestPath, MAX_PATH_LENGTH);

    // Dividir o caminho mais curto em IDs
    char *token = strtok(shortestPathCopy, "-");
    if(token == NULL || strcmp(token, me.ID) == 0) {
        // O primeiro ID é o do próprio nó, então pegamos o próximo
        token = strtok(NULL, "-");
    }

    // Não encontrou um próximo hop válido
    if(token == NULL) {
        return -1;
    }
    char nextHopID[3];
    strncpy(nextHopID, token, sizeof(nextHopID) - 1);
    nextHopID[2] = '\0';

    // Comparar com os vizinhos conhecidos
    if(strcmp(nextHopID, pred.ID) == 0) {
        return pred_fd;
    } else if(strcmp(nextHopID, succ.ID) == 0) {
        return succ_fd;
    } else {
        // Procurar entre as cordas ativas
        for (int i = 0; i < MAX_CORDS; i++) {
            if(chords[i].is_active && strcmp(chords[i].nodeID, nextHopID) == 0) {
                return chords[i].fd;
            }
        }
    }
    // Se o próximo hop não for nem o predecessor, nem o sucessor, nem uma das cordas, retorna -1
    return -1;
}
