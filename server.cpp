//
// Simple chat server for TSAM-409
//
// Command line: ./chat_server 4000
//
// Author: Jacky Mallett (jacky@ru.is)
//
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <vector>

#include <iostream>
#include <sstream>
#include <thread>
#include <map>

#include <mutex>
#define BUFSIZE 1024
#define BACKLOG  5          // Allowed length of queue of waiting connections

//mutexes
std::mutex changeConnections;

// global variables for simplicity
std::string SOH = "\01";
std::string EOT = "\04";
int connections = 0; // this is not allowed to be higher than 5, need a mutext around the update function
std::string ID = "V_GROUP_42";
const int MAXSERVERCONNECTIONS = 5;

// Simple class for handling connections from clients.
//
// Client(int socket) - socket to send/receive traffic from client.
class Client
{
    public:
        int sock;              // socket of client connection
        std::string name;           // Limit length of name of client's user

        Client(int socket) : sock(socket){}

        ~Client(){}            // Virtual destructor defined for base class
};

class Server
{
    public:
        int sockfd;
        std::string id;

        Server(int socket) : sockfd(socket){}

        ~Server(){}  
};

// Note: map is not necessarily the most efficient method to use here,
// especially for a server with large numbers of simulataneous connections,
// where performance is also expected to be an issue.
//
// Quite often a simple array can be used as a lookup table,
// (indexed on socket no.) sacrificing memory for speed.

std::map<int, Client*> clients; // Lookup table for per Client information
std::map<int, Server*> servers;  // Loopkup table for Server information
                                // the key in the table is the server ID

// Open socket for specified port.
//
// Returns -1 if unable to create the socket for any reason.

int openSocket(int portno)
{
    struct sockaddr_in sk_addr;   // address settings for bind()
    int sock;                     // socket opened for this port
    int set = 1;                  // for setsockopt

    // Create socket for connection

    if((sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0)) < 0)
    {
        perror("Failed to open socket");
        return(-1);
    }

    // Turn on SO_REUSEADDR to allow socket to be quickly reused after
    // program exit.

    if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &set, sizeof(set)) < 0)
    {
        perror("Failed to set SO_REUSEADDR:");
    }

    memset(&sk_addr, 0, sizeof(sk_addr));

    sk_addr.sin_family      = AF_INET;
    sk_addr.sin_addr.s_addr = INADDR_ANY;
    sk_addr.sin_port        = htons(portno);

    // Bind to socket to listen for connections from clients

    if(bind(sock, (struct sockaddr *)&sk_addr, sizeof(sk_addr)) < 0)
    {
        perror("Failed to bind to socket:");
        return(-1);
    }
    else
    {
        return(sock);
    }
}

// Close a client's connection, remove it from the client list, and
// tidy up select sockets aft5erwards.

void closeClient(int clientSocket, fd_set *openSockets, int *maxfds)
{
    // Remove client from the clients list
    clients.erase(clientSocket);

    // If this client's socket is maxfds then the next lowest
    // one has to be determined. Socket fd's can be reused by the Kernel,
    // so there aren't any nice ways to do this.

    if(*maxfds == clientSocket)
    {
        for(auto const& p : clients)
        {
            *maxfds = std::max(*maxfds, p.second->sock);
        }
    }

    // And remove from the list of open sockets.

    FD_CLR(clientSocket, openSockets);
}

// Close a servers connection, remove it from the server list, and
// tidy up select sockets aft5erwards.

void closeServer(int serverSocket, fd_set *openSockets, int *maxfds)
{
    servers.erase(serverSocket);

    if(*maxfds == serverSocket)
    {
       for(auto const& p : servers)
       {
         *maxfds = std::max(*maxfds, p.second->sockfd);
       }
    }

    FD_CLR(serverSocket, openSockets);
}

// Process command from client on the server

