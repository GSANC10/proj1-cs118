#include "consts.h"
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>




//Note for the editor (Will) We essentially need to also implement 
// 1. head_smallest, 
// 2. drop_acked, push, 
// 3. flush_recv_buf 
// 4.5.6.7.8 as I just wrote them out as placeholder functions to deal with later

//hook into your listen_loop() to:
// Send returned packets via sendto()
// Emit pure-ACK in its own branch and transition client state
// Perform timeout and 3-dup-ACK retransmissions









/*
 * the following variables are only informational, 
 * not necessarily required for a valid implementation.
 * feel free to edit/remove.
 */
int state = 0;           // Current state for handshake
int our_send_window = 0; // Total number of bytes in our send buf
int their_receiving_window = MIN_WINDOW;   // Receiver window size
int our_max_receiving_window = MIN_WINDOW; // Our max receiving window
int our_recv_window = 0;                   // Bytes in our recv buf
int dup_acks = 0;        // Duplicate acknowledgements received
uint32_t ack = 0;        // Acknowledgement number
uint32_t seq = 0;        // Sequence number
uint32_t last_ack = 0;   // Last ACK number to keep track of duplicate ACKs
bool pure_ack = false;  // Require ACK to be sent out
packet* base_pkt = NULL; // Lowest outstanding packet to be sent out

//Holds all incoming data packets 
buffer_node* recv_buf =
    NULL; // Linked list storing out of order received packets

//holdes every data packet you've sent but not yet ACKed. Used by retransmission logic for timeout when we resend our base_pkt, and 3x duplicata ckenowledge for fast retransmit.
buffer_node* send_buf =
    NULL; // Linked list storing packets that were sent but not acknowledged

ssize_t (*input)(uint8_t*, size_t); // Get data from layer
void (*output)(uint8_t*, size_t);   // Output data from layer

struct timeval start; // Last packet sent at this time
struct timeval now;   // Temp for current time

//Push node (packet) to head of list where we are storing everything. (Packet Chain)
buffer_node* push(buffer_node* head, packet* pkt, size_t len){
    buffer_node* n = malloc(sizeof(buffer_node) + len);
    memcpy(&n->pkt, pkt, sizeof(packet) + len);
    n->next = head;
    return n;
}
//Clean up our send buffer essentially
buffer_node* drop_acked(buffer_node* head, uint16_t bound) {
    // buffer_node** cur = &head;
    // while (*cur) {
    //     if (ntohs((*cur)->pkt.seq) < bound) {
    //         buffer_node* tmp = *cur;
    //         *cur = (*cur)->next;
    //         free(tmp);
    //     } else {
    //         cur = &(*cur)->next;
    //     }
    // }
    // return head;
}


//We jsut want to ultimately want to know the first unacked packet
buffer_node* head_smallest(buffer_node* head) {
}


//Smooth out fragments
void flush_recv_buf(void) {
    // bool again = true;
    // while (again) {
    //     again = false;
    //     buffer_node** cur = &recv_buf;
    //     while (*cur) {
    //         if (ntohs((*cur)->pkt.seq) == ack) {
    //             output((*cur)->pkt.payload, ntohs((*cur)->pkt.length));
    //             ack++;
    //             buffer_node* tmp = *cur;
    //             *cur = (*cur)->next;
    //             free(tmp);
    //             again = true;
    //             break;
    //         }
    //         cur = &(*cur)->next;
    //     }
    // }
}










