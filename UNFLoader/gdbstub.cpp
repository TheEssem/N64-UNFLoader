/***************************************************************
                            gdbstub.cpp
                               
Handles basic GDB communication
***************************************************************/ 

#ifndef LINUX
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/tcp.h>
    #include <unistd.h>
    #include <signal.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <thread>
#include <chrono>
#include <string>
#include "gdbstub.h"
#include "helper.h"
#include "term.h"


/*********************************
              Macros
*********************************/

#define TIMEOUT 3
#define LOG_ERRORS TRUE

#ifdef LINUX
    #define SOCKET  int
#endif


/*********************************
              Enums
*********************************/

typedef enum {
    STATE_SEARCHING,
    STATE_HEADER,
    STATE_PACKETDATA,
    STATE_CHECKSUM,
} ParseState;


/*********************************
        Function Prototypes
*********************************/

static void gdb_replypacket(std::string packet);


/*********************************
             Globals
*********************************/

std::string local_packetdata = "";
std::string local_packetchecksum = "";
std::string local_lastpacket = "";
ParseState local_parserstate = STATE_SEARCHING;
SOCKET local_socket = -1;


/*==============================
    socket_connect
    TODO
==============================*/

static int socket_connect(char* address, char* port) 
{
    short sock;
    int optval;
    socklen_t socklen;
    struct sockaddr_in remote;

    // Create a socket for GDB
    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == -1)
    {
        #if LOG_ERRORS
            log_colored("Unable to create socket for GDB\n", CRDEF_ERROR);
        #endif
        return -1;
    }

    // Allow us to reuse this socket if it is killed
    optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval));
    optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (char*)&optval, sizeof(optval));

    // Setup the socket struct
    remote.sin_port = htons(atoi(port));
    remote.sin_family = PF_INET;
    remote.sin_addr.s_addr = inet_addr(address);
    if (bind(sock, (struct sockaddr *)&remote, sizeof(remote)) != 0)
    {
        #if LOG_ERRORS
            log_colored("Unable to bind socket for GDB\n", CRDEF_ERROR);
        #endif
        return -1;
    }

    // Listen for (at most one) client
    if (listen(sock, 1))
    {
        #if LOG_ERRORS
            log_colored("Unable to listen to socket for GDB\n", CRDEF_ERROR);
        #endif
        return -1;
    }

    // Accept a client which connects
    socklen = sizeof(remote);
    local_socket = accept(sock, (struct sockaddr *)&remote, &socklen);
    if (local_socket == -1)
    {
        #if LOG_ERRORS
            log_colored("Unable to accept socket for GDB\n", CRDEF_ERROR);
        #endif
        return -1;
    }

    // Enable TCP keep alive process
    optval = 1;
    setsockopt(local_socket, SOL_SOCKET, SO_KEEPALIVE, (char*)&optval, sizeof(optval));

    // Don't delay small packets, for better interactive response
    optval = 1;
    setsockopt(local_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&optval, sizeof(optval));

    // Cleanup
    close(sock);
    signal(SIGPIPE, SIG_IGN);  // So we don't exit if client dies
    return 0;
}


/*==============================
    socket_send
    TODO
==============================*/

static int socket_send(SOCKET sock, char* data, size_t size)
{
    int ret = -1;
    struct timeval tv;
    tv.tv_sec = TIMEOUT;
    tv.tv_usec = 0;

    log_simple("Sending: %s\n", data);

    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv,sizeof(tv)) < 0)
        return -1;

    ret = send(sock, data, size, 0);
    return ret;
}


/*==============================
    socket_receive
    TODO
==============================*/

static int socket_receive(SOCKET sock, char* data, size_t size)
{
    int ret = -1;
    struct timeval tv;
    tv.tv_sec = TIMEOUT;
    tv.tv_usec = 0;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&tv,sizeof(tv)) < 0)
        return -1;

    ret = recv(sock, data, size, 0);
    return ret;
}


/*==============================
    packet_getchecksum
    TODO
==============================*/

static uint8_t packet_getchecksum(std::string packet)
{
    uint32_t checksum = 0;
    uint32_t strln = packet.size();
    for (uint32_t i=0; i<strln; i++)
        checksum += packet[i];
    return (uint8_t)(checksum%256);
}


/*==============================
    packet_appendchecksum
    TODO
==============================*/

static std::string packet_appendchecksum(std::string packet)
{
    char str[3];
    uint8_t checksum = packet_getchecksum(packet);
    sprintf(str, "%02x", checksum);
    return packet + "#" + str;
}


/*==============================
    gdb_connect
    TODO
==============================*/