int clientCommand(int clientSocket, fd_set *openSockets, int *maxfds, char *buffer)
{
    std::vector<std::string> tokens;
    std::string token;

    // Split command from client into tokens for parsing
    std::stringstream stream(buffer);

    while(stream >> token)
        tokens.push_back(token);

    if((tokens[0].compare("CONNECT") == 0) && (tokens.size() == 2))
    {
        clients[clientSocket]->name = tokens[1];
    }
    else if(tokens[0].compare("LEAVE") == 0)
    {
        // Close the socket, and leave the socket handling
        // code to deal with tidying up clients etc. when
        // select() detects the OS has torn down the connection.

         closeClient(clientSocket, openSockets, maxfds);
    }
    else if(tokens[0].compare("WHO") == 0)
    {
        std::cout << "Who is logged on" << std::endl;
        std::string msg;

        for(auto const& names : clients)
        {
            msg += names.second->name + ",";
        }
        // Reducing the msg length by 1 loses the excess "," - which
        // granted is totally cheating.
        send(clientSocket, msg.c_str(), msg.length()-1, 0);
    }
    else if(tokens[0].compare("ID") == 0)
    {
      send(clientSocket, ID.c_str(), ID.length(), 0);
    }
    // This is slightly fragile, since it's relying on the order
    // of evaluation of the if statement.
    else if((tokens[0].compare("MSG") == 0) && (tokens[1].compare("ALL") == 0))
    {
        std::string msg;
        for(auto i = tokens.begin()+2;i != tokens.end();i++)
        {
            msg += *i + " ";
        }

        for(auto const& pair : clients)
        {
            send(pair.second->sock, msg.c_str(), msg.length(),0);
        }
    }
    else if(tokens[1].compare("MSG") == 0)
    {
        for(auto const& pair : clients)
        {
            if(pair.second->name.compare(tokens[1]) == 0)
            {
                std::string msg;
                for(auto i = tokens.begin()+2;i != tokens.end();i++)
                {
                    msg += *i + " ";
                }
                send(pair.second->sock, msg.c_str(), msg.length(),0);
            }
        }
    }
    else
    {
        std::cout << "Unknown command from client:" << buffer << std::endl;
    }
}

// Process command from server connected to the server

void processServerCommand (char *buffer, int serverSockfd)
{
    std::string serverMsg = ""; // automated message sent from server

    std::vector<std::string> tokens;
    std::string token;
    std::istringstream stream(buffer);

    while(std::getline(stream, token, ',')) {
      tokens.push_back(token);
    }

    if((tokens[0].compare("LISTSERVERS") == 0) &&(tokens.size() == 1))
    {
      std::cout << "LISTSERVERS command Recieved" << std::endl;
      std::cout << "This has not been completed" <<std::endl;
    }
    else if((tokens[0].compare("KEEPALIVE") == 0) &&(tokens.size() == 1))
    {
      std::cout << "KEEPALIVE command Recieved" << std::endl;
      std::cout << "This has not been completed" <<std::endl;
    }
    else if((tokens[0].compare("LISTROUTES") == 0) &&(tokens.size() == 1))
    {
      std::cout << "LISTROUTES command Recieved" << std::endl;
      std::cout << "This has not been completed" <<std::endl;
    }
    else if((tokens[0].compare("CMD") == 0) && (tokens.size() == 4))
    {
        std::cout << "CMD command Recieved" << std::endl;
        std::cout << "This has not been completed" <<std::endl;

        if(tokens[1] == ID || tokens[1].compare("") == 0) {
            std::cout << "I am receiveing a command" << std::endl;       
            
            if(tokens[3].compare("ID") == 0)
            {   
                // add the id to the database for the correct socket
                servers[serverSockfd]->id = tokens[2];
                //std::cout << "adding id to map and sending a reply";
                // send id command with both TOSERVERID and FROMSERVERID filled out
                serverMsg = SOH + "RSP," + tokens[2] + "," + ID + ",ID" + EOT;
                if(send(serverSockfd, serverMsg.c_str(), serverMsg.length(), 0) == -1)
                {
                    perror("Failed to send id information\n");
                }          
                // if the id information is filled out then I should just add it to my map
            }
        }
        else {
            std::cout << "This message is not for me" << std::endl;
            // forward?
        }
    }
    else if((tokens[0].compare("RSP") == 0) && (tokens.size() == 4))
    {
        std::cout << "RSP command Recieved" << std::endl;
        std::cout << "This has not been completed" <<std::endl;
        if(tokens[1] == ID) {
            std::cout << "I am receiveing a command" << std::endl;
            if(tokens[3].compare("ID") == 0)
            {
                servers[serverSockfd]->id = tokens[2];
            }
        }
        else {
            std::cout << "This message is not for me" << std::endl;
            // forward?
        }
    }
    else if((tokens[0].compare("FETCH") == 0) && (tokens.size() == 2))
    {
        std::string uniqueHashArr[5] = {"2990b1f8a30a2766e7d9ca603e22051f",
                                        "31f23ba6e971170f63781be4d88b3ceb",
                                        "df3f079de6961496f0460dcfdbf9bca3",
                                        "f970e2767d0cfe75876ea857f92e319b",
                                        "22d424f309c391d7a2c2f25fbdfb70ea"};
        
        
        std::cout << "Fetch command Recieved" << std::endl;
        std::cout << "This has not been completed" <<std::endl;

    }
    else if(tokens[0].compare("ID") == 0)
    {
        //send(clientSocket, ID.c_str(), ID.length(), 0);
    }
    else {
        std::cout << "Unknown server command: " << buffer << std::endl;
    }
}