///GET DATA
packet* get_data() {
    //Flags to ensure that we only sned our SYN/SYN_ACK 
    //(Handhsake flags)
    static bool syn_sent = false;
    static bool synack_sent = false;
    
    switch (state) {
    //On our first call in CLIENT_START, we build a zero-payload packet with only the SYN flag and your randomized seq.
    case CLIENT_START:
        if(!syn_sent){
            packet* pkt = calloc(1, sizeof(packet));   // zero entire header
            if (!pkt) return NULL;
            //Build zero-lengthm SYN-only packet
            pkt->seq = htons(seq); //Our intial seq
            pkt->ack = 0; //Haen't receved an ACK yet
            pkt->length = htons(0); //Zero payload to work with
            pkt->win = htons(our_max_receiving_window); //Per spec
            pkt->flags = SYN; //SYN bit dude!
            syn_sent = true;
            //Must change state or else we would be building up and returning new SYNS-> flooding
            state = CLIENT_AWAIT; //Waiting for our glorious SYN+ACK from server now! Also future calls will skip this now
            print_diag(pkt, SEND);
            return pkt; //Send that shit!
        }
        break;
    //After seeing the client's SYN, we build a SYN+ACK packet. Note this is the SERVER that just recevied the SYN, AWAITING IT, and sending that SYN+ACK back.
    //(Server -(SYN+ACK)> Client)
    case SERVER_AWAIT:
        //Stay here until we see client's final ACK
        //Return the handshake packet
        if(!synack_sent){
            packet* pkt = calloc(1, sizeof(packet));
            if (!pkt) return NULL;

            //Fill that header in brother!
            pkt->seq = htons(seq);  //Server intial seq
            pkt->ack = htons(ack);  //Client's seq+1
            pkt->win = htons(our_max_receiving_window);
            pkt->flags = SYN | ACK; //Both bits notw sent
            synack_sent = true;
            print_diag(pkt,SEND);
            //We don't change state and stay here.
            return pkt;  
        }
        break;
    //Normal: Connection established
    case NORMAL:{
        //Read up to 1012B from stdin into buf
        uint8_t buf[MAX_PAYLOAD];
        ssize_t len = input(buf, MAX_PAYLOAD); //Read from stdin
        if (len > 0) {
            //Don't exceed the receiver's window
            if((size_t)our_send_window + len > (size_t)their_receiving_window){
                return NULL; //Wait for ACKs. HOLD YOUR HORSES!
            }
            //Alocate header + payload, and then zero it
            packet* dpkt = calloc(1, sizeof(packet) + len);
            if (!dpkt) return NULL;
            //Copy payload
            memcpy(dpkt->payload, buf, len);

            dpkt->seq = htons(seq++); //Post-incrmenet seq
            dpkt->ack = htons(ack);    //Last ACK we have
            dpkt->length = htons(len);  //Actual payload size
            dpkt->win = htons(our_max_receiving_window);
            dpkt->flags = 0;    //Pure data fam. Get those flags outta here

            /* Buffer for retransmission */
            send_buf = push(send_buf, dpkt, sizeof(packet)+len); //Call push to add this packet to our send_buf linked list as noted above. Note that send_buf holds all un-ACKed packets
            base_pkt = head_smallest(send_buf); //Picks the oldest for timeout/fast-retransmit
            our_send_window+=len; //Accumulates in-flight bytes for flow contorl

            
            print_diag(dpkt, SEND);
            return dpkt; //SEND THIS PACKET NOW!!! HOTTT!!! (sendto())
            }
        }
        break;
    default: 
        break;
    }
    return NULL;
}