void gdb_connect(char* fulladdr)
{
    char* addr;
    char* port;
    char* fulladdrcopy = (char*)malloc((strlen(fulladdr)+1)*sizeof(char));
    strcpy(fulladdrcopy, fulladdr);

    // Grab the address and port
    addr = strtok(fulladdrcopy, ":");
    port = strtok(NULL, ":");

    // Connect to the socket
    if (socket_connect(addr, port) != 0)
    {
        #if LOG_ERRORS
            log_colored("Unable to connect to socket: %d\n", CRDEF_ERROR, errno);
        #endif
        local_socket = -1;
        free(fulladdrcopy);
        return;
    }

    // Cleanup
    free(fulladdrcopy);
}


/*==============================
    gdb_isconnected
    TODO
==============================*/

bool gdb_isconnected()
{
    return local_socket != -1;
}


/*==============================
    gdb_disconnect
    TODO
==============================*/

void gdb_disconnect()
{
    // TODO: Tell GDB we're disconnecting
    shutdown(local_socket, SHUT_RDWR);
    local_socket = -1;
}


/*==============================
    gdb_parsepacket
    TODO
==============================*/

static void gdb_parsepacket(char* buff, int buffsize)
{
    int left = buffsize, read = 0;
    while (left > 0)
    {
        switch (local_parserstate)
        {
            case STATE_SEARCHING:
                while (left > 0 && buff[read] != '$')
                {
                    if (buff[read] == '-') // Resend last packet in case of failure
                        socket_send(local_socket, (char*)local_lastpacket.c_str(), local_lastpacket.size()+1);
                    read++;
                    left--;
                }
                if (buff[read] == '$')
                    local_parserstate = STATE_HEADER;
                break;
            case STATE_HEADER:
                if (left > 0 && buff[read] == '$')
                {
                    read++;
                    left--;
                    local_parserstate = STATE_PACKETDATA;
                }
                break;
            case STATE_PACKETDATA:
                while (left > 0 && buff[read] != '#') // Read bytes until we hit a checksum marker, or we run out of bytes
                {
                    local_packetdata += buff[read];
                    read++;
                    left--;
                }
                if (buff[read] == '#')
                    local_parserstate = STATE_CHECKSUM;
                break;
            case STATE_CHECKSUM:
                if (left > 0)
                {
                    if (buff[read] != '#')
                    {
                        local_packetchecksum += buff[read];
                        if (local_packetchecksum.size() == 2)
                        {
                            uint32_t checksum = packet_getchecksum(local_packetdata);

                            // Check if the checksum failed, if it didn't then parse the data
                            if (checksum == strtol(local_packetchecksum.c_str(), NULL, 16L))
                            {
                                log_simple("Deconstructed %s\n", local_packetdata.c_str());
                                gdb_replypacket(local_packetdata);
                            }
                            else
                            {
                                log_simple("Checksum failed. Expected %x, got %x\n", checksum, strtol(local_packetchecksum.c_str(), NULL, 16L));
                                socket_send(local_socket, (char*)"-", 2);
                            }

                            // Finish
                            local_packetdata = "";
                            local_packetchecksum = "";
                            local_parserstate = STATE_SEARCHING;
                        }
                    }
                    read++;
                    left--;
                }
                break;
        }
    }
}


/*==============================
    gdb_replypacket
    TODO
==============================*/

static void gdb_replypacket(std::string packet)
{
    std::string reply = "+$";
    if (packet.find("qSupported")  != std::string::npos)
    {
        reply += packet_appendchecksum("PacketSize=512");
    }
    else if (packet == "g")
    {
        // TODO: This properly
        //std::string registers = "";
        //for (int i=0; i<32; i++)
        //    registers += "00000000";
        //registers += "00000001"; // sr
        //registers += "00000002"; // Lo
        //registers += "00000003"; // Hi
        //registers += "00000004"; // Bad
        //registers += "00000005"; // Cause
        //registers += "00000400"; // PC
        //registers += "00000007"; // fsr
        //registers += "00000008"; // fir
        //reply += packet_appendchecksum(registers);
    }
    else if (packet == "?")
    {
        reply += packet_appendchecksum("S05");
    }
    else
        reply += "#00";
    local_lastpacket = reply;
    socket_send(local_socket, (char*)reply.c_str(), reply.size()+1);
}


/*==============================
    gdb_thread
    TODO
==============================*/

void gdb_thread(char* addr)
{
    gdb_connect(addr);
    while (gdb_isconnected())
    {
        int readsize;
        char buff[512];
        memset(buff, 0, 512*sizeof(char));
        readsize = socket_receive(local_socket, buff, 512);
        if (readsize > 0)
        {
            log_simple("received: %s\n", buff);
            gdb_parsepacket(buff, readsize);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}