void sendServerCommands(char* buffer, fd_set *openSockets, int *maxfds)
{
    //while(true) {
        std::vector<std::string> tokens;
        std::string token;
        std::string serverMsg;
        //char buffer[1025];
        // Split command from server into tokens for parsing
        //fgets(buffer, sizeof(buffer), stdin);
        std::istringstream stream(buffer);

        while(std::getline(stream, token, ',')) {
          std::cout << token << std::endl;
          tokens.push_back(token);
        }

        std::cout << "TOKENS: "<< tokens[0] << " token size" << tokens.size() << std::endl;

        if((tokens[0].compare("CONNECT") == 0) && (tokens.size() == 3))
        {
            if(servers.size() < MAXSERVERCONNECTIONS) {
                int sockfd;
                struct sockaddr_in serv_addr;
                struct hostent *server;

                sockfd = socket(AF_INET, SOCK_STREAM, 0); // Open tcp Socket

                server = gethostbyname(tokens[1].c_str());

                bzero((char *) &serv_addr, sizeof(serv_addr));//fill the serv_addr with zeros
                serv_addr.sin_family = AF_INET; // This is always set to AF_INET
                bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
                serv_addr.sin_port = htons(atoi(tokens[2].c_str()));

                if(connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0) {
                    printf("Failed to open socket to server: %s\n", tokens[1].c_str());
                    perror("Connect failed: ");
                    exit(0);
                } else {
                    printf("succesfull connect\n");
                    servers[sockfd] = new Server(sockfd);
                    
                    //fd_set
                    FD_SET(sockfd, openSockets);
                    *maxfds = std::max(sockfd, *maxfds);

                    // send id to the server, their id is then reveiceid and
                    // the processesd in process server commands function
                    serverMsg = SOH + "CMD,," + ID + ",ID" + EOT; // to TOSERVERID is unknown so I leave it empty
                    if(send(sockfd, serverMsg.c_str(), serverMsg.length(), 0) == -1)
                    {
                        perror("Failed to send id information\n");
                    }
                }
            }
        }
        // size >= 4 because the command can be longer than just one word, e.g. Fetch
        else if((tokens[0].compare("CMD") == 0) && (tokens.size() >= 4))
        {
            std::cout << "CMD command" << std::endl;
            serverMsg = SOH + buffer + EOT;
            //std::cout << "server: " << (servers[tokens[1]].sockfd) << std::endl;
            std::cout << serverMsg << std::endl;
            for(auto const& pair : servers) {
                Server *server = pair.second;
                if(server->id == tokens[1]) {
                    std::cout << "found server in map" << std::endl;
                    if(send(servers[server->sockfd]->sockfd, serverMsg.c_str(), serverMsg.length(), 0) == -1) {
                        perror("Failed to send CMD command\n");
                    }
                }
            }
        }
        else if((tokens[0].compare("FETCH") && (tokens.size() == 2)))
        {

        }
        else {
            std::cout << "Unknown server command: " << buffer << std::endl;
        }
    //}
}