// Process Inbound Packets
void recv_data(packet* pkt) {
    //Extract flag bits for handshake/normal
    uint16_t flags = pkt->flags;
    //If SYN bit or ACK bit is set in flags this will return true
    bool syn = flags & SYN;
    bool ack_flag = flags & ACK;

    switch (state) {
    //The server here is recevied our syn from client so let's store our shit and move to server_await for SYN+ACK, from get_data^
    case SERVER_START:
        if (syn && !ack_flag) {
            //Server received SYN from client
            ack = ntohs(pkt->seq) + 1;       // Record client's seq + 1
            state = SERVER_AWAIT;            // Prepare to send SYN+ACK
        }
        break;
    
    //We got the server's SYN+ACK, record it, and schedule that final ACK
    case CLIENT_AWAIT:
        if (syn && ack_flag) {
            // Client received SYN+ACK from server
            ack = ntohs(pkt->seq) + 1;       // Remember server's seq + 1
            pure_ack = true;                 // Trigger so loop can send final ACK
        }
        break;

    case SERVER_AWAIT:
        //We only care about the ACK here
        if (!syn && ack_flag) {
            // Server receives final ACK from client LETS START THAT TRANSMISSION BABY ABBEY ROAD!
            state = NORMAL;
        }
        break;

    //Fix up dude
    case NORMAL:{
        //Get shit, bababoee
        // uint16_t seq_num = ntohs(pkt->seq); Can maybe use to check for duplicates BEFORE CALLING PUSH. Necessary? Not sure tbh.
        uint16_t length = ntohs(pkt->length);
    // 1) Handle payload (if any)
    if (length > 0) {
        //If ^, then push packet onto recv_buf: (linked lsit out of order packets)
        recv_buf = push(recv_buf, pkt, sizeof(packet) + length);
        flush_recv_buf(); //This guy scans recv_buf for a node whose seq == ack (the next expected), outputs its payload, increments ack, removes that nodes, repaets until no more in-order packets reamin. (The muscle) Ensures in-order delivery
        pure_ack = true; //Set this to ACK highest contiguous byte

    }
    // Process ACKs
    if (ack_flag) {
        uint16_t their_ack = ntohs(pkt->ack);
        //Fast retransmit: count duplicates if their_ack equals previous last_ack increment dup_acks, else reset. Think timeout/out of order/resend
        if(their_ack == last_ack){
            dup_acks++;
        }
        else {
            dup_acks = 0;
            last_ack = their_ack;
        }
    
    //Drop any send_buf packets seq < their_ack
    send_buf = drop_acked(send_buf, their_ack);
    //Pick new oldest for timeout
    base_pkt = head_smallest(send_buf);

    //Recompute outstanding bytes in flight
    our_send_window = 0;
            for (buffer_node* t = send_buf; t; t = t->next){
                our_send_window += ntohs(t->pkt.length);
            }
        }
    }
    default:
        break;
    }
}









// Main function of transport layer; never quits
void listen_loop(int sockfd, struct sockaddr_in* addr, int initial_state,
                 ssize_t (*input_p)(uint8_t*, size_t),
                 void (*output_p)(uint8_t*, size_t)) {

    // Set initial state (whether client or server)
    state = initial_state;

    // Set input and output function pointers
    input = input_p;
    output = output_p;

    // Set socket for nonblocking
    int flags = fcntl(sockfd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(sockfd, F_SETFL, flags);
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int) {1}, sizeof(int));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &(int) {1}, sizeof(int));

    // Set initial sequence number
    uint32_t r;
    int rfd = open("/dev/urandom", 'r');
    read(rfd, &r, sizeof(uint32_t));
    close(rfd);
    srand(r);
    seq = (rand() % 10) * 100 + 100;

    // Setting timers
    gettimeofday(&now, NULL);
    gettimeofday(&start, NULL);

    // Create buffer for incoming data
    char buffer[sizeof(packet) + MAX_PAYLOAD] = {0};
    packet* pkt = (packet*) &buffer;
    socklen_t addr_size = sizeof(struct sockaddr_in);

    // Start listen loop
    while (true) {
        memset(buffer, 0, sizeof(packet) + MAX_PAYLOAD);
        // Get data from socket
        int bytes_recvd = recvfrom(sockfd, &buffer, sizeof(buffer), 0,
                                   (struct sockaddr*) addr, &addr_size);
        // If data, process it
        if (bytes_recvd > 0) {
            print_diag(pkt, RECV);
            recv_data(pkt);
        }

        packet* tosend = get_data();
        // Data available to send
        if (tosend != NULL) {
        }
        // Received a packet and must send an ACK
        else if (pure_ack) {
        }

        // Check if timer went off
        gettimeofday(&now, NULL);
        if (TV_DIFF(now, start) >= RTO && base_pkt != NULL) {
        }
        // Duplicate ACKS detected
        else if (dup_acks == DUP_ACKS && base_pkt != NULL) {
        }
        // No data to send, so restart timer
        else if (base_pkt == NULL) {
            gettimeofday(&start, NULL);
        }
    }
}