// Link layer protocol implementation
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#include <signal.h>

#include "link_layer.h"
#include "serial_port.h"

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source

#define BUF_SIZE 6

#define FLAG 0x7E

#define Awrite 0x03
#define CSet 0x03
#define BCC1w (Awrite ^ CSet)

#define Aread 0x01
#define CUA 0x07
#define BCC1r (Aread ^ CUA)

int alarmEnabled = FALSE;
int alarmCount = 0;
char bitTx = 0;
int retransmissions = 0;
int timeout = 3;

// Alarm function handler
void alarmHandler(int signal)
{   
    printf("Alarm triggered!\n");
    alarmEnabled = TRUE;
    alarmCount++;

    printf("Alarm #%d\n", alarmCount);
}

////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////


unsigned char byte;

int llopen(LinkLayer connectionParameters){

    LinkLayerState state = START;

    int fd = openSerialPort(connectionParameters.serialPort,connectionParameters.baudRate);
    if (fd < 0) return -1;

    retransmissions = connectionParameters.nRetransmissions;
    timeout = connectionParameters.timeout;

    switch (connectionParameters.role)
    {
    case LlTx:
        (void)signal(SIGALRM, alarmHandler);

        while(retransmissions != 0){

            //Send the supervision frame
            unsigned char supFrame[5] = {FLAG, Awrite, CSet, BCC1w, FLAG};
            write(fd, supFrame, 5);
            
            
            alarm(timeout);
            alarmEnabled = FALSE;

            while(alarmEnabled == FALSE && state != READ){

                if (read(fd, &byte, 1) > 0){
                switch (state)
                {
                case START:
                    if (byte == FLAG) state = FLAG1;
                    break;
                
                case FLAG1:
                    if (byte == Aread) state = A;
                    else if(byte != FLAG) state = START;
                    break;

                case A:
                    if (byte == CUA) state = C;
                    else if(byte == FLAG) state = FLAG1;
                    else state = START;
                    break;

                case C:
                    if (byte == (BCC1r)) state = BCC;
                    else if(byte == FLAG) state = FLAG1;
                    else state = FLAG;
                    break;

                case BCC:
                    if (byte == FLAG) state = READ;
                    else if(byte != (BCC1w)) state = START;
                    break;
                
                default:
                    break;
                }
                }
            }
            retransmissions --;
        }

        if (state != READ) return -1;
        break;
    
    case LlRx: 

        while(state != READ){

                if (read(fd, &byte, 1) > 0){
                switch (state)
                {
                case START:
                    if (byte == FLAG) state = FLAG1;
                    break;
                
                case FLAG1:
                    if (byte == Awrite) state = A;
                    else if(byte != FLAG) state = START;
                    break;

                case A:
                    if (byte == CSet) state = C;
                    else if(byte == FLAG) state = FLAG1;
                    else state = START;
                    break;

                case C:
                    if (byte == (BCC1w)) state = BCC;
                    else if(byte == FLAG) state = FLAG1;
                    else state = FLAG;
                    break;

                case BCC:
                    if (byte == FLAG) state = READ;
                    else state = START;
                    break;
                
                default:
                    break;
                }
            }
        }
        //Send the supervision frame
        unsigned char supFrame[5] = {FLAG, Aread, CUA, BCC1r, FLAG};
        write(fd, supFrame, 5);
        break;

    default:
        return -1;
        break;
    }

    return fd;
}

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(LinkLayer connectionParameters,const unsigned char *buf, int bufSize)
{
    int fd = openSerialPort(connectionParameters.serialPort,connectionParameters.baudRate);
    if (fd < 0) return -1;
    int frameSize = bufSize + 6;
    unsigned char *frame = (unsigned char*) malloc(frameSize);
    frame[0] = FLAG;
    frame[1] = Awrite;
    frame[2] = bitTx << 7; //nao sei se é 6 ou 7 | later bitTx = (bitTx++)%2
    frame[3] = frame[1] ^ frame[2];


    (void)signal(SIGALRM, alarmHandler);

    
    //Passa todos os bytes do buf para o frame (a partir do byte 4) 
    for (int i = 0; i < bufSize; i++){
        frame[i+4] = buf[i];
    }

    //calcula o bcc2
    u_int8_t bcc2 = buf[0];
    for(int i = 1; i< bufSize; i++){
        bcc2 = bcc2 ^ buf[i];
    }

    //STUFFING (swapping 0x7E for 0x7D 0x5E and 0x7D for 0x7D 0x5D)
    unsigned char stuffedFrame[MAX_PAYLOAD_SIZE*2 + 6];
    int stuffedFrameSize = byteStuffing(frame+1, frameSize-2, stuffedFrame);

    memcpy(frame, stuffedFrame, stuffedFrameSize+4);
    frame[stuffedFrameSize+4] = bcc2;
    frame[stuffedFrameSize+5] = FLAG;

    int aceite = 0;
    int rejeitado = 0;
    int transmission = 0;
    while (transmission < retransmissions){
        alarmEnabled = FALSE;
        alarm(timeout);
        rejeitado = 0;
        aceite = 0;

        while (alarmEnabled == FALSE && rejeitado == 0 && aceite == 0){

            write(fd, frame, stuffedFrameSize+6);
            unsigned char check = readAckFrame(fd);

            if (!check)
                continue;
            else if(check ==  0x54 || check == 0x55)
                rejeitado = 1;
            else if (check == 0xAA || check == 0xAB){
                aceite = 1;
                bitTx = (bitTx + 1)%2;
            }
            else continue;
            
        }
        if (aceite) break;
        transmission++;
    }

    free(frame);
    if(aceite) return frameSize;
    
    else{
        //FALTA FAZER CENAS PARA FECHAR
        return -1;
    }
}