void keepAlive() 
{
    std::string keepAliveMsg = SOH + "KEEPALIVE" + EOT;
    while(true) {
        sleep(90);
        for(auto const& pair : servers) {
            Server *server = pair.second;
            if(send(servers[server->sockfd]->sockfd, keepAliveMsg.c_str(), keepAliveMsg.length(), 0) == -1) {
                std::cout << "Failed to send Keepalive to server" << server->id << std::endl;
                perror("Failed to send KEEPALIVE\n");
            }
        }
    }
}

int main(int argc, char* argv[])
{
    bool finished;
    int listenSock;                 // Socket for connections to server
    int listenServerSock;
    int clientSock;                 // Socket of connecting client
    int serverSock;                 // Socket of connectubg server
    fd_set openSockets;             // Current open sockets
    fd_set readSockets;             // Socket list for select()
    fd_set exceptSockets;           // Exception socket list
    int maxfds;                     // Passed to select() as max fd in set
    struct sockaddr_in client;
    socklen_t clientLen;
    char buffer[BUFSIZE];              // buffer for reading from clients
    std::string idReply = SOH + "CMD,," + ID + ",ID" + EOT; // command sent to a server that does a connect

    //ports
    char* tcpServerPort;
    char* udpServerPort;
    char* tcpClientPort;

    std::cout << INADDR_ANY << std::endl;

    if(argc != 4)
    {
        printf("Usage: chat_server <server tcp port> <server udp port> <client tcp port>\n");
        exit(0);
    }

    // Setup socket for server to listen to client
    tcpClientPort = argv[3];
    std::cout << "client port: " << tcpClientPort << std::endl;

    listenSock = openSocket(atoi(tcpClientPort));
    printf("Listening on port: %s for clients\n", tcpClientPort);

    if(listen(listenSock, BACKLOG) < 0)
    {
        printf("Listen failed on port %s\n", tcpClientPort);
        exit(0);
    }
    else
    // Add listen socket to socket set
    {
        FD_SET(listenSock, &openSockets);
        maxfds = listenSock;
    }

    // Setup socket for server to listen to other servers
    tcpServerPort = argv[1];
    listenServerSock = openSocket(atoi(tcpServerPort));
    printf("Listening on port: %s for servers\n", tcpServerPort);

    if(listen(listenServerSock, BACKLOG) < 0)
    {
        printf("Listen failed on port%s\n", tcpServerPort);
        exit(0);
    }
    else
    // Add listen server socket to socket set
    {
        FD_SET(listenServerSock, &openSockets);
        maxfds = std::max(listenServerSock, maxfds);
    }

    //TA reccommended this!
    FD_SET(0, &openSockets);

    //std::thread serverThread(sendServerCommands, &openSockets, &maxfds)    ;
    std::thread keepAliveThread(keepAlive);

    finished = false;

    while(!finished)
    {
        // Get modifiable copy of readSockets
        readSockets = exceptSockets = openSockets;
        memset(buffer, 0, sizeof(buffer));

        int n = select(maxfds + 1, &readSockets, NULL, &exceptSockets, NULL);
        //int n = select(FD_SETSIZE, &readSockets, NULL, NULL, NULL);
        std::cout << "selected" << std::endl;
        if(n < 0)
        {
            perror("select failed - closing down\n");
            finished = true;
        }
        else
        {
            if(FD_ISSET(0, &readSockets)) {
                bzero(buffer, BUFSIZE);
                if(read(0, buffer, BUFSIZE-1) < 0);
                std::cout << buffer << std::endl;
                sendServerCommands(buffer, &openSockets, &maxfds);

            }
            // Accept  any new connections to the server
            // allow servers to connect aswell
            // - assume that it is a server
            //for(int i = 0; i < FD_SETSIZE; i++) {
            if(FD_ISSET(listenSock, &readSockets))
            {

                clientSock = accept(listenSock, (struct sockaddr *)&client, &clientLen);

                FD_SET(clientSock, &openSockets);
                maxfds = std::max(maxfds, clientSock);

                // add to client map
                clients[clientSock] = new Client(clientSock);
                n--;

                //printf("Client connected on server: %s\n", argv[3]);
                printf("Client connected from host %s, port %hd.\n",
                       inet_ntoa (client.sin_addr),
                       ntohs (client.sin_port));

            }
            else if(FD_ISSET(listenServerSock, &readSockets))
            {
                std::cout << "map  size: " << servers.size() << std::endl;
                if(servers.size() < MAXSERVERCONNECTIONS) {
                    serverSock = accept(listenServerSock, (struct sockaddr *)&client, &clientLen);
                    // TODO: WHAT HAPPENS WHEN ACCEPT FAILS <- BAD FILE DESCRIPTOR HAPPENS HERE
                    FD_SET(serverSock, &openSockets);
                    maxfds = std::max(maxfds, serverSock);

                    printf("Server connected from host %s, port, %hd.\n",
                           inet_ntoa (client.sin_addr),
                           (client.sin_port));

                    // send id to the server, their id is then reveiceid and
                    // the processesd in process server commands function
                    if(send(serverSock, idReply.c_str(), idReply.length(), 0) == -1) {
                        perror("Failed to send id information to incoming connection\n");
                    }

                    servers[serverSock] = new Server(serverSock);
                }
                else
                {
                    // accept
                    serverSock = accept(listenServerSock, (struct sockaddr *)&client, &clientLen);
                    // send that the server is full
                    std::string connectionDeniedMsg = "server is full";
                    if(send(serverSock, connectionDeniedMsg.c_str(), connectionDeniedMsg.length(), 0) == -1) 
                    {
                        perror("Failed to send denied message\n");
                    }
                    // close
                    close(serverSock);
                }
            }
            // Now check for commands from clients
            for(auto const& pair : clients)
            {
                Client *client = pair.second;

                if(FD_ISSET(client->sock, &readSockets))
                {
                    if(recv(client->sock, buffer, sizeof(buffer), MSG_DONTWAIT) == 0)
                    {
                        printf("Client closed connection: %d", client->sock);
                        close(client->sock);

                        closeClient(client->sock, &openSockets, &maxfds);
                    }
                    else
                    {
                        std::cout << "BUFFER: " << buffer << std::endl;
                        // check if command is server command or client command

                        if(buffer[0] == '\01') 
                        {
                            buffer[strlen(buffer) - 1] = '\0';
                            processServerCommand(&buffer[1], client->sock);
                        }
                        else
                        {
                            clientCommand(client->sock, &openSockets, &maxfds, buffer);
                        }
                    }
                }
            }
            for(auto const& pair : servers)
            {
                Server *server = pair.second;
                std::cout << "server sockfd: " << server->sockfd << std::endl;
                if(FD_ISSET(server->sockfd, &readSockets))
                {
                    std::cout << "It is set" << std::endl;
                    if(recv(server->sockfd, buffer, sizeof(buffer), MSG_DONTWAIT) == 0)
                    {
                        printf("Server closed connection: %d\n", server->sockfd);
                        close(server->sockfd);

                        closeServer(server->sockfd, &openSockets, &maxfds);
                    }
                    else
                    {
                        std::cout << "BUFFER: " << buffer << std::endl;
                        // check if command is server command or client command

                        if(buffer[0] == '\01') 
                        {
                            buffer[strlen(buffer) - 1] = '\0';
                            processServerCommand(&buffer[1], server->sockfd);
                        }
                        else
                        {
                            //clientCommand(client->sock, &openSockets, &maxfds, buffer);
                        }
                    }
                }
            }
        }
    }
}