// -------- AUXILIARY TO LLWRITE 
int byteStuffing(const unsigned char *frame, int frameSize, unsigned char *stuffedData){
    int j = 0;
    for (int i = 0; i < frameSize; i++) {
        if (frame[i] == 0x7E) {
            stuffedData[j] = 0x7D;
            j++;
            stuffedData[j] = 0x5E;
            j++;
        } 
        else if (frame[i] == 0x7D) {
            stuffedData[j] = 0x7D;
            j++;
            stuffedData[j] = 0x5D;
            j++;
        } 
        else {
            stuffedData[j] = frame[i];
            j++;
        }
    }
    return j;
}

//máquina de estados para ler o acknoldege frame    
unsigned char readAckFrame(int fd){

    unsigned char byte, cByte = 0;
    LinkLayerState state = START;
    
    while (state != READ&& alarmEnabled == FALSE) {  
        if (read(fd, &byte, 1) > 0 || 1) {
            switch (state) {
                case START:
                    if (byte == FLAG) state = FLAG1;
                    break;
                case FLAG1:
                    if (byte == Aread) state = A;
                    else if (byte != FLAG) state = START;
                    break;
                case A:
                    if (byte == 0xAA || byte == 0xAB || byte ==  0x54 || byte ==  0x55 || byte == 0x0B){ //basicamente só a checkar se o byte é mm o C
                        state = C;
                        cByte = byte;   
                    }
                    else if (byte == FLAG) state = FLAG1;
                    else state = START;
                    break;
                case C:
                    if (byte == (Awrite ^ cByte)) state = BCC;
                    else if (byte == FLAG) state = FLAG1;
                    else state = START;
                    break;
                case BCC:
                    if (byte == FLAG){
                        state = READ;
                    }
                    else state = START;
                    break;
                default: 
                    break;
            }
        } 
    } 
    return cByte;
}


////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////
int llread(unsigned char *packet)
{
    // TODO

    return 0;
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llclose(int showStatistics)
{
    // TODO
    /*
    int clstat = closeSerialPort(fd);
    return clstat;*/

    return 1;
}
